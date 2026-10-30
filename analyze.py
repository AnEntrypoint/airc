#!/usr/bin/env python3
"""Strip ANSI from a raw PTY capture and derive a readable transcript.

Two views:
  strip  - ANSI removed, control chars collapsed, everything else kept
  text   - 'strip' with TUI chrome removed (borders, prompt glyph, status bar)

Usage: python3 analyze.py runs/r1.raw [--view strip|text|both] [--tail N]
"""
import argparse
import re
import sys
import unicodedata

# ESC [ ... final-byte  (CSI)
CSI = re.compile(r"\x1b\[[0-9;:?<=>]*[ -/]*[@-~]")
# ESC ] ... (BEL | ST)  (OSC)
OSC = re.compile(r"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")
# ESC P ... ST (DCS) ; ESC _ ... ST (APC) ; ESC ^ ... ST (PM)
STR = re.compile(r"\x1b[P_^][^\x1b]*(?:\x1b\\)?")
# lone ESC + one char
ESC1 = re.compile(r"\x1b[@-Z\\-_]")
# other C0 controls except \n \r \t
CTRL = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")


def strip_ansi(b: bytes) -> str:
    s = b.decode("utf-8", "replace")
    s = OSC.sub("", s)
    s = STR.sub("", s)
    s = CSI.sub("", s)
    s = ESC1.sub("", s)
    s = CTRL.sub("", s)
    return s


# Lines that are pure TUI chrome rather than agent output.
CHROME = [
    re.compile(r"^\s*[─═━╌┄┈\-—_]{3,}\s*$"),          # rules / separators
    re.compile(r"^\s*[▁▂▃▅▇█▉▊▋▌▍▎▏]+\s*$"),              # bars
    re.compile(r"^\s*❯?\s*$"),                          # bare prompt
    re.compile(r"^\s*⏵⏵\s"),                             # mode chrome
    re.compile(r"^\s*(✻|✽|✢|✳|✶|·|\*)\s"),               # spinner
    re.compile(r"^\s*esc to interrupt"),
    re.compile(r"^\s*\? for shortcuts"),
    re.compile(r"^\s*(Claude Code v|/workspace|hf:|claude-)"),
    re.compile(r"^\s*tokens?\b"),
    re.compile(r"^\s*[0-9]+m? ?[0-9]*s?\s*$"),
]


def looks_chrome(line: str) -> bool:
    return any(p.search(line) for p in CHROME)


def to_text(stripped: str) -> str:
    # The TUI uses \r to rewrite a line in place; keep the last write.
    lines = []
    for raw_line in stripped.split("\n"):
        parts = raw_line.split("\r")
        line = parts[-1] if len(parts) > 1 else parts[0]
        lines.append(line.rstrip())
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
