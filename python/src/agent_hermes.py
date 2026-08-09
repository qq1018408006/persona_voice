import asyncio
import json
import os
import time
import uuid
from pathlib import Path
from typing import Any

import aiohttp
from dotenv import load_dotenv
from google.genai import types
from livekit import api as livekit_api
from livekit.agents import (
    Agent,
    AgentServer,
    AgentSession,
    APIConnectOptions,
    JobContext,
    RunContext,
    cli,
    function_tool,
    room_io,
)
from livekit.plugins import google

from backend_support import (
    BackendRoute,
    CodexClient,
    CodexError,
    channel_delivery_instructions,
    route_explicit_backend,
)

# =============================================================================
# 配置
# =============================================================================

PROJECT_ROOT = Path(__file__).resolve().parents[1]
# 统一环境变量文件在仓库根目录（persona_voice/.env）
ENV_FILE = PROJECT_ROOT.parent / ".env"
load_dotenv(ENV_FILE, override=True)

GEMINI_MODEL = os.getenv(
    "GEMINI_LIVE_MODEL",
    "gemini-3.1-flash-live-preview",
)
GEMINI_VOICE = os.getenv("GEMINI_LIVE_VOICE", "Puck")

# Gemini 3.1 Live 不接受 LiveKit AgentSession.generate_reply()，因为它的
# 会话上下文不可变。唤醒、待机确认和后台结果因此直接走 Google Realtime
# transport。对于 Gemini 3.1，普通会话进行中只能使用 RealtimeInput.text：
# ClientContent 只允许作为初始历史，不能拿来追加每一次系统提示。
SPARKBOT_WAKE_GREETING_TEXT = (
    "系统唤醒：SparkBot。立刻用中文语音只说“我在，请说”。"
    "不要解释，不要调用工具。"
)

SPARKBOT_SLEEP_CANCEL_TEXT = (
    "系统事件：SparkBot 用户在排空待机期间再次说了唤醒词，已取消待机并重新打开麦克风。"
    "请立刻用一句简短自然的中文回应，例如“我在，继续说”。"
    "不要调用工具；已有后台任务必须继续执行。"
)


def sparkbot_sleep_ack_text(has_background_work: bool) -> str:
    """Return the one controlled spoken acknowledgement for local standby."""
    if has_background_work:
        return (
            "系统事件：SparkBot 已收到本地待机口令，用户麦克风已经关闭。"
            "这不是用户说的“休息吧”，而是一条必须执行的语音输出指令。"
            "立刻用中文语音只说：好的，任务继续执行，完成后告诉你。"
            "不要调用工具，不要输出其他内容。"
        )
    return (
        "系统事件：SparkBot 已收到本地待机口令，用户麦克风已经关闭。"
        "这不是用户说的“休息吧”，而是一条必须执行的语音输出指令。"
        "立刻用中文语音只说：好的，正在进入待机。"
        "不要调用工具，不要输出其他内容。"
    )


def _gemini_realtime_session(session: AgentSession) -> Any | None:
    """Return the current Google realtime transport without assuming its type."""
    activity = getattr(session, "_activity", None)
    return getattr(activity, "realtime_llm_session", None)


def restart_gemini_realtime_transport(session: AgentSession) -> bool:
    """Request a transport reset for the explicit local stop command.

    Gemini's ``ActivityStart`` is only valid when automatic activity detection
    is disabled.  This agent deliberately keeps Gemini automatic endpointing
    for ordinary speech, so a raw ActivityStart is not a valid stop packet.
    The Google Live bridge has no response-cancel event; its supported hard
    cancellation boundary is closing/restarting the realtime transport.  The
    bridge reopens it with the latest session-resumption handle and the local
    chat context, while already-started backend tasks remain outside the
    realtime connection.

    ``_mark_restart_needed`` is a guarded compatibility seam in the installed
    LiveKit Google bridge.  Feature-detect it so an SDK change degrades to the
    local playout stop instead of crashing the data-channel callback.
    """
    realtime_session = _gemini_realtime_session(session)
    if realtime_session is None:
        return False
    request_restart = getattr(realtime_session, "_mark_restart_needed", None)
    if not callable(request_restart):
        return False
    try:
        request_restart()
    except Exception as exc:
        print(
            f"[Gemini停止] Realtime 传输重置请求失败：{type(exc).__name__}: {exc}",
            flush=True,
        )
        return False
    return True


def send_gemini_realtime_text(session: AgentSession, text: str) -> bool:
    """Send system-originated text over Gemini's active realtime input.

    The installed bridge has no public AgentSession method for a
    Gemini-3.1-compatible forced response.  ``RealtimeInput.text`` is the
    protocol-supported way to inject a new text input after a Live session has
    begun; Gemini's automatic activity detection owns the resulting response.
    """
    realtime_session = _gemini_realtime_session(session)
    enqueue = getattr(realtime_session, "_send_client_event", None)
    if not callable(enqueue):
        return False

    normalized = text.strip()
    if not normalized:
        return False

    try:
        enqueue(types.LiveClientRealtimeInput(text=normalized))
    except Exception as exc:
        print(f"[SparkBot] 注入 Gemini 文本轮次失败：{type(exc).__name__}: {exc}", flush=True)
        return False
    return True


SPARKBOT_STOP_TRANSCRIPTS = frozenset(
    {
        # Chinese forms registered in the device's single MultiNet model.
        "等一等",
        "等一下",
        "停停停",
        "别说了",
        "停一下",
        "先停一下",
        "先别说",
        "暂停一下",
        "停下吧",
        "闭嘴",
        "打住",
        "停一停",
        "停下停下",
        "别讲了",
        "别说话",
        "你别说了",
        "你先停一下",
        "等一下等一下",
        "你闭嘴",
        "安静",
        # Common traditional-character ASR output and pinyin residuals.
        "別說了",
        "暫停一下",
        "閉嘴",
        "別講了",
        "別說話",
        "dengyideng",
        "dengyixia",
        "tingtingting",
        "bieshuole",
        "tingyixia",
        "xiantingyixia",
        "xianbieshuo",
        "zantingyixia",
        "tingxiaba",
        "bizui",
        "dazhu",
        "tingyiting",
        "tingxiatingxia",
        "biejiangle",
        "bieshuohua",
        "nibieshuole",
        "nixiantingyixia",
        "dengyixiadengyixia",
        "nibizui",
        "anjing",
    }
)

# When the device-local MultiNet misses a stop phrase, the Gemini realtime
# transcript is the fallback authority.  It arrives only after the user has
# already spoken, so the guard is intentionally short: enough to swallow the
# interrupted turn's trailing transcripts, but not so long that the next
# instruction feels blocked.
SPARKBOT_STOP_TRANSCRIPT_FALLBACK_GUARD_SECONDS = 2.0


def is_sparkbot_stop_transcript(text: str) -> bool:
    """Recognize a device-local Chinese stop phrase echoed by Gemini."""
    normalized = "".join(character for character in text.lower() if character.isalnum())
    if not normalized:
        return False
    # MultiNet normally emits no transcript for the local command.  This form
    # covers Gemini's occasional echo if the command tail was already in
    # flight when the device muted RTP.
    return normalized in SPARKBOT_STOP_TRANSCRIPTS


def close_gemini_realtime_audio_input(session: AgentSession) -> bool:
    """Confirm that the SparkBot-side RTP mute has stopped Gemini audio input.

    The installed LiveKit Google bridge forwards audio/text/activity realtime
    inputs but currently drops Gemini's ``audio_stream_end`` field.  Sending
    that field would therefore be a misleading no-op.  The firmware mutes at
    the LiveKit RTP boundary *before* it sends ``sparkbot.sleep_request``, so
    no more frames can reach this realtime session; retain only this readiness
    check for clear diagnostics.
    """
    realtime_session = _gemini_realtime_session(session)
    return realtime_session is not None

HERMES_BASE_URL = os.getenv(
    "HERMES_BASE_URL",
    "http://127.0.0.1:8642",
).rstrip("/")
HERMES_API_KEY = os.getenv("HERMES_API_KEY", "")
HERMES_MODEL = os.getenv("HERMES_MODEL", "hermes-agent")
HERMES_RUN_TIMEOUT_SECONDS = float(os.getenv("HERMES_RUN_TIMEOUT_SECONDS", "300"))
HERMES_POLL_INTERVAL_SECONDS = float(os.getenv("HERMES_POLL_INTERVAL_SECONDS", "0.5"))

# 用于 Hermes 长期记忆的稳定作用域。
# 多用户场景应改成每个用户唯一的值。
HERMES_SESSION_KEY = os.getenv(
    "HERMES_SESSION_KEY",
    "voice-agent-local-user",
)

