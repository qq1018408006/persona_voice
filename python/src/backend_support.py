from __future__ import annotations

import asyncio
import os
import re
import shlex
import shutil
import tempfile
from dataclasses import dataclass
from enum import Enum
from pathlib import Path


class BackendRoute(str, Enum):
    CODEX = "codex"
    HERMES = "hermes"


@dataclass(frozen=True)
class RoutedTask:
    backend: BackendRoute
    task: str


_ROUTE_PATTERN = re.compile(r"让\s*小\s*(?P<backend>[cChH马])\s*做(?:一下)?[\s：:，,]*")


def route_explicit_backend(utterance: str) -> RoutedTask | None:
    """Parse the two deterministic spoken backend prefixes."""
    matches = list(_ROUTE_PATTERN.finditer(utterance))
    if not matches:
        return None

    backends = {match.group("backend").lower() for match in matches}
    if len(backends) > 1:
        raise ValueError("一句话中同时指定了小 C 和小马，请一次指定一个后端。")

    match = matches[0]
    task = utterance[match.end() :].strip(" \t\r\n，,。:：")
    backend = (
        BackendRoute.CODEX
        if match.group("backend").lower() == "c"
        else BackendRoute.HERMES
    )
    return RoutedTask(backend=backend, task=task)


def channel_delivery_instructions(
    *,
    backend_name: str,
    target: str,
    sender_command: str,
    send_via_hermes: bool = False,
) -> str:
    """Tell a backend how to deliver artifacts itself, not through LiveKit."""
    if not target.strip():
        return f"""

{backend_name} 当前没有配置可用的 channel 目标。如果用户要求向 channel
发送文本或文件，不得声称已经发送；请在最终结果中明确说明需要先配置
{backend_name} 的 channel。
"""

    command = shlex.join([sender_command, "send", "--to", target])
    delegation = ""
    if send_via_hermes:
        delegation = f"""
6. 你是 Codex，不直接向 channel 上传文件。当用户要求通过任何 channel
   （包括但不限于微信、QQ、Telegram）发送文件时，必须先把文件完整生成并
   检查可用性，再调用本机 Hermes 的 channel 发送能力完成交付。
7. 如果用户明确指定了平台或目标，优先把该平台对应的 Hermes target 传给
   `{sender_command} send --to`；用户没有指定时才使用默认目标 {target}。
   例如微信、QQ、Telegram 可分别使用本机 Hermes 已配置的 weixin、qqbot、
   telegram target。目标未配置或发送失败时不得声称发送成功。
"""
    return f"""

Channel 交付规则：
1. 只有用户在本次任务中明确要求“发送到你的 channel/频道”或指定发送时，
   才允许向外部 channel 发送；否则不得发送。
2. 发送动作必须由你在任务执行过程中亲自完成，实时语音 Agent 和手机 App
   不负责中转文件。
3. 你的 channel 目标是 {target}。发送普通文本可执行：
   {command} "要发送的文本"
4. 发送 PPT、PDF、DOCX、XLSX、图片等文件时，先确认文件真实存在，再执行：
   {command} "[[as_document]] MEDIA:/绝对路径/文件名"
5. 必须检查发送命令的退出状态。只有命令成功后才能说“已发送”；失败时
   如实返回错误。不要把 MEDIA 路径或内部标记交给语音 Agent 代发。
{delegation}"""


class CodexError(RuntimeError):
    pass


class CodexClient:
    """Small cancellable adapter around the non-interactive Codex CLI."""

    def __init__(
        self,
        *,
        command: str,
        workspace: Path,
        model: str,
        timeout_seconds: float,
        channel_target: str = "",
        channel_sender_command: str = "hermes",
    ) -> None:
        self._command = shlex.split(command)
        self._workspace = workspace
        self._model = model
        self._timeout_seconds = timeout_seconds
        self._channel_enabled = bool(channel_target.strip())
        self._channel_instructions = channel_delivery_instructions(
            backend_name="Codex",
            target=channel_target,
            sender_command=channel_sender_command,
            send_via_hermes=True,
        )
        self._active_process: asyncio.subprocess.Process | None = None

    def available(self) -> bool:
        return bool(self._command and shutil.which(self._command[0]))

    async def stop(self) -> None:
        process = self._active_process
        if not process or process.returncode is not None:
            return
        process.terminate()
        try:
            await asyncio.wait_for(process.wait(), timeout=5)
        except asyncio.TimeoutError:
            process.kill()
            await process.wait()

    async def run(self, task: str) -> str:
        if not self.available():
            command_name = self._command[0] if self._command else "codex"
            raise CodexError(f"找不到 Codex 命令：{command_name}")
        if not self._workspace.is_dir():
            raise CodexError(f"Codex 工作目录不存在：{self._workspace}")

        output_file = tempfile.NamedTemporaryFile(
            prefix="voice-codex-",
            suffix=".txt",
            delete=False,
        )
        output_path = Path(output_file.name)
        output_file.close()

        command = [
            *self._command,
            "exec",
            "--cd",
            str(self._workspace),
            "--sandbox",
            "workspace-write",
            "--skip-git-repo-check",
            "--ephemeral",
            "--color",
            "never",
            "--output-last-message",
            str(output_path),
        ]
        if self._channel_enabled:
            # workspace-write blocks outbound traffic by default. A configured
            # backend channel needs network access for its own send command,
            # while filesystem writes remain limited to the workspace.
            command.extend(("--config", "sandbox_workspace_write.network_access=true"))
        if self._model:
            command.extend(("--model", self._model))
        command.append("-")

        prompt = (
            task.rstrip() + "\n\n你通过实时语音 Agent 为用户执行这个任务。"
            "请真实完成任务并简洁报告结果。" + self._channel_instructions
        )
        child_environment = os.environ.copy()
        for secret_name in (
            "GOOGLE_API_KEY",
            "HERMES_API_KEY",
            "LIVEKIT_API_KEY",
            "LIVEKIT_API_SECRET",
        ):
            child_environment.pop(secret_name, None)
        try:
            process = await asyncio.create_subprocess_exec(
                *command,
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                cwd=self._workspace,
                env=child_environment,
            )
            self._active_process = process
            try:
                stdout, stderr = await asyncio.wait_for(
                    process.communicate(prompt.encode("utf-8")),
                    timeout=self._timeout_seconds,
                )
            except asyncio.TimeoutError as error:
                await self.stop()
                raise CodexError(
                    f"Codex 处理超过 {self._timeout_seconds:.0f} 秒，已停止。"
                ) from error

            if process.returncode != 0:
                detail = (stderr or stdout).decode("utf-8", errors="replace")
                raise CodexError(
                    f"Codex 执行失败（退出码 {process.returncode}）："
                    f"{detail[-800:].strip()}"
                )

            result = output_path.read_text(encoding="utf-8").strip()
            if not result:
                raise CodexError("Codex 已退出，但没有返回结果。")
            return result[:12000]
        except OSError as error:
            raise CodexError(
                f"无法启动 Codex：{type(error).__name__}: {error}"
            ) from error
        finally:
            self._active_process = None
            output_path.unlink(missing_ok=True)
