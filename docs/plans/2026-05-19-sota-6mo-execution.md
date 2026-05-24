# 6-Month SOTA Execution Plan — h-uman as iMessage "better-than-human"

> **Status:** active
> **Owner:** seth
> **Started:** 2026-05-19
> **Backed by:** persona-eval Layer 0 closeout (commits 33f8eaa5, 528755cb,
> 9a6640a5, df441caa). All prerequisites shipped.

## Thesis

h-uman has fixed the bypass that prevented persona from reaching the eval
chain. Persona-wrapped iMessage outputs are now demonstrably in-voice
through the production gateway (M4 A/B `RESULT_verifier=PASS` 6/6 behaviors,
2026-05-18). What remains is to compound that base into a measurably
**better-than-human** texting partner across five orthogonal axes.

Each axis is one **layer**. Each layer ships as a real Python tool you can
run today, with the wiring into C-side production code following only after
the tool's metrics prove the layer is worth wiring.

## Why this layering, not just "make the model bigger"

The frontier model is shared. Anyone calling Gemma-4-26B / Gemini 3.1 has
the same baseline. Our wins come from things the frontier doesn't do per-
user:

- Best-of-N inference (Layer 5) — humans don't get to redraft texts; we can
- Voice-of-user fine-tune (Layer 1) — Gemini doesn't have your tapback
  history; we do
- Targeted memory retrieval (Layer 2) — we know which prior conversations
  are relevant; cloud APIs guess
- Multi-turn stamina (Layer 3) — we measure drift; cloud APIs hide it
- Multimodal taste (Layer 4) — "use a tapback here, not a paragraph" is a
  per-user policy, not a model capability

## Layers

### L5 — Verifier-Driven Test-Time Training (Month 1, passive-friendly)

**Tool:** `scripts/verifier_ttt.py`
**Status:** shipped 2026-05-19, smoke-passes locally.

Generates N candidates per prompt through the production gateway, scores
each with the deterministic shape classifier, argmaxes. Logs the
(prompt, chosen, rejected_k) triples to `dpo_pairs` with `source='ttt_verifier'`
so the next ORPO round can train against them.

