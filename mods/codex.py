from __future__ import annotations

import glob
import json
import os
import re
import sqlite3

from harness import Channel, Harness

BUSY_MARKER = "esc to interrupt"
PROMPT_MARKER = "Ask Codex to do anything"
ONBOARDING_MARKER = "to get started, describe a task"
HOME = os.path.join(os.path.expanduser("~"), ".codex")
UUID_RE = re.compile(r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")
SESSION_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 ._-]{0,127}$")


def codex_home() -> str:
    return os.environ.get("CODEX_HOME") or HOME


def rollout_paths() -> list[str]:
    return sorted(glob.glob(os.path.join(codex_home(), "sessions", "*", "*", "*",
                                         "rollout-*.jsonl")))


def session_meta(path: str) -> dict:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return json.loads(f.readline()).get("payload") or {}
    except (OSError, ValueError):
        return {}


def session_id_of(path: str) -> str:
    meta = session_meta(path)
    if meta.get("id"):
        return str(meta["id"])
    match = UUID_RE.search(os.path.basename(path))
    if match:
        return match.group(0)
    return os.path.basename(path)[:-len(".jsonl")]


def thread_dbs() -> list[str]:
    paths = glob.glob(os.path.join(codex_home(), "state_*.sqlite"))

    def version(path: str) -> int:
        match = re.search(r"state_(\d+)", os.path.basename(path))
        return int(match.group(1)) if match else -1

    return sorted(paths, key=version, reverse=True)


def db_sessions(cwd: str) -> list[dict]:
    found: dict[str, dict] = {}
    for db in thread_dbs():
        try:
            con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
            rows = con.execute(
                "select id, rollout_path, coalesce(updated_at_ms, created_at_ms, 0), "
                "title, name, first_user_message, preview from threads "
                "where cwd = ? and coalesce(archived, 0) = 0 "
                "and coalesce(has_user_event, 1) = 1", (os.path.abspath(cwd),)).fetchall()
            con.close()
        except (sqlite3.Error, OSError, ValueError):
            continue
        for sid, rollout, updated_ms, title, name, first, preview in rows:
            size = 0
            try:
                if rollout:
                    size = os.stat(rollout).st_size
            except OSError:
                pass
            found[str(sid)] = {"session_id": str(sid), "path": rollout or "",
                               "mtime": (updated_ms or 0) / 1000.0, "size": size,
                               "title": title or name or first or preview or ""}
    return list(found.values())


def rollout_sessions(cwd: str) -> list[dict]:
    rows = []
    for path in rollout_paths():
        if session_meta(path).get("cwd") != os.path.abspath(cwd):
            continue
        try:
            stat = os.stat(path)
        except OSError:
            continue
        rows.append({"session_id": session_id_of(path), "path": path,
                     "mtime": stat.st_mtime, "size": stat.st_size})
    return rows


def sessions(cwd: str) -> list[dict]:
    rows = db_sessions(cwd)
    seen = {row["session_id"] for row in rows}
    for row in rollout_sessions(cwd):
        if row["session_id"] not in seen:
            rows.append(row)
            seen.add(row["session_id"])
    return sorted(rows, key=lambda row: row["mtime"])


class CodexChannel(Channel):
    def __init__(self, cwd: str, since: float, at_end: bool = False,
                 session_id: str | None = None):
        super().__init__(cwd, since, at_end, session_id)
        self.rollouts = os.path.join(codex_home(), "sessions")

    def all_rollouts(self) -> list[str]:
        return rollout_paths()

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
        if self.session_id:
            for path in self.all_rollouts():
                if session_id_of(path) == self.session_id:
                    return self._bind(path)
            return False
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

    def channel(self, cwd: str, since: float, at_end: bool = False,
                session_id: str | None = None) -> Channel:
        return CodexChannel(cwd, since, at_end, session_id)

    def sessions(self, cwd: str) -> list[dict]:
        return sessions(cwd)

    def resume_args(self, session_id: str) -> tuple[str, ...]:
        if not SESSION_RE.match(session_id):
            raise ValueError(f"bad session id {session_id!r}")
        return ("resume", session_id)

    def continue_args(self) -> tuple[str, ...]:
        return ("resume", "--last")


MOD = Codex()
