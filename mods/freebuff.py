from __future__ import annotations

import glob
import json
import os
import re

from harness import Channel, Harness

BUSY_MARKER = "Starting your model session"
PROMPT_MARKER = "Enter a coding task"
READY_MARKER = "for history " + "\u00b7" + " ? for help"
HOME = os.path.join(os.path.expanduser("~"), ".config", "manicode")
NOTICE_MARKERS = ("Not enough hours", "Choose another model", "not available on this connection")
CHAT_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,255}$")


def project_dir(cwd: str) -> str:
    return os.path.join(os.environ.get("FREEBUFF_HOME") or HOME, "projects",
                        os.path.basename(os.path.abspath(cwd)))


def sessions(cwd: str) -> list[dict]:
    rows = []
    pattern = os.path.join(project_dir(cwd), "chats", "*", "chat-messages.json")
    for path in glob.glob(pattern):
        chat_id = os.path.basename(os.path.dirname(path))
        if not CHAT_ID_RE.match(chat_id):
            continue
        try:
            stat = os.stat(path)
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                data = json.load(f)
        except (OSError, ValueError):
            continue
        messages = data if isinstance(data, list) else data.get("messages", [])
        if not isinstance(messages, list) or not messages:
            continue
        rows.append({"session_id": chat_id, "path": path,
                     "mtime": stat.st_mtime, "size": stat.st_size})
    return sorted(rows, key=lambda row: row["mtime"])


class FreebuffChannel(Channel):
    def __init__(self, cwd: str, since: float, at_end: bool = False,
                 session_id: str | None = None):
        super().__init__(cwd, since, at_end, session_id)
        self.project = project_dir(self.cwd)

    def newest_chat(self) -> str | None:
        pattern = os.path.join(self.project, "chats", "*", "chat-messages.json")
        return self.newest(glob.glob(pattern))

    def chat_path(self) -> str | None:
        if self.session_id:
            path = os.path.join(self.project, "chats", self.session_id, "chat-messages.json")
            if os.path.exists(path):
                return path
        return self.newest_chat()

    def messages(self, path: str) -> list:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                data = json.load(f)
        except (OSError, ValueError):
            return []
        messages = data if isinstance(data, list) else data.get("messages", [])
        return messages if isinstance(messages, list) else []

    def message_key(self, index: int, message: dict) -> str:
        return str(message.get("id") or f"index-{index}")

    def seed(self) -> None:
        self.text_len = {}
        self.done_ids = set()
        for index, message in enumerate(self.messages(self.path)):
            if not isinstance(message, dict):
                continue
            mid = self.message_key(index, message)
            self.done_ids.add(mid)
            if message.get("variant") != "ai":
                continue
            blocks = message.get("blocks") or []
            said = "".join(b.get("content", "") for b in blocks
                           if isinstance(b, dict) and b.get("type") == "text"
                           and b.get("textType") != "reasoning")
            thought = "".join(b.get("content", "") for b in blocks
                              if isinstance(b, dict) and b.get("type") == "text"
                              and b.get("textType") == "reasoning")
            self.text_len[mid] = len(said)
            self.text_len[mid + "#thinking"] = len(thought)

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
        path = self.chat_path()
        if not path:
            yield from self.pane_notice(set(kinds) | {"turn_end"})
            return
        if path != self.path:
            self._bind(path)
            if self.at_end:
                self.seed()
            else:
                self.text_len = {}
                self.done_ids = set()
        want = set(kinds) | {"turn_end"}
        messages = self.messages(path)
        for index, message in enumerate(messages):
            if not isinstance(message, dict):
                continue
            mid = self.message_key(index, message)
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

    def channel(self, cwd: str, since: float, at_end: bool = False,
                session_id: str | None = None) -> Channel:
        return FreebuffChannel(cwd, since, at_end, session_id)

    def sessions(self, cwd: str) -> list[dict]:
        return sessions(cwd)

    def find(self, session_id: str) -> dict | None:
        return None

    def selector_kind(self, selector: str) -> str:
        if CHAT_ID_RE.match(selector):
            return "id"
        raise ValueError(f"bad conversation id {selector!r}")

    def resume_args(self, session_id: str) -> tuple[str, ...]:
        self.selector_kind(session_id)
        return ("--continue", session_id)

    def continue_args(self) -> tuple[str, ...]:
        return ("--continue",)


MOD = Freebuff()