This is the o3 / DeepSeek-R1-class capability: spend N× inference compute
per turn to deliver substantially higher quality. Costs ~5× latency for the
quality lift; latency is acceptable for async iMessage replies (people
don't expect instant replies anyway).

**Wiring (deferred):** in `src/channels/imessage.c`, behind an opt-in flag,
replace single-shot generation with TTT. Default OFF until the layer's
shape-score lift is proven on the M4 suite.

### L3 — Multi-turn naturalness (Month 1, in-flight)

**Tool:** `scripts/multiturn_drift_test.py`
**Status:** shipped + currently running against production gateway.

3 conversations × ~16 turns each, scoring each Seth response with the
shape classifier. Outputs:

- per-conversation drift slope (negative = degrading per turn)
- early-5 vs late-5 mean shape (regression sign)
- latency drift (does context bloat slow us down?)

Single-turn eval has been closed (M4 PASS). The multi-turn suite is the
remaining unanswered question: does the agent hold shape across 15+ turns,
or does it degrade? **If shape holds flat across 20 turns, that's literally
better than human** — humans drift in long conversations.

### L1 — ORPO adapter on real preference signal (Month 2-3, passive wait)

**Tools:**
- `scripts/orpo_readiness_watcher.sh` — nightly cron candidate; fires
  training when ≥500 rows of REAL preference signal (imessage_tapback,
  slack_reactji, outbound_edit, user_feedback) accumulate.
- `scripts/personaeval_speaker_id.py` — PersonaEval-style logistic
  regression on 15 style features; produces P(Seth) per candidate
  response. Used as a second signal alongside shape_score for L1's
  effectiveness check.

**Status:** scaffolding shipped. Awaiting real-world tapback corpus
(passive). Per `docs/plans/2026-05-18-adapter-routing-decision.md`,
target the imessage-tapback-v1 channel-scoped adapter; threshold ≥500
rows per Cui et al. 2025 (arXiv:2507.04889) data floor.

PersonaEval methodology mirrors Bai et al. 2025 (arXiv:2508.10014) but
with a pure-Python logistic regression in place of LLM-as-judge — we
ship our own deterministic style scorer because it's CI-friendly.

**Wiring (when threshold met):**
1. Watcher fires `human ml dpo-train --backend auto --output ~/.human/training-data/adapters/imessage-tapback-v1`
2. Score the resulting checkpoint with both shape classifier AND
   `personaeval_speaker_id.py`
3. If P(Seth) lift is ≥+0.10 absolute, swap the channel-scoped adapter
   in production via the existing adapter swap path (M3 Bridge B,
   `POST /v1/adapters/swap`).

### L2 — Memory RAG refactor (Month 4)

**Tool:** `scripts/memory_ablation.py`
**Status:** shipped.

Tests the silent assumption that the giant memory context block
(personal_model, world_model, contact_context, conversation_context,
humanness_context, residue_carryover) materially changes outputs.

Compares:
  A. Full agent_turn pipeline (gateway with --with-agent, all memory injected)
  B. Direct MLX with persona-only prompt (no memory context)

Verdict logic per Eval4Sim 2026 (arXiv:2603.02876) atomic-claim
faithfulness shape. If A and B produce statistically-equivalent shape +
PersonaEval probabilities, memory is NOT moving outputs — and the Layer
2 refactor (RAG-targeted retrieval) reclaims throughput + tokens.

**Wiring (after evidence):** if the ablation shows memory is dead weight,
replace the dump-everything injection in `src/prompt_build.c` with a
retrieval-targeted surface that pulls only memory fields actually relevant
to the current incoming. If the ablation shows specific fields help,
ablate those one at a time and keep only the load-bearing ones.

### L4 — Multimodal policy (Month 5)

**Tool:** `scripts/multimodal_policy.py`
**Status:** shipped + 11/11 golden tests pass.

A pure predicate (per `~/.claude/rules/security-predicate-extraction.md`)
that decides whether each response should be tapback / text / voice memo
/ GIF, and which tapback flavor. Today h-uman always picks text — that's
a tell. Real iMessage pros use tapback for ack moments, voice for
emotional moments, GIF for hyped moments.

**Wiring (M5+):** in `src/channels/imessage.c`, before render-and-send,
call `hu_multimodal_policy_decide(incoming, history)`. Route on the
modality:
- tapback @ conf > 0.7 → existing react() vtable path
- voice @ conf > 0.8 → queue voice synthesis (future) or fall through +
  log to dpo_pairs as "would-have-been-voice" for training
- gif @ conf > 0.7 → select from curated bank via existing attachment path
- text → as today

The policy classifier is a Python prototype; the production wiring will
be a port to C following `src/follow_up.c::ci_contains_word` style word-
boundary classifiers (per `substring-classifier-pitfalls.md`).

### Month 6 — Integration & ablation studies

**Tool:** `scripts/ablation_orchestrator.py`
**Status:** shipped (R0/R5 endpoints wired; R1-R4 endpoints stub-routed
to the same gateway pending feature flags).

Runs 6 configurations (R0 baseline through R5 all-layers) against the
shipped iMessage humanness suite. Produces a comparison table with mean
shape, pass rate, length, latency. Computes deltas vs R0.

**Deliverable:** the ablation table for the anonymized writeup.

## Sequencing — what's on the critical path NOW

```
NOW (Month 1):
  ✅ L5 tool shipped         scripts/verifier_ttt.py
  ✅ L3 tool shipped + running  scripts/multiturn_drift_test.py
  ✅ L1 scoring tool          scripts/personaeval_speaker_id.py
  ✅ L1 readiness watcher     scripts/orpo_readiness_watcher.sh
  ✅ L2 tool shipped          scripts/memory_ablation.py
  ✅ L4 tool shipped + tests  scripts/multimodal_policy.py
  ✅ Month 6 orchestrator     scripts/ablation_orchestrator.py
  ⏳ multi-turn results       — running (background task bbwsx8fwo)

Month 2-3 (passive corpus accrual):
  ⏳ orpo_readiness threshold — passive wait for ≥500 real-signal rows
  → cron the watcher        — wire to launchd / cron
  → ORPO training fires automatically when ready
  → channel-scoped adapter swap via existing M3 Bridge B endpoint

Month 4:
  → run memory_ablation.py vs the M4 suite
  → if memory adds no signal → refactor prompt_build.c to RAG retrieval
  → if memory adds signal → ablate field-by-field, prune dead weight

Month 5:
  → port multimodal_policy.py to C as hu_multimodal_policy_decide
  → wire into src/channels/imessage.c before send path
  → A/B vs text-only baseline

Month 6:
  → ablation_orchestrator full run (R0..R5)
  → write up the data
  → anonymize + publish
```

## Measurement protocol

Every layer's "did it help?" decision uses the same three metrics:

1. **Shape score** (deterministic, regex classifier — no LLM judge needed,
   reproducible in CI). Per-channel rules.
2. **PersonaEval P(Seth)** — logistic regression on 15 style features
   from `scripts/personaeval_speaker_id.py`. Score on candidate responses
   to estimate "does this sound like Seth?"
3. **Latency** — total wall time per turn, including any TTT N×
   amplification.

A layer ships only if it moves shape OR P(Seth) by ≥0.05 absolute on the
M4 suite, AND latency stays within product budget (currently ~10s p95 for
async iMessage; tighter for interactive).

## Why "better than human"

| Capability | Human | h-uman with layers |
|---|---|---|
| Draft 5 versions, pick best | No (typing fatigue) | L5 — yes, transparently |
| Match a specific person's voice perfectly | Years of relationship | L1 — adapter trained on your real tapbacks |
| Recall the right thing from 3 conversations ago | Hit-or-miss | L2 — RAG-targeted retrieval |
| Stay in voice over 20 turns at 11pm | No (fatigue, drift) | L3 — measured to hold flat |
| Pick "tapback" vs "voice memo" for the moment | Cultural taste, learned | L4 — codified per-user policy |

Each row is a layer. The composite product is what h-uman ships at end of
Month 6.

## Risks

- **No real tapback corpus.** Mitigated by the readiness watcher: passive
  accumulation, no schedule pressure. ORPO fires when there's enough data,
  not before.
- **Memory ablation shows context IS helping.** Then L2 becomes a
  field-by-field study instead of a refactor — but the test tool was
  designed for that pivot.
- **Multimodal policy ports poorly to C.** Mitigated by predicate
  extraction style — the policy IS data + a switch table, the port should
  be mechanical.
- **PersonaEval logistic regression overfits.** Mitigated by 5-fold cross
  validation in `personaeval_speaker_id.py`. If CV accuracy drops below
  0.75, we treat the scores as unreliable and fall back to shape-only
  ranking.

## Closeout

This plan succeeds when:
- All 5 layers have a shape-OR-P(Seth) lift vs R0 measured on the M4 suite
- The ablation table is published (anonymized)
- The full composite (R5) holds shape ≥0.9 across 20-turn drift test
- One real-world signal: someone says "wait, that sounds exactly like you"
  in a blind A/B I run on myself

## Cross-references

- Layer 0 closeout: `docs/plans/2026-05-18-persona-eval-sota-closeout.md`
- Phase 5 honest status: `docs/plans/2026-05-18-phase5-eval-honest-status.md`
- Adapter routing: `docs/plans/2026-05-18-adapter-routing-decision.md`
- M3 MLX bridge execution: `docs/plans/2026-05-17-m3-mlx-bridge-execution-plan.md`
- Verifier-TTT design: `docs/plans/2026-05-11-init-05-verifier-driven-ttt.md`
- Substring classifier pitfalls rule: `~/.claude/rules/substring-classifier-pitfalls.md`
- Security predicate extraction rule: `.claude/rules/security-predicate-extraction.md`
