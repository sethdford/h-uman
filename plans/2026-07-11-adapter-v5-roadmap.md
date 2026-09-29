# Adapter v5→v10 Roadmap — Trigger: rating sheet 12/12

> Written 2026-07-11. Self-contained: a fresh session can execute from this doc alone.
> A scheduled routine watches the trigger and pings when it fires.
> **STATUS 2026-07-25: v5 EXECUTED — on a NEW BASE (GLM-4.5-Air, not gemma).
> See the execution record at the end of this doc. Human sheet running (~12 d).**

## Why we're on v4-repair (context a fresh session needs)

- **v3** died to the mlx_lm `scale=20` default catastrophe (see
  `.claude/rules/lora-scale-default-or-die.md`).
- **v4-repair** (2026-05-25, scale 2.0, 500 iters, 1,963 SFT pairs) fixed it and
  solved terseness + single-turn voice. Live at
  `~/.human/training-data/adapters/seth-lora-v4-repair-20260525-071921`.
- **A v5 candidate already lost**: `seth-voice-l4` (2026-06-01, 961 pairs) tied
  v4-repair in blind A/B (detect 0.134 vs 0.142, CIs overlap) — kept incumbent.
- **Root blocker is the JUDGE, not training**: the synthetic Gemini judge picks
  h-uman as real ~86% (saturated). Every candidate measures as a tie. Promotion
  discipline (registry + evidence-required `m3_promote`, shipped 2026-07-11)
  correctly refuses ties. **Do not train v5 against the synthetic judge again.**

## TRIGGER CONDITION (what the routine watches)

`scripts/blind_ab/rating_ingest.py status` reports **12/12 rated** AND
`docs/evaluation/blind_ab_gate.json` `human.verdict != "ABSENT"`.
The rating drip sends Seth 1 blind question/day (launchd `ai.human.rating-drip`,
send+harvest live since 2026-07-11). Expected ~12 days → ~2026-07-23.
If ratings stall (no progress ≥4 days), the drip/harvest broke — debug
`~/.human/logs/rating-drip.log` + `rating_ingest.py status`.

## v5 — corpus-refresh SFT (execute when trigger fires)

Unlock discovered 2026-07-11: the old training corpus predated attributedBody
decoding — v4 never saw ~2,200 of Seth's real messages. Refreshed corpus:
**1,192 training pairs + 1,180 voice pairs + 711 conversation windows**
(regenerate anytime: `python3 scripts/extract_imessage_pairs.py` — NOTE: no
argparse guard, ANY invocation runs it; writes `data/imessage/*.jsonl`).

1. Train (config — scale 2.0 EXPLICIT, verify adapter_config.json after):
   ```yaml
   # v5 recipe (mirror v4-repair, refreshed corpus)
   model: mlx-community/gemma-4-31b-it-4bit
   data: data/imessage/voice_training_pairs.jsonl   # decode-complete corpus
   lora_parameters: {rank: 8, scale: 2.0, dropout: 0.0}
   iters: 500
   learning_rate: 1e-5
   batch_size: 1
   num_layers: 8
   max_seq_length: 2048
   ```
   Use `scripts/training_loop.py` (production path — quality gate + registry
   wiring included) or `scripts/run-dpo-training.sh` shape for manual.
2. Base-capability smoke BEFORE any eval (persona + instruction + multi-turn
   prompts — the v3 lesson).
3. Candidate eval WITHOUT touching :8741: serve on spare :8743
   (`project_h-uman_blind_ab_serving_routing` memory: gen_direct.py pattern,
   exact product prompt via hu_persona_build_prompt_compact).
4. **Judge = HUMAN tier + fresh nightly proxy, not the saturated synthetic
   judge alone.** Candidate must beat v4-repair, not tie it.
5. Promote only via `scripts/m3_promote.py promote --adapter <path>
   --evidence '<verdict ref>'` (refuses without evidence; registry records it).

## The ladder after v5 (each rung = data + method + a judge that can see it)

| Ver | Method | Data source | Gate/judge | Ready when |
|-----|--------|-------------|------------|------------|
| v6 | DPO | dpo_pairs (347 @ 2026-07-11; 5 sources, collection live) | loss-regression gate (dpo_results.py) + human tier | ~1k+ pairs |
| v7 | KTO | feedback_signals single-sided (143 @ 2026-07-11; kto_export.py live) | kto_results.py + human tier | ~300+ rows |
| v8 | multi-turn SFT | 711 conversation windows | eval_multiturn_local.py verdict (the unmeasured gap) | after v5 lands |
| v9/v10 | continual loop | nightly train→eval→registry→auto-promote/demote | full pipeline (skeleton shipped 2026-07-11) | after one manual v5→promote cycle proves the rail |

## Standing decisions already made (don't relitigate)

- Steering program CLOSED by measurement — no activation-engineering shortcut.
- Terseness SFT hit diminishing returns — v5's thesis is the *decode-complete
  corpus*, not more terseness.
- GraphRAG default-ON pending fresh nightly verdict — separate thread.

## 2026-07-25 EXECUTION RECORD — v5 trained on GLM-4.5-Air (base pivot)

The trigger fired (12/12, human detection 0.75 FAIL, tell = lol-register
collapse). v5 was executed per this doc's recipe with ONE deliberate change:
the base is **mlx-community/GLM-4.5-Air-4bit** (110.5B-A12B MoE), not gemma —
a same-day three-way probe showed the raw GLM base alone halves synthetic
distinguishability vs v4-repair and matches Seth's lol-rate exactly (2/160).

- Recipe: rank 8, **scale 2.0 verified in adapter_config.json**, 500 iters,
  lr 1e-5, batch 1, seq 2048, decode-complete corpus (1202/64 split, seed 42).
  Val loss 7.08 → 2.94. Base-capability smoke PASS (persona/instruct/multi-turn).
- Adapter: `~/.human/training-data/adapters/seth-glm-air-v5-20260725-093742`.
- Three-way synthetic verdict (full numbers:
  `docs/plans/2026-07-18-sota-roadmap/results/glm-air-v5-three-way-2026-07-25.json`):
  |detect−0.5| = 0.358 (v4-repair) → 0.191 (GLM base) → **0.153 (GLM+v5)**;
  voice 3.86/4.94/5.16; lol 47/2/2 vs real-Seth 2.
- Human gate: fresh 12-item sheet from the v5 arm deployed to
  `~/.human/blind_ab_human/` 2026-07-25 (v4 sheet archived in
  `archive-v4repair-20260725/`); drip live, ETA ~12 days.
- **Promotion mechanics changed by the base pivot:** NOT an `m3_promote`
  adapter hot-swap. Going live = point the `mlx_local` provider at
  GLM-4.5-Air + adapter (spare-port rehearsal first), which also depends on
  the mlx-server venv rebuild (python@3.13 was uninstalled — restart landmine).
- v6/v7 ladder rungs (DPO/KTO) should retarget the GLM base if the human
  sheet confirms; do not train further gemma adapters without a reason.
