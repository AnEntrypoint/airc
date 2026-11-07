#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import shlex
import time

import mods
from harness import tmux


class AgentSession:
    def __init__(self, cwd: str, harness: str = mods.DEFAULT, name: str | None = None,
                 cols: int = 200, rows: int = 50, launch_args: tuple[str, ...] = (),
                 at_end: bool = False):
        self.cwd = os.path.abspath(cwd)
        self.harness = mods.load(harness)
        self.launch_args = tuple(launch_args)
        self.at_end = at_end
        self.name = name or f"ccb-{os.getpid()}-{int(time.time() * 1000) % 10_000_000}"
        self.cols, self.rows = cols, rows
        self.start_time = time.time()
        self.channel = None
        self._started = False

    def command(self) -> str:
        parts = ["env"]
        for key in self.harness.env_strip:
            parts += ["-u", key]
        parts += list(self.harness.argv) + list(self.launch_args)
        return " ".join(shlex.quote(part) for part in parts)

    def start(self):
        os.makedirs(self.cwd, exist_ok=True)
        if self.launch_args:
            self.stop()
        tmux("new-session", "-d", "-s", self.name, "-c", self.cwd,
             "-x", str(self.cols), "-y", str(self.rows), self.command())
        self._started = True
        self.start_time = time.time()
        return self

    def stop(self):
        tmux("kill-session", "-t", self.name)
        self._started = False

    def alive(self) -> bool:
        return tmux("has-session", "-t", self.name).returncode == 0

    def pane(self) -> str:
        done = tmux("capture-pane", "-t", self.name, "-p")
        return done.stdout if done.returncode == 0 else ""

    def busy(self) -> bool:
        return self.harness.pane_busy(self.pane())

    def wait_ready(self, timeout: float = 60.0) -> bool:
        end = time.time() + timeout
        while time.time() < end:
            if self.harness.pane_ready(self.pane()):
                time.sleep(1.0)
                return True
            time.sleep(0.3)
        return False

    def send(self, text: str):
        self.harness.send(self.name, text)

    def events(self, kinds=("text",)):
        if self.channel is None:
            self.channel = self.harness.channel(self.cwd, self.start_time, self.at_end)
            self.channel.pane = self.pane
        yield from self.channel.poll(kinds)


def ClaudeSession(cwd: str, name: str | None = None, transcript_root: str | None = None,
                  cols: int = 200, rows: int = 50) -> AgentSession:
    return AgentSession(cwd, harness="claude", name=name, cols=cols, rows=rows)


def _main():
    ap = argparse.ArgumentParser(description="ccbridge: harness text-in/text-out engine")
    ap.add_argument("prompts", nargs="+", help="prompts to send in order")
    ap.add_argument("--harness", default=mods.DEFAULT, choices=mods.names())
    ap.add_argument("--cwd", default="/workspace/airc/.ccbridge/scratch")
    ap.add_argument("--kinds", default="text", help="comma list: text,thinking,tool,result,you")
    ap.add_argument("--step-timeout", type=float, default=180.0)
    a = ap.parse_args()

    session = AgentSession(a.cwd, harness=a.harness).start()
    kinds = tuple(k for k in a.kinds.split(",") if k)
    try:
        if not session.wait_ready():
            raise SystemExit(f"{a.harness} did not become ready")
        print(f"[session {session.name} harness={a.harness} cwd={session.cwd}]", flush=True)
        for prompt in a.prompts:
            print(f"\n>>> {prompt}", flush=True)
            session.send(prompt)
            end = time.time() + a.step_timeout
            while time.time() < end:
                events = list(session.events(kinds))
                for event in events:
                    if event["kind"] == "turn_end":
                        break
                    if event["kind"] == "tool":
                        print(f"  [tool] {event['text']}", flush=True)
                    elif event["kind"] == "thinking":
                        print(f"  [think] {event['text'][:200]}", flush=True)
                    else:
                        print(event["text"], end="", flush=True)
                if any(event["kind"] == "turn_end" for event in events):
                    break
                time.sleep(0.2)
            print()
    finally:
        session.stop()


if __name__ == "__main__":
    _main()
