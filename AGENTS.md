# Agent instructions for airc

ccbridge drives an unmodified interactive coding agent in a tmux pane: text in over a
WebSocket, plain text out, no display artifacts. Only the harness adapters are
harness-specific; the session, server and capture instrument are not.

## Invariants

- Never pass a flag that makes a harness non-interactive or key-based (`-p`, `--print`,
  `--bare`, `--api-key`, `--settings`, `--safe-mode`, `--restricted`): the session must
  authenticate exactly as a human's does. `server.py` and `capture.py` refuse them.
- Output is read from the harness's own transcript/store, never by scraping the TUI: the
  TUIs are alternate-screen with no scrollback and repaint diffs, so screen text loses
  spacing and anything past one screenful. The pane is used only for ready/busy.
- A harness launches with the vars that mark a *nested* session of itself dropped
  (`env -u <VARS> <argv>`), so the child is a normal top-level session.
- No comments in source: intent lives in names, durable facts live here.

## Harnesses (mods/)

`harness.py` defines the `Harness` interface and `Channel` (incremental transcript
reader). `mods/__init__.py` registers the mods; `mods.load(name)` resolves one.
A mod answers exactly four questions:

| fact | member |
|---|---|
| how to launch | `argv`, `env_strip`, `term` |
| read the window | `pane_ready(pane)`, `pane_busy(pane)` |
| type a prompt | `send(target, text)` (default: bracketed paste + Enter; `submit_delay` before Enter) |
| clean output | `channel(cwd, since)` -> `Channel.poll(kinds)` yielding `text|thinking|tool|result|you|turn_end` |

Measured facts per harness (source: live panes and stores on this machine):

| harness | ready marker | busy marker | transcript |
|---|---|---|---|
| claude | `❯` or `Claude Code` | `esc to interrupt` | `~/.claude/projects/<slug(cwd)>/*.jsonl`; `assistant`/`user` records, text blocks stitched by `apiBlockIndex` within `message.id`; `stop_reason` ends the turn |
| codex | `Ask Codex to do anything` (or `to get started, describe a task` onboarding) | `esc to interrupt` (shown as `• Working (Ns • esc to interrupt)`) | `$CODEX_HOME/sessions/YYYY/MM/DD/rollout-*.jsonl` (codex-cli 0.166.0); `response_item` payload `message` role `assistant` content `output_text`; `event_msg` `task_complete` ends the turn; `session_meta.payload.cwd` selects the session |
| freebuff | `Enter a coding task` or `for history · ? for help` | `Starting your model session` | `~/.config/manicode/projects/<basename(cwd)>/chats/<ISO>/chat-messages.json`; array of `{variant: user|ai, blocks:[{type:text, content, textType?}]}`; `isComplete`/`userError` end the turn |

- freebuff's config home is `~/.config/manicode` (the package is `freebuff`, the binary
  `~/.config/manicode/freebuff`). `projects/<name>` keys on the cwd's basename, not a slug.
- codex's store is JSONL under `$CODEX_HOME` (default `~/.codex`); its SQLite files
  (`state_*.sqlite`, `queue_*.sqlite`) are internal.
- Adding a harness: create `mods/<name>.py` exposing a `Harness` instance as `MOD`,
  register it in `mods/__init__.py`, and measure its four facts with
  `python3 capture.py --harness <name> --seq '["hi"]' --out runs/<name>.raw --dump-markers`.

## Files

| file | role |
|---|---|
| `harness.py` | `Harness` interface, `Channel` reader, tmux wrapper, bracketed-paste send |
| `mods/claude.py`, `mods/codex.py`, `mods/freebuff.py` | one adapter each |
| `ccsession.py` | `AgentSession(cwd, harness=...)` (start/send/events/busy/ready); `ClaudeSession` shim |
| `server.py` | aiohttp WebSocket server, hubs keyed by session name, per-hub harness |
| `capture.py` | PTY capture instrument for any harness (raw bytes + marker report) |
| `analyze.py` | ANSI stripper / transcript derivation for raw captures |
| `wstest.py` | CLI WebSocket client |

## Run

```bash
python3 server.py --host 127.0.0.1 --port 8787          # http://127.0.0.1:8787/?harness=codex
python3 ccsession.py --harness codex --cwd DIR "prompt"
python3 capture.py --harness freebuff --seq '["hi"]' --out runs/fb.raw --dump-markers
```

`/ws` query params: `session` (named hub, reconnectable), `harness` (claude|codex|freebuff,
fixed at hub creation), `cwd` (new hub only), `mode=plain|tagged`, `kinds=text,tool`.
`GET /sessions` lists live hubs with their harness.

## Constraints

- Max-plan / OAuth is never bypassed: no injected API key, base URL or auth token.
- A client attaching mid-turn sees only later events (the hub polls once, no replay).
- Idle hubs with no clients are reaped after `CCBRIDGE_IDLE_REAP` seconds (1800).
- Transcript paths and record shapes are undocumented harness internals; keep them
  isolated in `mods/<name>.py`, one `Channel` per harness.
