#!/usr/bin/env bash
# Drive one scripted session against a given textworld binary in a pristine
# temp directory, so runs are comparable byte-for-byte.
#
#   run.sh <binary> <outdir> [extra env assignments...]
#
# Hermetic by construction: ANTHROPIC_API_KEY is unset and TEXTWORLD_AI=0, so
# no network call is made and stdout is deterministic. Caller-supplied env
# assignments are applied on top (that is how TEXTWORLD_PROFILE /
# TEXTWORLD_LOG_LEVEL are set).
set -u
BIN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
OUT="$2"
shift 2

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
DRIVE="$ROOT/.lore/work/validation/turn-latency-polish/drive.pl"
SCRIPT="$ROOT/.lore/work/validation/turn-latency-polish/session.txt"

mkdir -p "$OUT"
rm -rf "$OUT/run" && mkdir -p "$OUT/run"
cp "$ROOT/world.db" "$OUT/run/" && cp -r "$ROOT/seed" "$OUT/run/"

(
  cd "$OUT/run" || exit 1
  unset ANTHROPIC_API_KEY
  export TEXTWORLD_AI=0
  for kv in "$@"; do export "${kv?}"; done
  perl "$DRIVE" "$SCRIPT" | "$BIN" > stdout.txt 2> stderr.txt
  echo "$?" > exit-code.txt
)
echo "exit=$(cat "$OUT/run/exit-code.txt") stdout=$(wc -c < "$OUT/run/stdout.txt")B stderr=$(wc -c < "$OUT/run/stderr.txt")B"
