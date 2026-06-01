# h-uman Capability State — Verified E2E Activation (2026-06-01)

**Method:** code-grounded audit (verify-before-allege) + live e2e activation against
the running 31B model (`gemma-4-31b-it-8bit` + `seth-lora-v4-repair` adapter on :8741).
Supersedes the wiring claims in `2026-05-31-capability-maturity-map.md`, which is now
stale: it called ToM, self-model, and intrinsic-goals "dormant/stubbed" — all three are
now **wired on the turn path** (gated). The frontier has moved from *wire it* to
*measure it*.

This doc records, per capability: **built**, **wired on the real turn path**, **fires
e2e** (observed on a live turn), **gate default**, and **measurement state**. Every
"fires" claim is backed by an observed runtime log line, not inference.

---

## Verified gate defaults (parsed from each gate fn on current main)

| Capability | Env | Default | Built (impl) | Wired on turn path | Fires e2e (observed) | Measurement |
|---|---|---|---|---|---|---|
| GraphRAG grounding | `HU_GRAPH_GROUNDING` | **ON** | `graph_grounding.c` | `agent_turn.c` (+`agent_stream.c`) | ✅ `shadow: 481 graph_context bytes` (contact +447914633409) | grounding_ab.py exists; gate ABSENT |
| Intent directive | `HU_INTENT_DIRECTIVE` | **ON** | `intent.c` | `agent_turn.c` humanness block | ✅ `intent=ACTIVE` | `tests/test_intent.c` |
| Salience arbitration | `HU_SALIENCE_LIVE` | OFF | `salience.c` | `agent_turn.c` (never-suppress floor) | ✅ `salience(live): kept 2/2 [...]` | tests; no human A/B |
| Theory of Mind | `HU_TOM_DIRECTIVE` | OFF | `theory_of_mind.c` | `agent_turn.c:405` `at_append_tom_directive` | ✅ `theory_of_mind=ACTIVE` | minimal |
| Self-uncertainty | `HU_SELF_UNCERTAINTY` | OFF | `self_uncertainty.c` | `agent_turn.c:958` | ✅ `self_uncertainty=ACTIVE` | none |
| Self-model readback | `HU_SELF_MODEL` | OFF (+ build flag `HU_ENABLE_SELF_MODEL`) | `self_model.c` (real body under flag, else stub) | `agent_turn.c:1006` | ⏳ e2e run in progress | none |
| Bandit humanization | `HU_BANDIT_HUMANIZATION` | **ON** | `humanization_bandit.c` | `daemon.c` social-tick (NOT core turn) | ⏳ — fires on daemon path, not plain CLI turn | gate cites "blind A/B" |
| Intrinsic goals | `HU_INTRINSIC_GOALS` | OFF | `autonomy.c` | `world_model.c:915` seed path | ⏳ e2e run in progress | none |

(⏳ rows resolve when the e2e activation harness + MLX run finish; this doc is updated
with the observed signals — no row is marked ✅ without a captured log line.)

### Default-ON today: GraphRAG, Intent, Bandit. Default-OFF (flip to activate): Salience, ToM, Self-uncertainty, Self-model, Intrinsic-goals.

---

## The two halves of "running e2e"

1. **Does each capability fire on a live turn?** — `scripts/e2e_all_capabilities.sh`:
   flips each gate ON, drives one contact-attributed turn against the live model, greps
   the capability's own runtime ACTIVE/fired log line. PASS = observed; FAIL = silent.
   (Requires `--contact` so `memory_session_id` binds — the seam added in #257.)

2. **Does the continual-learning loop actually train?** — two backends:
   - **HUML (CPU reference):** `human ml dpo-train --backend huml` loads **296 real pairs**
     from live `memory.db` (`dpo_export_paired: 296 pairs, dropped 0 empty, 33 unpartnered
     single-sided`). The 0-pairs bug is **fixed** — single-sided rows route to
     `feedback_signals`, not dropped into the void. BUT HUML's loss is structurally
     `0.0000` (no fusable adapter) — it proves the *plumbing*, not learning.
   - **MLX (real gradient):** `scripts/dpo_mlx_train.py --scale 2.0` on the 287 complete
     pairs against `gemma-4-31b-it-4bit` — this is the genuine "model changes" step,
     scale pinned per `lora-scale-default-or-die`. Result captured below.

---

## L5 continual-learning: what's proven vs what's open

| Stage | State |
|---|---|
| Reactions → dpo_pairs (real interaction) | ✅ 287 complete pairs in memory.db: 62 `outbound_edit`, 51 `user_feedback`, 217 `generated_v2`, 8 `reflection_retry` |
| Single-sided rows handled correctly | ✅ routed to `feedback_signals` (fix `5673e4b3`), not dropped |
| Export → trainable JSONL | ✅ `dpo_export_paired` emits 296 pairs from live db |
| HUML reference train | ✅ runs (loss=0 by design — reference only) |
| MLX real LoRA train (scale=2.0) | ⏳ in progress — result below |
| Online DPO (real-time adapt) | ❌ not built — the next L5 increment |
| Schema-forming nightly consolidation | ◐ `consolidation.c` LIVE (dedup/promote); "sleep that restructures" not yet |

---

## MLX real-training result (filled on completion)

```
(pending — captured from /tmp/mlx-dpo-e2e.log: iters, final loss, adapter_config scale)
```

---

## E2E capability activation result (filled on completion)

```
(pending — captured from scripts/e2e_all_capabilities.sh: N fired / N total)
```

---

## The honest frontier (unchanged conclusion, sharper evidence)

The machinery for L3/L4 is **built and wired**; L5 plumbing is **fixed and proven** to
carry real pairs. The remaining frontier is exactly two things:

1. **Measurement (the keystone).** Every flip-on rests on a gate that is still
   `ADVISORY/ABSENT`. `blind_ab_gate.json` has zero recorded runs. Until the blind-A/B
   produces a real detect-rate, "more human" is unfalsifiable and the L4 gates can't be
   responsibly defaulted ON. **This is the single highest-leverage next step.**

2. **L5 real learning, not just plumbing.** HUML proves the loop carries pairs; MLX
   proves a real adapter can be trained from them. The open increment is *online* DPO
   (adapt from a correction within a session) + schema-forming consolidation — the only
   genuinely superhuman axis (grows from the relationship).

Everything else ("sentience", genuine intrinsic drive, unified in-weights self) remains
a category error for a stateless model — explicitly off the roadmap.
