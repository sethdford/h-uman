# Sprint 8 — Pre-planning smoke run results (2026-05-16)

Captured during the post-Sprint-7-close session. The first time the full
digital-twin loop was actually invoked end-to-end on Seth's real `~/.human/memory.db`.

## What ran

```
human ml mine-corrections --db ~/.human/memory.db
  -> 70 new triples mined; 367 cumulative pairs in dpo_pairs;
     /tmp/sprint8_smoke_pairs.jsonl exported (193 KB)

python3 -m mlx_lm_lora.train --train-mode sft \
  --model mlx-community/gemma-4-e2b-it-4bit \
  --iters 50 --batch-size 4 --num-layers 8 --learning-rate 5e-5
  -> Val loss 6.683 -> 0.896 (86% reduction); 1m36s
  -> Adapter: ~/.human/training-data/adapters/seth-sprint8-sft-e2b/

python3 -m mlx_lm_lora.train --train-mode dpo \
  --resume-adapter-file <SFT-adapter> \
  --reference-model-path mlx-community/gemma-4-e2b-it-4bit \
  --iters 50 --beta 0.1 --dpo-cpo-loss-type sigmoid
  -> Val accuracy 55.7% -> 76%; Train margin 0.608 -> 9.163
  -> Adapter: ~/.human/training-data/adapters/seth-sprint8-dpo-e2b/

# Generate from each adapter on 5 held-out prompts
mlx_lm.generate --model <base> --adapter-path <each-adapter> ...
  -> Captured to /tmp/sprint8_ab_sft.json, /tmp/sprint8_ab_dpo.json

human ml lora-ab --persona seth \
  --before /tmp/sprint8_ab_sft.json \
  --after /tmp/sprint8_ab_dpo.json
  -> SFT mean: 0.611
  -> DPO mean: 0.611
  -> delta:   +0.000
```

## Headline finding

**The Sprint 7 headline goal metric (`lora-ab delta > 0.05 above SFT-only baseline`) FAILS on real data at this scale.** Delta is 0.000, far below the 0.05 threshold.

This is exactly what Sprint 7's adversarial auditor predicted in AC-7.1.2:
the existing `check-lora-ab.sh` compares pre-baked fixtures and reports
delta=0.368, but running on real adapters reports delta=0.000.

## Real bugs surfaced

### US-8.6 (P0) — `--lora-parameters` CLI flag is rejected
Neither stock `mlx_lm.lora` nor `mlx_lm_lora.train` accepts `--lora-parameters`
as a CLI flag. US-7.4's argv-shape test passes (string emitted) but the actual
subprocess call exits 2 with "unrecognized arguments". The `lora_parameters`
must be in a YAML config file passed via `-c <path>`, NOT as a CLI flag.

Impact: `scripts/finetune-gemma.py` Phase 1 (SFT) was completely broken at
runtime when invoked end-to-end. Workaround used in smoke: invoke
`mlx_lm_lora.train` directly without the flag, accept rank=8/layers=8 defaults
instead of US-7.4's intended rank=32/layers=16.

### US-8.5 (P1) — UTF-8 boundary truncation in PII redactor
Of 193,416 bytes in the exported JSONL, 6 are invalid UTF-8. Likely emoji or
multi-byte chars truncated mid-sequence by `hu_pii_redact`. Consumers must use
`errors='replace'` to read the file. Sprint 7 tests used clean ASCII fixtures
and missed this.

## Qualitative findings — DPO behavior shifted, mixed direction

5 held-out prompts, deterministic generation (temp=0.0):

| Prompt | SFT direction | DPO direction | Verdict |
|---|---|---|---|
| "hey what's up" | "I'm doing well! I'm just here..." | "I'm doing well. How can I help you today?" | ✅ Terser, less chirpy |
| "tattoo thinking" | Markdown bullet list | Natural prose | ✅ Less markdown |
| "Bláthnaid pics" | Mild refusal | Hard refusal | ⚠️ Regression — would engage in real persona |
| "how old are your kids?" | LLM-disclaimer | Non-English garbage | ❌ Failure (broken) |
| Betty Ford check-in | Clean refusal | Confused jumble | ❌ Failure |

