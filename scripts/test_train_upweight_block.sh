#!/usr/bin/env bash
# Runs the REAL depth-upweight block from train-glm-adapter.sh (extracted by its
# marker comment, so this can't drift from the shipped code) against a tiny
# scratch corpus, and asserts: CONFIG is repointed at a staged -depth- copy,
# train.jsonl grew, valid.jsonl is byte-identical, the source is untouched,
# and HU_TRAIN_UPWEIGHT_DEPTH=0 leaves CONFIG alone.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

BLOCK="$(awk '/^# --- depth upweighting/{f=1} f{print} f&&/^fi$/{exit}' "$HERE/train-glm-adapter.sh")"
[ -n "$BLOCK" ] || fail "upweight block marker not found in train-glm-adapter.sh"

mkdir -p "$T/corpus"
{
  echo '{"prompt":"Them: hi","completion":"hey"}'
  echo '{"prompt":"Them: I miss you","completion":"miss you too, when are you back?"}'
} > "$T/corpus/train.jsonl"
echo '{"prompt":"Them: yo","completion":"sup"}' > "$T/corpus/valid.jsonl"
printf 'model: x\ndata: %s\n' "$T/corpus" > "$T/corpus/config.yaml"
cp "$T/corpus/train.jsonl" "$T/train.orig"

run_block() {  # $1 = UPWEIGHT_DEPTH value; prints the final CONFIG
  bash -c "
    set -u
    say() { :; }
    die() { echo \"FATAL: \$*\" >&2; exit 1; }
    TRAIN_PY=python3 CONFIG='$T/corpus/config.yaml' STAMP=TEST LOG='$T/log' UPWEIGHT_DEPTH='$1'
    cd '$HERE'
    $BLOCK
    echo \"\$CONFIG\"
  " | tail -n 1   # the block tees its own progress line to stdout
}

cfg="$(run_block 1)"
[ "$cfg" = "$T/corpus-depth-TEST/config.yaml" ] || fail "CONFIG not repointed: $cfg"
grep -q "^data: $T/corpus-depth-TEST\$" "$cfg" || fail "staged config data: line wrong"
[ "$(wc -l < "$T/corpus-depth-TEST/train.jsonl")" -eq 4 ] || fail "expected 4 weighted rows"
cmp -s "$T/corpus/valid.jsonl" "$T/corpus-depth-TEST/valid.jsonl" || fail "valid.jsonl changed"
cmp -s "$T/train.orig" "$T/corpus/train.jsonl" || fail "source train.jsonl mutated"
[ -f "$T/corpus-depth-TEST/train.upweight_stats.json" ] || fail "no stats sidecar"

cfg0="$(run_block 0)"
[ "$cfg0" = "$T/corpus/config.yaml" ] || fail "UPWEIGHT_DEPTH=0 still repointed CONFIG: $cfg0"

echo "PASS: train-glm-adapter.sh depth upweight block"
