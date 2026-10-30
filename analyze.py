#!/usr/bin/env python3
import argparse
import re
import sys

CSI = re.compile(r"\x1b\[[0-9;:?<=>]*[ -/]*[@-~]")
OSC = re.compile(r"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")
DCS_APC_PM = re.compile(r"\x1b[P_^][^\x1b]*(?:\x1b\\)?")
ESC_SINGLE_CHAR = re.compile(r"\x1b[@-Z\\-_]")
C0_CONTROLS = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")


def strip_ansi(b: bytes) -> str:
    s = b.decode("utf-8", "replace")
    s = OSC.sub("", s)
    s = DCS_APC_PM.sub("", s)
    s = CSI.sub("", s)
    s = ESC_SINGLE_CHAR.sub("", s)
    s = C0_CONTROLS.sub("", s)
    return s


HORIZONTAL_RULE = re.compile(r"^\s*[─═━╌┄┈\-—_]{3,}\s*$")
BAR_GLYPHS = re.compile(r"^\s*[▁▂▃▅▇█▉▊▋▌▍▎▏]+\s*$")
EMPTY_PROMPT = re.compile(r"^\s*❯?\s*$")
MODE_INDICATOR = re.compile(r"^\s*⏵⏵\s")
SPINNER_GLYPHS = re.compile(r"^\s*(✻|✽|✢|✳|✶|·|\*)\s")
INTERRUPT_HINT = re.compile(r"^\s*esc to interrupt")
SHORTCUTS_HINT = re.compile(r"^\s*\? for shortcuts")
CLAUDE_BANNER = re.compile(r"^\s*(Claude Code v|/workspace|hf:|claude-)")
TOKEN_COUNTER = re.compile(r"^\s*tokens?\b")
ELAPSED_TIME = re.compile(r"^\s*[0-9]+m? ?[0-9]*s?\s*$")

CHROME_PATTERNS = (
    HORIZONTAL_RULE,
    BAR_GLYPHS,
    EMPTY_PROMPT,
    MODE_INDICATOR,
    SPINNER_GLYPHS,
    INTERRUPT_HINT,
    SHORTCUTS_HINT,
    CLAUDE_BANNER,
    TOKEN_COUNTER,
    ELAPSED_TIME,
)


def looks_chrome(line: str) -> bool:
    return any(p.search(line) for p in CHROME_PATTERNS)


def last_terminal_rewrite(raw_line: str) -> str:
    return raw_line.split("\r")[-1]


def to_text(stripped: str) -> str:
    lines = [last_terminal_rewrite(raw_line).rstrip() for raw_line in stripped.split("\n")]
    out = []
    prev_blank = False
    for line in lines:
        if looks_chrome(line):
            continue
        if not line.strip():
            if prev_blank:
                continue
            prev_blank = True
        else:
            prev_blank = False
        out.append(line)
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--view", default="both", choices=("strip", "text", "both"))
    ap.add_argument("--tail", type=int, default=0)
    ap.add_argument("--out", default=None, help="write chosen view(s) to files <out>.strip/.text")
    a = ap.parse_args()

    stripped = strip_ansi(open(a.path, "rb").read())
    text = to_text(stripped)

    if a.out:
        open(a.out + ".strip", "w").write(stripped)
        open(a.out + ".text", "w").write(text)
        print(f"wrote {a.out}.strip and {a.out}.text", file=sys.stderr)

    def show(name, s, tail):
        body = "\n".join(s.split("\n")[-tail:]) if tail else s
        print(f"\n{'=' * 30} {name} ({len(s)} chars) {'=' * 30}")
        print(body)

    if a.view in ("strip", "both"):
        show("STRIP", stripped, a.tail)
    if a.view in ("text", "both"):
        show("TEXT", text, a.tail)


if __name__ == "__main__":
    main()
