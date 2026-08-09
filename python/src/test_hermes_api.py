import asyncio
import json
import os

import aiohttp
from dotenv import load_dotenv


load_dotenv(".env.local", override=True)

BASE_URL = os.getenv("HERMES_BASE_URL", "http://127.0.0.1:8642").rstrip("/")
API_KEY = os.getenv("HERMES_API_KEY", "")


async def main() -> None:
    headers = {
        "Authorization": f"Bearer {API_KEY}",
        "Content-Type": "application/json",
    }

    async with aiohttp.ClientSession(headers=headers) as http:
        async with http.get(f"{BASE_URL}/health") as response:
            print("health:", response.status, await response.text())

        payload = {
            "model": os.getenv("HERMES_MODEL", "hermes-agent"),
            "input": "请只返回一句话：Hermes 本地接口连接成功。",
            "session_id": "voice-smoke-test",
            "instructions": "只输出一句简洁中文，不要使用 Markdown。",
        }

        async with http.post(
            f"{BASE_URL}/v1/runs",
            json=payload,
        ) as response:
            body = await response.json()
            print("start:", response.status, json.dumps(body, ensure_ascii=False))

        run_id = body["run_id"]

        while True:
            await asyncio.sleep(0.5)
            async with http.get(
                f"{BASE_URL}/v1/runs/{run_id}"
            ) as response:
                status = await response.json()
                print("status:", json.dumps(status, ensure_ascii=False))

            if status.get("status") in {
                "completed",
                "failed",
                "cancelled",
            }:
                break


if __name__ == "__main__":
    asyncio.run(main())
