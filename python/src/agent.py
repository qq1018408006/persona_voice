import asyncio
import os
import sqlite3
from contextlib import suppress
from pathlib import Path
from typing import Callable

from dotenv import load_dotenv
from google.genai import types
from livekit.agents import (
    APIConnectOptions,
    Agent,
    AgentServer,
    AgentSession,
    JobContext,
    RunContext,
    cli,
    function_tool,
    room_io,
)
from livekit.agents.llm import ChatMessage
from livekit.plugins import google


# ---------------------------------------------------------------------------
# 基础配置
# ---------------------------------------------------------------------------

PROJECT_ROOT = Path(__file__).resolve().parents[1]
ENV_FILE = PROJECT_ROOT / ".env.local"

# override=True：避免 shell 里残留的旧 GOOGLE_API_KEY 覆盖 .env.local。
load_dotenv(ENV_FILE, override=True)

DB_PATH = Path(
    os.getenv("MEMO_DB_PATH", str(PROJECT_ROOT / "memos.db"))
).expanduser().resolve()

GEMINI_MODEL = os.getenv(
    "GEMINI_LIVE_MODEL",
    "gemini-2.5-flash-native-audio-preview-12-2025",
)
GEMINI_VOICE = os.getenv("GEMINI_LIVE_VOICE", "Puck")

# Gemini WebSocket 连接失败时的底层重试。
MODEL_MAX_RETRIES = int(os.getenv("MODEL_MAX_RETRIES", "10"))
MODEL_RETRY_INTERVAL_SECONDS = float(
    os.getenv("MODEL_RETRY_INTERVAL_SECONDS", "2")
)
MODEL_CONNECT_TIMEOUT_SECONDS = float(
    os.getenv("MODEL_CONNECT_TIMEOUT_SECONDS", "20")
)

# generate_reply 没有成功完成时，应用层最多重新发送几次。
REPLY_MAX_ATTEMPTS = int(os.getenv("REPLY_MAX_ATTEMPTS", "4"))
REPLY_WATCHDOG_SECONDS = float(
    os.getenv("REPLY_WATCHDOG_SECONDS", "15")
)
REPLY_ATTEMPT_TIMEOUT_SECONDS = float(
    os.getenv("REPLY_ATTEMPT_TIMEOUT_SECONDS", "20")
)

# 手机短暂退出时保留原 AgentSession，避免立即删除房间造成重连竞态。
# 超过宽限期仍未回来，关闭 Session 并删除房间。
RECONNECT_GRACE_SECONDS = float(
    os.getenv("RECONNECT_GRACE_SECONDS", "20")
)

# 设为大于 0 时，用户进入 away 后再持续静默这么久便关闭 Session。
# 默认 0 表示关闭该功能；例如在 .env.local 中设为 60 可节省待机额度。
IDLE_SHUTDOWN_SECONDS = float(
    os.getenv("IDLE_SHUTDOWN_SECONDS", "0")
)

ENABLE_GREETING = os.getenv("ENABLE_GREETING", "true").lower() in {
    "1",
    "true",
    "yes",
    "on",
}


def validate_environment() -> None:
    required = (
        "LIVEKIT_URL",
        "LIVEKIT_API_KEY",
        "LIVEKIT_API_SECRET",
        "GOOGLE_API_KEY",
    )
    missing = [name for name in required if not os.getenv(name)]
    if missing:
        raise RuntimeError(
            "缺少环境变量：" + ", ".join(missing)
            + f"。请检查 {ENV_FILE}。"
        )

    google_key = os.getenv("GOOGLE_API_KEY", "")
    if "你的" in google_key or "Gemini_API_Key" in google_key:
        raise RuntimeError(
            f"{ENV_FILE} 中的 GOOGLE_API_KEY 仍是占位符。"
        )


# ---------------------------------------------------------------------------
# SQLite 备忘录
# ---------------------------------------------------------------------------

def open_db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH, timeout=5.0)
    conn.execute("PRAGMA busy_timeout = 5000")
    return conn


