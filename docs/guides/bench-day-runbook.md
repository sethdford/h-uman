---
title: Bench Day Runbook — Gemma Throughput Program
created: 2026-05-24
status: operator-facing
parent: docs/plans/2026-05-24-gemma-throughput-program.md
applies_to: Tier 1 phases (0, 1, 1b, 1c, 2, 2b, 2b.2, 3a, 3a.2, 3b, 4, 4b, 4c, 4d, 4e)
---

# Bench Day Runbook

Step-by-step for measuring the Gemma throughput program's Tier-1 wins
on your M4 Max. Every code-level commit in the program is now on
`main`; the only thing left is the empirical proof that the wired
gains actually show up. This runbook is that.

**Time budget:** ~45 minutes once the models are downloaded. ~2 hours
if you also have to fetch a 31B-4bit model the first time.

**What this proves vs claims:**

| Phase | Claim (in commit body) | This runbook measures |
|---|---|---|
| 1   | Q8 KV → ~50% RSS, ~10-15% TPS | RSS delta @ warmup, TPS delta on `nstream_short_reply` |
| 4   | Flash Attention → 15-30% TPS on Metal | TPS delta on `nstream_long_reply` |
| 3b  | Cross-model spec decode → 1.5-2× decode TPS | TPS delta on `nstream_long_reply` (spec decode shines on long generations) |
| 2b.2| Decode-skip → 30-50% TTFT on warm hits | TTFT delta on the SECOND iteration of any prompt |

If a measured number is lower than the lower bound of the claim, that
phase needs investigation — either the wiring isn't firing, or the
operator-machine + model combination behaves differently than the
practitioner research we cited. Either way, the bench result is the
source of truth.

---

## Step 0 — Pre-flight (5 min)

**Models present?**

```bash
# Main model: Gemma-3-4B-it Q4_K_M
ls -lh ~/.human/models/gemma-3-4b-it-Q4_K_M.gguf 2>/dev/null \
    || scripts/fetch-gemma.sh

# Optional: 31B-4bit for the analytical tier bench
ls -lh ~/.human/models/*31b*4bit*.gguf 2>/dev/null \
    || echo "No 31B — that's fine, this runbook benches 4B"

# Draft model for spec decode (Phase 3b)
ls -lh ~/.human/models/gemma-4-E2B*.gguf ~/.human/models/gemma-3-270m*.gguf 2>/dev/null \
    || echo "No draft — Step 6 (Phase 3b) will be skipped without one"
```

If draft missing, fetch one. Production guidance:
- **Preferred for gemma-4-* targets**: `gemma-4-E2B-it` GGUF (e.g.
  unsloth/gemma-4-E2B-it-GGUF) — same Gemma 4 family as the 31B target,
  highest acceptance rate, NOT gated on HuggingFace.
- **Ideal once tooling catches up**: `google/gemma-4-31B-it-assistant`
  (470M, purpose-built draft for the 31B target). NOT gated, but its
  `gemma4_assistant` architecture is not yet supported by mlx_lm 0.31.2
  — track the upstream-watch (`scripts/check-mlx-lm-cb-upstream.sh`).
- **Fallback**: `gemma-3-270m` GGUF. Smallest, lowest RAM, but
  cross-generation draft so acceptance rate is lower. The Gemma 3
  family shares the same 262k vocab as Gemma 4 so the tokens line up,
  but the distributions diverge more than within-family.

```bash
# Same-family draft for gemma-4 targets (recommended):
scripts/fetch-gemma.sh \
    --draft "<URL-to-gemma-4-E2B-it-Q4_K_M.gguf>" \
    --draft-sha "<sha256-from-upstream>"
```

**Server runnable?**

```bash
# Start in foreground so we can ctrl+C between modes
python3 scripts/mlx-server.py --model "$(echo ~/.human/models/gemma-3-4b-it-Q4_K_M.gguf)"
```

In a second terminal, confirm it's up:

```bash
curl -s http://127.0.0.1:8741/health
# Expect: {"ok":true,"active_adapter":"","model_loaded":true,...}
```

**Doctor clean?**

```bash
./build/human doctor 2>&1 | grep -E '\[doctor\] HU_LLAMACPP_|inference'
```

Look for: `kv_quant: unset (FP16 default)`, `flash_attn: unset (FA on,
default)`, `draft_model: unset`. If anything shows `UNRECOGNIZED` or
`NOT READABLE`, fix it before benching.

---

## Step 1 — Baseline (Phase 0 reference) (5 min)

