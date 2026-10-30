from __future__ import annotations

import glob
import json
import os

from harness import Channel, Harness

BUSY_MARKER = "Starting your model session"
PROMPT_MARKER = "Enter a coding task"
READY_MARKER = "for history " + "\u00b7" + " ? for help"
HOME = os.path.join(os.path.expanduser("~"), ".config", "manicode")
NOTICE_MARKERS = ("Not enough hours", "Choose another model", "not available on this connection")


class FreebuffChannel(Channel):
    def __init__(self, cwd: str, since: float):
        super().__init__(cwd, since)
        self.project = os.path.join(os.environ.get("FREEBUFF_HOME") or HOME, "projects",
                                    os.path.basename(self.cwd))

    def newest_chat(self) -> str | None:
        pattern = os.path.join(self.project, "chats", "*", "chat-messages.json")
        return self.newest(glob.glob(pattern))

    def pane_notice(self, want):
        for line in self.pane().splitlines():
            if not any(marker in line for marker in NOTICE_MARKERS):
                continue
            key = "notice:" + line.strip()
            if key in self.done_ids:
                return
            self.done_ids.add(key)
            if "text" in want:
                yield {"kind": "text", "text": line.strip()}
            if "turn_end" in want:
                yield {"kind": "turn_end"}
            return

    def poll(self, kinds=("text", "tool")):
        path = self.newest_chat()
        if not path:
            yield from self.pane_notice(set(kinds) | {"turn_end"})
            return
        if path != self.path:
            self._bind(path)
            self.text_len = {}
            self.done_ids = set()
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                data = json.load(f)
        except (OSError, ValueError):
            return
        want = set(kinds) | {"turn_end"}
        messages = data if isinstance(data, list) else data.get("messages", [])
        for message in messages:
            if not isinstance(message, dict):
                continue
            mid = message.get("id") or str(len(self.done_ids))
            variant = message.get("variant")
            if variant == "user":
                if mid not in self.done_ids and "you" in want:
                    self.done_ids.add(mid)
                    yield {"kind": "you", "text": message.get("content", "")}
                continue
            if variant != "ai":
                continue
            blocks = message.get("blocks") or []
            said = "".join(b.get("content", "") for b in blocks
                           if isinstance(b, dict) and b.get("type") == "text" and b.get("textType") != "reasoning")
            thought = "".join(b.get("content", "") for b in blocks
                              if isinstance(b, dict) and b.get("type") == "text" and b.get("textType") == "reasoning")
            if said and "text" in want:
                piece = self.delta(mid, said, self.text_len)
                if piece:
                    yield {"kind": "text", "text": piece}
            if thought and "thinking" in want:
                piece = self.delta(mid + "#thinking", thought, self.text_len)
                if piece:
                    yield {"kind": "thinking", "text": piece}
            if message.get("isComplete") and mid not in self.done_ids:
                self.done_ids.add(mid)
                error = message.get("userError")
                if error and "result" in want:
                    yield {"kind": "result", "text": str(error)[:4000]}
                if "turn_end" in want:
                    yield {"kind": "turn_end"}


class Freebuff(Harness):
    name = "freebuff"
    argv = ("freebuff",)
    env_strip = ()
    submit_delay = 0.15

    def pane_busy(self, pane: str) -> bool:
        return BUSY_MARKER in pane

    def pane_ready(self, pane: str) -> bool:
        return PROMPT_MARKER in pane or READY_MARKER in pane or BUSY_MARKER in pane

    def channel(self, cwd: str, since: float) -> Channel:
        return FreebuffChannel(cwd, since)


MOD = Freebuff()
