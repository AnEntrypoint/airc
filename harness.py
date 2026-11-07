from __future__ import annotations

import glob
import json
import os
import subprocess
import time
from typing import Iterator


PASTE_OPEN = "\x1b[200~"
PASTE_CLOSE = "\x1b[201~"


def tmux(*args: str) -> "subprocess.CompletedProcess":
    return subprocess.run(["tmux", *args], capture_output=True, text=True)


class Channel:
    def __init__(self, cwd: str, since: float, at_end: bool = False,
                 session_id: str | None = None):
        self.cwd = os.path.abspath(cwd)
        self.since = since
        self.at_end = at_end
        self.session_id = session_id
        self.pane = lambda: ""
        self.path: str | None = None
        self.offset = 0
        self.pending = ""
        self.text_len: dict[str, int] = {}
        self.done_ids: set[str] = set()

    def poll(self, kinds=("text", "tool")) -> Iterator[dict]:
        return iter(())

    def _bind(self, path: str) -> bool:
        if path != self.path:
            self.path = path
            try:
                self.offset = os.path.getsize(path) if self.at_end else 0
            except OSError:
                self.offset = 0
            self.pending = ""
        return True

    def _tail_records(self) -> Iterator[dict]:
        if not self.path:
            return
        try:
            with open(self.path, "r", encoding="utf-8", errors="replace") as f:
                f.seek(self.offset)
                chunk = f.read()
                self.offset = f.tell()
        except OSError:
            return
        if not chunk:
            return
        lines = (self.pending + chunk).split("\n")
        self.pending = lines.pop()
        for line in lines:
            line = line.strip()
            if not line:
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                continue

    @staticmethod
    def newest(paths, since: float = 0.0) -> str | None:
        best: tuple[float, str] | None = None
        for path in paths:
            try:
                stat = os.stat(path)
            except OSError:
                continue
            if stat.st_size == 0 or stat.st_mtime + 3 < since:
                continue
            if best is None or stat.st_mtime > best[0]:
                best = (stat.st_mtime, path)
        return best[1] if best else None

    @staticmethod
    def delta(mid: str, text: str, store: dict[str, int]) -> str:
        previous = store.get(mid, 0)
        if len(text) > previous:
            store[mid] = len(text)
            return text[previous:]
        if len(text) < previous:
            store[mid] = len(text)
        return ""


def glob_paths(pattern: str) -> list[str]:
    return glob.glob(pattern)


class Harness:
    name = "harness"
    argv: tuple[str, ...] = ()
    env_strip: tuple[str, ...] = ()
    term = "xterm-256color"
    submit_delay = 0.0

    def pane_ready(self, pane: str) -> bool:
        raise NotImplementedError(self.name)

    def pane_busy(self, pane: str) -> bool:
        raise NotImplementedError(self.name)

    def send(self, target: str, text: str) -> None:
        tmux("send-keys", "-t", target, "-l", PASTE_OPEN)
        tmux("send-keys", "-t", target, "-l", text)
        tmux("send-keys", "-t", target, "-l", PASTE_CLOSE)
        if self.submit_delay:
            time.sleep(self.submit_delay)
        tmux("send-keys", "-t", target, "Enter")

    def channel(self, cwd: str, since: float, at_end: bool = False,
                session_id: str | None = None) -> Channel:
        raise NotImplementedError(self.name)

    def sessions(self, cwd: str) -> list[dict]:
        raise NotImplementedError(self.name)

    def selector_kind(self, selector: str) -> str:
        raise NotImplementedError(self.name)

    def find(self, session_id: str) -> dict | None:
        return None

    def resume_args(self, session_id: str) -> tuple[str, ...]:
        raise NotImplementedError(self.name)

    def continue_args(self) -> tuple[str, ...]:
        raise NotImplementedError(self.name)

    def fork_args(self, session_id: str) -> tuple[str, ...]:
        raise NotImplementedError(self.name)

    def fork_continue_args(self) -> tuple[str, ...]:
        raise NotImplementedError(self.name)

    def env(self, base: dict) -> dict:
        env = dict(base)
        for key in self.env_strip:
            env.pop(key, None)
        if not env.get("TERM") or env["TERM"] == "dumb":
            env["TERM"] = self.term
        return env
