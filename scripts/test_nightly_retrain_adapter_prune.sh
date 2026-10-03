#!/usr/bin/env bash
# Hermetic tests for the 2026-10-03 disk-filling fix in nightly-retrain.sh:
# free_gb_check's new 50 GB default + "skipped: disk" marker, and
# run_adapter_prune_stage calling scripts/retrain/prune_adapters.py.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; SCRIPT="$HERE/nightly-retrain.sh"; fail=0
export HU_REPO_DIR="$(cd "$HERE/.." && pwd)"
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
T=$(mktemp -d); export HOME="$T/home"; mkdir -p "$HOME/.human/training-data/adapters" "$HOME/.human/logs"
A="$HOME/.human/training-data/adapters"

# ── free_gb_check: default is now 50 GB, and the skip line is greppable ────
out=$(HU_RETRAIN_STAGE_TEST=1 bash -c '
  df() { echo "Filesystem 512-blocks Used Available Capacity iused ifree %iused  Mounted"; echo "/dev/disk3s1 1 1 30 1% 1 1 1% /"; }
  source "$0"; free_gb_check; echo "rc=$?"
' "$SCRIPT" 2>&1)
check "default floor is 50 GB (30 free fails with no override)" 'grep -q "rc=1" <<<"$out"'
check "skip line is greppable as skipped: disk" 'grep -q "skipped: disk" <<<"$out"'

out2=$(HU_RETRAIN_STAGE_TEST=1 HU_RETRAIN_MIN_FREE_GB=10 bash -c '
  df() { echo "Filesystem 512-blocks Used Available Capacity iused ifree %iused  Mounted"; echo "/dev/disk3s1 1 1 30 1% 1 1 1% /"; }
  source "$0"; free_gb_check; echo "rc=$?"
' "$SCRIPT" 2>&1)
check "override floor passes when free exceeds it" 'grep -q "rc=0" <<<"$out2"'

# ── run_adapter_prune_stage: calls the real pruner against a fixture dir ───
for i in 1 2 3 4 5; do
  d="$A/seth-m3-outcomes-202609${i}0-030000"; mkdir -p "$d"; echo w > "$d/adapters.safetensors"
done
SERVING="$A/seth-m3-outcomes-20260905-030000"; mkdir -p "$SERVING"; echo w > "$SERVING/adapters.safetensors"
printf '{"personalization":{"lora_adapter_path":"%s"}}\n' "$SERVING" > "$HOME/.human/config.json"
out3=$(HU_RETRAIN_STAGE_TEST=1 bash -c 'source "$0"; run_adapter_prune_stage "$1"' "$SCRIPT" "$A" 2>&1)
check "prune stage runs in shadow by default and deletes nothing" \
  '[ "$(ls -d "$A"/seth-m3-outcomes-* | wc -l | tr -d " ")" = 6 ]'
check "prune stage logs the adapter_prune summary line" 'grep -q "\[adapter_prune\] mode=shadow" <<<"$out3"'
check "prune stage exit is logged" 'grep -q "adapter_prune: exited rc=0" <<<"$out3"'

out4=$(HU_RETRAIN_STAGE_TEST=1 HU_ADAPTER_PRUNE=live bash -c 'source "$0"; run_adapter_prune_stage "$1"' "$SCRIPT" "$A" 2>&1)
check "live mode actually deletes beyond the newest 2 + served" \
  '[ "$(ls -d "$A"/seth-m3-outcomes-* | wc -l | tr -d " ")" -lt 6 ]'
check "served adapter survives live pruning" '[ -f "$SERVING/adapters.safetensors" ]'

rm -rf "$T"; exit $fail
