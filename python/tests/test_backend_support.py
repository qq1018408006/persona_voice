from pathlib import Path

import pytest

from backend_support import (
    BackendRoute,
    CodexClient,
    channel_delivery_instructions,
    route_explicit_backend,
)


@pytest.mark.parametrize(
    ("utterance", "backend", "task"),
    [
        ("让小c做一个项目总结", BackendRoute.CODEX, "一个项目总结"),
        ("让小 C 做：检查代码", BackendRoute.CODEX, "检查代码"),
        ("让小h做整理下载目录", BackendRoute.HERMES, "整理下载目录"),
        ("让小 H 做一下，查询天气", BackendRoute.HERMES, "查询天气"),
    ],
)
def test_routes_explicit_backend_phrases(
    utterance: str,
    backend: BackendRoute,
    task: str,
) -> None:
    routed = route_explicit_backend(utterance)

    assert routed is not None
    assert routed.backend is backend
    assert routed.task == task


def test_does_not_force_route_without_explicit_phrase() -> None:
    assert route_explicit_backend("帮我检查一下代码") is None


def test_rejects_ambiguous_dual_backend_phrase() -> None:
    with pytest.raises(ValueError, match="同时指定"):
        route_explicit_backend("让小c做检查，再让小h做修改")


def test_channel_delivery_is_performed_by_backend() -> None:
    instructions = channel_delivery_instructions(
        backend_name="Hermes",
        target="telegram",
        sender_command="hermes",
    )

    assert 'hermes send --to telegram "要发送的文本"' in instructions
    assert "[[as_document]] MEDIA:/绝对路径/文件名" in instructions
    assert "手机 App\n   不负责中转文件" in instructions


def test_missing_channel_never_claims_delivery() -> None:
    instructions = channel_delivery_instructions(
        backend_name="Codex",
        target="",
        sender_command="hermes",
    )

    assert "没有配置可用的 channel" in instructions
    assert "不得声称已经发送" in instructions


@pytest.mark.asyncio
async def test_codex_client_uses_safe_noninteractive_workspace(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    captured: dict[str, object] = {}

    class FakeProcess:
        returncode = 0

        async def communicate(self, prompt: bytes) -> tuple[bytes, bytes]:
            captured["prompt"] = prompt.decode()
            output_path = Path(
                captured["command"][
                    captured["command"].index("--output-last-message") + 1
                ]
            )
            output_path.write_text("任务完成", encoding="utf-8")
            return b"", b""

    async def fake_subprocess(*command: str, **kwargs: object) -> FakeProcess:
        captured["command"] = list(command)
        captured["cwd"] = kwargs["cwd"]
        captured["env"] = kwargs["env"]
        return FakeProcess()

    monkeypatch.setattr("asyncio.create_subprocess_exec", fake_subprocess)
    monkeypatch.setenv("LIVEKIT_API_SECRET", "must-not-leak")
    monkeypatch.setenv("GOOGLE_API_KEY", "must-not-leak")
    client = CodexClient(
        command="/bin/true",
        workspace=tmp_path,
        model="",
        timeout_seconds=10,
        channel_target="telegram",
    )

    result = await client.run("检查代码")

    command = captured["command"]
    assert result == "任务完成"
    assert command[:2] == ["/bin/true", "exec"]
    assert command[command.index("--sandbox") + 1] == "workspace-write"
    assert "sandbox_workspace_write.network_access=true" in command
    assert "--ephemeral" in command
    assert command[-1] == "-"
    assert captured["cwd"] == tmp_path
    assert "LIVEKIT_API_SECRET" not in captured["env"]
    assert "GOOGLE_API_KEY" not in captured["env"]
    assert captured["prompt"].startswith("检查代码")
    assert "hermes send --to telegram" in captured["prompt"]
    assert "[[as_document]] MEDIA:/绝对路径/文件名" in captured["prompt"]
    assert "不直接向 channel 上传文件" in captured["prompt"]
    assert "调用本机 Hermes 的 channel 发送能力" in captured["prompt"]
    assert "微信、QQ、Telegram" in captured["prompt"]
    assert "weixin、qqbot、" in captured["prompt"]
