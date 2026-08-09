#!/usr/bin/env python3
"""Minimal LAN-only LiveKit token service for the SparkBot terminal."""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit


def _b64url(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def _jwt(payload: dict, secret: str) -> str:
    header = {"alg": "HS256", "typ": "JWT"}
    parts = [
        _b64url(json.dumps(header, separators=(",", ":")).encode()),
        _b64url(json.dumps(payload, separators=(",", ":")).encode()),
    ]
    signature = hmac.new(secret.encode(), ".".join(parts).encode(), hashlib.sha256).digest()
    return ".".join((*parts, _b64url(signature)))


class TokenHandler(BaseHTTPRequestHandler):
    server_version = "SparkBotToken/1.0"

    def _reply(self, status: int, payload: dict) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        if self.path != "/livekit/token":
            self._reply(404, {"error": "not found"})
            return

        service_key = os.environ.get("TOKEN_SERVICE_KEY", "")
        supplied_key = self.headers.get("X-Device-Key", "")
        if service_key and not secrets.compare_digest(service_key, supplied_key):
            self._reply(401, {"error": "unauthorized"})
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 4096:
                raise ValueError("invalid request size")
            request = json.loads(self.rfile.read(length))
            identity = str(request["identity"])
            requested_room = str(request.get("room", ""))
            requested_agent = str(request.get("agent_name", ""))
        except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
            self._reply(400, {"error": str(error)})
            return

        api_key = os.environ.get("LIVEKIT_API_KEY", "")
        api_secret = os.environ.get("LIVEKIT_API_SECRET", "")
        # The Python agent may legitimately use loopback, but an ESP32 must be
        # given a LAN/public address.  Keep both values independently
        # configurable when this service shares the agent's environment.
        server_url = os.environ.get("LIVEKIT_PUBLIC_URL", "") or os.environ.get(
            "LIVEKIT_URL", ""
        )
        parsed_server_url = urlsplit(server_url)
        if parsed_server_url.hostname in {"127.0.0.1", "localhost", "::1"}:
            # A device calling this HTTP service cannot use the server's
            # loopback address. Derive the LAN host from the request as a safe
            # fallback when LIVEKIT_PUBLIC_URL was omitted.
            request_host = self.headers.get("Host", "").rsplit(":", 1)[0]
            public_port = int(
                os.environ.get("LIVEKIT_PUBLIC_PORT", parsed_server_url.port or 7880)
            )
            if request_host:
                server_url = f"{parsed_server_url.scheme}://{request_host}:{public_port}"
        firmware_room = os.environ.get("LIVEKIT_ROOM", "").strip()
        android_room = os.environ.get("LIVEKIT_ANDROID_ROOM", "").strip()
        if not android_room:
            android_room = "sparkbot-android-room"
        # SparkBot 固件和手机 App 各用独立房间，避免 LiveKit AgentSession
        # 只绑定第一个说话人导致另一个设备“只能听不能说”。
        if identity.startswith("sparkbot-android-"):
            room = android_room
        else:
            room = firmware_room or requested_room
        agent_name = os.environ.get("LIVEKIT_AGENT_NAME", requested_agent)
        if not api_key or not api_secret or not server_url or not room:
            self._reply(500, {"error": "LiveKit environment is incomplete"})
            return
        if requested_room and requested_room != room:
            self._reply(403, {"error": "room not allowed"})
            return
        if len(identity) > 128 or not identity.startswith("sparkbot-"):
            self._reply(403, {"error": "invalid device identity"})
            return

        now = int(time.time())
        payload = {
            "iss": api_key,
            "sub": identity,
            "name": identity,
            "iat": now,
            "nbf": now - 5,
            "exp": now + 600,
            "video": {
                "roomJoin": True,
                "room": room,
                "canPublish": True,
                "canSubscribe": True,
                "canPublishData": True,
            },
        }
        if agent_name:
            payload["roomConfig"] = {"agents": [{"agentName": agent_name}]}

        self._reply(200, {"server_url": server_url, "token": _jwt(payload, api_secret)})

    def log_message(self, message: str, *args: object) -> None:
        print(f"{self.client_address[0]} - {message % args}")


def _load_env_file() -> str | None:
    """Load KEY=VALUE lines from the repository .env into os.environ.

    Mirrors the old ``set -a; source .env; set +a`` flow: values in .env
    override already-exported variables. Returns the loaded path or None.
    """
    # 统一环境变量文件在仓库根目录（persona_voice/.env）
    candidate = Path(__file__).resolve().parents[2] / ".env"
    if not candidate.exists():
        return None
    for raw in candidate.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export ") :].strip()
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
            value = value[1:-1]
        os.environ[key] = value
    return str(candidate)


def main() -> None:
    env_path = _load_env_file()
    host = os.environ.get("TOKEN_SERVER_HOST", "0.0.0.0")
    port = int(os.environ.get("TOKEN_SERVER_PORT", "8080"))
    loaded = f"（已加载 {env_path}）" if env_path else "（未找到 .env，使用环境变量/默认值）"
    print(f"SparkBot token service listening on http://{host}:{port}/livekit/token {loaded}")
    ThreadingHTTPServer((host, port), TokenHandler).serve_forever()


if __name__ == "__main__":
    main()
