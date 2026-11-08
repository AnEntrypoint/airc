#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${CCBRIDGE_BIN:-$ROOT/build/ccbridge}
case "$BIN" in /*) ;; *) BIN=$PWD/$BIN ;; esac
FIX=$ROOT/tests/fixtures/templates
TMP=$(mktemp -d)
export HOME=$TMP/home
mkdir -p "$HOME"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; exit 1; }

PROJ=$TMP/proj; mkdir -p "$PROJ"
OUT=$(CCBRIDGE_TEMPLATES=$FIX "$BIN" send --harness fake --cwd "$PROJ" "hello") || fail "send exited non-zero"
case "$OUT" in *"ECHO: hello"*) ;; *) fail "send output lacks 'ECHO: hello': $OUT" ;; esac

CCBRIDGE_TEMPLATES=$ROOT/templates "$BIN" --templates "$FIX" templates --check >/dev/null || fail "templates --check (fixtures + repo)"

printf 'plain\n\033[31mred\033[0m text\n\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\342\224\200\n' > "$TMP/raw.txt"
VIEW=$("$BIN" analyze "$TMP/raw.txt" --view text) || fail "analyze exited non-zero"
case "$VIEW" in *"red text"*) ;; *) fail "analyze did not strip color: $VIEW" ;; esac
case "$VIEW" in *$'\033'*) fail "analyze left an escape byte" ;; esac
case "$VIEW" in *"─"*) fail "analyze kept the rule line" ;; esac

echo PASS
