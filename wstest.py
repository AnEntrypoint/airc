#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import sys

import aiohttp

import mods


async def main():
    ap = argparse.ArgumentParser(description="ccbridge websocket client")
    ap.add_argument("prompts", nargs="*")
    ap.add_argument("--harness", default=mods.DEFAULT, choices=mods.names())
    ap.add_argument("--session", default="clitest")
    ap.add_argument("--cwd", default=None)
    ap.add_argument("--url", default="ws://127.0.0.1:8787/ws")
    ap.add_argument("--kinds", default="text,tool")
    ap.add_argument("--turn-timeout", type=float, default=180.0)
    a = ap.parse_args()
    prompts = a.prompts or ["Reply with exactly: BRIDGE-OK"]
    url = f"{a.url}?session={a.session}&harness={a.harness}&kinds={a.kinds}"
    if a.cwd:
        url += f"&cwd={a.cwd}"

    async with aiohttp.ClientSession() as session:
        async with session.ws_connect(url) as ws:
            for prompt in prompts:
                print(f"\n>> {prompt}", flush=True)
                await ws.send_str(prompt)
                while True:
                    message = await asyncio.wait_for(ws.receive(), a.turn_timeout)
                    if message.type != aiohttp.WSMsgType.TEXT:
                        print("\n[closed]", flush=True)
                        return
                    if message.data == "":
                        print("\n<< turn end >>", flush=True)
                        break
                    print(message.data, end="", flush=True)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except (asyncio.TimeoutError, KeyboardInterrupt):
        sys.exit(1)
