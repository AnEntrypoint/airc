from __future__ import annotations

import glob
import json
import os

from harness import Channel, Harness

BUSY_MARKER = "esc to interrupt"
PROMPT_MARKER = "Ask Codex to do anything"
ONBOARDING_MARKER = "to get started, describe a task"
HOME = os.path.join(os.path.expanduser("~"), ".codex")


class CodexChannel(Channel):
    def __init__(self, cwd: str, since: float, at_end: bool = False):
        super().__init__(cwd, since, at_end)
        self.rollouts = os.path.join(os.environ.get("CODEX_HOME") or HOME, "sessions")

    def candidates(self) -> list[str]:
        pattern = os.path.join(self.rollouts, "*", "*", "*", "rollout-*.jsonl")
        return sorted(p for p in glob.glob(pattern) if self.newest([p], self.since))

    def session_cwd(self, path: str) -> str | None:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                meta = json.loads(f.readline())
        except (OSError, ValueError):
            return None
        return (meta.get("payload") or {}).get("cwd")

    def bind(self) -> bool:
        candidates = self.candidates()
        for path in reversed(candidates):
            if self.session_cwd(path) == self.cwd:
                return self._bind(path)
        return False

    def poll(self, kinds=("text", "tool")):
        if not self.bind():
            return
        want = set(kinds) | {"turn_end"}
        for record in self._tail_records():
            rtype = record.get("type")
            payload = record.get("payload") or {}
            ptype = payload.get("type")
            if rtype == "event_msg":
                if ptype == "task_complete" and "turn_end" in want:
                    yield {"kind": "turn_end"}
                continue
            if rtype != "response_item":
                continue
            if ptype == "message":
                role = payload.get("role")
                text = "".join(
                    block.get("text", "") for block in (payload.get("content") or [])
                    if isinstance(block, dict))
                if role == "assistant":
                    if "text" not in want or not text:
                        continue
                    mid = payload.get("id") or str(record.get("ordinal"))
                    piece = self.delta(mid, text, self.text_len)
                    if piece:
                        yield {"kind": "text", "text": piece}
                elif role == "user" and text.strip() and "you" in want:
                    yield {"kind": "you", "text": text}
            elif ptype == "reasoning" and "thinking" in want:
                text = "".join(
                    block.get("text", "") for block in (payload.get("summary") or [])
                    if isinstance(block, dict))
                if text:
                    yield {"kind": "thinking", "text": text}
            elif ptype == "function_call" and "tool" in want:
                yield {"kind": "tool", "text": payload.get("name", ""), "input": payload.get("arguments")}
            elif ptype == "function_call_output" and "result" in want:
                yield {"kind": "result", "text": str(payload.get("output"))[:4000]}


class Codex(Harness):
    name = "codex"
    argv = ("codex",)
    env_strip = ()

    def pane_busy(self, pane: str) -> bool:
        return BUSY_MARKER in pane

    def pane_ready(self, pane: str) -> bool:
        return PROMPT_MARKER in pane or ONBOARDING_MARKER in pane or BUSY_MARKER in pane

    def channel(self, cwd: str, since: float, at_end: bool = False) -> Channel:
        return CodexChannel(cwd, since, at_end)


MOD = Codex()
