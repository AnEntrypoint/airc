# Agent instructions for airc

ccbridge drives an unmodified interactive coding agent in a pseudo-terminal: text in over a WebSocket, plain text out, no display artifacts. Only the harness templates are harness-specific; the PTY, screen, session, server and tools are not.

## Invariants

- Never pass a flag that makes a harness non-interactive or key-based: `-p`, `--print`, `--bare`, `--api-key`, `--settings`, `--safe-mode`, `--restricted`. The session must authenticate exactly as a human's does. `ccbridge capture` refuses them. Server launch args come only from a template's `resume`, `continue`, `fork` and `fork_continue` forms, never from client input.
- Output is read from the harness's own transcript or store, never by scraping the screen. The screen is used for ready and busy markers, freebuff notice lines, and the `pane_tail` field of `GET /sessions`.
- A harness launches with its `env_strip` variables removed from the child environment, so a harness started inside another session runs as a normal top-level session.
- Max-plan / OAuth is never bypassed: no injected API key, base URL or auth token.
- No comments in source: intent lives in names, durable facts live here.

## Layout

| path | role |
|---|---|
| `CMakeLists.txt`, `vcpkg.json` | target `ccbridge`; Boost.Beast/Asio, nlohmann-json, CLI11, sqlite3; libvterm built from source |
| `src/main.cpp` | CLI: `serve`, `send`, `client`, `capture`, `analyze`, `templates` |
| `src/common/` | resource and template paths, text, time, `pty` (PTY spawn and I/O), `screen` (libvterm) |
| `src/harness/` | `event` (kinds), `channel` (incremental transcript reader), `transcript` (kind factory), `kinds/` (one file per transcript kind), `harness` (markers, env, paste, launch forms), `registry` (template loader) |
| `src/session/agent_session.*` | one PTY-hosted agent: start, send, events, ready and busy |
| `src/server/server.*` | WebSocket and HTTP server, hubs, reaper |
| `src/tools/` | `send`, `client`, `capture`, `analyze` |
| `templates/*.json` | harness definitions, read at run time (schema in `templates/README.md`) |
| `web/index.html` | browser client served at `/` |
| `third_party/libvterm/` | vendored libvterm 0.3.3 (MIT), unmodified |
| `tests/e2e.sh`, `tests/fixtures/` | end-to-end test with a fake freebuff-kind harness |
| `tests/unit/claude_channel_test.cpp` | target `claude_channel_test` (EXCLUDE_FROM_ALL, `ctest`): claude-jsonl reader polled across records; text stitched per message across polls |
| `.github/workflows/ci.yml` | six-target build; e2e on Linux and macOS; release on main |

## Harnesses

A harness is a template, `templates/<name>.json`. Templates are read at run time from the search path: `--templates DIR`, `$CCBRIDGE_TEMPLATES`, `<resource root>/templates`, `./templates`. The built-in transcript kinds are `claude-jsonl`, `codex-jsonl` and `freebuff-json`.

| fact | claude | codex | freebuff |
|---|---|---|---|
| ready markers (any) | `❯`, `esc to interrupt`, `Claude Code` | `Ask Codex to do anything`, `to get started, describe a task`, `esc to interrupt` | `Enter a coding task`, `for history · ? for help`, `Starting your model session` |
| busy marker | `esc to interrupt` | `esc to interrupt` | `Starting your model session` |
| transcript | `~/.claude/projects/<slug(cwd)>/*.jsonl` | `$CODEX_HOME/sessions/YYYY/MM/DD/rollout-*.jsonl` plus `state_*.sqlite` table `threads` | `$FREEBUFF_HOME` or `~/.config/manicode`, then `projects/<basename(cwd)>/chats/<id>/chat-messages.json` |
| record shape | `assistant`/`user` records; `message.content` blocks; text stitched by `apiBlockIndex` within `message.id`, accumulated across polls and emitted as deltas of the stitched text | `response_item` payloads: `message` (assistant and user text), `reasoning` (summary), `function_call`, `function_call_output`; `session_meta.payload.cwd` selects the session | JSON array of `{variant: user or ai, blocks: [{type, content, textType?}], isComplete?, userError?}` |
| turn end | `system` record with `subtype: turn_duration` | `event_msg` with `payload.type: task_complete` | `isComplete`; a `userError` is sent as a result first; a notice line on the screen (`Not enough hours`, `Choose another model`, `not available on this connection`) also ends the turn |
| resume | `--resume <id>` | `resume <id>` | `--continue <id>` |
| continue | `--continue` | `resume --last` | `--continue` |
| fork | `--resume <id> --fork-session`, `--continue --fork-session` | `fork <id>`, `fork --last` | none |
| selector | absolute `.jsonl` path, session UUID, or name | session UUID or name | chat directory id |

