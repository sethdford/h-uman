#!/usr/bin/env bash
# THROWAWAY spike: optimistic per-item judge cost for Kimi K3 on this Mac, 10 of 93 layers.
# Outputs land in ~/k3spike/results/. Synthetic prompts only (no real messages).
set -uo pipefail
K3=~/k3spike/engine/bin/k3
M=~/k3spike/model
P=~/k3spike/prompts
R=~/k3spike/results
mkdir -p "$R"
hour=$(date +%H)
if [ "$hour" -ge 2 ] && [ "$hour" -lt 5 ]; then echo "refusing: 02-05 is the retrain window"; exit 2; fi

COMMON=(--layers 10 --tok "$M" --cache-gb 8 --incremental)
run() { # name, env, args...
  local name=$1 envs=$2; shift 2
  echo "=== $name ($envs) $(date +%T)"
  /usr/bin/time -l env $envs nice -n 10 "$K3" "$M" "${COMMON[@]}" "$@" --out "$R/$name.json" \
    > "$R/$name.log" 2> "$R/$name.time"
  echo "exit=$? ; $(grep -E 'real|maximum resident' "$R/$name.time" | tr -s ' ' | tr '\n' ' ')"
  grep -E "tokens in|prompt|STEP|^ *[0-9]+ " "$R/$name.log" | head -12
}

# R0 correctness: does a wide prefill chunk change the math? (first-step logits, bit compare)
run r0_chunk64   "K3_PREFILL_CHUNK=64"   --prompt-file "$P/suffix.txt" --gen 1 --dump-logits "$R/logits64.bin"
run r0_chunk4096 "K3_PREFILL_CHUNK=4096" --prompt-file "$P/suffix.txt" --gen 1 --dump-logits "$R/logits4096.bin"
if cmp -s "$R/logits64.bin" "$R/logits4096.bin"; then echo "R0: logits BIT-IDENTICAL"; else echo "R0: logits DIFFER"; fi

# R1/R2: full ~3.4k-token judge prompt, as shipped vs wide chunk
run r1_full_chunk64   "K3_PREFILL_CHUNK=64"   --prompt-file "$P/full.txt" --gen 2
run r2_full_chunk4096 "K3_PREFILL_CHUNK=4096" --prompt-file "$P/full.txt" --gen 2

# R3/R4: cached-prefix design: prefill the shared prefix once, then per item only the suffix
run r3_prefix_save  "K3_PREFILL_CHUNK=4096" --prompt-file "$P/prefix.txt" --gen 0 --save-state "$R/prefix.state"
run r4_suffix_load  "K3_PREFILL_CHUNK=4096" --prompt-file "$P/suffix.txt" --gen 2 --load-state "$R/prefix.state"
echo "=== done $(date +%T)"
