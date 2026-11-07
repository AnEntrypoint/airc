#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import os
import time
from typing import Dict, Set

from aiohttp import WSMsgType, web

import ccsession as C
import mods

HUBS: Dict[str, "Hub"] = {}
DEFAULT_CWD = os.path.abspath(os.environ.get("CCBRIDGE_CWD", "/workspace/airc/.ccbridge/scratch"))
IDLE_REAP_SECONDS = float(os.environ.get("CCBRIDGE_IDLE_REAP", "1800"))
TAG = {"text": "t", "thinking": "k", "tool": "u", "result": "r", "you": "y"}


class Hub:
    def __init__(self, name: str, cwd: str, harness: str, kinds=("text", "tool")):
        self.name = name
        self.cwd = cwd
        self.harness = harness
        self.kinds = kinds
        self.sess = C.AgentSession(cwd, harness=harness, name=f"ccb-{name}")
        self.clients: Set[web.WebSocketResponse] = set()
        self.modes: Dict[web.WebSocketResponse, str] = {}
        self.queue: list[str] = []
        self.ready = False
        self.task: asyncio.Task | None = None
        self.last_touch = time.time()

    async def start(self):
        await asyncio.to_thread(self.sess.start)
        self.ready = await asyncio.to_thread(self.sess.wait_ready, 60.0)
        for text in self.queue:
            await asyncio.to_thread(self.sess.send, text)
        self.queue.clear()
        self.task = asyncio.create_task(self._pump())

    @property
    def alive(self) -> bool:
        return self.sess.alive()

    async def submit(self, text: str):
        self.last_touch = time.time()
        if self.ready:
            await asyncio.to_thread(self.sess.send, text)
        else:
            self.queue.append(text)

    def _drop(self, ws):
        self.clients.discard(ws)
        self.modes.pop(ws, None)

    async def _emit(self, event: dict):
        kind = event["kind"]
        if kind == "turn_end":
            for ws, mode in list(self.modes.items()):
                try:
                    await ws.send_str("e:" if mode == "tagged" else "")
                except (ConnectionResetError, RuntimeError):
                    self._drop(ws)
            return
        text = event.get("text", "")
        if not text and kind != "tool":
            return
        for ws, mode in list(self.modes.items()):
            if mode != "tagged" or kind not in TAG:
                continue
            try:
                await ws.send_str(f"{TAG[kind]}:{text}")
            except (ConnectionResetError, RuntimeError):
                self._drop(ws)
        if kind == "text":
            for ws, mode in list(self.modes.items()):
                if mode == "plain":
                    try:
                        await ws.send_str(text)
                    except (ConnectionResetError, RuntimeError):
                        self._drop(ws)

    async def _pump(self):
        ticks = 0
        while True:
            events = await asyncio.to_thread(lambda: list(self.sess.events(self.kinds)))
            for event in events:
                await self._emit(event)
            ticks += 1
            if ticks % 20 == 0 and not self.alive:
                for ws in list(self.clients):
                    try:
                        await ws.send_str("")
                    except (ConnectionResetError, RuntimeError):
                        self._drop(ws)
                break
            await asyncio.sleep(0.15)

    async def stop(self):
        if self.task:
            self.task.cancel()
        await asyncio.to_thread(self.sess.stop)


async def reap_idle():
    while True:
        await asyncio.sleep(60)
        now = time.time()
        for name, hub in list(HUBS.items()):
            if not hub.clients and now - hub.last_touch > IDLE_REAP_SECONDS:
                await hub.stop()
                HUBS.pop(name, None)


async def ws_handler(request: web.Request) -> web.WebSocketResponse:
    ws = web.WebSocketResponse(heartbeat=30, max_msg_size=16 << 20)
    await ws.prepare(request)

    name = request.query.get("session", "default")
    mode = request.query.get("mode", "plain")
    cwd = request.query.get("cwd") or DEFAULT_CWD
    harness = request.query.get("harness") or mods.DEFAULT
    if harness not in mods.MODS:
        await ws.send_str(f"unknown harness {harness!r}; known: {', '.join(mods.names())}")
        await ws.close()
        return ws
    kinds = tuple(k for k in request.query.get("kinds", "text,tool").split(",") if k)

    hub = HUBS.get(name)
    if hub is not None and not hub.alive:
        await hub.stop()
        HUBS.pop(name, None)
        hub = None
    if hub is None:
        hub = Hub(name, cwd, harness, kinds)
        HUBS[name] = hub
        await hub.start()
    hub.clients.add(ws)
    hub.modes[ws] = mode
    hub.last_touch = time.time()

    try:
        async for msg in ws:
            if msg.type == WSMsgType.TEXT:
                text = msg.data
                if text.strip():
                    await hub.submit(text)
            elif msg.type == WSMsgType.ERROR:
                break
    finally:
        hub.clients.discard(ws)
        hub.modes.pop(ws, None)
    return ws


