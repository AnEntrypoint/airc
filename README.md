# ccbridge

Text in, text out, over a WebSocket, for interactive coding agents. A client sends a prompt as a text frame. The server types it into the agent's own terminal and returns only what the agent says, as it says it: no banner, no spinner, no prompt box, no scroll loss.

    client -> server   : a text frame is a prompt typed into the agent
    server -> client   : a text frame is a chunk of the agent's output
                         an empty text frame means the turn finished

Three agents ship as templates: claude, codex and freebuff. Anything else is a template file you can add without rebuilding.

## Why it is built this way

The interactive TUIs are fullscreen apps that repaint a diff of the screen: most bytes are spinner repaints, words are placed by cursor moves, and there is no scrollback. So the screen is a bad output channel.

| concern | choice | why |
|---|---|---|
| input | bracketed paste and Enter into a PTY that ccbridge owns | the agent is a normal interactive session, exactly as a human runs it |
| output | the harness's own transcript or store | no display artifacts, no lost spaces, no scroll loss |
| busy and ready | the rendered screen (libvterm) matched against the template's markers | cheap and reliable status, per harness |

## Build

CMake 3.21 or newer, a C++20 compiler, and the packages in `vcpkg.json`: Boost (Beast and Asio), nlohmann-json, CLI11 and sqlite3. libvterm 0.3.3 is vendored under `third_party/libvterm` and built by CMake. GitHub Actions builds six targets (Linux, macOS and Windows, on x64 and arm64) and runs the end-to-end test on Linux and macOS.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Use

```bash
./build/ccbridge serve --host 127.0.0.1 --port 8787    # browser client at http://127.0.0.1:8787/
./build/ccbridge client --harness codex "list the files here"
./build/ccbridge send --harness freebuff --cwd DIR "hi"  # no server, just the engine
./build/ccbridge templates --check
```

Options and query parameters are listed in AGENTS.md (Protocol). In short: `session` names the conversation, `harness` picks the agent when it is created, `mode=tagged` prefixes each chunk with its kind, `resume`, `continue` and `fork` reattach to an earlier agent conversation.

## Harnesses

Each harness is a JSON file in `templates/`. The search path is `--templates DIR`, then `$CCBRIDGE_TEMPLATES`, then `share/ccbridge/templates` next to the binary, then `./templates`. `templates/README.md` documents every field. The fact table (markers, transcript locations, record shapes, turn ends, resume forms) is in AGENTS.md.

## Requirements

- The agent CLIs (`claude`, `codex`, `freebuff`) on the PATH and signed in the way a human signs in. ccbridge never injects keys.
- A PTY: forkpty on Linux and macOS; ConPTY (Windows 10 1809 or newer) on Windows. tmux is not needed.
- Windows: the runtime is compiled and linked in CI but has not yet been run end to end.

## Files

| file | role |
|---|---|
| `src/main.cpp` | the `ccbridge` command line |
| `src/common/pty.*`, `screen.*` | the agent's PTY and its libvterm screen |
| `src/harness/` | channel, transcript kinds, harness markers and launch forms, template registry |
| `src/session/agent_session.*` | one agent in a PTY: start, send, events |
| `src/server/server.*` | WebSocket and HTTP server with named hubs |
| `src/tools/` | `send`, `client`, `capture` (PTY capture instrument), `analyze` |
| `templates/` | harness definitions |
| `web/index.html` | browser client |
| `tests/e2e.sh` | end-to-end test with a fake harness |

## Limits

- The end-to-end test uses a fake harness. Runs against the real claude, codex and freebuff CLIs are not in CI.
- The Windows runtime has not been run yet.
- A transcript format that no built-in kind reads needs C++; templates only reuse the three built-in kinds.