DPO learned SOMETHING (verifiable via the val accuracy lift and train margin
growth), but at 50 iters on 331 pairs with the rank=8/layers=8 (workaround
defaults), the result is unstable. 2 of 5 prompts produced regressions or
collapse.

## What Sprint 8 must address (re-prioritized)

The smoke run promotes/demotes some of the original Sprint 8 stories:

| Story | Status |
|---|---|
| US-8.6 (`--lora-parameters` CLI fix) | **HOTTEST P0** — until this is fixed, US-7.4's rank/layers settings don't apply, and DPO trains with weaker defaults |
| US-8.1 (real DPO vs SFT gate) | **P0 confirmed** — we now have a working harness pattern; productionize it |
| US-8.2 (NLL backend) | **P0 confirmed** — synthetic fingerprint isn't sensitive enough; real Seth NLL needed |
| US-8.3 (simpo P0) | P0 — still hide-or-wire decision |
| US-8.4 (step discriminator) | P1 |
| US-8.5 (UTF-8 redactor) | P1 |
| US-8.7 (NEW) | Long-training-run sweep — 50 iters is far too few; run 800-1200 iters per script defaults and re-measure |
| US-8.8 (NEW) | Eval corpus size — 5 prompts isn't statistically meaningful; build a 30+ held-out set |
| US-8.9 (NEW) | personal_model.bin bootstrap — synthetic fingerprint is the wrong baseline; build the real one from chat history |

## Reproducibility — exact commands to re-run

```bash
# Prereq: PEP 668 override for homebrew Python
pip3 install --user --break-system-packages 'mlx-lm-lora>=2.1.0,<3'

# Step 1: mine corrections from real chat.db
./build/human ml mine-corrections --db ~/.human/memory.db

# Step 2: prep DPO data (split 90/10)
python3 << 'EOF'
import json, random; random.seed(42)
pairs = [json.loads(l) for l in open('/Users/sethford/.human/dpo/pairs.jsonl', 'rb').read().decode('utf-8', errors='replace').splitlines() if l.strip()]
random.shuffle(pairs)
n_valid = max(8, len(pairs) // 10)
import os; d = os.path.expanduser('~/.human/training-data/dpo_finetune'); os.makedirs(d, exist_ok=True)
open(f'{d}/train.jsonl', 'w').write('\n'.join(json.dumps(p, ensure_ascii=False) for p in pairs[n_valid:]))
open(f'{d}/valid.jsonl', 'w').write('\n'.join(json.dumps(p, ensure_ascii=False) for p in pairs[:n_valid]))
EOF

# Step 3: SFT baseline
python3 -m mlx_lm_lora.train --model mlx-community/gemma-4-e2b-it-4bit \
  --train --train-mode sft --train-type lora \
  --data ~/.human/training-data/finetune \
  --adapter-path ~/.human/training-data/adapters/seth-sft \
  --iters 800 --batch-size 4 --num-layers 16 --learning-rate 5e-5 \
  --max-seq-length 2048 --grad-checkpoint

# Step 4: DPO on top
python3 -m mlx_lm_lora.train --model mlx-community/gemma-4-e2b-it-4bit \
  --train --train-mode dpo --train-type lora \
  --data ~/.human/training-data/dpo_finetune \
  --adapter-path ~/.human/training-data/adapters/seth-dpo \
  --resume-adapter-file ~/.human/training-data/adapters/seth-sft/adapters.safetensors \
  --reference-model-path mlx-community/gemma-4-e2b-it-4bit \
  --iters 200 --batch-size 2 --num-layers 16 --learning-rate 1e-5 \
  --max-seq-length 2048 --grad-checkpoint --beta 0.1 --dpo-cpo-loss-type sigmoid

# Step 5: A/B compare (requires the Python eval script — see this session's commit history)
```

## Wall-clock budget

- Smoke run (this session): SFT 1m36s + DPO ~2m + eval ~30s = **~4-5 min** at 50 iters / 5 prompts.
- Production run (Sprint 8): SFT 800 iters + DPO 200 iters + 30-prompt eval ≈ **30-60 min**.

## Honest one-line verdict

**Sprint 7 shipped the parts. Sprint 8 must prove they compose into the headline outcome — because today they don't.**
