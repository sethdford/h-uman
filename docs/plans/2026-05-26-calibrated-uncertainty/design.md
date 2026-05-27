# Calibrated Uncertainty — Design

**Status:** Draft (post-brainstorm 2026-05-27)
**Owner:** Seth
**Mission:** Completes #5 from the "5 highest-leverage moves" list. Lays groundwork for measurable calibration (M4 trust → 100 DAU thesis).
**Sibling specs:** [`2026-05-26-reflection-loop/`](../2026-05-26-reflection-loop/) (reflection pattern confidence is one of the new signal sources this spec consumes).

## Why

`src/agent/uncertainty.c` already exists and computes a 4-level confidence score from six heuristic signals. It's wired to exactly one call site (`src/agent/agent_turn.c:5921`), has zero tests, returns a NULL hedge prefix for LOW and VERY_LOW levels, and uses regex hedging-detection rather than the rich confidence signals that already exist elsewhere in the codebase (`hu_heuristic_fact_t.confidence`, persona fidelity, reflection patterns).

The result: h-uman has *uncertainty quantification* (a number) without *calibration* (a guarantee that the number reflects reality). And the existing quantification is partial — it never reaches half of h-uman's surfaces.

This spec closes the gap with three SOTA techniques that work *without* logit access (a constraint imposed by calling external providers like Vertex/Anthropic that don't expose token probabilities):

1. **Grounded confidence** — replace heuristic regex with mean confidence of facts/patterns that actually grounded the answer. Uses `hu_heuristic_fact_effective_confidence` which already applies half-life decay.
2. **Verbalized self-confidence** — prompt the model to self-report 0-1 confidence and parse it as a third signal. The current SOTA approach for closed-API LLMs (used by OpenAI o-series, Anthropic constitutional models).
3. **Contradiction signal** — when memory retrieval returns conflicting facts about the same (subject, predicate), the disagreement IS uncertainty. New signal field.

Plus an ECE-ready logging schema so future scope-C measurement work plugs in without re-designing storage.

## Goals (Phase 1)

1. Replace the NULL-hedge-for-LOW/VERY_LOW bug with persona-aware phrase banks.
2. Add three new signal sources (grounded confidence, verbalized self-confidence, contradiction) to the existing six heuristic signals via *soft blend* — heuristics dominate when no real signals exist; real signals dominate as evidence accumulates.
3. Wire uncertainty into three surfaces beyond `agent_turn.c`: `init_proposer.c` (gate proactive surfacing), `src/reflection/consumer.c` (annotate system-prompt patterns), and persona overlay phrase selection.
4. Log every uncertainty evaluation with enough fields that future scope-C work can compute ECE (Expected Calibration Error) and reliability diagrams without schema migration.
5. Make the existing module ASan-clean and test-pinned (16+ new tests, deterministic via seeded RNG).

## Non-goals (Phase 1)

- **Reliability diagram / ECE computation** — schema lands now; computation in a follow-up sub-project (we need real production data first).
- **LLM-rephrased hedges** — phrase banks are static + persona-overridable; LLM-paraphrasing of every hedge is scope D, not B.
- **Streaming integration** — hedge prefix emitted at stream start (vs. response complete) is scope C; defer until we measure whether it matters.
- **`response_guard` interaction with confidence** — regenerating low-confidence responses has real cost; scope C.
- **Logit-based confidence** — providers don't expose logits; verbalized confidence is the SOTA workaround.
- **Per-channel calibration thresholds** — defaults are channel-neutral in Phase 1; per-channel tuning happens after we have measurement.

## Architecture overview

```
┌────────────────────────────────────────────────────────────────────┐
│  Existing module: src/agent/uncertainty.c                          │
│                                                                    │
│  hu_uncertainty_extract_signals(response, query, ...)              │
│         ↓                                                          │
│  hu_uncertainty_signals_t {                                        │
│    /* existing heuristic signals (unchanged) */                    │
│    retrieval_coverage, response_length_ratio,                      │
│    has_hedging_language, has_citations,                            │
│    tool_results_count, memory_results_count, is_factual_query      │
│                                                                    │
│    /* NEW (Phase 1) */                                             │
│    grounded_confidence,     // mean effective fact/pattern conf   │
│    fact_count,              // # contributing facts/patterns       │
│    verbalized_confidence,   // parsed from model self-report      │
│    has_verbalized,          // true if model emitted [conf=X]      │
│    contradiction_present,   // memory had conflicting facts        │
│  }                                                                 │
│         ↓                                                          │
│  hu_uncertainty_evaluate(alloc, signals, result)                   │
│         ↓                                                          │
│  hu_uncertainty_result_t {                                         │
│    confidence (soft-blended), level, recommendation,               │
│    hedge_prefix (from persona overlay or default),                 │
│    /* NEW */ contributing_signals_json (ECE-ready audit)           │
│  }                                                                 │
└────────────────────────────────────────────────────────────────────┘
            ↓                ↓                          ↓
    agent_turn.c     init_proposer.c              reflection/consumer.c
    (hedge prefix     (gate by level:              (annotate patterns
     prepended;       VERY_LOW=drop,                with confidence
     stream start)    LOW=require>0.9)              indicators)
```

## The SOTA techniques in detail

### 1. Grounded confidence with effective-decay

The agent's memory loader (in `src/agent/memory_loader.c` per `src/agent/CLAUDE.md`) already retrieves `hu_heuristic_fact_t` instances. Extend the turn context to track which fact IDs contributed to the response, then at uncertainty-evaluation time:

```c
double mean_effective_confidence(
    const hu_heuristic_fact_t *facts, size_t count, int64_t now_ms)
{
    if (count == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < count; i++) {
        sum += hu_heuristic_fact_effective_confidence(&facts[i], now_ms);
    }
    return sum / (double)count;
}
```

Critically: we use the EFFECTIVE confidence (with 90-day half-life decay already applied), not the raw stored confidence. A fact from 60 days ago with stored confidence 0.9 contributes ~0.57 effective — and *that's* the honest signal. This is one of the few SOTA techniques specific to h-uman's existing architecture: the decay function exists, it's tested, just nobody was calling it from this code path.

For reflection patterns (when they're contributing to the system prompt via `hu_reflection_query_for_system_prompt`), the same shape applies — pattern confidence × time-since-`last_observed_at_ms` decay using the same half-life curve. The reflection module ships pattern.confidence; we read it.

### 2. Verbalized self-confidence (SOTA prompt engineering)

For factual queries (when `signals.is_factual_query == true`), the agent's system prompt gets one additional instruction:

> *At the end of your response, if you made a factual claim, append a confidence tag in the format `[conf=0.X]` where 0.X is your honest self-assessment from 0.0 (pure guess) to 1.0 (certain). This tag will be stripped before sending to the user.*

Then `hu_uncertainty_extract_signals` parses for the tag, sets `signals.verbalized_confidence` and `signals.has_verbalized = true`. The tag is stripped from the user-facing response.

This is the published SOTA technique for closed-API LLMs that don't expose logits — referred to in the literature as "verbalized confidence" or "self-reported confidence." Research finding (Tian et al. 2023, Lin et al. 2022): verbalized confidence is **better calibrated than logit-based confidence on factual QA** when the model has been prompted explicitly to self-report. It's the right SOTA primitive for our constraint.

Implementation cost: ~30 lines (prompt addendum, regex parse, strip). Cost in latency: zero (no extra LLM call). Cost in tokens: ~8 output tokens per response on factual queries.

### 3. Contradiction signal

When `memory_loader.c` retrieves facts, it may return multiple facts with the same `(subject, predicate)` but different `object` values:

```
fact_1: Seth | works_at  | Anthropic    (confidence 0.92, asserted Jan 2026)
fact_2: Seth | works_at  | OpenAI       (confidence 0.45, asserted Nov 2025)
```

That contradiction is uncertainty — the agent should hedge harder. New signal: `signals.contradiction_present = true` when ≥2 facts share `(subject, predicate)` with different `object`, weighted by their respective effective-confidences. Acts as a -0.15 penalty in the score function.

This signal is unique to h-uman because it depends on the typed propositional fact extraction (`hu_fact_extract`) that the codebase already ships. Most LLM systems don't have typed memory; this leverages an architectural advantage.

### 4. Soft-blended score function

Replacing the existing equal-weighted sum with a blend that interpolates between heuristics and real signals based on evidence availability:

```c
double evidence_weight = (signals->fact_count >= 3)
    ? 1.0
    : (double)signals->fact_count / 3.0;

double heuristic_score = /* the existing 6-signal sum, max 1.0 */;
double real_score      = /* new computation, max 1.0 */;

double blended = (1.0 - evidence_weight) * heuristic_score
               + evidence_weight * real_score;

/* Contradiction penalty applies regardless of evidence weight */
if (signals->contradiction_present) blended -= 0.15;
/* Verbalized confidence acts as an honesty boost when present and aligned */
if (signals->has_verbalized) {
    /* If model says it's uncertain, trust it (the model knows its own limits) */
    if (signals->verbalized_confidence < blended) {
        blended = 0.6 * blended + 0.4 * signals->verbalized_confidence;
    }
    /* If model is confident but our signals say otherwise, don't over-trust */
}

if (blended < 0.0) blended = 0.0;
if (blended > 1.0) blended = 1.0;
```

The asymmetric handling of verbalized confidence (trust model's low-confidence claims more than its high-confidence claims) is intentional — it matches the SOTA finding that LLMs over-claim confidence on hard questions but under-claim on easy ones. The blend leans toward humility.

### 5. Persona-overlay hedge phrase banks

Extend `hu_persona_overlay_t` with a `hedge_phrases` field:

```json
{
  "channel": "imessage",
  "formality": 0.3,
  "hedge_phrases": {
    "HIGH": [""],
    "MEDIUM": ["pretty sure", "best read I have", "going off memory"],
    "LOW": ["not 100%, but", "could be off — ", "worth double-checking but"],
    "VERY_LOW": ["honestly guessing here —", "I don't really know but"]
  }
}
```

Selection: random pick via `rand() % count` per turn. In tests, `srand(<fixed-seed>)` makes selection deterministic.

Defaults ship in `src/agent/uncertainty.c` as `static const char *const k_default_hedges[4][5]` — 3-4 phrases per level, neutral voice. Overlay overrides defaults when `hedge_phrases` field present.

### 6. Temporal-aware hedging

When `grounded_confidence` differs materially from the *raw* (un-decayed) confidence of the contributing facts (delta > 0.15), the agent has a fact it once knew well but is now stale. Use a *temporal hedge* phrase variant:

- Default MEDIUM: `"I'm pretty sure — "`
- Temporal MEDIUM: `"I think — though it's been a while — "`

Decided by checking the per-fact `created_at` vs current time during signal extraction. New signal field: `signals.has_temporal_decay = true` when the gap is material.

This is genuinely SOTA — most LLM systems don't have time-aware memory in the first place. h-uman's existing half-life decay infrastructure makes this trivial to ship.

## ECE-ready logging schema (for future scope-C measurement)

Every `hu_uncertainty_evaluate` call writes one row to a new `uncertainty_evaluations` table (gated on `HU_ENABLE_SQLITE`):

```sql
CREATE TABLE IF NOT EXISTS uncertainty_evaluations (
    eval_id           TEXT PRIMARY KEY,           -- ulid
    turn_id           TEXT NOT NULL,              -- ref to agent turn
    channel           TEXT NOT NULL,
    query_text        TEXT,                       -- truncated to 256
    response_text     TEXT,                       -- truncated to 512
    stated_confidence REAL NOT NULL,              -- the blended score
    confidence_level  TEXT NOT NULL,              -- HIGH|MEDIUM|LOW|VERY_LOW
    hedge_phrase_used TEXT,                       -- which phrase was selected
    signals_json      TEXT NOT NULL,              -- full hu_uncertainty_signals_t
    -- These fields populated by FUTURE feedback signals (scope C):
    outcome_label     TEXT,                       -- NULL | "correct" | "incorrect" | "abstain"
    outcome_source    TEXT,                       -- NULL | "user_reaction" | "user_contradiction" | "judge_llm"
    outcome_recorded_at_ms INTEGER,
    created_at_ms     INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_uncertainty_recent
  ON uncertainty_evaluations(created_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_uncertainty_unlabeled
  ON uncertainty_evaluations(outcome_label, created_at_ms DESC)
  WHERE outcome_label IS NULL;
```

The `outcome_label` field is the future ECE input. When the reaction collector receives a thumbs-down on a turn, it can backfill this field. When the user contradicts a previous claim, same. When a judge LLM evaluates batches asynchronously, same. The schema is ready; we just don't compute ECE yet.

Future ECE computation (scope C, not this spec):
```
ECE = Σ_bins (n_b / N) * |accuracy_in_bin - mean_stated_conf_in_bin|
```
Bins: [0, 0.2), [0.2, 0.5), [0.5, 0.8), [0.8, 1.0]. SOTA target for well-calibrated systems: ECE < 0.05. We won't claim SOTA-calibrated until we measure.

## Components

### `include/human/agent/uncertainty.h` — MODIFY

- Add new fields to `hu_uncertainty_signals_t`: `grounded_confidence`, `fact_count`, `verbalized_confidence`, `has_verbalized`, `contradiction_present`, `has_temporal_decay`
- Add `contributing_signals_json` to `hu_uncertainty_result_t` (caller frees)
- New public function: `hu_uncertainty_pick_hedge(level, channel, persona_overlay, seed) → const char *`
- New public function: `hu_uncertainty_strip_verbalized(response, response_len, out_stripped_len)` — modifies response in place, returns stripped length, populates `*out_confidence` if found
- New public function: `hu_uncertainty_log(db, eval) → hu_error_t` — write one row to SQLite

### `src/agent/uncertainty.c` — MODIFY

- Add `mean_effective_confidence(facts, count, now_ms)` static helper
- Add `compute_real_score(signals)` static helper (mean of grounded + verbalized + 0.5 baseline if neither, minus contradiction penalty)
- Rewrite `hu_uncertainty_evaluate` to use soft blend (see code in section 4 above)
- Add `k_default_hedges[4][5]` static phrase table
- Implement `hu_uncertainty_pick_hedge`
- Implement `hu_uncertainty_strip_verbalized` (regex parse for `[conf=0.X]` at response tail; strip and return parsed value)
- Implement `hu_uncertainty_log` (INSERT into uncertainty_evaluations)

### `include/human/persona.h` — MODIFY

- Add `hedge_phrases` field to `hu_persona_overlay_t`: `char **phrases[4]` (per-level arrays) + `size_t counts[4]`
- New getter: `hu_persona_overlay_hedge_phrase(overlay, level, seed) → const char *`

### `src/persona/overlay.c` — MODIFY

- Parse `hedge_phrases` JSON object when present in overlay
- Free phrase arrays in `hu_persona_overlay_free`

### `src/agent/agent_turn.c` (existing call site at line 5921) — MODIFY

- Populate `signals.grounded_confidence` and `fact_count` from turn context (which tracks contributing fact IDs during memory load — add the tracking field if not present)
- Call `hu_uncertainty_strip_verbalized` on response before parsing signals; pass stripped response to signal extractor; populate `verbalized_confidence`
- Detect contradictions: scan retrieved facts for shared `(subject, predicate)` with different `object`; set `contradiction_present`
- Call `hu_uncertainty_pick_hedge` instead of using the static result hedge_prefix
- Call `hu_uncertainty_log` to write the eval row

### `src/agent/init_proposer.c` — NEW call site

- For each reflection candidate, build `hu_uncertainty_signals_t` with `grounded_confidence = candidate.confidence`, `fact_count = candidate.observation_count`
- Call `hu_uncertainty_evaluate`
- Hard rule: VERY_LOW → drop candidate from bundle
- Soft rule: LOW → require candidate.confidence > 0.9 to remain in bundle (over-confident at LOW level is suspicious)

### `src/reflection/consumer.c` — MODIFY

- In `hu_reflection_query_for_system_prompt` output formatter, append confidence annotation:
  - HIGH → no annotation (plain bullet)
  - MEDIUM → `" (likely)"`
  - LOW → `" (uncertain)"`
  - VERY_LOW → already filtered by query threshold; should never appear

### `src/reflection/storage.c` — MODIFY (small extension)

- Add `hu_reflection_pattern_effective_confidence(pattern, now_ms)` helper — applies same half-life decay to pattern.confidence as facts get. Returns effective confidence for consumers that need temporal awareness.

### `tests/test_uncertainty.c` — NEW

Full coverage; see Testing strategy below.

### `tests/test_init_proposer_uncertainty.c` — NEW

Integration tests for init_proposer's new uncertainty consultation.

## Reflection prompt addendum (verbalized confidence)

The system prompt assembly in `src/agent/prompt.c` gets one new section, conditionally added when `signals.is_factual_query == true` (detected ahead of generation via the query-prefix heuristic already in the codebase):

```
[CONFIDENCE TAGGING]
If your response contains a factual claim, append a confidence tag in the
format [conf=0.X] at the very end where 0.X is your honest self-assessment:
- 0.9-1.0: certain (you have direct evidence in context)
- 0.7-0.9: confident (evidence is recent and unambiguous)
- 0.5-0.7: probable (evidence exists but may be stale or partial)
- 0.3-0.5: unsure (going off general knowledge, not specific evidence)
- 0.0-0.3: guessing (no real evidence)
The tag will be stripped before display. Be honest — over-claiming hurts trust.
```

This addendum is ~120 tokens; cost is real but bounded. Gate on `is_factual_query` so it doesn't appear on greetings/small-talk turns.

## Failure handling

- **No real signals (`fact_count == 0`, no verbalized):** Falls back to existing heuristic-only path. AC-4 pins this as a regression test.
- **Verbalized tag malformed or absent on factual query:** `has_verbalized = false`; score uses other signals. Not an error.
- **Persona overlay missing `hedge_phrases`:** Use code defaults. AC-1 pins this.
- **Persona overlay has empty `phrases` array for a level:** Use code defaults for that level only. Avoids `rand() % 0`.
- **SQLite write to `uncertainty_evaluations` fails:** Log warning; agent_turn continues (logging is observability, not blocking).
- **Contradiction detection on >100 retrieved facts:** O(n²) scan capped at first 100 facts. Memory loader rarely returns more.

## Testing strategy

### Unit tests (`tests/test_uncertainty.c`):

1. `test_score_unchanged_with_no_real_signals` — AC-4 regression: existing heuristic-only path produces identical score
2. `test_score_blend_at_one_fact` — soft blend with fact_count=1 gives 33% real + 67% heuristic
3. `test_score_blend_at_three_facts` — soft blend with fact_count=3 gives 100% real
4. `test_grounded_confidence_uses_effective_decay` — fact from 60 days ago contributes ~0.57, not its stored 0.9
5. `test_contradiction_penalty_applies` — same (subject, predicate), different object → -0.15
6. `test_verbalized_low_pulls_score_down` — model self-reports 0.3 → blended score drops
7. `test_verbalized_high_does_not_over_inflate` — model self-reports 0.95 but signals say 0.6 → score stays near 0.6
8. `test_strip_verbalized_tag_at_response_tail` — `"Answer text. [conf=0.7]"` → strips tag, returns 0.7
9. `test_strip_verbalized_no_tag_returns_no_match` — `"Plain answer."` → has_verbalized = false
10. `test_default_hedges_present_for_all_four_levels` — AC-1: no NULL phrases
11. `test_persona_overlay_overrides_defaults` — AC-2: overlay phrases win
12. `test_persona_overlay_empty_array_falls_back_to_default` — failure-handling edge
13. `test_hedge_selection_deterministic_with_seed` — `srand(42)` then pick → assert specific phrase index
14. `test_temporal_hedge_used_when_decay_material` — fact aged 60d → temporal phrase variant
15. `test_uncertainty_log_inserts_row` — SQLite write with correct fields
16. `test_uncertainty_log_outcome_field_starts_null` — ECE-ready: outcome_label NULL on initial insert
17. `test_uncertainty_log_outcome_can_be_backfilled` — UPDATE works for future scope-C feedback

### Integration tests (`tests/test_init_proposer_uncertainty.c`):

18. `test_init_proposer_drops_very_low_candidates` — AC-5
19. `test_init_proposer_keeps_low_with_high_pattern_conf` — soft rule: LOW level only blocked if pattern.confidence ≤ 0.9
20. `test_reflection_slice_annotates_medium_patterns_likely` — AC-6
21. `test_reflection_slice_annotates_low_patterns_uncertain` — AC-6

### Gate symmetry:

- `src/agent/uncertainty.c` is NOT gated on `HU_ENABLE_SQLITE` (existing code isn't), but the new `hu_uncertainty_log` function IS — and `uncertainty_evaluations` table only exists when SQLite is enabled. The log function compiles to a no-op stub when SQLite is off. Tests for the log function gate on `HU_ENABLE_SQLITE`.

## Sprint sequencing

**Day 1-2 — Lock existing behavior:** Tests 1, 10-12, 13 first. AC-4 regression test must pass before any score changes. Pin the current production behavior in tests so we know if we regress it.

**Day 2-3 — Add new signal fields + soft blend:** Tests 2-7. Implement the new fields, the soft blend math, the contradiction detection. No call site changes yet.

**Day 3 — Verbalized confidence:** Tests 8-9. Prompt addendum, strip function, integration with signal extraction.

**Day 4 — Persona overlay phrases:** Persona schema extension, parser, defaults, lookup function. Tests 10-14.

**Day 5 — ECE-ready logging:** Migrations, hu_uncertainty_log, tests 15-17.

**Day 6 — Wire into 3 call sites:** agent_turn (existing site, refactor to use new pieces), init_proposer (new site), reflection consumer (new annotations). Tests 18-21.

**Day 7 — Acceptance verification + manual smoke:** Run all 7 ACs, document results.

## Acceptance criteria

- **AC-1:** All four confidence levels return a non-NULL hedge phrase from `hu_uncertainty_pick_hedge` when no persona overlay is provided (defaults guarantee)
- **AC-2:** Persona overlay with `hedge_phrases` field overrides defaults for that channel
- **AC-3:** With `grounded_confidence > 0` AND `fact_count >= 3`, heuristic regex signals contribute 0% to final score (soft blend at full saturation)
- **AC-4:** With `fact_count == 0` AND `has_verbalized == false` AND `contradiction_present == false`, the score equals the pre-change value bit-for-bit (regression test for existing call site)
- **AC-5:** A reflection pattern with VERY_LOW confidence level is dropped by `init_proposer` and never appears in the candidate bundle
- **AC-6:** Reflection slice in system prompts annotates MEDIUM patterns with `" (likely)"` and LOW patterns with `" (uncertain)"`
- **AC-7:** 21+ new tests pass, 0 failures, 0 ASan errors. Gate-symmetry check passes (`src/reflection/storage.c` extensions test-gated correctly).

## Risks

- **R1 — Score regression breaks existing behavior on no-memory turns.** *Mitigation:* AC-4 is a bit-for-bit regression test pinning current behavior on the no-real-signals path. CI-blocking.
- **R2 — Verbalized confidence tag leaks to user.** *Mitigation:* `hu_uncertainty_strip_verbalized` is called BEFORE any response display path. Test 8 covers the strip. Defense in depth: response_guard could be extended in a follow-up to flag any `[conf=` substring in outbound text.
- **R3 — Persona overlay schema evolution breaks existing personas.** *Mitigation:* `hedge_phrases` is OPTIONAL; missing field → use defaults. Persona-parser tests AC-1 explicitly. Existing Tier-1 persona JSON unchanged.
- **R4 — Contradiction signal is too aggressive (penalizes legitimate updates).** *Mitigation:* The penalty is -0.15 (one notch on the confidence scale, not a level jump). If user updated their job and the old fact is stale-but-not-retired, the agent hedges slightly — that's correct behavior. Future scope-C work could add fact-retirement awareness to skip the penalty when one of the conflicting facts has been explicitly retired.
- **R5 — ECE logging fills disk on heavy use.** *Mitigation:* Migration includes vacuum guidance in the comments. Realistic volume: ~50 turns/day × ~2KB per eval = ~100KB/day = ~36MB/year. Negligible.
- **R6 — Verbalized confidence prompt addendum may bias the model toward over-hedging.** *Mitigation:* The instruction explicitly says "be honest — over-claiming hurts trust" with calibrated bands (0.5-0.7 = "probable" with examples). Once scope-C measurement lands, we can iterate the prompt against actual ECE numbers.
- **R7 — Test 13 (`srand(42)` deterministic) is fragile across libc implementations.** *Mitigation:* Use h-uman's own RNG if one exists; otherwise wrap libc rand in `hu_rng_*` for test-time deterministic substitution. Search `grep -rn "hu_rng\|hu_random" include/ src/` before relying on libc rand.

## Open questions

1. **Should the verbalized confidence prompt addendum gate on persona formality?** Casual iMessage with family might not want `[conf=0.7]` tagging behavior even internally — risks the model emitting the tag in non-factual chat. *Default:* gate on `is_factual_query` only (already in the code path). Revisit if false-positive verbalized confidences appear in chitchat.
2. **Should contradiction detection use word-boundary matching on `object` (per `substring-classifier-pitfalls.md`)?** Currently we'd be comparing exact strings. *Default:* exact `strcmp`; if false-positives appear (e.g., "Anthropic" vs "Anthropic, PBC" being treated as different), upgrade to normalized comparison.
3. **Should the ECE schema include the persona overlay used?** Per-persona calibration is a future scope-C concern. *Default:* include `channel` (already in schema) which proxies for persona. Add `persona_id` field only if measurement reveals per-persona variance matters.

## Related rules

- `~/.claude/rules/security-predicate-extraction.md` — the existing `hu_uncertainty_evaluate` already follows this pattern (pure data in, pure data out). New helpers (`mean_effective_confidence`, `compute_real_score`, `pick_hedge`) follow the same shape.
- `~/.claude/rules/substring-classifier-pitfalls.md` — `hu_uncertainty_strip_verbalized` regex parse must use word-boundary checks (the tag must be `[conf=0.X]` with bracket boundaries, not loose substring match that would catch e.g., "(conf=0.7 in our test)" embedded in chat).
- `~/.claude/rules/silent-config-gated-subsystems.md` — `hu_uncertainty_log` emits a one-shot warning if SQLite is disabled (logging falls back to no-op).
- `.claude/rules/tests-that-pin-bugs.md` — Test 1 (AC-4) is a *positive contract* pinning current production behavior. Critical: test the BEHAVIOR we want preserved, not the code path. If the regression test would pass when behavior subtly changes (e.g., score is "close to" pre-change but not bit-equal), it doesn't actually pin the bug.
- `.claude/rules/test-source-gate-symmetry.md` — log function and its test gate on `HU_ENABLE_SQLITE` identically.
- `.claude/rules/asan-pthread-stack-aliasing-darwin.md` — uncertainty_log writes happen on the agent_turn thread, which is the main thread; no cross-thread pointer aliasing risk.
- `.claude/rules/classifier-score-plus-flag-gate.md` — the score-AND-flag composition pattern; uncertainty's verbalized + heuristic + grounded triple is conceptually similar to that rule's gate composition.
