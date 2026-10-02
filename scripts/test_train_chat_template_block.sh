#!/usr/bin/env bash
# Runs the REAL chat-template staging block and the mlx_lm_lora refusal block
# from train-glm-adapter.sh (extracted by their marker comments, so this cannot
# drift from the shipped code) against a tiny scratch corpus.
#
# Asserts: for mlx_tune on a GLM base, CONFIG is repointed at a staged -tmpl-
# copy whose rows are all chat-templated and whose manifest names <|user|>;
# the source corpus is untouched; a non-GLM base or a non-mlx_tune trainer
# leaves CONFIG alone; and mlx_lm_lora on a GLM base is refused unless
# HU_TRAIN_ALLOW_UNTEMPLATED=1.
#
# Uses the mlx-tune venv and the local GLM tokenizer files (no weights, no
# network, no server). Skips with exit 0 if either is unavailable.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PY="${MLXTUNE_PY:-$HOME/.human/venvs/mlxtune312/bin/python}"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$PY" ] || { echo "SKIP: mlx-tune venv not found at $PY"; exit 0; }
HF_HUB_OFFLINE=1 "$PY" -c "from mlx_lm.utils import load_tokenizer; load_tokenizer('mlx-community/GLM-4.5-Air-4bit')" \
  >/dev/null 2>&1 || { echo "SKIP: local GLM-4.5-Air-4bit tokenizer not cached"; exit 0; }

STAGE="$(awk '/^# --- chat-template staging/{f=1} f{print} f&&/^fi$/{exit}' "$HERE/train-glm-adapter.sh")"
[ -n "$STAGE" ] || fail "staging block marker not found in train-glm-adapter.sh"
REFUSE="$(awk '/^# Chat-template contract \(2026-10-01\)/{f=1} f{print} f&&/^fi$/{exit}' "$HERE/train-glm-adapter.sh")"
[ -n "$REFUSE" ] || fail "refusal block marker not found in train-glm-adapter.sh"

mkdir -p "$T/corpus"
{
  echo '{"prompt":"Them: you around?","chosen":"yeah whats up","rejected":"Hello! I am here and happy to help."}'
  echo '{"prompt":"Them: dinner sat?","chosen":"down","rejected":""}'
} > "$T/corpus/train.jsonl"
echo '{"prompt":"Them: yo","chosen":"sup","rejected":"Greetings!"}' > "$T/corpus/valid.jsonl"
printf 'model: mlx-community/GLM-4.5-Air-4bit\ndata: %s\nmax_seq_length: 2048\n' "$T/corpus" > "$T/corpus/config.yaml"
cp "$T/corpus/train.jsonl" "$T/train.orig"

run_stage() {  # $1 = TRAINER, $2 = IS_GLM; prints the final CONFIG
  bash -c "
    set -u
    say() { :; }
    die() { echo \"FATAL: \$*\" >&2; exit 1; }
    MLXTUNE_PY='$PY' CONFIG='$T/corpus/config.yaml' STAMP=TEST LOG='$T/log'
    TRAINER='$1' IS_GLM='$2' MODEL_ID=mlx-community/GLM-4.5-Air-4bit MAX_SEQ=2048
    cd '$HERE'
    $STAGE
    echo \"\$CONFIG\"
  " 2>/dev/null | tail -n 1
}

cfg="$(run_stage mlx_tune 1)"
[ "$cfg" = "$T/corpus-tmpl-TEST/config.yaml" ] || fail "CONFIG not repointed: $cfg"
grep -q "^data: $T/corpus-tmpl-TEST\$" "$cfg" || fail "staged config data: line wrong"
"$PY" - "$T/corpus-tmpl-TEST" <<'PY' || fail "staged rows are not templated"
import json, sys
from pathlib import Path
d = Path(sys.argv[1])
rows = [json.loads(l) for l in (d / "train.jsonl").read_text().splitlines()]
assert len(rows) == 2, rows
for r in rows:
    assert r["prompt"].startswith("[gMASK]<sop><|user|>\n"), r["prompt"]
    assert r["prompt"].endswith("/nothink<|assistant|>\n<think></think>"), r["prompt"]
    assert r["chosen"].endswith("<|user|>") and r["rejected"].endswith("<|user|>"), r
assert rows[1]["rejected"] == "<|user|>", rows[1]
m = json.loads((d / "chat_template_manifest.json").read_text())
assert m["end_of_turn"] == "<|user|>" and m["end_of_turn_id"] == 151336, m
assert (d / "valid.jsonl").is_file()
PY
cmp -s "$T/corpus/train.jsonl" "$T/train.orig" || fail "source corpus was mutated"

[ "$(run_stage mlx_lm_lora 1)" = "$T/corpus/config.yaml" ] || fail "non-mlx_tune trainer must not template"
[ "$(run_stage mlx_tune 0)" = "$T/corpus/config.yaml" ] || fail "non-GLM base must not template"

run_refuse() {  # $1 = TRAINER, $2 = override; prints rc
  set +e
  bash -c "
    set -u
    CONFIG='$T/corpus/config.yaml' TRAINER='$1' HU_TRAIN_ALLOW_UNTEMPLATED='$2'
    $REFUSE
    exit 0
  " >/dev/null 2>&1
  echo $?
  set -e
}
[ "$(run_refuse mlx_lm_lora 0)" = "2" ] || fail "mlx_lm_lora on GLM must be refused"
[ "$(run_refuse mlx_lm_lora 1)" = "0" ] || fail "HU_TRAIN_ALLOW_UNTEMPLATED=1 must override"
[ "$(run_refuse mlx_tune 0)" = "0" ] || fail "mlx_tune must not be refused"

echo "PASS: chat-template staging + mlx_lm_lora refusal blocks"
