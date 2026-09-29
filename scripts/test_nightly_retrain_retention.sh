#!/usr/bin/env bash
# Hermetic test for retain_adapters() in nightly-retrain.sh: fake HOME + fake adapters dir.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; SCRIPT="$HERE/nightly-retrain.sh"; fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
T=$(mktemp -d); export HOME="$T/home"; mkdir -p "$HOME/.human/training-data/adapters" "$HOME/.human/logs"
A="$HOME/.human/training-data/adapters"
for i in 1 2 3 4 5 6 7; do d="$A/seth-m3-outcomes-2026090${i}-030700-glm"; mkdir -p "$d"; echo w > "$d/adapters.safetensors"; echo c > "$d/0000500_adapters.safetensors"; touch -t "2026090${i}0300" "$d"; done
for i in 1 2 3 4 5 6 7 8; do d="$A/seth-glm-air-mlxtune-simpo-2026091${i}-0313-2026091${i}-031300"; mkdir -p "$d"; echo w > "$d/adapters.safetensors"; touch -t "2026091${i}0300" "$d"; done
mkdir -p "$A/seth-lora-v6" "$A/dpo-20260802-040017"; echo w > "$A/seth-lora-v6/model.safetensors"
SERVING="$A/seth-glm-air-mlxtune-orpo-20260905-0856-20260905-085655"; mkdir -p "$SERVING"; echo w > "$SERVING/adapters.safetensors"; touch -t 202609010300 "$SERVING"
printf '{"personalization":{"lora_adapter_path":"%s"}}\n' "$SERVING" > "$HOME/.human/config.json"
printf '[{"name":"seth-glm-air-mlxtune-simpo-20260911-0313-20260911-031300","status":"retired"}]\n' > "$A/registry.json"
touch -t 202609110300 "$A/seth-glm-air-mlxtune-simpo-20260911-0313-20260911-031300"
out=$(HU_RETRAIN_STAGE_TEST=1 HU_RETRAIN_KEEP_PER_FAMILY=3 bash -c 'source "$0"; retain_adapters "$1"' "$SCRIPT" "$A" 2>&1)
check "keeps newest 3 m3-outcomes" '[ "$(ls -d "$A"/seth-m3-outcomes-* | wc -l | tr -d " ")" = 3 ]'
check "newest m3-outcomes survive" '[ -d "$A/seth-m3-outcomes-20260907-030700-glm" ] && [ -d "$A/seth-m3-outcomes-20260905-030700-glm" ] && [ ! -d "$A/seth-m3-outcomes-20260901-030700-glm" ]'
check "keeps newest 3 mlxtune candidates + the registered one" '[ "$(ls -d "$A"/seth-glm-air-mlxtune-simpo-* | wc -l | tr -d " ")" = 4 ] && [ -d "$A/seth-glm-air-mlxtune-simpo-20260911-0313-20260911-031300" ]'
check "serving adapter untouched" '[ -f "$SERVING/adapters.safetensors" ]'
check "hand-placed dirs untouched" '[ -f "$A/seth-lora-v6/model.safetensors" ] && [ -d "$A/dpo-20260802-040017" ]'
check "intermediate checkpoints beside a final adapter removed" '[ "$(find "$A" -name "0*_adapters.safetensors" | wc -l | tr -d " ")" = 0 ] && [ -f "$A/seth-m3-outcomes-20260907-030700-glm/adapters.safetensors" ]'
check "logs a summary" 'grep -q "retention: .* adapter dir(s) removed" <<<"$out"'
rm -rf "$T"; exit $fail