Capture the pre-Phase-1 baseline. Everything later phases diff against
this JSON.

```bash
mkdir -p tests/fixtures/bench

# Server: unset all Phase 1+ env vars, restart, then:
scripts/bench-gemma-perf.py \
    --tag baseline-$(date +%Y-%m-%d) \
    --n 5 \
    --measure-rss \
    --out tests/fixtures/bench/01-baseline.json
```

**Expected output (M4 Max, Gemma-3-4B-Q4):**

```
  nstream_short_reply       n= 5  tps=  XX.XX  ttft= X.XXs  tot=  X.XXs  gen=   XX
  nstream_long_reply        n= 5  tps=  XX.XX  ttft= X.XXs  tot=  X.XXs  gen=  XXX
RSS: baseline XXX MiB → warmup YYY MiB → end ZZZ MiB
```

Numbers depend on your machine and the bf16/Q4 split, but the SHAPE
matters: TPS should be in the 30-60 range for Q4 Gemma-3-4B on M4 Max
per the practitioner signal we cited.

---

## Step 2 — Q8 KV quant (Phase 1) (5 min)

```bash
# Server: restart with
#   HU_LLAMACPP_KV_QUANT=q8_0
# then:
scripts/bench-gemma-perf.py \
    --tag q8 \
    --n 5 \
    --measure-rss \
    --out tests/fixtures/bench/02-q8.json

# Compare:
scripts/bench-gemma-perf.py --compare \
    tests/fixtures/bench/01-baseline.json \
    tests/fixtures/bench/02-q8.json
```

**Expected deltas:**
- `Δ tps` on `nstream_*`: **+8% to +15%** (memory-bandwidth-bound at batch=1)
- `RSS @ warmup`: **−30% to −50%** (KV is half the model RSS for 4B)
- TTFT: roughly unchanged (KV quant helps decode, not prefill)

**If TPS delta is < +5%:**
- Run `./build/human inference-status` — confirm `kv_quant: q8_0`. If
  `fp16 (default — env HU_LLAMACPP_KV_QUANT unset)`, the env didn't
  reach the server.
- Confirm your llama.cpp build supports KV quant — older builds
  silently ignore the `cp.type_k` field.

---

## Step 3 — Flash Attention (Phase 4) (5 min)

```bash
# Server: restart with
#   HU_LLAMACPP_KV_QUANT=q8_0
#   HU_LLAMACPP_FLASH_ATTN=on    # this is the default; setting is explicit
# then:
scripts/bench-gemma-perf.py --tag q8+fa --n 5 --measure-rss \
    --out tests/fixtures/bench/03-q8+fa.json

scripts/bench-gemma-perf.py --compare \
    tests/fixtures/bench/02-q8.json \
    tests/fixtures/bench/03-q8+fa.json
```

**Expected deltas:**
- `Δ tps` on `nstream_long_reply`: **+15% to +30%** (FA shines on long
  generations where attention is a bigger fraction of compute)
- `Δ tps` on `nstream_short_reply`: smaller (+5% to +15%)
- RSS: roughly unchanged

**If TPS delta is near zero:**
- Your llama.cpp build may already have FA on by default — the env var
  is then a no-op. Try `HU_LLAMACPP_FLASH_ATTN=off` and confirm the
  bench REGRESSES (i.e. it was on before too).

---

## Step 4 — Spec decode (Phase 3b) (10 min — only if draft present)

```bash
# Server: restart with
#   HU_LLAMACPP_KV_QUANT=q8_0
#   HU_LLAMACPP_FLASH_ATTN=on
#   HU_LLAMACPP_DRAFT_MODEL=$HOME/.human/models/gemma-4-E2B-it-...gguf
#     (or gemma-3-270m-...gguf as a smaller cross-family fallback)
#   HU_LLAMACPP_DRAFT_MIN_P=0.05
#   HU_LLAMACPP_DRAFT_MAX_TOKENS=5
# then:
scripts/bench-gemma-perf.py --tag q8+fa+draft --n 5 --measure-rss \
    --out tests/fixtures/bench/04-q8+fa+draft.json

scripts/bench-gemma-perf.py --compare \
    tests/fixtures/bench/03-q8+fa.json \
    tests/fixtures/bench/04-q8+fa+draft.json
```

