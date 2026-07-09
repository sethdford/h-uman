# Coherence Arbitration — Making h-uman Speak With One Voice

**Date:** 2026-05-30
**Status:** Plan (approved for implementation pending Seth's calls on §10)
**Author:** Planning team `coherence-arbitration` (5 read-only specialists + lead synthesis)
**Scope:** Gap 1 (Coherence) as the primary lever, with Gap 2 (Grounding) and Gap 3 (Measurement) wired in as dependencies — because the coherence change is unprovable without measurement and ungrounded without calibration.

---

## 0. TL;DR

h-uman computes ~18–25 behavioral signals per turn and emits 10–15 of them simultaneously. Real people surface **one coherent thread**. That over-stuffing is the loudest "AI tell" in the system and the largest single contributor to the "feels like 5%" gap.

The fix is **subtraction, not construction**, and most of the machinery already exists:

- `src/agent/arbitrator.c` already implements priority scoring + selection. It is **called 0 times** from the live path (`agent_turn.c`). We wire and feed it.
- `include/human/agent/prompt_budget.h` already reserves a config block that "gates the future trim." We plug in there.
- `src/calibration/*` already extracts Seth's style/timing fingerprint. It runs CLI-only; we add a daemon tick so it feeds the arbitrator continuously.

**North star (measurable):** people who know Seth, shown arbitration-on messages mixed with his real texts, pick the bot at **~chance (50%)**. Not "Turing score 18/18" — that's the proxy. Blind human discrimination is the ground truth.

**Approach:** ship behind a flag in **shadow mode first** (compute the selection, log what it *would* suppress, change nothing), validate selection quality against the measurement harness, then flip to live.

---

## 1. The SOTA North Star (what "done" means)

| Tier | Metric | Source | Target |
|---|---|---|---|
| Proxy (cheap, every run) | 18-dim heuristic Turing score | `src/eval/turing_score.c` | No regression; ideally ↑ on NATURAL_LANGUAGE / APPROPRIATE_LENGTH |
| Proxy (LLM judge) | Persona-fidelity 1–5 | `src/eval/persona_fidelity.c` | Baseline → +0.10 with non-overlapping bootstrap CI |
| **Ground truth** | **Blind human A/B discrimination** | New protocol (§6.4) | Raters who know Seth pick the bot at **≤60%, trending to 50%** |
| Guardrail | Latency p50/p95 | turn instrumentation | No >25% regression (arbitration is heuristic, not LLM-per-directive) |

Honesty clause (§11): this plan does **not** deliver "sentient" or "AGI." It delivers *indistinguishable-from-Seth-to-his-contacts*, which is the only humanness target that is both measurable and the actual product.

---

## 2. Problem Statement (empirical, verified)

`hu_agent_turn` (`src/agent/agent_turn.c:773`) assembles behavioral signals across **three accumulation tiers**, none relevance-ranked:

**Tier 1 — `hum_buf[4096]`** (`agent_turn.c:~2619–2761`): 6 directives append first-come-first-served until the 4KB buffer fills. **Silent truncation** — `if (hum_pos + dir_len + 2 < sizeof(hum_buf))` skips the tail with no log/counter (`:2634,2658,2679,2705`). Likely loser of the race: evolved-opinions (4th in line).

**Tier 2 — 12 "Frontiers"** (`:2771–3362`): **8 fire unconditionally every single turn** regardless of the message — somatic, narrative_self, attachment, presence, micro_expression, creative_voice, growth_narrative, trust_calibration — each injected as its own `hu_prompt_config_t` field. **This is the biggest over-stuffing source.**

**Tier 3 — ~14 more context builders** (STM, emotional, goals, commitment, …).

All flow into `hu_prompt_config_t` (~50 fields) → `hu_prompt_build_system` (`:3877`) → capped at **16KB with arbitrary newline-boundary truncation** (`:3894`). On a typical turn from an established contact: **~3.9–6.2 KB of behavioral directives co-fire**, ~10–15 distinct signals, no ranking by relevance to *this* message.

> Verified 2026-05-30: `hu_arbitrator_select` / `hu_directive_compute_priority` defined in `arbitrator.h:47,61`; grep count in `agent_turn.c` = **0**. The arbitration capability exists and is unwired.

---

## 3. Key Reframe — Integration, Not Invention

The recurring h-uman pattern (capability computed but not wired) holds here. Three assets already built:

1. **The arbitrator** — `src/agent/arbitrator.c:18–32` (`hu_directive_compute_priority`: recency 0.30, emotional_weight 0.25, relationship_depth 0.15, conversation_phase 0.15, novelty 0.15) and `:98–226` (`hu_arbitrator_select`: greedy selection with required-floor, token budget, max-directives). **Caveat:** written but never run in production → its weights are untuned against live data (Risk R7).
2. **The trim hook** — `prompt_budget.h` config block "gates the future trim" (anticipated, unbuilt).
3. **The fingerprint** — `src/calibration/{style_analyzer,timing_analyzer,calibrate}.c` extract Seth's measured style/timing. CLI-only today; not looped.

---

## 4. The Design (condensed from architect blueprint)

**One mechanism:** canonicalize every behavioral signal (Tier 1 + Tier 2 + selected Tier 3) into a single rankable list, score each by the existing arbitrator formula **modulated by Seth's fingerprint**, select top-k with a never-suppress floor, suppress the rest — Frontiers gated behind a flag so tests still pass.

### 4.1 Unified candidate model (new — `include/human/agent/salience.h`)

```c
typedef struct hu_directive_candidate {
    char *content; size_t content_len;
    hu_directive_source_t source;       /* 18 sources: hum_buf 6 + frontiers 8 + tier3 4 */
    uint32_t category;                  /* SAFETY | EMOTIONAL | MEMORY | PROACTIVE | IDENTITY ... */
    double raw_priority;                /* before Seth modulation */
    double seth_modulated_priority;     /* after */
    size_t token_cost;
    bool required;                      /* never-suppress floor */
    bool is_frontier;                   /* gated suppression for test-compat */
} hu_directive_candidate_t;
```

### 4.2 Scoring (default = cheap heuristic; LLM-judge is an opt-in upgrade)

```c
double hu_salience_score_candidate(const hu_directive_candidate_t *c,
                                   const hu_salience_seth_profile_t *profile,
                                   uint64_t turn_count, hu_allocator_t *alloc);
/* raw = hu_directive_compute_priority(category, recency, emotional_weight,
                                       relationship_depth, conv_phase, novelty)
   seth = profile->source_weights[source] * profile->recency_bins[bin].mult
   return min(raw * seth, 1.0)  — pure function, zero LLM calls on happy path */
```

### 4.3 Selection policy

- `max_directives` 7 → **5**; `max_directive_tokens` 1500 → **1200**.
- **Never-suppress floor** (`hu_salience_is_never_suppress`): `SAFETY | DIRECT_QUESTION | EMOTIONAL_CRISIS` always pass. This is the single most important safety control (Risks R1/R2).
- `allow_concurrent_frontiers = false` → **max 1 Frontier per turn** (this *is* the "one coherent thread" lever).
- Budget-aware: selection respects the 4KB Tier-1 and 16KB total ceilings *explicitly*, replacing silent truncation.

### 4.4 Modes (the rollout dial)

```
OFF      → all candidates pass (current behavior; default)
SHADOW   → compute selection, LOG what it would suppress, still emit everything
FLAGGED  → apply selection; Frontiers conditional on flag
DEFAULT  → apply selection fully
```

---

## 5. Measurement & Validation Harness (Gap 3 — makes the win falsifiable)

Verified state: scoring is **mostly real, not mocked**. Heuristic Turing always runs (`turing_score.c:55`); persona-fidelity uses a live judge unless `--canned` (`persona_fidelity.c:56`); only the **leaderboard cache** is `HU_IS_TEST`-gated (`leaderboard.c:86–109`). The doc's "fixed scores when `CI` env set" claim is **UNCONFIRMED** — no `getenv("CI")` found in eval sources. Trust the code, fix the doc.

1. **Real baseline (now):** `human eval baseline eval_suites/human_likeness.json --out-json baseline-before.json --seed 42` against a real provider + ~30–50 real inbound messages. ~$3–8. Produces fidelity mean + bootstrap CI (`bootstrap_ci.c:70`).
2. **Before/after:** identical fixed context set, `salience.mode=off` vs `salience.mode=default`. Win = treatment CI-lower > control CI-upper (`competitive_harness.c:59`). At n=30, ~0.15 effect is detectable.
3. **Blind human A/B (ground truth):** 5–8 raters who know Seth; 10–15 (control,treatment,real-Seth) triples each, shuffled; "which is most like Seth?" + 1–5 confidence. Pass = treatment chosen ≥ control by >10pts AND bot-detection trending to chance. Wilson CI on the proportion.
4. **Regression gate:** wire `eval_gate.c` floor into CI so coherence work cannot silently lower fidelity (floor = baseline − 0.05). `--no-verify` requires a written reason.

---

## 6. Grounding Tie-In (Gap 2 — rank by "what would *Seth* foreground")

Closed-loop state: **UNCONFIRMED-but-partial.** Per-turn style adaptation (`style_adapter.c:53–83`) and milestone re-analysis (`style_learner.c`, every 50th turn after 100) exist, but the calibration analyzers are **CLI/fixture-only — not looped per turn.**

To make salience rank by Seth (not generic-human), feed the scorer a cached profile:

```c
typedef struct hu_salience_seth_profile {
    hu_salience_source_weight_t source_weights[HU_DIRECTIVE_SOURCE_MAX]; /* what Seth foregrounds */
    hu_salience_recency_bin_t   recency_bins[5];                        /* how fast he forgets */
    /* + reaction signature, per-channel style, per-source timing */
    int64_t calibrated_at_unix;
} hu_salience_seth_profile_t;
```

Refreshed by a **24h daemon tick** calling existing `hu_calibrate_with_model(...)` → `~/.human/salience/seth-<date>.json`, loaded at agent start (atomic tmp+mv, read-only per turn). **Gap to close the full loop:** log predicted directives vs. what Seth actually sent (a `directive_audit_log` table) and feed mismatches back to rebalance `source_weights` — that's the continuous learning loop, scoped as a fast-follow after shadow validation.

---

## 7. Risk Register (top risks; full 14-row register in team task #5)

| # | Risk | Sev | Confirmed? | Mitigation |
|---|---|---|---|---|
| R1 | Suppressing a grief/loss/conflict signal → bot reads cold | HIGH | Confirmed (`agent_turn.c:2623–2643`) | Never-suppress floor; test: deceased-contact reference must reach prompt regardless of score |
| R2 | Suppressing absence-detection when a direct question is unanswered | HIGH | Confirmed (`:2670–2689`) | `DIRECT_QUESTION` in floor; test "How was your day?" must survive |
| R3 | LLM-per-directive scoring blows the 50–200ms latency budget | HIGH | Speculative | Default scorer is pure heuristic, **zero LLM calls**; reject if p95 +>25% |
| R4 | Double-trim: salience suppresses, then `prompt_budget` Phase-2 drops the now-thin field | HIGH | Confirmed (`prompt_budget.h:20`) | Integration test: `humanness_ctx` field never dropped by suppression; define ordering |
| R5 | `hum_pos` corruption if scoring mutates append logic | CRITICAL | Speculative | Scorer is a **pure** function; append after scoring; ASan fuzz, assert `hum_pos==byte_count` |
| R7 | Arbitrator priority formula untested in prod (double-weights recency 0.30) | HIGH | Confirmed (`arbitrator.c:18–31`) | Validate on 50 hand-built scenarios + adversarial "old deceased contact + high emotional_weight must score >0.7 despite low recency" |
| R9 | Suppressing the 8 Frontiers breaks tests that assert their presence | HIGH | Confirmed (`test_humanness_frontiers.c:1214L`, `test_agi_frontiers.c:1625L`) | Flag OFF by default + `required=true` shim + refactor content-assertions to shape-assertions |
| R12 | Proxy overfit: tuning on heuristic-Turing (`ai_tells` count), not real humanness | HIGH | Speculative | **Never tune on heuristic-Turing alone**; monthly human-judge sample; weekly per-source suppression stats; roll back if proxy ↑ but human-judge flat |

The pair R7+R12 is the deepest trap: a written-but-untuned formula tuned against a proxy. Mitigation is procedural — validate the formula on hand-built scenarios *before* wiring, and gate every promotion on the human-judge sample, not the heuristic.

---

## 8. Phased Build Sequence (task-sized per `agent-task-sizing.md`: ≤8 sites/unit, build+test separate)

| Phase | Deliverable | New/Modified | Verify contract |
|---|---|---|---|
| **P0** | **Real baseline number** (do this FIRST) | run `eval baseline` | A committed `baseline-before.json` with fidelity ± CI. "5%" becomes a number. |
| P1 | Core scoring + never-suppress floor | `salience.{h,c}` | Unit: each scoring dim ±0.1 on 3 scenarios; floor always passes SAFETY/DIRECT_Q/CRISIS |
| P2 | Seth profile load/save/default | `salience_profile.{h,c}` | Round-trip; missing-file → default profile, no crash |
| P3 | Directive canonicalization | `directive_canonicalize.{h,c}` | 18 sources → candidate[]; token costs correct |
| P4 | Wire into `agent_turn.c` (SHADOW only) | edit `agent_turn.c`, `CMakeLists.txt` | OFF/SHADOW mode: byte-identical output to today; shadow log written |
| P5 | Shadow logging + observer events | `salience_shadow.c` | Suppressed set logged; no user content in logs |
| P6 | 24h calibration tick | `daemon_calibration_salience.c` | Profile regenerated from turn history; daemon loads latest |
| P7 | Comprehensive tests | `tests/test_salience.c` | Full suite green; R1/R2/R5/R9 each pinned by a test |
| P8 | Rollout SHADOW→FLAGGED→DEFAULT | config flag | Each stage: before/after eval + human A/B sample hold or improve |

Sequencing rule: **P0 before everything** (measure first). P4 lands in SHADOW so nothing changes behavior until P8 validates. Frontier gating (R9) is the riskiest edit — isolate it in P4 behind the flag.

Each implementation unit is a separate agent/session per task-sizing discipline; build+test is the lead's job after merge (worktree-merge-before-cleanup).

---

## 9. Contracts to Verify (Definition of Done per phase)

- Behavior unchanged in OFF/SHADOW (P4): diff of `humanness_ctx` on a 100-turn fixture corpus = empty.
- Floor holds (P1/P7): grief, direct-question, and crisis directives reach the prompt at any salience score.
- No silent loss (P4): every suppressed directive is logged; `hum_buf` never silently truncates post-change.
- No latency regression (P8): p50/p95 within 25% of baseline.
- Provable win (P8): persona-fidelity +0.10 non-overlapping CI **and** human raters trending to chance — both, or it doesn't ship to DEFAULT.

---

## 10. Open Decisions Needing Seth's Call (each has a recommended default)

1. **Max Frontiers per turn** — *default: 1* ("one coherent thread"). Alternatives: 2 for richer turns.
2. **Recency decay curve** — *default: 5-bin exponential* `[1.0,0.9,0.7,0.5,0.2]`.
3. **Per-contact vs. global profile** — *default: one global Seth profile + per-contact modulation via existing `personality_match_score`.*
4. **Emotional-residue suppression** — *default: never-suppress under CRISIS; fade allowed after 48h.*
5. **LLM-judge scoring** — *default: OFF* (heuristic only; opt-in for research).
6. **Rollout aggressiveness** — *default: 7+ days in SHADOW before FLAGGED.*

---

## 11. What This Plan Does NOT Solve (honesty)

- **Sentience** — not an engineering deliverable; out of scope by definition.
- **AGI / general intelligence** — wrong target for a persona agent; we optimize *one specific human*, narrower and harder where it counts.
- **Continuity of self** (lived autobiographical thread, real stakes) — simulated by `life_sim.c`/`narrative_self.c`, re-instantiated per turn. The research asymptote; Gaps 1–3 reach ~60–70% perceived humanness without it.

Coherence is the highest-leverage *tractable* step toward the measurable north star. Everything here is integration of existing capability + a scoreboard.

---

## Appendix — Team Artifacts

| Task | Specialist | Output |
|---|---|---|
| #1 | inventory-cartographer | 3-tier roster, 8 always-on Frontiers, 4KB silent-truncation finding |
| #2 | arbitration-architect | full blueprint: candidate model, scorer, selection, 8 phases, files |
| #3 | measurement-architect | real-vs-mock map, baseline cmd, before/after + blind A/B + gate |
| #4 | grounding-liaison | calibration surface, seth_profile interface, loop-closure gaps |
| #5 | risk-critic | 14-row risk register with confirmed test-file regression surface |
