# ccbridge harness templates

A template is one JSON file that teaches ccbridge how to drive an agent. Add one without rebuilding.

- File name = harness name: lowercase letters, digits, dashes (`my-agent.json` -> `--harness my-agent`).
- Search order, first match wins: the `--templates DIR` option, `$CCBRIDGE_TEMPLATES`, `<resource root>/templates`, `./templates`.
- Resource root: `$CCBRIDGE_HOME`, else `<exe dir>/../share/ccbridge`, else the directories above the exe that hold a `templates` dir.
- Check your files: `ccbridge --templates DIR templates --check`.

## Fields

| field | meaning |
|---|---|
| `argv` | launch command as a list, run in a PTY owned by ccbridge |
| `argv_env` | rules that add args depending on the environment (below) |
| `env_strip` | env vars removed at launch (`env -u`) so the child is a top-level session, not a nested one |
| `term` | value for `TERM` in the screen |
| `submit_delay` | seconds between the pasted prompt and Enter |
| `ready` | strings; any one on the screen means the agent accepts input |
| `busy` | strings; any one on the screen means a turn is running |
| `resume` | extra args to resume a session; `{session}` is the session id |
| `continue` | extra args to continue the latest session |
| `fork` | extra args to fork a session; `{session}` is the id |
| `fork_continue` | extra args to fork the latest session |
| `default` | true marks the harness used when none is named |
| `transcript` | where the clean output is read from (below) |

`argv_env` is a list of rules like `{"env": "NAME", "equals": "1", "append": ["--flag", "$VALUE"]}`. When `$NAME` is set and
equals `equals` (any set value if `equals` is omitted), the `append` args are added to `argv`; `$VALUE` expands to the
variable's value.

Never add flags that make an agent non-interactive or key-based (`-p`, `--print`, `--bare`, `--api-key`): the session
must authenticate exactly as a human's does.

## transcript

`{"kind": "...", "root": "{home}/...", "root_env": "NAME", "notices": [...]}`

- `kind`: a built-in reader (below).
- `root`: store directory; `{home}` expands to the user's home.
- `root_env`: env var that overrides `root` when set (for example `CODEX_HOME`).
- `notices`: strings in the output reported as notices rather than text.

Output is read from the agent's own transcript, never scraped from the screen. Built-in kinds:

| kind | reads |
|---|---|
| `claude-jsonl` | `<root>/projects/<slug(cwd)>/*.jsonl`; assistant/user records, text blocks stitched by `apiBlockIndex` within `message.id`; `stop_reason` ends a turn |
| `codex-jsonl` | `<root>/sessions/YYYY/MM/DD/rollout-*.jsonl`; `response_item` assistant `output_text`; `task_complete` ends a turn; `session_meta.payload.cwd` selects the session |
| `freebuff-json` | `<root>/projects/<basename(cwd)>/chats/<id>/chat-messages.json`; array of `{variant: user or ai, blocks}`; `isComplete` ends a turn |

## Example: a new agent that reuses a kind

An agent that stores chats in freebuff's format under its own home:

```json
{
  "argv": ["myagent", "--tui"],
  "env_strip": ["MYAGENT_SESSION"],
  "term": "xterm-256color",
  "submit_delay": 0.3,
  "ready": ["Type a task"],
  "busy": ["Thinking"],
  "transcript": {"kind": "freebuff-json", "root": "{home}/.config/myagent"},
  "continue": ["--continue"],
  "resume": ["--continue", "{session}"]
}
```

Save it as `my-agent.json` in any search directory, run `ccbridge serve` and open `/?harness=my-agent`. Measure the
ready and busy strings with `ccbridge capture --harness my-agent --seq '["hi"]' --out runs/x.raw --dump-markers`.

## What templates cannot add

A transcript format the binary does not know. A new on-disk format needs a new built-in kind in the binary (one reader
that yields text, thinking, tool, result, you and turn_end events); everything else is a template.