CODEX_COMMAND = os.getenv("CODEX_COMMAND", "codex")
CODEX_MODEL = os.getenv("CODEX_MODEL", "")
CODEX_WORKSPACE = (
    Path(os.getenv("CODEX_WORKSPACE", str(PROJECT_ROOT.parent))).expanduser().resolve()
)
CODEX_RUN_TIMEOUT_SECONDS = float(os.getenv("CODEX_RUN_TIMEOUT_SECONDS", "600"))
CHANNEL_SENDER_COMMAND = os.getenv("CHANNEL_SENDER_COMMAND", "hermes")
HERMES_CHANNEL_TARGET = os.getenv("HERMES_CHANNEL_TARGET", "telegram")
CODEX_CHANNEL_TARGET = os.getenv("CODEX_CHANNEL_TARGET", HERMES_CHANNEL_TARGET)

ENABLE_GREETING = os.getenv("ENABLE_GREETING", "false").lower() in {
    "1",
    "true",
    "yes",
    "on",
}

FRONT_AGENT_INSTRUCTIONS = """
你是一个实时中文语音交互 Agent，直接通过语音与用户交流。

你的职责：
1. 保持自然、简洁、拟人的中文语音交流。
2. 普通闲聊、简单解释、澄清问题可以直接回答。
3. 用户说“让小C做……”或“让小 C 做……”时，必须调用
   delegate_to_codex，并把“让小C做”后面的内容整理成 task。禁止改调
   Hermes。
4. 用户说“让小马做……”或“让小 马 做……”时，必须调用
   delegate_to_hermes，并把“让小马做”后面的内容整理成 task。禁止改调
   Codex。
5. 用户没有点名后端时：需要写代码、修改工程、代码审查或运行开发工具的
   任务优先交给 Codex；其他本机操作、资料处理和复杂任务交给 Hermes。
6. 不得假装已经执行任务。后端没有成功返回前，不得声称任务完成。
7. 调用后端工具时，把用户目标、限制条件和必要上下文整理成一条完整、
   独立、可执行的中文任务描述。
8. 当你决定调用 delegate_to_hermes 或 delegate_to_codex 时，必须立刻在
   同一轮内发出函数调用（工具会在后台立即启动）。可以在发出调用之前或
   同时说一句简短确认，例如“好的，我来处理”；但确认语不能替代函数调用，
   禁止只确认而不调用，也禁止把调用推迟到后续轮次。确认语只能表示已接受
   任务，不得提前声称任务完成。
9. 后端返回后，直接用自然中文向用户播报结果。如果后端报告已把文件发送
   到自己的 channel，只需说明发送状态，不要朗读完整路径。
10. 可以让语气更口语化，但不得改变成功或失败状态、数字、时间、路径、
文件名和重要事实。
11. 用户插话时立即停止当前表达，优先处理用户的新要求。
12. “Jarvis”（以及同音的“贾维斯/嘉维斯”）在对话中是 SparkBot 的本地
待机口令，不是普通对话内容；语音转写里出现它时不要回复、不要调用工具。
若随后收到明确的 SparkBot 系统事件，按事件要求用一句确认语音回应。
13. 用户明确表示要休息、待机或睡觉（例如“休息吧”“我要休息了”
“我要睡觉了”；语音转写可能不准确，甚至变成其他语言/脚本）时，必须
立即调用 request_standby。不要把它当普通对话回复，不要调用其他工具。
14. delegate_to_hermes / delegate_to_codex 会立即返回“后台任务已启动”。
真正的后端结果稍后可能以“系统事件：……后台任务已经完成”的实时文本到达；
收到这类事件时，直接自然、简洁地播报其中的真实结果，不要声称是你在执行，
不要再次调用工具，播报后恢复正常对话。
"""

# 个人日记目录通过环境变量注入，避免在公开源码里暴露本机路径。
HERMES_DIARY_DIR = os.getenv("HERMES_DIARY_DIR", "").strip()
_diary_directive = (
    f'存储的用户个人日记目录为"{HERMES_DIARY_DIR}"，只可读、禁止写入删除。'
    if HERMES_DIARY_DIR
    else "未配置个人日记目录；若需要读取，请在仓库根目录 .env 设置 HERMES_DIARY_DIR。"
)

HERMES_INSTRUCTIONS = f"""
你是运行在本机的任务执行 Agent，通过另一个实时语音 Agent 为用户服务。

请使用你拥有的工具、技能、记忆和本地能力真实完成用户任务。
不要假装执行；工具未成功前不得声称完成。

外部文件：
{_diary_directive}

安全要求：
1. 删除文件、发送消息、付款、修改系统关键配置等高风险操作，如果用户
   没有明确确认，不得执行。请在最终结果中简洁说明需要确认什么。
2. 用户的输入来自语音转写，可能有少量同音字或识别错误；必要时根据
   上下文合理理解，但不能擅自改变关键数字、时间、路径或对象。
3. 不要输出思考过程、工具日志或冗长解释。
4. 最终先输出一段简洁、自然、适合语音朗读的纯文本中文，通常一到三句。
""" + channel_delivery_instructions(
    backend_name="Hermes",
    target=HERMES_CHANNEL_TARGET,
    sender_command=CHANNEL_SENDER_COMMAND,
)


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
            "缺少环境变量：" + ", ".join(missing) + f"。请检查 {ENV_FILE}。"
        )

    if not HERMES_API_KEY:
        raise RuntimeError(
            "缺少 HERMES_API_KEY。请在仓库根目录 .env 中配置本地 Hermes API 密钥。"
        )


# =============================================================================
# Hermes Runs API 客户端
# =============================================================================


class HermesError(RuntimeError):
    pass


class HermesClient:
    def __init__(
        self,
        *,
        base_url: str,
        api_key: str,
        model: str,
        session_key: str,
    ) -> None:
        self._base_url = base_url.rstrip("/")
        self._api_key = api_key
        self._model = model
        self._session_key = session_key

    def _headers(self) -> dict[str, str]:
        return {
            "Authorization": f"Bearer {self._api_key}",
            "Content-Type": "application/json",
            "X-Hermes-Session-Key": self._session_key,
        }

    async def _read_json(
        self,
        response: aiohttp.ClientResponse,
    ) -> dict[str, Any]:
        text = await response.text()

        if response.status < 200 or response.status >= 300:
            raise HermesError(f"Hermes HTTP {response.status}: {text[:500]}")

        try:
            data = json.loads(text)
        except json.JSONDecodeError as error:
            raise HermesError(f"Hermes 返回了无效 JSON：{text[:500]}") from error

        if not isinstance(data, dict):
            raise HermesError("Hermes 返回格式不是 JSON 对象。")

        return data

    async def health(self) -> bool:
        timeout = aiohttp.ClientTimeout(total=5)

        try:
            async with (
                aiohttp.ClientSession(
                    timeout=timeout,
                    headers=self._headers(),
                ) as http,
                http.get(f"{self._base_url}/health") as response,
            ):
                data = await self._read_json(response)
                return data.get("status") == "ok"
        except (aiohttp.ClientError, asyncio.TimeoutError, HermesError):
            return False

    async def start_run(
        self,
        *,
        task: str,
        session_id: str,
    ) -> str:
        payload = {
            "model": self._model,
            "input": task,
            "session_id": session_id,
            "instructions": HERMES_INSTRUCTIONS,
        }

        timeout = aiohttp.ClientTimeout(total=20)

        try:
            async with (
                aiohttp.ClientSession(
                    timeout=timeout,
                    headers=self._headers(),
                ) as http,
                http.post(
                    f"{self._base_url}/v1/runs",
                    json=payload,
                ) as response,
            ):
                data = await self._read_json(response)
        except (aiohttp.ClientError, asyncio.TimeoutError) as error:
            raise HermesError(
                f"无法启动 Hermes 任务：{type(error).__name__}: {error}"
            ) from error

        run_id = data.get("run_id")
        if not isinstance(run_id, str) or not run_id:
            raise HermesError(f"Hermes 没有返回 run_id：{data}")

        return run_id

    async def get_run(self, run_id: str) -> dict[str, Any]:
        timeout = aiohttp.ClientTimeout(total=10)

        try:
            async with (
                aiohttp.ClientSession(
                    timeout=timeout,
                    headers=self._headers(),
                ) as http,
                http.get(f"{self._base_url}/v1/runs/{run_id}") as response,
            ):
                return await self._read_json(response)
        except (aiohttp.ClientError, asyncio.TimeoutError) as error:
            raise HermesError(
                f"读取 Hermes 状态失败：{type(error).__name__}: {error}"
            ) from error

    async def stop_run(self, run_id: str) -> None:
        timeout = aiohttp.ClientTimeout(total=10)

        try:
            async with (
                aiohttp.ClientSession(
                    timeout=timeout,
                    headers=self._headers(),
                ) as http,
                http.post(
                    f"{self._base_url}/v1/runs/{run_id}/stop",
                    json={},
                ) as response,
            ):
                # 已经完成或已经停止时，服务端可能返回不同状态；
                # 这里只在明显的 HTTP 错误时记录。
                if response.status < 200 or response.status >= 300:
                    text = await response.text()
                    print(
                        f"[Hermes] 停止请求返回 HTTP {response.status}: {text[:300]}",
                        flush=True,
                    )
        except (aiohttp.ClientError, asyncio.TimeoutError) as error:
            print(
                f"[Hermes] 停止请求失败：{type(error).__name__}: {error}",
                flush=True,
            )

    @staticmethod
    def _normalize_output(output: Any) -> str:
        if isinstance(output, str):
            text = output.strip()
        elif isinstance(output, dict):
            speech = output.get("speech")
            if isinstance(speech, str):
                text = speech.strip()
            else:
                text = json.dumps(output, ensure_ascii=False)
        else:
            text = str(output).strip()

        if not text:
            raise HermesError("Hermes 已完成，但没有返回可播报内容。")

        # 防止异常长输出直接交给语音模型。
        return text[:4000]

    async def wait_for_result(
        self,
        run_id: str,
    ) -> str:
        loop = asyncio.get_running_loop()
        deadline = loop.time() + HERMES_RUN_TIMEOUT_SECONDS
        transient_failures = 0

        while True:
            if loop.time() >= deadline:
                await self.stop_run(run_id)
                raise HermesError(
                    f"Hermes 处理超过 {HERMES_RUN_TIMEOUT_SECONDS:.0f} 秒，已请求停止。"
                )

            try:
                data = await self.get_run(run_id)
                transient_failures = 0
            except HermesError as error:
                transient_failures += 1
                if transient_failures >= 5:
                    raise
                print(
                    f"[Hermes] 状态查询暂时失败 ({transient_failures}/5)：{error}",
                    flush=True,
                )
                await asyncio.sleep(min(HERMES_POLL_INTERVAL_SECONDS * 2, 2.0))
                continue

            status = str(data.get("status", "")).lower()

            if status == "completed":
                return self._normalize_output(data.get("output"))

            if status == "failed":
                error_value = data.get("error") or data.get("message") or "未知错误"
                raise HermesError(f"Hermes 执行失败：{error_value}")

            if status == "cancelled":
                raise HermesError("Hermes 任务已取消。")

            if status in {
                "pending_approval",
                "awaiting_approval",
                "approval_required",
                "needs_approval",
            }:
                await self.stop_run(run_id)
                return (
                    "这项操作需要用户进一步确认，当前没有执行。"
                    "请简洁询问用户是否确认继续。"
                )

            await asyncio.sleep(HERMES_POLL_INTERVAL_SECONDS)