**Expected deltas:**
- `Δ tps` on `nstream_long_reply`: **+50% to +100%** (1.5-2× per the
  research stream's consensus for unaligned draft)
- `Δ tps` on `nstream_short_reply`: smaller (+10% to +30%; spec decode
  has fixed overhead per turn)
- RSS: +small (draft model takes a few hundred MB)

**If acceptance rate looks low** (TPS delta < +20%):
- That's *expected* for an UNALIGNED draft. The Phase 6 milestone
  trains a persona-aligned draft to lift acceptance from ~25% to ≥50%.
- Watch for `[llama.cpp]` log lines in the server output mentioning
  the draft model loading — if it fails silently to load (wrong
  tokenizer, wrong arch), spec decode degrades to single-model decode
  with extra overhead.

---

## Step 5 — Decode-skip (Phase 2b.2) (5 min)

Note: this opt-in is the highest-risk perf lever (silent KV corruption
possible if the libllama build differs from what was tested). Verify
sanity-gate quality after enabling.

```bash
# Server: restart with
#   HU_LLAMACPP_KV_QUANT=q8_0
#   HU_LLAMACPP_FLASH_ATTN=on
#   HU_LLAMACPP_DRAFT_MODEL=...
#   HU_LLAMACPP_KVCACHE_SKIP_DECODE=1   # the new opt-in
# then:
scripts/bench-gemma-perf.py --tag full-stack --n 5 --measure-rss \
    --out tests/fixtures/bench/05-full-stack.json

scripts/bench-gemma-perf.py --compare \
    tests/fixtures/bench/04-q8+fa+draft.json \
    tests/fixtures/bench/05-full-stack.json
```

**Expected deltas (on the SECOND iteration of any prompt — the warm path):**
- `Δ ttft`: **−30% to −50%** (the prefix tokens are no longer
  re-decoded — this is the actual Phase 2b.2 TTFT win that Phase 2b's
  tokens_would_skip counter quantified)
- `Δ tps`: smaller (TPS measures decode rate, not prefill; skip mostly
  helps TTFT)

**Quality check after enabling:**

```bash
scripts/run-gemma-sanity-gate.sh
```

Should score within 2% of the baseline run. If quality regresses
loudly (incoherent output, wrong-language tokens, repetition loops),
disable immediately:

```bash
unset HU_LLAMACPP_KVCACHE_SKIP_DECODE
# restart server
```

And report the regression — the libllama version interaction is the
suspect.

---

## Step 6 — One-shot all-modes wrapper (Phase 4e — alternate workflow)

If you'd rather skip the per-step env-shuffling and prefer a guided
workflow:

```bash
scripts/bench-gemma-perf.py \
    --compare-modes baseline,q8,q8+fa,q8+fa+draft \
    --n 5 \
    --measure-rss \
    --compare-modes-out tests/fixtures/bench/
```

The script will prompt at each mode boundary with the exact env vars
to set before you restart the server. Press Enter when ready. At the
end it prints comparisons of every mode vs baseline.

(`q8+fa+draft` requires the draft env vars set yourself — the wrapper
puts `<set-to-draft-path>` as a placeholder.)

---

## Step 7 — Persist + commit results

```bash
# Pick the winning combination (typically the full stack) as the new
# bench-reference. Future bench-day runs compare against this JSON.
cp tests/fixtures/bench/05-full-stack.json \
    tests/fixtures/bench/reference-$(date +%Y-%m-%d).json

git add tests/fixtures/bench/
git commit -m "bench(perf): $(date +%Y-%m-%d) measured deltas vs baseline

Tier-1 stack: Q8 + FA + spec-decode + skip-decode
Measured on M4 Max, Gemma-3-4B Q4_K_M.

[paste the compare() output for each step]
"
```

---

## What success looks like

| Metric | Pre-program (baseline) | Post-Tier-1 (full stack) | Source of expectation |
|---|---|---|---|
| `nstream_long_reply` TPS | 30-50 | **70-120** (≥2× via spec decode + FA) | Phase 3b + 4 commit bodies |
| Warm-prompt TTFT | 500-700ms | **150-250ms** (≥2× via decode-skip) | Phase 2b.2 + 2b commit bodies |
| KV RSS @ 2K context | baseline | **−40% to −50%** | Phase 1 commit body |
| `kvcache.tokens_would_skip` | 0 | accumulating | Phase 2b instrumentation |

If you hit the lower bound of every row, the throughput program
delivered as designed. If two or more rows underperform by >30%, that
warrants investigation — open a chip via `mcp__ccd_session__spawn_task`
with the bench JSON pair attached.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `nstream_*` tps=0.00 in results JSON | mlx-server.py older than Phase 0.1; `usage: 0,0,0` | Rebuild from current main; Phase 0.1 wires real token counts at scripts/mlx-server.py:286-330 |
| RSS column shows `n/a` | `--measure-rss` set but `lsof` can't find the server | Pass `--server-pid <pid>` explicitly |
| Q8 changes nothing | env var not reaching the server (different shell, different `restart`) | `./build/human inference-status` from the operator's shell — confirm `kv_quant: q8_0`. If `fp16 default`, the env didn't survive the restart. |
| spec decode regresses TPS | draft tokenizer mismatch with target | Confirm both are Gemma-family; check server stderr for the draft load message |
| Phase 2b.2 quality regression | libllama batch-position semantics differ | `unset HU_LLAMACPP_KVCACHE_SKIP_DECODE`; report which libllama version was linked |
| `human doctor` shows `UNRECOGNIZED` | env value typo | Fix typo — accepted values are listed in the WARN message |

## Where to find the relevant code

| Phase | Source | Header |
|---|---|---|
| 0 measurement | [scripts/bench-gemma-perf.py](../../scripts/bench-gemma-perf.py), [scripts/mlx-server.py](../../scripts/mlx-server.py) | — |
| 0.3 hit/miss counters | [src/providers/llamacpp_kvcache.c](../../src/providers/llamacpp_kvcache.c) | [include/human/providers/llamacpp_kvcache.h](../../include/human/providers/llamacpp_kvcache.h) |
| 1 KV quant | [src/providers/llamacpp.c](../../src/providers/llamacpp.c) (GGML type wiring at `llama_init_from_model`) | [include/human/providers/llamacpp.h](../../include/human/providers/llamacpp.h) (`hu_kv_quant_t`) |
| 1b env bridge | [src/providers/factory.c](../../src/providers/factory.c) (`factory_apply_kv_quant_env`) | — |
| 1c config schema | [src/config/config_parse.c](../../src/config/config_parse.c) (`parse_inference`) | — |
| 2 LRU + 2b counters | [src/providers/llamacpp_kvcache.c](../../src/providers/llamacpp_kvcache.c) | [include/human/providers/llamacpp_kvcache.h](../../include/human/providers/llamacpp_kvcache.h) |
| 2b.2 skip-decode | [src/providers/llamacpp.c](../../src/providers/llamacpp.c) (chat path), [src/providers/factory.c](../../src/providers/factory.c) (`factory_apply_kvcache_skip_decode_env`) | — |
| 3a SSE streaming | [scripts/mlx-server.py](../../scripts/mlx-server.py) (`_stream_chat_completion`) | — |
| 3a.2 mlx-http alias | [src/providers/factory.c](../../src/providers/factory.c) (compat-provider table) | — |
| 3b spec decode config | [src/providers/factory.c](../../src/providers/factory.c) (`factory_apply_spec_decode_env`), [scripts/mlx-server.py](../../scripts/mlx-server.py) (draft load) | [include/human/providers/llamacpp.h](../../include/human/providers/llamacpp.h) (`draft_model_path`) |
| 4 Flash Attention | [src/providers/llamacpp.c](../../src/providers/llamacpp.c), [src/providers/factory.c](../../src/providers/factory.c) | — |
| 4b doctor checks | [src/doctor/doctor.c](../../src/doctor/doctor.c) (`hu_doctor_check_inference`) | [include/human/doctor.h](../../include/human/doctor.h) |
| 4c CLI status | [src/app/main.c](../../src/app/main.c) (`cmd_inference_status`) | — |
| 4d fetch --draft | [scripts/fetch-gemma.sh](../../scripts/fetch-gemma.sh) | — |
| 4e bench wrapper | [scripts/bench-gemma-perf.py](../../scripts/bench-gemma-perf.py) (`compare_modes`) | — |

## What this runbook does NOT cover

- **Phase 5 head-to-head bench (llama.cpp Metal vs MLX-server-via-HTTP).**
  That's a separate evaluation worth its own session — the operator
  chooses a single backend per turn, but knowing which one wins on
  YOUR workload requires running both. The same `bench-gemma-perf.py`
  works against either; just point `--url` at the right port.
- **Phase 6 persona-aligned draft adapter training.** Multi-week,
  hardware-bound. See `human ml lora-draft --help` (Phase 6 sub-slice
  commit).
- **Phase 7 RadixAttention cross-user prefix sharing.** Gated on
  mlx-lm shipping continuous batching upstream — see
  `docs/guides/radix-attention-upstream-watch.md` for the trigger
  condition and what to do when it fires.