INDEX = """<!doctype html><meta charset="utf-8">
<title>ccbridge</title>
<style>
 body{font:13px/1.45 ui-monospace,Menlo,monospace;background:#111;color:#ddd;margin:0;height:100vh;display:flex;flex-direction:column}
 #out{flex:1;overflow:auto;white-space:pre-wrap;padding:10px;margin:0}
 #in{display:flex;border-top:1px solid #333}
 #t{flex:1;background:#1a1a1a;color:#eee;border:0;padding:10px;font:inherit}
 button{background:#2a2a2a;color:#ddd;border:0;padding:0 16px;font:inherit}
</style>
<pre id="out"></pre>
<form id="in"><input id="t" placeholder="type a prompt, Enter to send" autocomplete="off"><button>send</button></form>
<script>
 var out=document.getElementById('out'), t=document.getElementById('t');
 var q = new URLSearchParams(location.search);
 var name = q.get('session') || 'web';
 var harness = q.get('harness') || 'claude';
 var proto = location.protocol === 'https:' ? 'wss' : 'ws';
 var ws = new WebSocket(proto + '://' + location.host + '/ws?session=' + name + '&harness=' + harness);
 ws.onmessage = function(e){
   if (e.data === '') { out.textContent += '\n\u2014 turn done \u2014\n'; }
   else { out.textContent += e.data; }
   out.scrollTop = out.scrollHeight;
 };
 ws.onopen = function(){ out.textContent += '[connected: session=' + name + ' harness=' + harness + ']\n'; };
 ws.onclose = function(){ out.textContent += '[disconnected]\n'; };
 document.getElementById('in').onsubmit = function(ev){
   ev.preventDefault();
   if (!t.value) return;
   out.textContent += '\n> ' + t.value + '\n';
   ws.send(t.value); t.value=''; out.scrollTop = out.scrollHeight;
 };
</script>"""


async def index(_):
    return web.Response(text=INDEX, content_type="text/html")


def describe(hub: "Hub") -> dict:
    pane = hub.sess.pane()
    lines = [line.rstrip() for line in pane.splitlines() if line.strip()]
    return {"clients": len(hub.clients), "alive": hub.alive, "harness": hub.harness,
            "cwd": hub.cwd, "busy": hub.sess.harness.pane_busy(pane), "pane_tail": lines[-12:]}


async def sessions(_):
    return web.json_response({
        name: await asyncio.to_thread(describe, hub) for name, hub in HUBS.items()})


async def delete_session(request: web.Request):
    name = request.match_info["name"]
    hub = HUBS.pop(name, None)
    if hub is None:
        return web.json_response({"ok": False, "error": f"unknown session {name}"}, status=404)
    await hub.stop()
    return web.json_response({"ok": True, "session": name})


async def shutdown(app):
    for hub in list(HUBS.values()):
        await hub.stop()
    HUBS.clear()


async def start_reaper(app):
    app["reaper"] = asyncio.create_task(reap_idle())


async def stop_reaper(app):
    app["reaper"].cancel()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8787)
    a = ap.parse_args()

    app = web.Application()
    app.router.add_get("/", index)
    app.router.add_get("/ws", ws_handler)
    app.router.add_get("/sessions", sessions)
    app.router.add_delete("/sessions/{name}", delete_session)
    app.on_shutdown.append(shutdown)
    app.on_startup.append(start_reaper)
    app.on_cleanup.append(stop_reaper)
    print(f"ccbridge on http://{a.host}:{a.port}/  (ws: /ws?session=NAME&harness={'|'.join(mods.names())})")
    web.run_app(app, host=a.host, port=a.port, print=None)


if __name__ == "__main__":
    main()