# =============================================================================
# LiveKit + Gemini Live
# =============================================================================

validate_environment()


async def cleanup_stale_sparkbot_rooms() -> None:
    """Delete SparkBot rooms that no longer have a live agent in them.

    LiveKit only applies the token's roomConfig (explicit agent dispatch) when
    the room is *first created*.  The device's warm standby keeps the room
    alive across an agent restart, so a freshly registered worker is never
    dispatched into it.  Deleting those stale rooms at startup lets the device
    fall back to cold standby; the next wake then creates a brand-new room and
    the token dispatch applies again.

    Only rooms that contain a ``sparkbot-`` participant and no ``agent-``
    participant are touched, so an active conversation on another worker is
    never disrupted.
    """
    room_name = os.getenv("LIVEKIT_ROOM", "").strip()
    deleted: list[str] = []
    try:
        async with livekit_api.LiveKitAPI() as lkapi:
            list_request = (
                livekit_api.ListRoomsRequest(names=[room_name])
                if room_name
                else livekit_api.ListRoomsRequest()
            )
            listed = await lkapi.room.list_rooms(list_request)
            for room in listed.rooms:
                try:
                    participants = await lkapi.room.list_participants(
                        livekit_api.ListParticipantsRequest(room=room.name)
                    )
                except Exception as exc:
                    print(
                        f"[LiveKit启动清理] 查看房间 {room.name} 参与者失败："
                        f"{type(exc).__name__}: {exc}",
                        flush=True,
                    )
                    continue

                identities = [p.identity for p in participants.participants]
                has_device = any(i.startswith("sparkbot-") for i in identities)
                has_agent = any(i.startswith("agent-") for i in identities)
                if not has_device or has_agent:
                    continue

                try:
                    await lkapi.room.delete_room(
                        livekit_api.DeleteRoomRequest(room=room.name)
                    )
                    deleted.append(room.name)
                except Exception as exc:
                    print(
                        f"[LiveKit启动清理] 删除旧房间 {room.name} 失败："
                        f"{type(exc).__name__}: {exc}",
                        flush=True,
                    )
    except Exception as exc:
        print(
            f"[LiveKit启动清理] 无法连接 LiveKit API：{type(exc).__name__}: {exc}",
            flush=True,
        )
        return

    if deleted:
        print(
            "[LiveKit启动清理] 已删除无 Agent 的旧房间："
            + ", ".join(deleted)
            + "；设备会自动回到冷待机，下次唤醒将重建房间并正常派发。",
            flush=True,
        )
    else:
        print("[LiveKit启动清理] 未发现需要清理的 SparkBot 房间。", flush=True)


# In `dev` mode LiveKit otherwise keeps zero idle job processes, which adds a
# cold process spawn before every true-standby wake. One prewarmed process is a
# modest persistent memory cost on this local machine but removes that delay.
server = AgentServer(num_idle_processes=1)


