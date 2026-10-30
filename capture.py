#!/usr/bin/env python3
from __future__ import annotations

import argparse
import fcntl
import json
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import time

import analyze
import mods
from harness import PASTE_CLOSE, PASTE_OPEN

BANNED = {"--bare", "--api-key", "--settings", "-p", "--print", "--safe-mode", "--restricted"}
MARKER_TAIL_LINES = 60


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def marker_report(raw: bytes, harness) -> str:
    text = analyze.strip_ansi(raw)
    lines = text.splitlines()[-MARKER_TAIL_LINES:]
    ready = [ln for ln in lines if harness.pane_ready(ln)]
    busy = [ln for ln in lines if harness.pane_busy(ln)]
    return (f"harness={harness.name} ready={bool(ready)} busy={bool(busy)}\n"
            f"ready_lines={ready[:4]}\nbusy_lines={busy[:4]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--harness", default=mods.DEFAULT, choices=mods.names())
    ap.add_argument("--out", required=True)
    ap.add_argument("--cols", type=int, default=120)
    ap.add_argument("--rows", type=int, default=45)
    ap.add_argument("--seq", default=None)
    ap.add_argument("--seq-file", default=None)
    ap.add_argument("--startup-delay", type=float, default=5.0)
    ap.add_argument("--gap", type=float, default=5.0)
    ap.add_argument("--min-turn", type=float, default=6.0)
    ap.add_argument("--first-turn-grace", type=float, default=2.5)
    ap.add_argument("--final-idle", type=float, default=15.0)
    ap.add_argument("--max-seconds", type=float, default=300.0)
    ap.add_argument("--dump-markers", action="store_true")
    ap.add_argument("--env-mode", choices=("nesting-stripped", "verbatim"), default="nesting-stripped")
    ap.add_argument("args", nargs=argparse.REMAINDER)
    a = ap.parse_args()

    harness = mods.load(a.harness)
    seq = json.loads(a.seq) if a.seq else []
    if a.seq_file:
        seq = json.load(open(a.seq_file))
    extra = [x for x in a.args if x != "--"]
    banned = sorted(x for x in extra if x in BANNED)
    if banned:
        sys.exit(f"refusing {banned}: would not be an unmodified interactive harness")

    env = dict(os.environ) if a.env_mode == "verbatim" else harness.env(dict(os.environ))
    command = [*harness.argv, *extra]

    master, slave = pty.openpty()
    set_winsize(master, a.rows, a.cols)
    started = time.time()
    proc = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave,
                            env=env, cwd=os.getcwd(), start_new_session=True)
    os.close(slave)

    out_path = os.path.abspath(a.out)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    raw_bytes = bytearray()
    raw = open(out_path, "wb")

    index = 0
    sent_at = None
    last_out = time.time()
    log = []

    def note(message):
        sys.stderr.write(f"[tty {harness.name} t={time.time() - started:6.1f}] {message}\n")
        sys.stderr.flush()

    try:
        while True:
            now = time.time()
            if now - started > a.max_seconds:
                note("max-seconds hit")
                break
            if index < len(seq) and now - started >= a.startup_delay:
                if sent_at is None:
                    ready = True
                else:
                    ready = (now - last_out >= a.gap) and (now - sent_at >= a.min_turn)
                if ready:
                    item = seq[index]
                    keys = item if isinstance(item, str) else item.get("keys", "")
                    submit = item.get("submit", True) if isinstance(item, dict) else True
                    os.write(master, PASTE_OPEN.encode())
                    os.write(master, keys.encode())
                    os.write(master, PASTE_CLOSE.encode())
                    time.sleep(0.15)
                    if submit:
                        os.write(master, b"\r")
                    sent_at = time.time()
                    last_out = time.time()
                    note(f"send[{index}] {keys[:60]!r}")
                    log.append({"i": index, "t": round(sent_at - started, 2), "keys": keys})
                    index += 1
                    continue
            if index >= len(seq) and seq and sent_at is not None:
                if now - last_out >= a.final_idle and now - sent_at >= a.min_turn:
                    note(f"final idle {now - last_out:.1f}s -> quit")
                    break
            readable, _, _ = select.select([master], [], [], 0.2)
            if readable:
                try:
                    data = os.read(master, 65536)
                except OSError:
                    break
                if not data:
                    break
                raw.write(data)
                raw.flush()
                raw_bytes.extend(data)
                last_out = time.time()
            elif proc.poll() is not None:
                time.sleep(0.2)
                try:
                    while True:
                        data = os.read(master, 65536)
                        if not data:
                            break
                        raw.write(data)
                        raw_bytes.extend(data)
                except OSError:
                    pass
                break
    finally:
        try:
            os.write(master, PASTE_CLOSE.encode())
            os.write(master, b"\x03")
            time.sleep(0.3)
            os.write(master, b"/exit\r")
            time.sleep(0.6)
        except OSError:
            pass
        if proc.poll() is None:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
                time.sleep(0.5)
            except OSError:
                pass
        if proc.poll() is None:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except OSError:
                pass
        raw.close()
        os.close(master)

    note(f"captured {len(raw_bytes)} bytes -> {out_path}")
    with open(out_path + ".timing.json", "w") as f:
        json.dump(log, f, indent=2)
    if a.dump_markers:
        print(marker_report(bytes(raw_bytes), harness))


if __name__ == "__main__":
    main()
