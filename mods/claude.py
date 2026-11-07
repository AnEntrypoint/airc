from __future__ import annotations

import glob
import os
import re

from harness import Channel, Harness

NESTING_VARS = (
    "CLAUDECODE",
    "CLAUDE_CODE_CHILD_SESSION",
    "CLAUDE_CODE_SESSION_ID",
    "CLAUDE_CODE_SESSION_ATTENDED",
    "CLAUDE_PID",
    "CLAUDE_CODE_MESSAGING_SOCKET",
    "CLAUDE_CODE_MESSAGING_TOKEN",
    "CLAUDE_EFFORT",
)

BUSY_MARKER = "esc to interrupt"
PROMPT_GLYPH = "\u276f"
TITLE_MARKER = "Claude Code"
CONTENT_TYPES = {"assistant", "user"}
TRANSCRIPT_ROOT = os.path.join(os.path.expanduser("~"), ".claude", "projects")
UUID_RE = re.compile(r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$")
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 ._-]{0,127}$")


def project_dir(cwd: str) -> str:
    return os.path.join(TRANSCRIPT_ROOT, re.sub(r"[^A-Za-z0-9]", "-", os.path.abspath(cwd)))


def find_path(session_id: str) -> str | None:
    paths = glob.glob(os.path.join(TRANSCRIPT_ROOT, "*", f"{session_id}.jsonl"))
    if not paths:
        return None
    return max(paths, key=lambda path: os.stat(path).st_mtime)


def sessions(cwd: str) -> list[dict]:
    found = []
    for path in glob.glob(os.path.join(project_dir(cwd), "*.jsonl")):
        session_id = os.path.basename(path)[:-len(".jsonl")]
        if not session_id or session_id.startswith("agent-"):
            continue
        try:
            stat = os.stat(path)
        except OSError:
            continue
        if stat.st_size == 0:
            continue
        found.append({"session_id": session_id, "path": path,
                      "mtime": stat.st_mtime, "size": stat.st_size})
    return sorted(found, key=lambda item: item["mtime"])


class ClaudeChannel(Channel):
    def __init__(self, cwd: str, since: float, at_end: bool = False,
                 session_id: str | None = None):
        super().__init__(cwd, since, at_end, session_id)
        self.project = project_dir(self.cwd)

    def candidates(self) -> list[str]:
        seen = set()
        if os.path.isdir(self.project):
            seen.update(glob.glob(os.path.join(self.project, "*.jsonl")))
        if not seen:
            tag = re.sub(r"[^A-Za-z0-9]", "-", os.path.basename(self.cwd))
            for path in glob.glob(os.path.join(TRANSCRIPT_ROOT, "*", "*.jsonl")):
                if tag and tag not in os.path.basename(os.path.dirname(path)):
                    continue
                try:
                    if os.stat(path).st_mtime + 3 >= self.since:
                        seen.add(path)
                except OSError:
                    continue
        return [p for _, p in sorted((os.stat(p).st_mtime, p) for p in seen if os.path.exists(p))]

    def bind(self) -> bool:
        if self.session_id:
            if os.path.isabs(self.session_id) and os.path.isfile(self.session_id):
                return self._bind(self.session_id)
            target = os.path.join(self.project, f"{self.session_id}.jsonl")
            if os.path.exists(target):
                return self._bind(target)
            if UUID_RE.match(self.session_id):
                global_path = find_path(self.session_id)
                return self._bind(global_path) if global_path else False
        for path in reversed(self.candidates()):
            newest = self.newest([path], self.since)
            if newest:
                return self._bind(newest)
        return False

    def poll(self, kinds=("text", "tool")):
        if not self.bind():
            return
        want = set(kinds) | {"turn_end"}
        ordered: dict[str, dict[int, str]] = {}
        for record in self._tail_records():
            if self.before_start(record):
                continue
            rtype = record.get("type")
            if rtype == "system":
                if record.get("subtype") == "turn_duration":
                    yield {"kind": "turn_end"}
                continue
            if rtype not in CONTENT_TYPES:
                continue
            message = record.get("message") or {}
            blocks = message.get("content")
            if blocks is None:
                continue
            if isinstance(blocks, str):
                if "you" in want:
                    yield {"kind": "you", "text": blocks}
                continue
            mid = message.get("id") or record.get("uuid")
            index = record.get("apiBlockIndex") or 0
            for block in blocks:
                if not isinstance(block, dict):
                    continue
                btype = block.get("type")
                if btype == "text":
                    acc = ordered.setdefault(mid, {})
                    acc[index] = block.get("text", "")
                    full = "".join(acc[k] for k in sorted(acc))
                    if "text" in want:
                        piece = self.delta(mid, full, self.text_len)
                        if piece:
                            yield {"kind": "text", "text": piece}
                elif btype == "thinking" and "thinking" in want:
                    yield {"kind": "thinking", "text": block.get("thinking", "")}
                elif btype == "tool_use" and "tool" in want:
                    yield {"kind": "tool", "text": block.get("name", ""), "input": block.get("input")}
                elif btype == "tool_result" and "result" in want:
                    content = block.get("content")
                    if isinstance(content, list):
                        content = "".join(x.get("text", "") for x in content if isinstance(x, dict))
                    yield {"kind": "result", "text": str(content)[:4000]}


class Claude(Harness):
    name = "claude"
    argv = ("claude",)
    env_strip = NESTING_VARS

    def pane_busy(self, pane: str) -> bool:
        return BUSY_MARKER in pane

    def pane_ready(self, pane: str) -> bool:
        return PROMPT_GLYPH in pane or BUSY_MARKER in pane or TITLE_MARKER in pane

    def channel(self, cwd: str, since: float, at_end: bool = False,
                session_id: str | None = None) -> Channel:
        return ClaudeChannel(cwd, since, at_end, session_id)

    def sessions(self, cwd: str) -> list[dict]:
        return sessions(cwd)

    def find(self, session_id: str) -> dict | None:
        path = find_path(session_id)
        if not path:
            return None
        try:
            stat = os.stat(path)
        except OSError:
            return None
        return {"session_id": session_id, "path": path,
                "mtime": stat.st_mtime, "size": stat.st_size}

    def selector_kind(self, selector: str) -> str:
        if os.path.isabs(selector) and os.path.isfile(selector):
            return "path"
        if UUID_RE.match(selector):
            return "id"
        if NAME_RE.match(selector):
            return "name"
        raise ValueError(f"bad session selector {selector!r}")

    def resume_args(self, session_id: str) -> tuple[str, ...]:
        self.selector_kind(session_id)
        return ("--resume", session_id)

    def continue_args(self) -> tuple[str, ...]:
        return ("--continue",)

    def fork_args(self, session_id: str) -> tuple[str, ...]:
        return self.resume_args(session_id) + ("--fork-session",)

    def fork_continue_args(self) -> tuple[str, ...]:
        return ("--continue", "--fork-session")


MOD = Claude()