Binding: claude and codex bind an exact session id or path. An unknown non-UUID name binds the newest store for the cwd at its end. A UUID not found in the cwd is looked up across the whole store, and if that also fails the attach is refused; there is no silent fallback. Freebuff accepts chat ids only.

Channel: records whose timestamp is more than 3 s before the hub started are skipped. A file is bound only if its mtime is within 3 s of the start. A resumed hub reads its file from the end. Results are cut to 4000 code points.

## Protocol

`GET /ws` upgrades to WebSocket. Query parameters:

- `session`: named hub; reconnect with the same name to keep talking. Default `default`.
- `harness`: template name, fixed when the hub is created. Default: the template with `"default": true`.
- `cwd`: working directory, new hub only. Default `CCBRIDGE_CWD`, else `<process cwd>/.ccbridge/scratch`.
- `mode`: `plain` (default) or `tagged`, per client.
- `kinds`: comma list, default `text,tool`. Unknown names are ignored.
- `resume=<id>`: new hub only; launches the harness's resume form.
- `continue=1`: new hub only; continue form. `resume` and `continue` are exclusive.
- `fork=1`: with `resume` or `continue`, uses the fork form.

Frames:

- Client to server: a text frame is a prompt. Blank frames are ignored.
- Server to client, `plain`: text as it is produced; an empty frame ends the turn.
- Server to client, `tagged`: `t:` text, `k:` thinking, `u:` tool, `r:` result, `y:` you, `e:` turn end.
- A client attaching mid-turn receives only events produced after it attaches. Nothing is replayed.
- When the agent process exits, every client receives an empty frame.

HTTP:

- `GET /` serves the browser client.
- `GET /sessions` lists hubs with `clients`, `alive`, `harness`, `cwd`, `busy`, `launch_args` and `pane_tail` (the last 12 non-blank screen lines).
- `DELETE /sessions/{name}` stops a hub and its agent process.

Lifecycle:

- A hub whose agent is not alive is replaced on the next attach. An alive hub asked for different launch args is refused; stop it first.
- A hub with no clients and no activity for `CCBRIDGE_IDLE_REAP` seconds (default 1800) is stopped. The reaper runs every 60 s.
- Limits: 16 MiB per inbound frame; WebSocket idle timeout 60 s with keep-alive pings.

## Run

Builds and tests run in GitHub Actions (`.github/workflows/ci.yml`). Locally, the same steps are:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ccbridge serve --host 127.0.0.1 --port 8787
./build/ccbridge client --harness codex "hi"
./build/ccbridge send --harness freebuff --cwd DIR "hi"
./build/ccbridge capture --harness freebuff --seq '["hi"]' --out runs/fb.raw --dump-markers
./build/ccbridge analyze runs/fb.raw --view text --tail 40
./build/ccbridge templates --check
cmake --build build --target claude_channel_test && ./build/claude_channel_test
CCBRIDGE_BIN=build/ccbridge bash tests/e2e.sh
```

Adding an agent that uses an existing transcript kind needs only a new `templates/<name>.json` (fields in `templates/README.md`); no rebuild. A new transcript format needs a kind in `src/harness/kinds/`, registered in `src/harness/transcript.cpp`.

## gm state

The append-only PRD log `.gm/prd-log.jsonl`, its row shards `.gm/prd-log.d/` and `.gm/next-step.md` are authored project state and are not ignored. PRD row folders `.gm/prd/` and `.gm/unknowns/`, spools, caches, databases, counters, locks, dream-rsi scratch, browser captures, `.agentplug/`, `.agentplug-kv/` and core dumps are transient and ignored. `.env` and `.env.local` stay out of the repo.