def init_db() -> None:
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)

    with open_db() as conn:
        conn.execute("PRAGMA journal_mode = WAL")
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS memos (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                content TEXT NOT NULL,
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            )
            """
        )


@function_tool
async def create_memo(
    context: RunContext,
    content: str,
) -> str:
    """创建一条备忘录。

    Args:
        content: 要保存的完整备忘录内容。
    """
    del context

    normalized = content.strip()
    if not normalized:
        return "创建失败：备忘录内容为空。"

    with open_db() as conn:
        # 网络重试可能让同一工具调用再次出现。30 秒内相同内容只保存一次。
        existing = conn.execute(
            """
            SELECT id
            FROM memos
            WHERE content = ?
              AND created_at >= datetime('now', '-30 seconds')
            ORDER BY id DESC
            LIMIT 1
            """,
            (normalized,),
        ).fetchone()

        if existing:
            return (
                "这条备忘录刚刚已经创建，"
                f"编号为 {existing[0]}，未重复保存。"
            )

        cursor = conn.execute(
            "INSERT INTO memos(content) VALUES (?)",
            (normalized,),
        )
        memo_id = cursor.lastrowid

    return f"备忘录已创建，编号为 {memo_id}。"


@function_tool
async def list_memos(context: RunContext) -> str:
    """查询最近创建的十条备忘录。"""
    del context

    with open_db() as conn:
        rows = conn.execute(
            """
            SELECT id, content, created_at
            FROM memos
            ORDER BY id DESC
            LIMIT 10
            """
        ).fetchall()

    if not rows:
        return "目前没有备忘录。"

    return "\n".join(
        f"{memo_id}. {content}，创建于 {created_at}"
        for memo_id, content, created_at in rows
    )


# ---------------------------------------------------------------------------
# 回复重试
# ---------------------------------------------------------------------------

async def generate_reply_with_retry(
    session: AgentSession,
    *,
    instructions: str,
    generation_lock: asyncio.Lock,
    get_assistant_serial: Callable[[], int],
    is_session_closing: Callable[[], bool],
    get_model_error_state: Callable[[], tuple[int, bool, str]],
    max_attempts: int = REPLY_MAX_ATTEMPTS,
) -> bool:
    """主动生成回复；只在确实产生 assistant 消息后判定成功。"""

    async with generation_lock:
        for attempt in range(1, max_attempts + 1):
            if is_session_closing():
                print("[生成回复] Session 已关闭，停止重试", flush=True)
                return False

            assistant_serial_before = get_assistant_serial()
            error_serial_before, _, _ = get_model_error_state()

            print(
                f"[生成回复] 第 {attempt}/{max_attempts} 次发送",
                flush=True,
            )

            handle = None
            handle_error: Exception | None = None
            attempt_timed_out = False

            try:
                handle = session.generate_reply(
                    instructions=instructions,
                    allow_interruptions=True,
                )

                # 不再把 SpeechHandle 正常返回直接当作成功。
                # 必须同时观察 conversation_item_added 中的 assistant 消息。
                await asyncio.wait_for(
                    asyncio.shield(handle),
                    timeout=REPLY_ATTEMPT_TIMEOUT_SECONDS,
                )

            except asyncio.TimeoutError:
                attempt_timed_out = True
                print(
                    f"[生成回复] 第 {attempt} 次等待超过 "
                    f"{REPLY_ATTEMPT_TIMEOUT_SECONDS:.0f} 秒",
                    flush=True,
                )

            except asyncio.CancelledError:
                raise

            except Exception as error:
                handle_error = error
                print(
                    f"[生成回复] 第 {attempt} 次异常："
                    f"{type(error).__name__}: {error}",
                    flush=True,
                )

            # conversation_item_added 往往紧跟在 SpeechHandle 完成后发出，
            # 留出很短时间让事件处理器更新序号。
            for _ in range(20):
                if get_assistant_serial() > assistant_serial_before:
                    print("[生成回复] 成功", flush=True)
                    return True

                if is_session_closing():
                    print(
                        "[生成回复] Session 因错误关闭，停止重试",
                        flush=True,
                    )
                    return False

                await asyncio.sleep(0.05)

            error_serial, recoverable, model_error_text = (
                get_model_error_state()
            )

            if error_serial > error_serial_before:
                if not recoverable:
                    print(
                        "[生成回复] 遇到不可恢复模型错误，停止重试："
                        f"{model_error_text}",
                        flush=True,
                    )
                    return False

                print(
                    "[生成回复] 本次遇到可恢复模型错误："
                    f"{model_error_text}",
                    flush=True,
                )

            if (
                handle is not None
                and not attempt_timed_out
                and handle_error is None
            ):
                try:
                    reported_error = handle.exception()
                except Exception as error:
                    reported_error = error

                if reported_error is not None:
                    print(
                        f"[生成回复] 第 {attempt} 次失败："
                        f"{type(reported_error).__name__}: "
                        f"{reported_error}",
                        flush=True,
                    )
                else:
                    print(
                        "[生成回复] SpeechHandle 已结束，但没有产生 "
                        "assistant 消息，按失败处理",
                        flush=True,
                    )

            if attempt >= max_attempts:
                break

            # 取消迟到的旧回复，防止下一次生成与它重叠。
            if not is_session_closing():
                with suppress(Exception):
                    await session.interrupt(force=True)

            delay = min(2 ** (attempt - 1), 8)
            print(
                f"[生成回复] {delay} 秒后重新发送",
                flush=True,
            )
            await asyncio.sleep(delay)

    print("[生成回复] 达到最大重试次数", flush=True)
    return False


validate_environment()
init_db()
server = AgentServer()


# ---------------------------------------------------------------------------
# Agent Job
# ---------------------------------------------------------------------------

@server.rtc_session(agent_name="memo-agent")
async def entrypoint(ctx: JobContext) -> None:
    generation_lock = asyncio.Lock()

    last_user_text = ""
    turn_serial = 0
    answered_turn_serial = 0
    assistant_message_serial = 0
    model_error_serial = 0
    last_model_error_recoverable = False
    last_model_error_text = ""

    agent_state = "initializing"
    user_state = "listening"
    client_connected = True
    session_closing = False
    retry_in_progress = False

    retry_task: asyncio.Task[None] | None = None
    disconnect_shutdown_task: asyncio.Task[None] | None = None
    idle_shutdown_task: asyncio.Task[None] | None = None

    def get_assistant_serial() -> int:
        return assistant_message_serial

    def is_session_closing() -> bool:
        return session_closing

    def get_model_error_state() -> tuple[int, bool, str]:
        return (
            model_error_serial,
            last_model_error_recoverable,
            last_model_error_text,
        )

    session = AgentSession(
        llm=google.realtime.RealtimeModel(
            model=GEMINI_MODEL,
            voice=GEMINI_VOICE,
            instructions=(
                "你是中文语音助手。始终用简洁、自然的中文回答。"
                "听不清时请要求用户重复，不要猜测其他语言。"
            ),
            conn_options=APIConnectOptions(
                max_retry=MODEL_MAX_RETRIES,
                retry_interval=MODEL_RETRY_INTERVAL_SECONDS,
                timeout=MODEL_CONNECT_TIMEOUT_SECONDS,
            ),
            realtime_input_config=types.RealtimeInputConfig(
                automatic_activity_detection=(
                    types.AutomaticActivityDetection(
                        disabled=False,
                        start_of_speech_sensitivity=(
                            types.StartSensitivity.START_SENSITIVITY_HIGH
                        ),
                        end_of_speech_sensitivity=(
                            types.EndSensitivity.END_SENSITIVITY_HIGH
                        ),
                        prefix_padding_ms=20,
                        silence_duration_ms=500,
                    )
                ),
            ),
        ),
        # 这里只负责把长期静音标记为 away；是否关 Session 由下面可选逻辑决定。
        user_away_timeout=30.0,
    )

    agent = Agent(
        instructions="""
        你是一个中文语音备忘录助手。

        用户要求记录事项时，必须调用 create_memo。
        用户询问已记录内容时，必须调用 list_memos。
        工具没有返回成功前，不得声称操作成功。
        遇到网络重试时，先查看当前上下文中是否已经有工具结果；
        不要重复执行已经成功的写入操作。
        回复简洁、自然，适合语音朗读。
        """,
        tools=[create_memo, list_memos],
    )

    def cancel_task(task: asyncio.Task[None] | None) -> None:
        if task is not None and not task.done():
            task.cancel()

    async def retry_last_user_turn(
        *,
        expected_turn: int,
        transcript: str,
        delay: float,
        reason: str,
    ) -> None:
        nonlocal answered_turn_serial, retry_in_progress

        try:
            await asyncio.sleep(delay)

            if (
                session_closing
                or not client_connected
                or expected_turn != turn_serial
                or answered_turn_serial >= expected_turn
            ):
                return

            # 模型仍在思考/说话时再给它一点时间，避免重复回答。
            if agent_state in {"thinking", "speaking"}:
                await asyncio.sleep(8.0)

                if (
                    session_closing
                    or not client_connected
                    or expected_turn != turn_serial
                    or answered_turn_serial >= expected_turn
                ):
                    return

            retry_in_progress = True
            print(
                f"[自动重试] {reason}；重发第 {expected_turn} 轮",
                flush=True,
            )

            success = await generate_reply_with_retry(
                session,
                instructions=(
                    "上一轮因为网络或模型超时没有完成回复。"
                    "请继续回答用户最后一句话。"
                    "如果对应工具已经成功执行，不要重复调用；"
                    "如果尚未执行，则正常调用所需工具。"
                    f"\n用户最后一句转写：{transcript}"
                ),
                generation_lock=generation_lock,
                get_assistant_serial=get_assistant_serial,
                is_session_closing=is_session_closing,
                get_model_error_state=get_model_error_state,
            )

            if success:
                answered_turn_serial = max(
                    answered_turn_serial,
                    expected_turn,
                )
            else:
                print(
                    "[自动重试] 仍未成功，等待用户重新说一次",
                    flush=True,
                )

        except asyncio.CancelledError:
            return
        finally:
            retry_in_progress = False

    def schedule_turn_retry(
        *,
        delay: float,
        reason: str,
        replace_existing: bool,
    ) -> None:
        nonlocal retry_task

        if (
            session_closing
            or not client_connected
            or not last_user_text
            or answered_turn_serial >= turn_serial
        ):
            return

        if retry_task is not None and not retry_task.done():
            if not replace_existing or retry_in_progress:
                return
            retry_task.cancel()

        retry_task = asyncio.create_task(
            retry_last_user_turn(
                expected_turn=turn_serial,
                transcript=last_user_text,
                delay=delay,
                reason=reason,
            )
        )

    async def shutdown_after_disconnect(identity: str) -> None:
        try:
            print(
                f"[重连等待] {identity} 已离开，"
                f"保留 Session {RECONNECT_GRACE_SECONDS:.0f} 秒",
                flush=True,
            )
            await asyncio.sleep(RECONNECT_GRACE_SECONDS)

            if ctx.room.remote_participants:
                return

            print(
                "[重连等待] 超时且无人重连，关闭 Session 并删除房间",
                flush=True,
            )
            session.shutdown(drain=False)
        except asyncio.CancelledError:
            return

    async def shutdown_after_idle() -> None:
        try:
            await asyncio.sleep(IDLE_SHUTDOWN_SECONDS)

            if (
                IDLE_SHUTDOWN_SECONDS > 0
                and user_state == "away"
                and client_connected
                and not session_closing
            ):
                print(
                    "[空闲超时] 用户持续无交互，关闭 Session",
                    flush=True,
                )
                session.shutdown(drain=False)
        except asyncio.CancelledError:
            return

    # ------------------------- LiveKit Room 事件 -------------------------

    @ctx.room.on("active_speakers_changed")
    def on_active_speakers_changed(speakers) -> None:
        identities = [participant.identity for participant in speakers]
        print(f"[LiveKit活跃说话人] {identities}", flush=True)

    @ctx.room.on("participant_connected")
    def on_participant_connected(participant) -> None:
        nonlocal client_connected

        client_connected = True
        cancel_task(disconnect_shutdown_task)
        print(
            f"[参与者重连] {participant.identity}",
            flush=True,
        )

    @ctx.room.on("participant_disconnected")
    def on_participant_disconnected(participant) -> None:
        nonlocal client_connected, disconnect_shutdown_task

        client_connected = False
        cancel_task(retry_task)
        cancel_task(idle_shutdown_task)
        cancel_task(disconnect_shutdown_task)

        disconnect_shutdown_task = asyncio.create_task(
            shutdown_after_disconnect(participant.identity)
        )

    # ------------------------- AgentSession 事件 -------------------------

    @session.on("user_state_changed")
    def on_user_state_changed(event) -> None:
        nonlocal user_state, idle_shutdown_task

        user_state = event.new_state
        print(
            f"[用户状态] {event.old_state} -> {event.new_state}",
            flush=True,
        )

        cancel_task(idle_shutdown_task)

        if (
            event.new_state == "away"
            and IDLE_SHUTDOWN_SECONDS > 0
        ):
            idle_shutdown_task = asyncio.create_task(
                shutdown_after_idle()
            )

    @session.on("agent_state_changed")
    def on_agent_state_changed(event) -> None:
        nonlocal agent_state

        agent_state = event.new_state
        print(
            f"[Agent状态] {event.old_state} -> {event.new_state}",
            flush=True,
        )

    @session.on("user_input_transcribed")
    def on_user_input_transcribed(event) -> None:
        nonlocal last_user_text, turn_serial, retry_task

        transcript = event.transcript.strip()
        print(
            f"[用户转写] final={event.is_final}: {transcript}",
            flush=True,
        )

        if not event.is_final or not transcript:
            return

        turn_serial += 1
        last_user_text = transcript

        cancel_task(retry_task)
        retry_task = asyncio.create_task(
            retry_last_user_turn(
                expected_turn=turn_serial,
                transcript=transcript,
                delay=REPLY_WATCHDOG_SECONDS,
                reason="等待正常回复超时",
            )
        )

    @session.on("conversation_item_added")
    def on_conversation_item_added(event) -> None:
        nonlocal answered_turn_serial, assistant_message_serial

        item = event.item
        if not isinstance(item, ChatMessage):
            return

        role = item.role
        text = item.text_content
        print(
            f"[对话内容] role={role}, text={text}",
            flush=True,
        )

        if role == "assistant" and text:
            assistant_message_serial += 1

            if turn_serial > 0:
                answered_turn_serial = turn_serial
                if not retry_in_progress:
                    cancel_task(retry_task)

    @session.on("error")
    def on_error(event) -> None:
        nonlocal model_error_serial
        nonlocal last_model_error_recoverable
        nonlocal last_model_error_text

        print(f"[会话错误] {event}", flush=True)

        model_error = getattr(event, "error", None)
        recoverable = bool(
            getattr(model_error, "recoverable", False)
        )
        raw_error_text = str(model_error)
        error_text = raw_error_text.lower()

        model_error_serial += 1
        last_model_error_recoverable = recoverable
        last_model_error_text = raw_error_text

        network_like_error = any(
            keyword in error_text
            for keyword in (
                "timed out",
                "timeout",
                "1011",
                "connection",
                "ssl",
                "websocket",
                "record layer failure",
            )
        )

        # 不可恢复错误（例如模型配置错误）不能在同一个
        # RealtimeSession 上重试；Session 会由框架关闭。
        if not recoverable:
            print(
                "[会话错误] 不可恢复，不调度同 Session 重试",
                flush=True,
            )
            return

        if network_like_error:
            schedule_turn_retry(
                delay=2.0,
                reason="检测到可恢复的模型/网络错误",
                replace_existing=True,
            )

    @session.on("close")
    def on_close(event) -> None:
        nonlocal session_closing

        session_closing = True
        cancel_task(retry_task)
        cancel_task(disconnect_shutdown_task)
        cancel_task(idle_shutdown_task)
        print(
            f"[Session关闭] reason={event.reason}",
            flush=True,
        )

    # close_on_disconnect=False：允许手机在宽限期内快速重连。
    # delete_room_on_close=True：宽限期超时后关闭 Session 时清理固定房间，
    # 下一次手机进入会创建新房间并重新 Dispatch Agent。
    await session.start(
        agent=agent,
        room=ctx.room,
        room_options=room_io.RoomOptions(
            close_on_disconnect=False,
            delete_room_on_close=True,
        ),
    )

    await ctx.connect()
    client_connected = bool(ctx.room.remote_participants)

    if ENABLE_GREETING and client_connected:
        greeting_ok = await generate_reply_with_retry(
            session,
            instructions=(
                "请立刻用中文说："
                "连接成功，我现在可以和你通话了。"
            ),
            generation_lock=generation_lock,
            get_assistant_serial=get_assistant_serial,
            is_session_closing=is_session_closing,
            get_model_error_state=get_model_error_state,
        )

        if greeting_ok:
            print("[主动问候] 完成", flush=True)
        else:
            print(
                "[主动问候] 失败，但继续保持会话等待用户说话",
                flush=True,
            )


if __name__ == "__main__":
    cli.run_app(server)