@server.rtc_session(agent_name="memo-agent")
async def entrypoint(ctx: JobContext) -> None:
    hermes = HermesClient(
        base_url=HERMES_BASE_URL,
        api_key=HERMES_API_KEY,
        model=HERMES_MODEL,
        session_key=HERMES_SESSION_KEY,
    )
    codex = CodexClient(
        command=CODEX_COMMAND,
        workspace=CODEX_WORKSPACE,
        model=CODEX_MODEL,
        timeout_seconds=CODEX_RUN_TIMEOUT_SECONDS,
        channel_target=CODEX_CHANNEL_TARGET,
        channel_sender_command=CHANNEL_SENDER_COMMAND,
    )

    # 每次 LiveKit AgentSession 对应一个 Hermes 会话标识。
    hermes_session_id = f"voice-{uuid.uuid4().hex}"

    active_run_id: str | None = None
    forced_backend: BackendRoute | None = None
    background_tasks: set[asyncio.Task[Any]] = set()
    backend_work_count = 0
    backend_work_idle = asyncio.Event()
    backend_work_idle.set()
    backend_last_finished_at: float | None = None
    # SparkBot 的“假待机”会在 Gemini 服务端取消当前 FunctionCall；但
    # Hermes/Codex 进程仍会完成。结果必须改走独立 realtime-text 播报，
    # 不能再依赖已被取消的 FunctionResponse。
    sparkbot_sleep_draining_identity: str | None = None
    sparkbot_drain_result_queued = asyncio.Event()
    sparkbot_drain_speech_started = asyncio.Event()
    sparkbot_drain_playout_done = asyncio.Event()
    sparkbot_drain_delivery_generation = 0
    sparkbot_drain_speaking_seen = False
    sparkbot_drain_last_prompt: str | None = None
    # A local Chinese stop phrase advances this epoch and suppresses every response
    # belonging to the interrupted Gemini turn.  Backend tasks deliberately
    # keep running; their completion is delivered through a fresh realtime
    # text prompt instead of the cancelled FunctionResponse.
    sparkbot_stop_epoch = 0
    sparkbot_stop_suppression = False
    sparkbot_stop_ready = False
    sparkbot_stop_result_prompt_epoch: int | None = None
    sparkbot_stop_result_lock = asyncio.Lock()
    # Gemini-transcript fallback state for stop phrases the device missed.
    sparkbot_agent_state = "listening"
    sparkbot_stop_fallback_until = 0.0
    sparkbot_stop_fallback_epoch: int | None = None

    def track_background_task(task: asyncio.Task[Any]) -> asyncio.Task[Any]:
        background_tasks.add(task)
        task.add_done_callback(background_tasks.discard)
        return task

    def start_background(coroutine: Any) -> None:
        track_background_task(asyncio.create_task(coroutine))

    def mark_backend_work_started() -> None:
        nonlocal backend_work_count
        backend_work_count += 1
        backend_work_idle.clear()

    def mark_backend_work_finished() -> None:
        nonlocal backend_work_count, backend_last_finished_at
        backend_work_count = max(0, backend_work_count - 1)
        backend_last_finished_at = time.monotonic()
        if backend_work_count == 0:
            backend_work_idle.set()

    async def deliver_backend_result_during_sparkbot_drain(
        backend_name: str,
        result: str,
    ) -> bool:
        """Deliver a completed backend result after a SparkBot sleep request.

        Gemini Live cancels an in-flight function call when it receives a new
        realtime text input.  During normal conversation that is undesirable,
        but after the local mic has been muted for orderly standby it gives us
        a reliable way to replace the cancelled call with one final spoken
        result.  The ordinary tool return is then deliberately made inert.
        """
        nonlocal sparkbot_drain_delivery_generation
        nonlocal sparkbot_drain_speaking_seen
        nonlocal sparkbot_drain_last_prompt
        identity = sparkbot_sleep_draining_identity
        if not identity:
            return False

        sparkbot_drain_delivery_generation += 1
        sparkbot_drain_speaking_seen = False
        sparkbot_drain_speech_started.clear()
        sparkbot_drain_playout_done.clear()
        prompt = (
            "系统事件：SparkBot 正处于待机排空阶段，用户麦克风已关闭。"
            f"{backend_name} 已完成。请立刻用自然、简洁的中文播报以下真实结果，"
            "播报完成后不要调用工具、不要追问，也不要提及系统事件：\n"
            f"{result.strip()}"
        )
        sparkbot_drain_last_prompt = prompt
        for _ in range(20):
            if send_gemini_realtime_text(session, prompt):
                sparkbot_drain_result_queued.set()
                print(
                    f"[SparkBot待机] 已将 {backend_name} 完成结果转入独立播报通道：{identity}",
                    flush=True,
                )
                return True
            await asyncio.sleep(0.25)
        print(
            f"[SparkBot待机] {backend_name} 已完成，但 Gemini 实时通道未就绪，无法播报结果。",
            flush=True,
        )
        return False

    async def deliver_backend_result_after_sparkbot_stop(
        backend_name: str,
        result: str,
        tool_stop_epoch: int,
    ) -> bool:
        """Speak a completed pre-stop tool result on a new Gemini turn."""
        nonlocal sparkbot_stop_result_prompt_epoch
        if tool_stop_epoch >= sparkbot_stop_epoch:
            return False
        prompt = (
            "系统事件：用户说了中文停止口令，当前语音思考和播报已经停止。"
            f"{backend_name} 是在停止前已经启动的任务，并且现在已经完成。"
            "请立刻用简洁、自然的中文语音播报下面的真实结果；"
            "不要提及系统事件，不要调用工具，不要追问：\n"
            f"{result.strip()}"
        )
        # Serialize result injections so two concurrently finishing backends
        # cannot overwrite each other's controlled prompt state.
        async with sparkbot_stop_result_lock:
            for _ in range(20):
                # A user may already have started the next instruction.  The
                # backend result is still required to be spoken, so do not
                # couple this delivery to the short stop-input suppression
                # window.  If another stop arrives while waiting for a
                # reconnect, the current epoch is used for the next attempt.
                delivery_epoch = sparkbot_stop_epoch
                sparkbot_stop_result_prompt_epoch = delivery_epoch
                if send_gemini_realtime_text(session, prompt):
                    print(
                        f"[SparkBot停止] 已将 {backend_name} 完成结果转入独立播报通道。",
                        flush=True,
                    )
                    return True
                sparkbot_stop_result_prompt_epoch = None
                await asyncio.sleep(0.25)
            sparkbot_stop_result_prompt_epoch = None
        print(
            f"[SparkBot停止] {backend_name} 已完成，但 Gemini 实时通道未就绪，无法播报结果。",
            flush=True,
        )
        return False

    async def deliver_backend_result_during_active(
        backend_name: str,
        result: str,
    ) -> bool:
        """Speak a completed backend result during a normal active turn.

        The delegate tools return immediately after starting the backend, so
        Gemini never waits on a pending FunctionCall.  When the backend
        finishes, this injects a fresh realtime text turn that makes Gemini
        speak the real result while the conversation continues.
        """
        prompt = (
            "系统事件：你之前委托的后台任务已经完成。"
            "请用自然、简洁的中文向用户播报下面的真实结果；"
            "播报完成后恢复正常对话，不要再次调用工具，也不要追问：\n"
            f"{result.strip()}"
        )
        for _ in range(20):
            if send_gemini_realtime_text(session, prompt):
                print(
                    f"[{backend_name}] 已完成，已将结果转入独立播报通道。",
                    flush=True,
                )
                return True
            await asyncio.sleep(0.25)
        print(
            f"[{backend_name}] 已完成，但 Gemini 实时通道未就绪，无法播报结果。",
            flush=True,
        )
        return False

    async def deliver_backend_result(
        backend_name: str,
        result: str,
        tool_stop_epoch: int,
    ) -> bool:
        """Deliver a completed backend result via the channel that fits now."""
        if tool_stop_epoch < sparkbot_stop_epoch:
            return await deliver_backend_result_after_sparkbot_stop(
                backend_name,
                result,
                tool_stop_epoch,
            )
        if await deliver_backend_result_during_sparkbot_drain(backend_name, result):
            return True
        return await deliver_backend_result_during_active(backend_name, result)

    def handoff_backend_result_for_background(
        backend_name: str,
        result_task: asyncio.Task[str],
        tool_stop_epoch: int,
    ) -> None:
        """Wait for a running backend and deliver its result on any channel.

        The delegate tool has already returned, so the result cannot ride the
        FunctionResponse; it is spoken via a fresh realtime text turn whether
        the conversation is active, a stop phrase arrived, or SparkBot is
        draining for orderly standby.
        """

        async def wait_and_deliver() -> None:
            try:
                try:
                    result = await result_task
                except HermesError as error:
                    result = f"任务没有成功完成：{error}"
                except CodexError as error:
                    result = f"任务没有成功完成：{error}"
                except asyncio.CancelledError:
                    return
                except Exception as error:
                    print(
                        f"[{backend_name}] 后台结果收集异常：{type(error).__name__}: {error}",
                        flush=True,
                    )
                    result = "任务发生内部错误，没有完成，请稍后再试。"

                await deliver_backend_result(backend_name, result, tool_stop_epoch)
            finally:
                mark_backend_work_finished()

        start_background(wait_and_deliver())

    @function_tool
    async def delegate_to_hermes(
        context: RunContext,
        task: str,
    ) -> str | None:
        """把需要真实执行、调用本地工具或复杂推理的任务交给 Hermes。
            调用本工具前，必须先用原生语音向用户简短确认已接受任务。
            当提到用telegram展示结果时，调用这个工具。


        Args:
            task: 完整、明确、独立可执行的中文任务描述。
        """
        nonlocal active_run_id, forced_backend

        if forced_backend is BackendRoute.CODEX:
            return (
                "用户明确说了让小 C 做，路由错误。"
                "请立即改为调用 delegate_to_codex，不要向用户声称任务失败。"
            )
        forced_backend = None

        normalized_task = task.strip()
        if not normalized_task:
            return "没有收到可执行的任务内容，请重新询问用户。"
        tool_stop_epoch = sparkbot_stop_epoch

        mark_backend_work_started()
        result_task: asyncio.Task[str] | None = None
        handed_off = False
        try:
            # 收到 FunctionCall 就立即启动后端，不等确认语音播完：确认语
            # 播放期间被打断（插话/停止口令）不能把已接受的委托取消掉。
            # 工具随后立即返回，Gemini 恢复对话，不再挂起等待后台结果。
            if tool_stop_epoch < sparkbot_stop_epoch:
                print(
                    "[Hermes] 中文停止口令先于工具提交到达，跳过尚未启动的任务。",
                    flush=True,
                )
                return "工具尚未启动，已按中文停止口令取消本次提交。"

            print(
                f"[Hermes] 准备提交任务：{normalized_task}",
                flush=True,
            )

            run_id = await hermes.start_run(
                task=normalized_task,
                session_id=hermes_session_id,
            )
            active_run_id = run_id

            print(
                f"[Hermes] 已启动 run_id={run_id}（后台执行，完成结果自动播报）",
                flush=True,
            )

            result_task = track_background_task(
                asyncio.create_task(
                    hermes.wait_for_result(run_id),
                    name=f"hermes-result-{run_id}",
                )
            )

            def _clear_active_run(_task: asyncio.Task[Any]) -> None:
                nonlocal active_run_id
                if active_run_id == run_id:
                    active_run_id = None

            result_task.add_done_callback(_clear_active_run)
            # 工具立刻返回：Gemini 恢复对话，不再等待 FunctionCall。结果由
            # 后台任务按“活动对话 / 停止口令 / 待机排空”三条通道自动播报。
            handoff_backend_result_for_background(
                "Hermes",
                result_task,
                tool_stop_epoch,
            )
            handed_off = True
            return (
                "后台任务已启动，结果完成后系统会通过独立通道自动播报。"
                "请用一句话告知用户任务已在后台执行，然后恢复正常对话；"
                "不要再次调用工具，也不要询问或等待结果。"
            )
        except asyncio.CancelledError:
            if result_task is not None and not handed_off:
                handoff_backend_result_for_background(
                    "Hermes",
                    result_task,
                    tool_stop_epoch,
                )
                handed_off = True
            raise
        except HermesError as error:
            print(f"[Hermes错误] {error}", flush=True)
            return f"Hermes 任务启动失败：{error}"
        except Exception as error:
            print(
                f"[Hermes异常] {type(error).__name__}: {error}",
                flush=True,
            )
            return "Hermes 后台任务启动失败，请稍后再试。"
        finally:
            if not handed_off:
                mark_backend_work_finished()

    @function_tool
    async def delegate_to_codex(
        context: RunContext,
        task: str,
    ) -> str | None:
        """把“让小C做”的任务，以及代码开发、修改和审查任务交给 Codex。
            调用本工具前，必须先用原生语音向用户简短确认已接受任务。

        Args:
            task: 去掉“让小C做”前缀后的完整、独立、可执行任务描述。
        """
        nonlocal forced_backend

        if forced_backend is BackendRoute.HERMES:
            return (
                "用户明确说了让小马做，路由错误。"
                "请立即改为调用 delegate_to_hermes，不要向用户声称任务失败。"
            )
        forced_backend = None

        normalized_task = task.strip()
        if not normalized_task:
            return "没有收到可执行的 Codex 任务内容，请重新询问用户。"
        tool_stop_epoch = sparkbot_stop_epoch

        mark_backend_work_started()
        result_task: asyncio.Task[str] | None = None
        handed_off = False
        try:
            # 收到 FunctionCall 就立即启动后端，不等确认语音播完：确认语
            # 播放期间被打断不能取消已接受的委托。工具随后立即返回。
            if tool_stop_epoch < sparkbot_stop_epoch:
                print(
                    "[Codex] 中文停止口令先于工具提交到达，跳过尚未启动的任务。",
                    flush=True,
                )
                return "工具尚未启动，已按中文停止口令取消本次提交。"
            print(f"[Codex] 准备提交任务：{normalized_task}", flush=True)
            result_task = track_background_task(
                asyncio.create_task(
                    codex.run(normalized_task),
                    name="codex-result",
                )
            )
            handoff_backend_result_for_background(
                "Codex",
                result_task,
                tool_stop_epoch,
            )
            handed_off = True
            return (
                "后台任务已启动，结果完成后系统会通过独立通道自动播报。"
                "请用一句话告知用户任务已在后台执行，然后恢复正常对话；"
                "不要再次调用工具，也不要询问或等待结果。"
            )
        except asyncio.CancelledError:
            if result_task is not None and not handed_off:
                handoff_backend_result_for_background(
                    "Codex",
                    result_task,
                    tool_stop_epoch,
                )
                handed_off = True
            raise
        except CodexError as error:
            print(f"[Codex错误] {error}", flush=True)
            return f"Codex 任务启动失败：{error}"
        except Exception as error:
            print(
                f"[Codex异常] {type(error).__name__}: {error}",
                flush=True,
            )
            return "Codex 后台任务启动失败，请稍后再试。"
        finally:
            if not handed_off:
                mark_backend_work_finished()

    @function_tool
    async def request_standby(context: RunContext) -> str:
        """用户明确表示要休息、待机或睡觉时立即调用，让当前语音设备进入待机。

        语音转写可能不准确（包括其他语言/脚本，例如“休息吧”被转写成韩文），
        只要用户意图是进入待机就调用本工具；系统会自动播报待机确认。
        """
        room_io = getattr(context.session, "_room_io", None)
        participant = getattr(room_io, "linked_participant", None)
        identity = getattr(participant, "identity", "") if participant else ""
        if not identity.startswith("sparkbot-"):
            print(
                f"[SparkBot待机] request_standby 未找到已关联的语音设备：{identity!r}",
                flush=True,
            )
            return (
                "未识别到当前语音设备，无法进入待机。"
                "请用一句话提示用户稍后重试，不要调用其他工具。"
            )

        request_sparkbot_orderly_sleep(
            identity,
            payload='{"event":"local_sleep_request","phrase":"request_standby(gemini)"}',
        )
        print(f"[SparkBot待机] Gemini 调用 request_standby：{identity}", flush=True)
        return (
            "系统事件：已开始进入待机流程，待机确认语音会由系统自动播报。"
            "请不要再说话，也不要调用其他工具。"
        )

    session = AgentSession(
        llm=google.realtime.RealtimeModel(
            model=GEMINI_MODEL,
            voice=GEMINI_VOICE,
            instructions=FRONT_AGENT_INSTRUCTIONS,
            conn_options=APIConnectOptions(
                max_retry=10,
                retry_interval=2.0,
                timeout=20.0,
            ),
            realtime_input_config=types.RealtimeInputConfig(
                # The firmware now feeds ESP-SR AEC-cleaned audio to Gemini and
                # ducks the mic gain during playback, so Gemini's native
                # barge-in can decide when a real user onset should interrupt
                # the current reply.  The device-local stop phrase remains as
                # a fallback for cases Gemini misses.
                activity_handling=types.ActivityHandling.START_OF_ACTIVITY_INTERRUPTS,
                automatic_activity_detection=types.AutomaticActivityDetection(
                    disabled=False,
                    # The firmware now gates the uplink with the speaker
                    # reference, so residual echo/noise is zeroed before it
                    # reaches Gemini.  High onset sensitivity can be restored
                    # for reliable real barge-in without self-interruptions.
                    start_of_speech_sensitivity=types.StartSensitivity.START_SENSITIVITY_HIGH,
                    end_of_speech_sensitivity=(
                        # Long Chinese task instructions often contain natural
                        # short pauses; favor one complete turn.
                        types.EndSensitivity.END_SENSITIVITY_LOW
                    ),
                    prefix_padding_ms=20,
                    silence_duration_ms=900,
                ),
            ),
        ),
    )

    agent = Agent(
        instructions=FRONT_AGENT_INSTRUCTIONS,
        tools=[delegate_to_hermes, delegate_to_codex, request_standby],
    )

    # SparkBot 每次从真待机唤醒都会重新入房，但 identity 保持稳定。不能以
    # identity 做永久去重，否则第二次唤醒会被当作第一次的重复包。数据包是
    # 权威唤醒事件；入房事件仅在 worker 启动较慢、错过了该数据包时兜底。
    # 两个来源在同一次连接中通常相隔几十毫秒，因此只做很短的时间窗去重。
    sparkbot_greeting_tasks: dict[str, asyncio.Task[None]] = {}
    sparkbot_greeting_fallback_tasks: dict[str, asyncio.Task[None]] = {}
    sparkbot_last_wake_packet_at: dict[str, float] = {}
    sparkbot_last_greeting_request_at: dict[str, float] = {}
    sparkbot_greeting_dedup_seconds = 1.5
    sleep_drain_task: asyncio.Task[None] | None = None
    last_active_speaker_identities: tuple[str, ...] | None = None

    async def greet_sparkbot_when_ready(identity: str, source: str) -> None:
        try:
            # Gemini can take longer than 5 s to bring the realtime transport
            # up (first connect, retries, slow network).  Poll generously and
            # inject the greeting the moment the transport is usable, instead
            # of dropping the wake reply after a short fixed window.
            ready_interval = 0.5
            for attempt in range(60):
                if send_gemini_realtime_text(session, SPARKBOT_WAKE_GREETING_TEXT):
                    print(
                        f"[SparkBot唤醒] 已向 {GEMINI_MODEL} 注入实时文本问候"
                        f"（{source}，{identity}）；等待语音回复。",
                        flush=True,
                    )
                    return
                if attempt % 10 == 0:
                    realtime_session = _gemini_realtime_session(session)
                    if realtime_session is None:
                        reason = "realtime_llm_session 尚未创建"
                    elif not callable(
                        getattr(realtime_session, "_send_client_event", None)
                    ):
                        reason = "_send_client_event 不可用"
                    else:
                        reason = "enqueue 未生效"
                    print(
                        f"[SparkBot唤醒] 问候等待中（{attempt * ready_interval:.0f}s，"
                        f"{reason}，{identity}）。",
                        flush=True,
                    )
                await asyncio.sleep(ready_interval)

            print(
                f"[SparkBot唤醒] Gemini 实时会话 30 秒内未就绪（{source}，{identity}）；"
                "跳过本次主动问候，设备仍可直接接收后续语音。",
                flush=True,
            )
        finally:
            current_task = asyncio.current_task()
            if sparkbot_greeting_tasks.get(identity) is current_task:
                sparkbot_greeting_tasks.pop(identity, None)

    def request_sparkbot_greeting(identity: str, source: str) -> None:
        if not identity.startswith("sparkbot-"):
            return
        now = time.monotonic()
        existing_task = sparkbot_greeting_tasks.get(identity)
        last_request = sparkbot_last_greeting_request_at.get(identity, 0.0)
        if (
            existing_task is not None
            and not existing_task.done()
            and now - last_request < sparkbot_greeting_dedup_seconds
        ):
            print(f"[SparkBot唤醒] 忽略重复问候请求（{source}，{identity}）。", flush=True)
            return
        if now - last_request < sparkbot_greeting_dedup_seconds:
            print(f"[SparkBot唤醒] 忽略短时间重复请求（{source}，{identity}）。", flush=True)
            return

        sparkbot_last_greeting_request_at[identity] = now
        sparkbot_greeting_tasks[identity] = asyncio.create_task(
            greet_sparkbot_when_ready(identity, source),
            name=f"sparkbot-wake-greeting-{identity}",
        )

    async def greet_sparkbot_after_connection_delay(identity: str, delay: float) -> None:
        """Fallback only: a worker may start after the one-shot wake packet."""
        try:
            await asyncio.sleep(delay)
            last_packet = sparkbot_last_wake_packet_at.get(identity, 0.0)
            if time.monotonic() - last_packet < sparkbot_greeting_dedup_seconds:
                return
            request_sparkbot_greeting(identity, "设备入房兜底")
        finally:
            current_task = asyncio.current_task()
            if sparkbot_greeting_fallback_tasks.get(identity) is current_task:
                sparkbot_greeting_fallback_tasks.pop(identity, None)

    def schedule_sparkbot_greeting_fallback(identity: str, *, delay: float = 0.35) -> None:
        if not identity.startswith("sparkbot-"):
            return
        existing_task = sparkbot_greeting_fallback_tasks.get(identity)
        if existing_task is not None and not existing_task.done():
            return
        sparkbot_greeting_fallback_tasks[identity] = asyncio.create_task(
            greet_sparkbot_after_connection_delay(identity, delay),
            name=f"sparkbot-wake-fallback-{identity}",
        )

    async def send_sparkbot_sleep_ack_when_ready(identity: str, *, retry: bool = False) -> bool:
        """Speak one explicit confirmation after the device has closed its mic."""
        nonlocal sparkbot_drain_delivery_generation
        nonlocal sparkbot_drain_speaking_seen
        nonlocal sparkbot_drain_last_prompt
        acknowledgement = sparkbot_sleep_ack_text(backend_work_count > 0)
        if retry:
            acknowledgement += (
                "上一条语音输出没有开始。现在必须立刻用中文语音只说："
                "好的，正在进入待机。"
            )
        sparkbot_drain_last_prompt = acknowledgement
        for _ in range(20):
            sparkbot_drain_delivery_generation += 1
            sparkbot_drain_speaking_seen = False
            sparkbot_drain_speech_started.clear()
            sparkbot_drain_playout_done.clear()
            if send_gemini_realtime_text(session, acknowledgement):
                sparkbot_drain_result_queued.set()
                print(
                    f"[SparkBot待机] 已请求 Gemini 向 {identity} 播报待机确认。",
                    flush=True,
                )
                return True
            await asyncio.sleep(0.25)
        print(
            f"[SparkBot待机] Gemini 实时会话未就绪，无法播报待机确认：{identity}",
            flush=True,
        )
        return False

    async def retry_sparkbot_drain_prompt(identity: str) -> bool:
        """Resend the exact controlled result/ack when Gemini dropped it."""
        nonlocal sparkbot_drain_delivery_generation, sparkbot_drain_speaking_seen
        prompt = sparkbot_drain_last_prompt
        if not prompt:
            return False
        for _ in range(20):
            sparkbot_drain_delivery_generation += 1
            sparkbot_drain_speaking_seen = False
            sparkbot_drain_speech_started.clear()
            sparkbot_drain_playout_done.clear()
            if send_gemini_realtime_text(session, prompt):
                print(f"[SparkBot待机] 已重发最终播报请求：{identity}", flush=True)
                return True
            await asyncio.sleep(0.25)
        return False

    async def send_sparkbot_sleep_cancel_ack_when_ready(identity: str) -> bool:
        """Confirm that a pending standby was cancelled without cancelling work."""
        for _ in range(20):
            if send_gemini_realtime_text(session, SPARKBOT_SLEEP_CANCEL_TEXT):
                print(
                    f"[SparkBot待机] 已请求 Gemini 向 {identity} 播报待机取消确认。",
                    flush=True,
                )
                return True
            await asyncio.sleep(0.25)
        print(
            f"[SparkBot待机] Gemini 实时会话未就绪，无法播报待机取消确认：{identity}",
            flush=True,
        )
        return False

    async def finish_sparkbot_sleep_when_idle(identity: str) -> None:
        """Keep the downlink alive until backend work and spoken output drain."""
        nonlocal sparkbot_sleep_draining_identity
        try:
            # Give an already-issued function call time to enter Python.  This
            # avoids treating a just-starting backend task as an idle turn.
            await asyncio.sleep(1.0)
            saw_backend_work = not backend_work_idle.is_set()
            if saw_backend_work:
                await backend_work_idle.wait()

                # A task result normally calls deliver_backend_result... just
                # before it decrements backend_work_count.  Do *not* wait for
                # session.wait_for_idle here: Gemini's cancelled FunctionCall
                # can otherwise leave that state permanently non-idle.
                try:
                    await asyncio.wait_for(sparkbot_drain_result_queued.wait(), timeout=3.0)
                except asyncio.TimeoutError:
                    print(
                        "[SparkBot待机] 后台任务已结束但结果播报尚未排队；继续等待 2 秒。",
                        flush=True,
                    )
                    await asyncio.sleep(2.0)

            if not sparkbot_drain_result_queued.is_set():
                # No backend task was actually running, or a cancelled call
                # never reached Python.  Replace the stale Gemini turn with a
                # single controlled acknowledgement.
                sparkbot_drain_playout_done.clear()
                acknowledgement_sent = await send_sparkbot_sleep_ack_when_ready(identity)
                if not acknowledgement_sent:
                    await asyncio.sleep(0.5)
            else:
                acknowledgement_sent = True

            if acknowledgement_sent:
                try:
                    await asyncio.wait_for(sparkbot_drain_speech_started.wait(), timeout=4.0)
                except asyncio.TimeoutError:
                    # The realtime text bridge accepted the event but Gemini
                    # occasionally drops it while ending the preceding local
                    # “休息吧” turn. One idempotent retry gives it a clean,
                    # explicit audio-only instruction before true standby.
                    print("[SparkBot待机] 待机确认 4 秒内未开始播报，正在重试一次。", flush=True)
                    acknowledgement_sent = await retry_sparkbot_drain_prompt(identity)
                    if acknowledgement_sent:
                        try:
                            await asyncio.wait_for(
                                sparkbot_drain_speech_started.wait(), timeout=4.0
                            )
                        except asyncio.TimeoutError:
                            print("[SparkBot待机] 重试确认仍未开始播报。", flush=True)
                try:
                    await asyncio.wait_for(sparkbot_drain_playout_done.wait(), timeout=35.0)
                except asyncio.TimeoutError:
                    # Do not leave the battery-consuming moon state forever
                    # when a remote TTS/session error prevents the terminal
                    # callback from observing the final audio edge.
                    print(
                        "[SparkBot待机] 等待最终播报结束超时；按已完成状态进入待机。",
                        flush=True,
                    )
            else:
                await asyncio.sleep(1.0)

            # A fresh wake can cancel this coroutine while the final result
            # is talking.  In that case never send a stale sleep_ready packet.
            if sparkbot_sleep_draining_identity != identity:
                return
            # 手机 App 只在 standbyPending 时响应 sleep_ready；当待机由
            # Gemini 的 request_standby 工具发起（例如“休息吧”被转写成
            # 其他语言）时 App 没有 pending 状态，因此再发一条
            # android_standby，App 收到后无条件进入待机。固件忽略该 topic，
            # 仍按原有 sleep_ready 流程处理。
            await ctx.room.local_participant.publish_data(
                json.dumps({"event": "android_standby"}, ensure_ascii=False),
                reliable=True,
                destination_identities=[identity],
                topic="sparkbot.android_standby",
            )
            await ctx.room.local_participant.publish_data(
                json.dumps({"event": "sleep_ready"}, ensure_ascii=False),
                reliable=True,
                destination_identities=[identity],
                topic="sparkbot.sleep_ready",
            )
            sparkbot_sleep_draining_identity = None
            print(
                f"[SparkBot待机] 所有后台任务和语音播报已排空，已通知 {identity} 进入待机。",
                flush=True,
            )
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            # The device will use its normal disconnection fallback if the
            # data channel disappears. Do not cancel backend work here.
            print(f"[SparkBot待机] 无法发送待机完成回执：{type(exc).__name__}: {exc}", flush=True)

    def request_sparkbot_orderly_sleep(identity: str, payload: str) -> None:
        nonlocal sleep_drain_task, sparkbot_sleep_draining_identity
        nonlocal sparkbot_stop_suppression, sparkbot_stop_ready
        nonlocal sparkbot_stop_result_prompt_epoch
        if not identity.startswith("sparkbot-"):
            return
        if sleep_drain_task is not None and not sleep_drain_task.done():
            print(f"[SparkBot待机] 忽略重复待机请求：{identity}", flush=True)
            return

        # Standby is a new explicit device lifecycle boundary.  Let its
        # controlled acknowledgement/result prompt through even if the
        # previous active turn had been stopped by a Chinese stop phrase.
        sparkbot_stop_suppression = False
        sparkbot_stop_ready = False
        sparkbot_stop_result_prompt_epoch = None
        sparkbot_sleep_draining_identity = identity
        sparkbot_drain_result_queued.clear()
        sparkbot_drain_speech_started.clear()
        sparkbot_drain_playout_done.clear()
        audio_closed = close_gemini_realtime_audio_input(session)
        print(
            f"[SparkBot待机] 收到 {identity}: {payload}；"
            f"设备已{'停止向 Gemini 上传音频' if audio_closed else '请求停止音频但 Gemini 会话尚未就绪'}，"
            "保留后台任务和扬声器输出。",
            flush=True,
        )
        sleep_drain_task = asyncio.create_task(
            finish_sparkbot_sleep_when_idle(identity),
            name=f"sparkbot-orderly-sleep-{identity}",
        )

    def cancel_sparkbot_orderly_sleep(identity: str, payload: str) -> None:
        """Resume a draining SparkBot while preserving Hermes/Codex work."""
        nonlocal sleep_drain_task, sparkbot_sleep_draining_identity
        if not identity.startswith("sparkbot-"):
            return

        cancelled = False
        if sleep_drain_task is not None and not sleep_drain_task.done():
            sleep_drain_task.cancel()
            cancelled = True
        sleep_drain_task = None
        sparkbot_sleep_draining_identity = None
        sparkbot_drain_result_queued.clear()
        sparkbot_drain_speech_started.clear()
        sparkbot_drain_playout_done.clear()

        print(
            f"[SparkBot待机] 收到 {identity}: {payload}；"
            f"{'已取消排空待机' if cancelled else '设备请求恢复会话'}，后台任务不受影响。",
            flush=True,
        )
        asyncio.create_task(
            send_sparkbot_sleep_cancel_ack_when_ready(identity),
            name=f"sparkbot-sleep-cancel-{identity}",
        )

    def mark_sparkbot_stop_ready(identity: str, payload: str) -> None:
        nonlocal sparkbot_stop_ready
        if not identity.startswith("sparkbot-"):
            return
        sparkbot_stop_ready = True
        print(
            f"[SparkBot停止] 收到 {identity}: {payload}；已解除口令静音保护，可接收下一条指令。",
            flush=True,
        )

    def request_sparkbot_stop(identity: str, payload: str) -> None:
        """Cancel the current Gemini turn while preserving backend work."""
        nonlocal sparkbot_stop_epoch
        nonlocal sparkbot_stop_suppression
        nonlocal sparkbot_stop_ready
        nonlocal sparkbot_stop_result_prompt_epoch
        if not identity.startswith("sparkbot-"):
            return

        sparkbot_stop_epoch += 1
        sparkbot_stop_suppression = True
        sparkbot_stop_ready = False
        sparkbot_stop_result_prompt_epoch = None
        try:
            completion = session.interrupt(force=True)
        except RuntimeError as exc:
            print(
                f"[SparkBot停止] 当前播报不可立即中断（{exc}），仍请求重置 Realtime 传输。",
                flush=True,
            )
        else:
            def log_completion(task: asyncio.Future[None]) -> None:
                try:
                    task.result()
                except Exception as exc:
                    print(
                        f"[SparkBot停止] 本地播报未完全停止：{type(exc).__name__}: {exc}",
                        flush=True,
                    )

            completion.add_done_callback(log_completion)

        transport_reset_requested = restart_gemini_realtime_transport(session)
        print(
            f"[SparkBot停止] 收到 {identity}: {payload}；已停止当前 Gemini 思考/播报，"
            f"Realtime 传输重置={'已请求' if transport_reset_requested else '未请求'}；"
            "已启动的工具任务继续运行。",
            flush=True,
        )

    def request_sparkbot_stop_from_transcript() -> None:
        """Fallback interrupt when Gemini transcribed a stop phrase.

        The device-local MultiNet is the fast path, but loud speaker echo can
        make it miss the phrase entirely.  Gemini's ASR is far more robust to
        that echo, so a final exact-match transcript triggers the same stop
        semantics while the agent is still speaking/thinking.  Backend work is
        deliberately preserved, exactly like the device-triggered path.
        """
        nonlocal sparkbot_stop_epoch
        nonlocal sparkbot_stop_suppression
        nonlocal sparkbot_stop_ready
        nonlocal sparkbot_stop_result_prompt_epoch
        nonlocal sparkbot_stop_fallback_until
        nonlocal sparkbot_stop_fallback_epoch

        now = time.monotonic()
        if sparkbot_stop_suppression:
            print("[SparkBot停止] 转写兜底跳过：设备/上一次停止已接管。", flush=True)
            return
        if now < sparkbot_stop_fallback_until:
            print("[SparkBot停止] 转写兜底跳过：仍在冷却期内。", flush=True)
            return
        if sparkbot_agent_state not in {"speaking", "thinking"}:
            print(
                f"[SparkBot停止] 转写兜底跳过：当前 Agent 状态={sparkbot_agent_state}。",
                flush=True,
            )
            return

        sparkbot_stop_epoch += 1
        sparkbot_stop_suppression = True
        sparkbot_stop_ready = False
        sparkbot_stop_result_prompt_epoch = None
        sparkbot_stop_fallback_until = (
            now + SPARKBOT_STOP_TRANSCRIPT_FALLBACK_GUARD_SECONDS
        )
        sparkbot_stop_fallback_epoch = sparkbot_stop_epoch

        try:
            completion = session.interrupt(force=True)
        except RuntimeError as exc:
            print(
                f"[SparkBot停止] 转写兜底：当前播报不可立即中断（{exc}），"
                "仍请求重置 Realtime 传输。",
                flush=True,
            )
        else:
            def log_fallback_completion(task: asyncio.Future[None]) -> None:
                try:
                    task.result()
                except Exception as exc:
                    print(
                        "[SparkBot停止] 转写兜底本地播报未完全停止："
                        f"{type(exc).__name__}: {exc}",
                        flush=True,
                    )

            completion.add_done_callback(log_fallback_completion)

        transport_reset_requested = restart_gemini_realtime_transport(session)
        print(
            "[SparkBot停止] Gemini 转写兜底触发：已停止当前思考/播报，"
            f"Realtime 传输重置={'已请求' if transport_reset_requested else '未请求'}；"
            "已启动的工具任务继续运行。",
            flush=True,
        )

        async def release_fallback_guard() -> None:
            nonlocal sparkbot_stop_suppression
            nonlocal sparkbot_stop_ready
            await asyncio.sleep(SPARKBOT_STOP_TRANSCRIPT_FALLBACK_GUARD_SECONDS)
            # Only release the guard this fallback created.  A newer device
            # stop (or another fallback) advances the epoch and owns the guard.
            if (
                sparkbot_stop_epoch == sparkbot_stop_fallback_epoch
                and sparkbot_stop_suppression
            ):
                sparkbot_stop_suppression = False
                sparkbot_stop_ready = False
                print(
                    "[SparkBot停止] 转写兜底静音保护到期，已恢复接收下一条指令。",
                    flush=True,
                )

        asyncio.create_task(
            release_fallback_guard(),
            name="sparkbot-stop-fallback-release",
        )

    @ctx.room.on("active_speakers_changed")
    def on_active_speakers_changed(speakers) -> None:
        nonlocal last_active_speaker_identities
        identities = tuple(participant.identity for participant in speakers)
        if identities == last_active_speaker_identities:
            return
        last_active_speaker_identities = identities
        print(
            f"[LiveKit活跃说话人] {list(identities)}",
            flush=True,
        )

    @ctx.room.on("participant_connected")
    def on_participant_connected(participant) -> None:
        identity = getattr(participant, "identity", "")
        # SparkBot now prewarms its room at boot while the mic RTP gate is
        # closed. Joining the room is therefore *not* a wake request. The
        # reliable sparkbot.wake data packet is authoritative and avoids an
        # unsolicited Gemini greeting (and token use) during standby.
        if identity.startswith("sparkbot-"):
            print(f"[SparkBot唤醒] 设备入房预热：{identity}；等待本地唤醒事件。", flush=True)

    @ctx.room.on("participant_disconnected")
    def on_participant_disconnected(participant) -> None:
        # A true standby closes the LiveKit room. Clear only per-connection
        # short-window state so the same physical SparkBot can greet again on
        # its next wake.
        identity = getattr(participant, "identity", "")
        if not identity.startswith("sparkbot-"):
            return
        for task_map in (sparkbot_greeting_tasks, sparkbot_greeting_fallback_tasks):
            task = task_map.pop(identity, None)
            if task is not None and not task.done():
                task.cancel()
        sparkbot_last_wake_packet_at.pop(identity, None)
        sparkbot_last_greeting_request_at.pop(identity, None)
        print(f"[SparkBot唤醒] 设备离房，已清除问候去重状态：{identity}", flush=True)

    @ctx.room.on("data_received")
    def on_data_received(data_packet) -> None:
        """Handle SparkBot's local wake, stop, and standby control packets."""
        nonlocal sparkbot_stop_suppression
        nonlocal sparkbot_stop_ready
        nonlocal sparkbot_stop_result_prompt_epoch
        nonlocal sparkbot_stop_fallback_until
        nonlocal sparkbot_stop_fallback_epoch
        if data_packet.topic not in {
            "sparkbot.wake",
            "sparkbot.stop",
            "sparkbot.stop_ready",
            "sparkbot.sleep_request",
            "sparkbot.sleep_cancel",
        }:
            return
        sender = getattr(data_packet, "participant", None)
        identity = getattr(sender, "identity", "") if sender else ""
        if not identity.startswith("sparkbot-"):
            print(
                f"[SparkBot唤醒] 忽略非设备数据包，identity={identity!r}",
                flush=True,
            )
            return
        try:
            payload = data_packet.data.decode("utf-8")
        except UnicodeDecodeError:
            payload = "<binary>"
        if data_packet.topic == "sparkbot.sleep_request":
            request_sparkbot_orderly_sleep(identity, payload)
        elif data_packet.topic == "sparkbot.sleep_cancel":
            cancel_sparkbot_orderly_sleep(identity, payload)
        elif data_packet.topic == "sparkbot.stop":
            request_sparkbot_stop(identity, payload)
        elif data_packet.topic == "sparkbot.stop_ready":
            mark_sparkbot_stop_ready(identity, payload)
        else:
            # A new wake starts a fresh conversational turn.  Do not let a
            # stop guard from the previous active period suppress the wake
            # greeting after a standby/wake cycle.
            sparkbot_stop_suppression = False
            sparkbot_stop_ready = False
            sparkbot_stop_result_prompt_epoch = None
            sparkbot_stop_fallback_until = 0.0
            sparkbot_stop_fallback_epoch = None
            print(f"[SparkBot唤醒] 收到 {identity}: {payload}", flush=True)
            sparkbot_last_wake_packet_at[identity] = time.monotonic()
            fallback_task = sparkbot_greeting_fallback_tasks.pop(identity, None)
            if fallback_task is not None and not fallback_task.done():
                fallback_task.cancel()
            request_sparkbot_greeting(identity, "唤醒数据包")

    @session.on("agent_state_changed")
    def on_agent_state_changed(event) -> None:
        nonlocal sparkbot_drain_speaking_seen
        nonlocal sparkbot_stop_suppression
        nonlocal sparkbot_stop_result_prompt_epoch
        nonlocal sparkbot_agent_state
        sparkbot_agent_state = event.new_state
        print(
            f"[Agent状态] {event.old_state} -> {event.new_state}",
            flush=True,
        )
        if event.new_state == "speaking":
            if sparkbot_stop_result_prompt_epoch == sparkbot_stop_epoch:
                # This is the deliberate post-stop backend-result turn.  It
                # is the one response allowed through the stop suppression.
                sparkbot_stop_result_prompt_epoch = None
                print("[SparkBot停止] 已开始播报停止前工具结果。", flush=True)
            elif sparkbot_stop_suppression:
                # A stale Gemini response can arrive after the reset request.
                # Transport reset normally discards this generation.  Keep
                # the stop command authoritative if one trailing state event
                # races the reset until the device reports that the next-input
                # mute guard is over.
                try:
                    completion = session.interrupt(force=True)
                    completion.add_done_callback(
                        lambda task: task.exception() if not task.cancelled() else None
                    )
                except RuntimeError:
                    pass
                print("[SparkBot停止] 抑制停止口令之后到达的陈旧播报。", flush=True)
        # This is deliberately tied to a controlled realtime-text injection,
        # not to AgentSession.wait_for_idle().  The latter can remain blocked
        # forever after Gemini cancels the old FunctionCall for “休息吧”.
        if sparkbot_sleep_draining_identity:
            if event.new_state == "speaking":
                sparkbot_drain_speaking_seen = True
                sparkbot_drain_speech_started.set()
            elif (
                event.old_state == "speaking"
                and sparkbot_drain_speaking_seen
                and event.new_state in {"listening", "thinking"}
            ):
                sparkbot_drain_playout_done.set()
                print("[SparkBot待机] 已观测到最终播报结束。", flush=True)

    @session.on("user_input_transcribed")
    def on_user_input_transcribed(event) -> None:
        nonlocal forced_backend
        nonlocal sparkbot_stop_suppression, sparkbot_stop_ready
        print(
            f"[用户转写] final={event.is_final}: {event.transcript}",
            flush=True,
        )
        if is_sparkbot_stop_transcript(event.transcript):
            if event.is_final:
                request_sparkbot_stop_from_transcript()
            else:
                print(
                    "[SparkBot停止] 检测到停止口令的 Gemini 部分转写，"
                    "等待 final 后执行转写兜底。",
                    flush=True,
                )
            return
        if sparkbot_stop_suppression:
            # The firmware mutes the RTP boundary before emitting
            # sparkbot.stop_ready.  A transport reset can still flush the
            # previous generation's final/partial transcript while that mute
            # guard is active; never route that text as a new instruction.
            if not sparkbot_stop_ready:
                print(
                    "[SparkBot停止] 丢弃停止后静音保护期间的残留转写，等待 stop_ready。",
                    flush=True,
                )
                return
            if event.transcript.strip():
                sparkbot_stop_suppression = False
                sparkbot_stop_ready = False
                print("[SparkBot停止] 收到下一条用户指令，恢复正常响应。", flush=True)
        if event.is_final:
            try:
                routed = route_explicit_backend(event.transcript)
                forced_backend = routed.backend if routed else None
                if routed:
                    print(
                        f"[后端路由] 显式指定 {routed.backend.value}: {routed.task}",
                        flush=True,
                    )
            except ValueError as error:
                forced_backend = None
                print(f"[后端路由] {error}", flush=True)

    @session.on("error")
    def on_error(event) -> None:
        print(f"[会话错误] {event}", flush=True)

    @session.on("close")
    def on_close(event) -> None:
        print(
            f"[Session关闭] reason={getattr(event, 'reason', None)}",
            flush=True,
        )

        # Session 异常关闭时，尽力停止仍在运行的 Hermes 任务。
        if active_run_id:
            start_background(hermes.stop_run(active_run_id))
        start_background(codex.stop())

    await session.start(
        agent=agent,
        room=ctx.room,
        room_options=room_io.RoomOptions(
            # 保留快速重连能力，避免手机短暂断开导致房间竞态删除。
            close_on_disconnect=False,
            delete_room_on_close=False,
        ),
    )
    await ctx.connect()

    # 预热入房不会触发问候；只有 firmware 发出的可靠 sparkbot.wake
    # 数据包才会启动 Gemini 语音回复。这样待机不会产生无意义上下文。

    if await hermes.health():
        print(
            f"[Hermes] API 已连接：{HERMES_BASE_URL}",
            flush=True,
        )
    else:
        print(
            f"[Hermes] 警告：暂时无法访问 {HERMES_BASE_URL}/health",
            flush=True,
        )

    if codex.available():
        print(
            f"[Codex] CLI 已就绪：{CODEX_COMMAND}，工作目录：{CODEX_WORKSPACE}",
            flush=True,
        )
    else:
        print(f"[Codex] 警告：找不到命令 {CODEX_COMMAND}", flush=True)

    if ENABLE_GREETING and "3.1" not in GEMINI_MODEL:
        session.generate_reply(
            instructions=(
                "请用一句简短中文问候用户，告诉用户可以直接说出任务。不要调用任何工具。"
            ),
            allow_interruptions=True,
        )
    elif ENABLE_GREETING:
        print(
            f"[启动问候] {GEMINI_MODEL} 不支持 generate_reply，已跳过主动问候。",
            flush=True,
        )


if __name__ == "__main__":
    asyncio.run(cleanup_stale_sparkbot_rooms())
    cli.run_app(server)
