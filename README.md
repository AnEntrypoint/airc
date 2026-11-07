# ccbridge — text-in / text-out over WebSocket for any interactive coding harness

Ask a coding agent something over a WebSocket and get **only what it says** back, as it
says it. No banner, no status line, no spinner, no prompt box, no scroll loss.

    client -> server   : a UTF-8 text frame is a prompt typed into the harness
    server -> client   : a UTF-8 text frame is a chunk of the harness's output
                         a ZERO-LENGTH frame means "turn finished"

    ws = websocket.connect("ws://127.0.0.1:8787/ws?session=main&harness=codex")
    ws.send("list the files here")
    while True:
        frame = ws.recv()
        if frame == "": break          # turn over — no parsing needed at all
        print(frame, end="")

## Harnesses (mods)

The engine is harness-agnostic. A *mod* in `mods/` answers four questions about one
harness; everything else (session, tmux, server, capture instrument) is shared.

| mod | launch | ready / busy markers | clean output |
|---|---|---|---|
| `claude` | `env -u <nesting vars> claude` | `❯` | `Claude Code` / `esc to interrupt` | `~/.claude/projects/<slug(cwd)>/*.jsonl` |
| `codex` | `codex` | `Ask Codex to do anything` / `esc to interrupt` | `$CODEX_HOME/sessions/**/rollout-*.jsonl` |
| `freebuff` | `freebuff` | `Enter a coding task` / `Starting your model session` | `~/.config/manicode/projects/<basename(cwd)>/chats/*/chat-messages.json` |

Adding one: create `mods/<name>.py` with a `Harness` instance named `MOD`, register it in
`mods/__init__.py`, and measure its markers with
`python3 capture.py --harness <name> --seq '["hi"]' --out runs/<name>.raw --dump-markers`.

## Why it is built this way

The interactive TUIs are fullscreen alternate-screen apps that repaint a *diff*: most
bytes are spinner repaints, words are placed by cursor moves (ANSI-stripping loses
spaces), and there is no scrollback. So the TUI is a bad output channel.

| concern | choice | why |
|---|---|---|
| **input** | `tmux send-keys` (bracketed paste + Enter) into a real PTY | the agent is a normal interactive session, exactly as a human runs it |
| **output** | the harness's own transcript/store | zero display artifacts, no scroll loss, no space loss |
| **busy/idle** | `capture-pane` matched against the mod's markers | cheap, reliable status; per-harness |

## Files

| file | role |
|---|---|
| `harness.py` | `Harness` interface, `Channel` transcript reader, tmux wrapper, paste+submit |
| `mods/{claude,codex,freebuff}.py` | one adapter per harness |
| `ccsession.py` | `AgentSession(cwd, harness=…)`: start, `send(text)`, `events()`; `ClaudeSession` shim |
| `server.py` | aiohttp WebSocket server + tiny browser client at `/` |
| `wstest.py` | CLI WebSocket client for poking the server |
| `capture.py` | raw-PTY capture harness (instrument that measures a harness's window) |
| `analyze.py` | ANSI stripper / transcript derivation for the raw captures |
| `runs/` | captures and server logs |

## Usage

    python3 server.py --host 127.0.0.1 --port 8787   # open http://127.0.0.1:8787/?harness=codex
    python3 wstest.py --harness codex "hi"           # drive it from the CLI
    python3 ccsession.py --harness freebuff --cwd DIR "hi"   # no server, just the engine

### `/ws` query params

| param | meaning |
|---|---|
| `session=<name>` | attach to or create a named conversation; reconnect later to keep talking |
| `harness=claude\|codex\|freebuff` | which agent a *newly created* session runs (default `claude`); fixed at creation |
| `cwd=<path>` | working directory for a *newly created* session (fixed at creation) |
| `mode=plain` (default) | frames are the agent's text verbatim; empty frame = turn end |
| `mode=tagged` | frames are `<c>:<payload>`: `t` text, `k` thinking, `u` tool use, `r` result, `y` you, `e` turn end |
| `kinds=text,tool` | classes a new session captures (default `text,tool`) |
| `resume=<session-id>` | new hub only: resume that conversation (`claude --resume <id>`, `codex resume <id>`, `freebuff --continue <id>`) |
| `continue=1` | new hub only: continue the harness's latest conversation (`claude --continue`, `codex resume --last`, `freebuff --continue`) |
| `fork=1` | with `resume` or `continue`: fork into a new session id (`claude --fork-session`, `codex fork`) |

`GET /sessions` lists live hubs with their harness and launch args; `GET /` is a minimal browser client.
A resumed hub binds to the requested session and reads its store from the end, so it does not
replay the saved conversation (freebuff is seeded with the old message ids).

## Constraints this respects

* **Unmodified interactive harness.** No flags are passed. The only env adjustment is
  dropping the vars that mark *us* as a nested session of the harness, so the child is an
  ordinary top-level session.
* **Max-plan / OAuth is never bypassed.** Nothing injects an API key, base URL or auth
  token, and `server.py` / `capture.py` *refuse* `--bare`, `--api-key`, `--settings`, `-p`,
  `--print`. The session authenticates exactly as the host's normal CLI does.

## Known limits

* A client attaching **mid-turn** sees only later events (single poller, no replay).
* Idle hubs with no clients are reaped after `CCBRIDGE_IDLE_REAP` seconds (1800).
* Transcript paths and record shapes are undocumented internals of each harness; each is
  isolated in its own mod `Channel`.
