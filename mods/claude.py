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


def project_dir(cwd: str) -> str:
    return os.path.join(TRANSCRIPT_ROOT, re.sub(r"[^A-Za-z0-9]", "-", os.path.abspath(cwd)))


class ClaudeChannel(Channel):
    def __init__(self, cwd: str, since: float):
        super().__init__(cwd, since)
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
            rtype = record.get("type")
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
            stop = message.get("stop_reason")
            has_tool = any(isinstance(b, dict) and b.get("type") == "tool_use" for b in blocks)
            if rtype == "assistant" and stop in ("end_turn", "stop_sequence", "max_tokens") and not has_tool:
                yield {"kind": "turn_end"}


class Claude(Harness):
    name = "claude"
    argv = ("claude",)
    env_strip = NESTING_VARS

    def pane_busy(self, pane: str) -> bool:
        return BUSY_MARKER in pane

    def pane_ready(self, pane: str) -> bool:
        return PROMPT_GLYPH in pane or BUSY_MARKER in pane or TITLE_MARKER in pane

    def channel(self, cwd: str, since: float) -> Channel:
        return ClaudeChannel(cwd, since)


MOD = Claude()
