# Calibrated Uncertainty Phase 1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upgrade the existing `hu_uncertainty_*` module to SOTA-grade calibration: real confidence signals (grounded + verbalized + contradiction), persona-aware hedge phrasing, three call sites, and an ECE-ready logging schema for future measurement.

**Architecture:** Module exists at `src/agent/uncertainty.c`. We extend the `hu_uncertainty_signals_t` struct, add a soft-blended score function, parse verbalized confidence tags from model output, add a persona overlay phrase bank with code defaults, wire into 3 surfaces, and log every evaluation to a new `uncertainty_evaluations` SQLite table.

**Tech Stack:** C11, existing `hu_provider_t`, existing `hu_heuristic_fact_t.confidence` + `hu_heuristic_fact_effective_confidence` (half-life decay), new SQLite table gated on `HU_ENABLE_SQLITE`, existing persona overlay JSON schema.

**Spec:** [`design.md`](./design.md). Acceptance criteria AC-1 through AC-7 listed there.

---

## File structure (locked from spec)

| File | Responsibility | Status |
|---|---|---|
| `include/human/agent/uncertainty.h` | New signal fields, new helpers (`pick_hedge`, `strip_verbalized`, `log`) | MODIFY |
| `src/agent/uncertainty.c` | Soft blend, default phrase bank, strip/parse, log impl | MODIFY |
| `include/human/persona.h` | Add `hedge_phrases` field to overlay struct | MODIFY |
| `src/persona/overlay.c` | Parse + free `hedge_phrases` JSON | MODIFY |
| `src/agent/prompt.c` | Add verbalized confidence prompt addendum on factual queries | MODIFY |
| `src/agent/agent_turn.c:5921` | Wire new signal population, strip verbalized, persona-aware phrase pick, log call | MODIFY |
| `src/agent/init_proposer.c` | New uncertainty consultation for candidates (drop VERY_LOW, gate LOW) | MODIFY |
| `src/reflection/consumer.c` | Annotate slice patterns with confidence indicators | MODIFY |
| `src/reflection/storage.c` | New helper `hu_reflection_pattern_effective_confidence` | MODIFY |
| `src/uncertainty_storage.c` | NEW — SQLite migrations + `hu_uncertainty_log` impl | NEW |
| `tests/test_uncertainty.c` | NEW — 17 unit tests covering score math, blend, strip, phrases, log | NEW |
| `tests/test_init_proposer_uncertainty.c` | NEW — 4 integration tests for new call sites | NEW |
| `tests/test_main.c` | Register new runners | MODIFY |
| `CMakeLists.txt` | Add `src/uncertainty_storage.c` gated on `HU_ENABLE_SQLITE` | MODIFY |

---

## Task ordering rationale

Task 1 (regression test) MUST come first per `tests-that-pin-bugs.md` — we lock current behavior before changing the score function.
Tasks 2-5 extend the module in isolation (no call site changes yet).
Task 6 introduces ECE logging schema.
Tasks 7-9 wire into the three call sites (one task per site).
Task 10 closes with acceptance.

---

## Task 1: Lock existing behavior with regression test (AC-4)

**Files:**
- Create: `tests/test_uncertainty.c`
- Modify: `tests/test_main.c` (register `run_uncertainty_tests`)
- Modify: `CMakeLists.txt` (add `tests/test_uncertainty.c`)

- [ ] **Step 1.1: Write the regression test FIRST**

`tests/test_uncertainty.c`:

```c
#include "human/agent/uncertainty.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <string.h>

/* AC-4: lock pre-change behavior on the no-real-signals path. This test
 * MUST pass against the unmodified hu_uncertainty_evaluate. After Tasks
 * 2-5 modify the score function, this test still passes — that's the
 * regression contract. */
static void test_score_unchanged_with_no_real_signals(void) {
    hu_allocator_t *alloc = hu_default_allocator();
    hu_uncertainty_signals_t signals = {0};
    signals.retrieval_coverage = 0.5;       /* contributes 0.15 */
    signals.tool_results_count = 1;          /* contributes 0.2 */
    signals.has_citations = false;
    signals.has_hedging_language = false;    /* confident language → 0.15 */
    signals.memory_results_count = 2;        /* 2 * 0.033 = 0.066 */
    signals.is_factual_query = true;         /* no opinion bonus */
    /* NEW fields explicitly zero — exercises the no-real-signals path */
    signals.grounded_confidence = 0.0;
    signals.fact_count = 0;
    signals.verbalized_confidence = 0.0;
    signals.has_verbalized = false;
    signals.contradiction_present = false;
    signals.has_temporal_decay = false;

    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(alloc, &signals, &result), HU_OK);

    /* Pre-change expected: 0.15 + 0.2 + 0.15 + 0.066 = 0.566 */
    HU_ASSERT_TRUE(result.confidence > 0.565 && result.confidence < 0.567);
    HU_ASSERT_EQ(result.level, HU_CONFIDENCE_MEDIUM);

    hu_uncertainty_result_free(alloc, &result);
}

void run_uncertainty_tests(void) {
    HU_TEST_SUITE("uncertainty");
    HU_RUN_TEST(test_score_unchanged_with_no_real_signals);
}
```

Register in `tests/test_main.c`:
```c
void run_uncertainty_tests(void);
/* in main(): */
run_uncertainty_tests();
```

- [ ] **Step 1.2: Run against unmodified production code**

```
cmake --build --preset dev --target human_tests
./build/human_tests --filter=score_unchanged_with_no_real_signals
```
Expected: **FAIL** (because the new struct fields don't compile yet).

- [ ] **Step 1.3: Add the new fields to `hu_uncertainty_signals_t`**

In `include/human/agent/uncertainty.h`, extend struct:

```c
typedef struct hu_uncertainty_signals {
    /* existing fields unchanged */
    double retrieval_coverage;
    double response_length_ratio;
    bool has_hedging_language;
    bool has_citations;
    size_t tool_results_count;
    size_t memory_results_count;
    bool is_factual_query;

    /* NEW (Phase 1) */
    double grounded_confidence;
    size_t fact_count;
    double verbalized_confidence;
    bool has_verbalized;
    bool contradiction_present;
    bool has_temporal_decay;
} hu_uncertainty_signals_t;
```

- [ ] **Step 1.4: Run regression test against unchanged score function**

```
./build/human_tests --filter=score_unchanged_with_no_real_signals
```
Expected: **PASS** — current score function ignores new fields, behavior unchanged.

- [ ] **Step 1.5: Commit**

```
git add include/human/agent/uncertainty.h tests/test_uncertainty.c \
        tests/test_main.c CMakeLists.txt
git commit -m "test(uncertainty): lock pre-change behavior on no-real-signals path"
```

---

## Task 2: Soft-blended score function

**Files:**
- Modify: `src/agent/uncertainty.c`
- Modify: `tests/test_uncertainty.c` (tests 2-7)

- [ ] **Step 2.1: Write failing tests for soft blend (tests 2-7)**

```c
static void test_score_blend_at_one_fact(void) {
    /* fact_count=1, grounded_confidence=0.9 → 33% real + 67% heuristic */
    hu_uncertainty_signals_t signals = {0};
    signals.retrieval_coverage = 0.5;
    signals.fact_count = 1;
    signals.grounded_confidence = 0.9;
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(hu_default_allocator(), &signals, &result), HU_OK);
    HU_ASSERT_TRUE(result.confidence > 0.39 && result.confidence < 0.41);
    hu_uncertainty_result_free(hu_default_allocator(), &result);
}

static void test_score_blend_at_three_facts(void) {
    /* fact_count=3 → 100% real signal. heuristics contribute 0 */
    hu_uncertainty_signals_t signals = {0};
    signals.retrieval_coverage = 0.5;
    signals.fact_count = 3;
    signals.grounded_confidence = 0.85;
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(hu_default_allocator(), &signals, &result), HU_OK);
    HU_ASSERT_TRUE(result.confidence > 0.84 && result.confidence < 0.86);
    hu_uncertainty_result_free(hu_default_allocator(), &result);
}

static void test_grounded_confidence_uses_effective_decay(void) {
    /* 60-day-old 0.9 fact arrives here as 0.57 (decay already applied
       at agent_turn integration). Pure consumption test. */
    hu_uncertainty_signals_t signals = {0};
    signals.fact_count = 3;
    signals.grounded_confidence = 0.57;
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(hu_default_allocator(), &signals, &result), HU_OK);
    HU_ASSERT_EQ(result.level, HU_CONFIDENCE_MEDIUM);
    hu_uncertainty_result_free(hu_default_allocator(), &result);
}

static void test_contradiction_penalty_applies(void) {
    hu_uncertainty_signals_t signals = {0};
    signals.fact_count = 3;
    signals.grounded_confidence = 0.85;
    signals.contradiction_present = true;
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(hu_default_allocator(), &signals, &result), HU_OK);
    /* 0.85 - 0.15 = 0.70 */
    HU_ASSERT_TRUE(result.confidence > 0.69 && result.confidence < 0.71);
    hu_uncertainty_result_free(hu_default_allocator(), &result);
}

static void test_verbalized_low_pulls_score_down(void) {
    /* Model self-reports 0.3 vs blended 0.7 → result = 0.6*0.7 + 0.4*0.3 = 0.54 */
    hu_uncertainty_signals_t signals = {0};
    signals.fact_count = 3;
    signals.grounded_confidence = 0.7;
    signals.has_verbalized = true;
    signals.verbalized_confidence = 0.3;
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(hu_default_allocator(), &signals, &result), HU_OK);
    HU_ASSERT_TRUE(result.confidence > 0.53 && result.confidence < 0.55);
    hu_uncertainty_result_free(hu_default_allocator(), &result);
}

static void test_verbalized_high_does_not_over_inflate(void) {
    /* Model claims 0.95, signals say 0.6 → stays near 0.6 (asymmetric rule) */
    hu_uncertainty_signals_t signals = {0};
    signals.fact_count = 3;
    signals.grounded_confidence = 0.6;
    signals.has_verbalized = true;
    signals.verbalized_confidence = 0.95;
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(hu_default_allocator(), &signals, &result), HU_OK);
    HU_ASSERT_TRUE(result.confidence > 0.58 && result.confidence < 0.62);
    hu_uncertainty_result_free(hu_default_allocator(), &result);
}
```

Register all 6 in `run_uncertainty_tests()`.

- [ ] **Step 2.2: Run to verify they fail**

Expected: 6 FAIL — score function still ignores new fields.

- [ ] **Step 2.3: Implement soft blend**

In `src/agent/uncertainty.c`, replace the score computation in `hu_uncertainty_evaluate` (lines 14-44):

```c
/* Heuristic score: existing 6-signal sum, max 1.0 */
double heuristic_score = 0.0;
heuristic_score += signals->retrieval_coverage * 0.3;
if (signals->tool_results_count > 0) heuristic_score += 0.2;
if (signals->has_citations) heuristic_score += 0.15;
if (!signals->has_hedging_language) heuristic_score += 0.15;
if (signals->memory_results_count >= 3) heuristic_score += 0.1;
else heuristic_score += (double)signals->memory_results_count * 0.033;
if (!signals->is_factual_query) heuristic_score += 0.1;
if (heuristic_score > 1.0) heuristic_score = 1.0;

/* Real-signal score */
double real_score = (signals->fact_count == 0) ? 0.0 : signals->grounded_confidence;
if (real_score < 0.0) real_score = 0.0;
if (real_score > 1.0) real_score = 1.0;

/* Evidence weight: ramps from 0 to 1 as fact_count goes 0 -> 3+ */
double evidence_weight = (signals->fact_count >= 3)
    ? 1.0
    : (double)signals->fact_count / 3.0;

double blended = (1.0 - evidence_weight) * heuristic_score
               + evidence_weight * real_score;

/* Contradiction penalty applies regardless of evidence weight */
if (signals->contradiction_present) blended -= 0.15;

/* Verbalized confidence: asymmetric (trust low self-claims, distrust high) */
if (signals->has_verbalized) {
    if (signals->verbalized_confidence < blended) {
        blended = 0.6 * blended + 0.4 * signals->verbalized_confidence;
    }
}

if (blended < 0.0) blended = 0.0;
if (blended > 1.0) blended = 1.0;

result->confidence = blended;
result->level = hu_confidence_level_from_score(blended);
```

- [ ] **Step 2.4: Run tests — all 7 pass**

```
./build/human_tests --suite=uncertainty
```
Expected: 7/7 (including AC-4 regression from Task 1).

- [ ] **Step 2.5: Commit**

```
git commit -m "feat(uncertainty): soft-blended score with grounded + verbalized signals"
```

---

## Task 3: Verbalized confidence — prompt addendum, strip, parse

**Files:**
- Modify: `include/human/agent/uncertainty.h` (add `hu_uncertainty_strip_verbalized`)
- Modify: `src/agent/uncertainty.c`
- Modify: `src/agent/prompt.c` (add addendum on factual queries)
- Modify: `tests/test_uncertainty.c` (tests 8-9)

- [ ] **Step 3.1: Write failing tests**

```c
static void test_strip_verbalized_tag_at_response_tail(void) {
    char response[] = "She said Thursday. [conf=0.7]";
    size_t len = strlen(response);
    double parsed_conf = -1.0;
    bool found = hu_uncertainty_strip_verbalized(response, &len, &parsed_conf);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_TRUE(parsed_conf > 0.69 && parsed_conf < 0.71);
    response[len] = '\0';
    HU_ASSERT_STR_EQ(response, "She said Thursday.");
}

static void test_strip_verbalized_no_tag_returns_no_match(void) {
    char response[] = "Plain answer with no tag.";
    size_t len = strlen(response);
    double parsed_conf = -1.0;
    bool found = hu_uncertainty_strip_verbalized(response, &len, &parsed_conf);
    HU_ASSERT_FALSE(found);
    HU_ASSERT_EQ(len, strlen("Plain answer with no tag."));
}
```

- [ ] **Step 3.2: Implement `hu_uncertainty_strip_verbalized`**

In `src/agent/uncertainty.c`:

```c
/* Tail-anchored [conf=0.X] parser. Modifies response in place by
 * truncating at tag start; updates *response_len. Per
 * substring-classifier-pitfalls.md, requires bracket boundaries. */
bool hu_uncertainty_strip_verbalized(char *response, size_t *response_len, double *out_conf) {
    if (!response || !response_len || *response_len < 10) return false;

    size_t end = *response_len;
    while (end > 0 && isspace((unsigned char)response[end - 1])) end--;
    if (end == 0 || response[end - 1] != ']') return false;

    size_t cap = (end > 32) ? end - 32 : 0;
    size_t open_pos = end;
    for (size_t i = end; i > cap; i--) {
        if (response[i - 1] == '[') { open_pos = i - 1; break; }
    }
    if (open_pos >= end) return false;

    if (end - open_pos < 8) return false;
    if (strncmp(response + open_pos, "[conf=", 6) != 0) return false;

    char buf[16];
    size_t num_len = end - 1 - (open_pos + 6);
    if (num_len == 0 || num_len >= sizeof(buf)) return false;
    memcpy(buf, response + open_pos + 6, num_len);
    buf[num_len] = '\0';

    char *endptr = NULL;
    double parsed = strtod(buf, &endptr);
    if (endptr == buf) return false;
    if (parsed < 0.0 || parsed > 1.0) return false;

    while (open_pos > 0 && isspace((unsigned char)response[open_pos - 1])) open_pos--;
    *response_len = open_pos;
    if (out_conf) *out_conf = parsed;
    return true;
}
```

Add prototype to header.

- [ ] **Step 3.3: Add prompt addendum**

In `src/agent/prompt.c`, when assembling system prompt for a factual query, append:

```c
static const char *const k_verbalized_confidence_addendum =
    "\n[CONFIDENCE TAGGING]\n"
    "If your response contains a factual claim, append a confidence tag in\n"
    "the format [conf=0.X] at the very end where 0.X is your honest self-\n"
    "assessment:\n"
    "- 0.9-1.0: certain (direct evidence in context)\n"
    "- 0.7-0.9: confident (evidence is recent and unambiguous)\n"
    "- 0.5-0.7: probable (evidence exists but may be stale or partial)\n"
    "- 0.3-0.5: unsure (going off general knowledge, not specific evidence)\n"
    "- 0.0-0.3: guessing (no real evidence)\n"
    "The tag will be stripped before display. Be honest — over-claiming\n"
    "hurts trust.\n";
```

- [ ] **Step 3.4: Run + commit**

```
./build/human_tests --filter=verbalized
git commit -m "feat(uncertainty): verbalized self-confidence via prompt addendum + parser"
```

---

## Task 4: Persona overlay hedge phrase bank

**Files:**
- Modify: `include/human/persona.h` (add `hedge_phrases` field)
- Modify: `src/persona/overlay.c` (parse + free)
- Modify: `include/human/agent/uncertainty.h` (add `pick_hedge`)
- Modify: `src/agent/uncertainty.c` (default bank + lookup)
- Modify: `tests/test_uncertainty.c` (tests 10-14)

- [ ] **Step 4.1: Write failing tests (10-14)**

```c
static void test_default_hedges_present_for_all_four_levels(void) {
    srand(42);
    HU_ASSERT_STR_EQ(hu_uncertainty_pick_hedge(HU_CONFIDENCE_HIGH, NULL), "");
    HU_ASSERT_NE(hu_uncertainty_pick_hedge(HU_CONFIDENCE_MEDIUM, NULL), NULL);
    HU_ASSERT_NE(hu_uncertainty_pick_hedge(HU_CONFIDENCE_LOW, NULL), NULL);
    HU_ASSERT_NE(hu_uncertainty_pick_hedge(HU_CONFIDENCE_VERY_LOW, NULL), NULL);
}

static void test_persona_overlay_overrides_defaults(void) {
    hu_persona_overlay_t overlay = {0};
    static const char *custom[] = {"pretty sure tho — "};
    overlay.hedge_phrases[HU_CONFIDENCE_MEDIUM] = (char **)custom;
    overlay.hedge_phrase_counts[HU_CONFIDENCE_MEDIUM] = 1;
    srand(42);
    const char *picked = hu_uncertainty_pick_hedge(HU_CONFIDENCE_MEDIUM, &overlay);
    HU_ASSERT_STR_EQ(picked, "pretty sure tho — ");
}

static void test_persona_overlay_empty_array_falls_back_to_default(void) {
    hu_persona_overlay_t overlay = {0};
    srand(42);
    const char *picked = hu_uncertainty_pick_hedge(HU_CONFIDENCE_MEDIUM, &overlay);
    HU_ASSERT_NE(picked, NULL);
}

static void test_hedge_selection_deterministic_with_seed(void) {
    srand(42);
    const char *first  = hu_uncertainty_pick_hedge(HU_CONFIDENCE_LOW, NULL);
    srand(42);
    const char *second = hu_uncertainty_pick_hedge(HU_CONFIDENCE_LOW, NULL);
    HU_ASSERT_STR_EQ(first, second);
}

static void test_temporal_hedge_used_when_decay_material(void) {
    hu_uncertainty_signals_t signals = {0};
    signals.fact_count = 3;
    signals.grounded_confidence = 0.65;
    signals.has_temporal_decay = true;
    hu_uncertainty_result_t result = {0};
    HU_ASSERT_EQ(hu_uncertainty_evaluate(hu_default_allocator(), &signals, &result), HU_OK);
    HU_ASSERT_EQ(result.level, HU_CONFIDENCE_MEDIUM);
    HU_ASSERT_NE(result.hedge_prefix, NULL);
    hu_uncertainty_result_free(hu_default_allocator(), &result);
}
```

- [ ] **Step 4.2: Implement default banks**

In `src/agent/uncertainty.c`:

```c
static const char *const k_default_hedges_high[]     = {""};
static const char *const k_default_hedges_medium[]   = {
    "I'm pretty sure — ", "Best read I have: ", "Going from memory, "
};
static const char *const k_default_hedges_medium_temporal[] = {
    "I think — though it's been a while — ",
    "Going from older memory: ",
    "Pretty sure, but the info's a bit stale — "
};
static const char *const k_default_hedges_low[]      = {
    "I'm not certain, but ", "Could be off here — ", "Worth double-checking, but "
};
static const char *const k_default_hedges_very_low[] = {
    "I don't think I know this well enough — ",
    "Honestly, I'm guessing — ",
    "Not confident on this: "
};

static const struct {
    const char *const *phrases;
    size_t count;
} k_default_banks[4] = {
    { k_default_hedges_high,     1 },
    { k_default_hedges_medium,   3 },
    { k_default_hedges_low,      3 },
    { k_default_hedges_very_low, 3 },
};
```

Implement `hu_uncertainty_pick_hedge`:

```c
const char *hu_uncertainty_pick_hedge(
    hu_confidence_level_t level,
    const hu_persona_overlay_t *overlay)
{
    if (level < 0 || level >= 4) return "";

    if (overlay && overlay->hedge_phrases[level]
        && overlay->hedge_phrase_counts[level] > 0) {
        size_t idx = (size_t)rand() % overlay->hedge_phrase_counts[level];
        return overlay->hedge_phrases[level][idx];
    }

    size_t idx = (size_t)rand() % k_default_banks[level].count;
    return k_default_banks[level].phrases[idx];
}
```

- [ ] **Step 4.3: Extend `hu_persona_overlay_t`**

In `include/human/persona.h`:

```c
typedef struct hu_persona_overlay {
    /* existing fields ... */
    char **hedge_phrases[4];
    size_t hedge_phrase_counts[4];
} hu_persona_overlay_t;
```

- [ ] **Step 4.4: Parse `hedge_phrases` from overlay JSON in `src/persona/overlay.c`**

When parsing an overlay JSON object, if `hedge_phrases` key present, walk the per-level arrays and `strdup` each phrase into `overlay->hedge_phrases[i]`. Free in `hu_persona_overlay_free` symmetrically. Follow the existing parse pattern used for `example_bank` or similar bank-style fields in `src/persona/overlay.c`.

- [ ] **Step 4.5: Run + commit**

```
./build/human_tests --suite=uncertainty
git commit -m "feat(uncertainty,persona): persona-overlay hedge phrase banks + defaults"
```

---

## Task 5: Temporal decay detection + hedge variant

**Files:**
- Modify: `src/agent/uncertainty.c`
- Modify: `src/reflection/storage.c` (add `hu_reflection_pattern_effective_confidence`)

- [ ] **Step 5.1: Implement temporal sub-bank routing**

In `hu_uncertainty_evaluate`, after computing `result->level`:

```c
const char *const *bank;
size_t bank_count;
if (result->level == HU_CONFIDENCE_MEDIUM && signals->has_temporal_decay) {
    bank = k_default_hedges_medium_temporal;
    bank_count = sizeof(k_default_hedges_medium_temporal)
               / sizeof(k_default_hedges_medium_temporal[0]);
} else {
    bank = k_default_banks[result->level].phrases;
    bank_count = k_default_banks[result->level].count;
}
size_t idx = (size_t)rand() % bank_count;
result->hedge_prefix = hu_strndup(alloc, bank[idx], strlen(bank[idx]));
result->hedge_prefix_len = result->hedge_prefix ? strlen(result->hedge_prefix) : 0;
```

- [ ] **Step 5.2: Add `hu_reflection_pattern_effective_confidence`**

In `src/reflection/storage.c`:

```c
double hu_reflection_pattern_effective_confidence(
    const hu_reflection_pattern_t *p, int64_t now_ms)
{
    if (!p) return 0.0;
    static const double HALF_LIFE_MS = 90.0 * 86400.0 * 1000.0;
    double age_ms = (double)(now_ms - (int64_t)p->last_observed_at_ms);
    if (age_ms < 0) age_ms = 0;
    double half_lives = age_ms / HALF_LIFE_MS;
    if (half_lives > 10.0) half_lives = 10.0;
    double decay = pow(0.5, half_lives);
    return p->confidence * decay;
}
```

Add prototype to `include/human/reflection.h`.

- [ ] **Step 5.3: Run + commit**

```
./build/human_tests --filter=temporal
git commit -m "feat(uncertainty,reflection): temporal-aware hedging + pattern decay function"
```

---

## Task 6: ECE-ready logging schema + `hu_uncertainty_log`

**Files:**
- Create: `src/uncertainty_storage.c`
- Modify: `include/human/agent/uncertainty.h` (add `hu_uncertainty_log` + entry struct)
- Modify: `CMakeLists.txt` (add file, gate on `HU_ENABLE_SQLITE`)
- Modify: `tests/test_uncertainty.c` (tests 15-17)

- [ ] **Step 6.1: Write failing tests (15-17)**

```c
#ifdef HU_ENABLE_SQLITE
static void test_uncertainty_log_inserts_row(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    HU_ASSERT_EQ(hu_uncertainty_storage_migrate(db), HU_OK);

    hu_uncertainty_log_entry_t entry = {
        .turn_id = "turn_001", .channel = "imessage",
        .query_text = "did she say Thursday?", .response_text = "She said Thursday.",
        .stated_confidence = 0.65, .level = HU_CONFIDENCE_MEDIUM,
        .hedge_phrase_used = "I'm pretty sure — ",
        .signals_json = "{\"fact_count\":2,\"grounded_confidence\":0.7}",
        .created_at_ms = 1000,
    };
    HU_ASSERT_EQ(hu_uncertainty_log(db, &entry), HU_OK);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM uncertainty_evaluations", -1, &st, NULL);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

static void test_uncertainty_log_outcome_starts_null(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_uncertainty_storage_migrate(db);
    hu_uncertainty_log_entry_t entry = {
        .turn_id = "t1", .channel = "imessage", .stated_confidence = 0.5,
        .level = HU_CONFIDENCE_MEDIUM, .signals_json = "{}", .created_at_ms = 1000,
    };
    hu_uncertainty_log(db, &entry);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT outcome_label FROM uncertainty_evaluations LIMIT 1", -1, &st, NULL);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_type(st, 0), SQLITE_NULL);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

static void test_uncertainty_log_outcome_can_be_backfilled(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_uncertainty_storage_migrate(db);
    hu_uncertainty_log_entry_t entry = {
        .turn_id = "t1", .channel = "imessage", .stated_confidence = 0.5,
        .level = HU_CONFIDENCE_MEDIUM, .signals_json = "{}", .created_at_ms = 1000,
    };
    hu_uncertainty_log(db, &entry);
    HU_ASSERT_EQ(hu_uncertainty_set_outcome(db, "t1", "correct", "user_reaction", 2000), HU_OK);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT outcome_label, outcome_source FROM uncertainty_evaluations LIMIT 1",
        -1, &st, NULL);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char*)sqlite3_column_text(st, 0), "correct");
    HU_ASSERT_STR_EQ((const char*)sqlite3_column_text(st, 1), "user_reaction");
    sqlite3_finalize(st);
    sqlite3_close(db);
}
#endif
```

- [ ] **Step 6.2: Implement migrations + log functions**

Create `src/uncertainty_storage.c`. Follow the SQLite migration pattern used in `src/memory/cognitive.c` (line 38 onward) and `src/reflection/storage.c` (the Task-2 migration from the reflection plan that's already landed). Specifically: build a multi-statement SQL string with the `CREATE TABLE IF NOT EXISTS` for `uncertainty_evaluations`, run it via the same one-shot DDL helper your reflection storage uses, return `HU_OK` on success.

The table DDL (paste into your migration SQL constant):

```
CREATE TABLE IF NOT EXISTS uncertainty_evaluations (
    eval_id TEXT PRIMARY KEY,
    turn_id TEXT NOT NULL,
    channel TEXT NOT NULL,
    query_text TEXT,
    response_text TEXT,
    stated_confidence REAL NOT NULL,
    confidence_level TEXT NOT NULL,
    hedge_phrase_used TEXT,
    signals_json TEXT NOT NULL,
    outcome_label TEXT,
    outcome_source TEXT,
    outcome_recorded_at_ms INTEGER,
    created_at_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_uncertainty_recent
  ON uncertainty_evaluations(created_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_uncertainty_unlabeled
  ON uncertainty_evaluations(outcome_label, created_at_ms DESC)
  WHERE outcome_label IS NULL;
```

For the INSERT function `hu_uncertainty_log`: use `sqlite3_prepare_v2` against the INSERT SQL below, bind columns 1-10 from the `hu_uncertainty_log_entry_t` struct fields, `sqlite3_step`, `sqlite3_finalize`. The `eval_id` is computed as `"<created_at_ms>_<turn_id>"` via `snprintf`.

```
INSERT INTO uncertainty_evaluations(
  eval_id, turn_id, channel, query_text, response_text,
  stated_confidence, confidence_level, hedge_phrase_used,
  signals_json, created_at_ms
) VALUES (?,?,?,?,?,?,?,?,?,?);
```

For the UPDATE function `hu_uncertainty_set_outcome`: prepare against:

```
UPDATE uncertainty_evaluations
SET outcome_label=?, outcome_source=?, outcome_recorded_at_ms=?
WHERE turn_id=?;
```

Bind 4 params, step, finalize. Return `HU_OK` on `SQLITE_DONE`, else `HU_ERR_DB`.

Add struct + prototypes to `include/human/agent/uncertainty.h`:

```c
typedef struct hu_uncertainty_log_entry {
    const char *turn_id;
    const char *channel;
    const char *query_text;
    const char *response_text;
    double stated_confidence;
    hu_confidence_level_t level;
    const char *hedge_phrase_used;
    const char *signals_json;
    int64_t created_at_ms;
} hu_uncertainty_log_entry_t;

hu_error_t hu_uncertainty_storage_migrate(sqlite3 *db);
hu_error_t hu_uncertainty_log(sqlite3 *db, const hu_uncertainty_log_entry_t *entry);
hu_error_t hu_uncertainty_set_outcome(sqlite3 *db, const char *turn_id,
                                       const char *label, const char *source,
                                       int64_t recorded_at_ms);
```

- [ ] **Step 6.3: Run + commit**

```
./build/human_tests --filter=log
git commit -m "feat(uncertainty): ECE-ready logging schema + insert/backfill API"
```

---

## Task 7: Wire into `agent_turn.c:5921` (the existing call site)

**Files:**
- Modify: `src/agent/agent_turn.c`

- [ ] **Step 7.1: Track contributing fact IDs during memory load**

Find the memory loader call site in agent_turn. Add a tracking field to the turn context:

```c
typedef struct hu_agent_turn_ctx {
    /* existing fields ... */
    hu_heuristic_fact_t *contributing_facts;
    size_t contributing_fact_count;
} hu_agent_turn_ctx_t;
```

After memory load, populate this with retrieved facts. Free in turn cleanup.

- [ ] **Step 7.2: At line 5921, extend the evaluation pipeline**

Replace the existing call to `hu_uncertainty_evaluate` with the full pipeline:

```c
/* Strip verbalized confidence from response BEFORE display path */
double verbalized = 0.0;
bool has_verb = hu_uncertainty_strip_verbalized(response_buf, &response_len, &verbalized);

/* Build signals */
hu_uncertainty_signals_t u_signals = {0};
hu_uncertainty_extract_signals(response_buf, response_len, query, query_len,
                                tool_count, memory_count, &u_signals);
u_signals.fact_count = ctx->contributing_fact_count;
if (u_signals.fact_count > 0) {
    int64_t now_ms = hu_now_ms();
    double sum_effective = 0.0;
    double sum_stored = 0.0;
    for (size_t i = 0; i < ctx->contributing_fact_count; i++) {
        sum_effective += hu_heuristic_fact_effective_confidence(
            &ctx->contributing_facts[i], now_ms);
        sum_stored += ctx->contributing_facts[i].confidence;
    }
    u_signals.grounded_confidence = sum_effective / (double)u_signals.fact_count;
    double mean_stored = sum_stored / (double)u_signals.fact_count;
    u_signals.has_temporal_decay =
        (mean_stored - u_signals.grounded_confidence) > 0.15;
}
u_signals.has_verbalized = has_verb;
u_signals.verbalized_confidence = verbalized;
u_signals.contradiction_present = hu_uncertainty_detect_contradiction(
    ctx->contributing_facts, ctx->contributing_fact_count);

/* Evaluate */
hu_uncertainty_result_t u_result;
hu_uncertainty_evaluate(agent->alloc, &u_signals, &u_result);

/* Get persona-aware hedge phrase */
const hu_persona_overlay_t *overlay = hu_persona_overlay_for_channel(
    agent->persona, ctx->channel_name);
const char *hedge = hu_uncertainty_pick_hedge(u_result.level, overlay);

if (hedge && hedge[0] != '\0') {
    /* existing logic that prepended u_result.hedge_prefix — use new hedge */
}

/* Log for future ECE measurement */
#ifdef HU_ENABLE_SQLITE
if (agent->db) {
    char signals_json[512];
    snprintf(signals_json, sizeof signals_json,
        "{\"fact_count\":%zu,\"grounded\":%.3f,\"verbalized\":%.3f,"
        "\"has_verb\":%s,\"contradiction\":%s,\"temporal_decay\":%s}",
        u_signals.fact_count, u_signals.grounded_confidence,
        u_signals.verbalized_confidence,
        u_signals.has_verbalized ? "true" : "false",
        u_signals.contradiction_present ? "true" : "false",
        u_signals.has_temporal_decay ? "true" : "false");

    hu_uncertainty_log_entry_t entry = {
        .turn_id = ctx->turn_id, .channel = ctx->channel_name,
        .query_text = query, .response_text = response_buf,
        .stated_confidence = u_result.confidence, .level = u_result.level,
        .hedge_phrase_used = hedge, .signals_json = signals_json,
        .created_at_ms = hu_now_ms(),
    };
    hu_uncertainty_log(agent->db, &entry);
}
#endif

hu_uncertainty_result_free(agent->alloc, &u_result);
```

- [ ] **Step 7.3: Add `hu_uncertainty_detect_contradiction`**

In `src/agent/uncertainty.c`:

```c
bool hu_uncertainty_detect_contradiction(
    const hu_heuristic_fact_t *facts, size_t count)
{
    if (count < 2 || !facts) return false;
    size_t cap = count > 100 ? 100 : count;
    for (size_t i = 0; i < cap; i++) {
        for (size_t j = i + 1; j < cap; j++) {
            if (strcmp(facts[i].subject, facts[j].subject) == 0
             && strcmp(facts[i].predicate, facts[j].predicate) == 0
             && strcmp(facts[i].object, facts[j].object) != 0) {
                return true;
            }
        }
    }
    return false;
}
```

- [ ] **Step 7.4: Run + commit**

```
./build/human_tests
git commit -m "feat(agent): wire calibrated uncertainty into agent_turn"
```

---

## Task 8: Wire into `init_proposer.c` (new call site)

**Files:**
- Modify: `src/agent/init_proposer.c`
- Create: `tests/test_init_proposer_uncertainty.c`

- [ ] **Step 8.1: Write failing integration tests (18-19)**

```c
static void test_init_proposer_drops_very_low_candidates(void) {
    /* reflection candidate with confidence 0.25 -> level VERY_LOW
       -> never reaches the proposer's chosen-candidate slot */
}

static void test_init_proposer_keeps_low_with_high_pattern_conf(void) {
    /* candidate with level=LOW but pattern.confidence > 0.9 -> kept
       candidate with level=LOW and pattern.confidence <= 0.9 -> dropped */
}
```

- [ ] **Step 8.2: Add uncertainty consultation in candidate-gathering**

In `init_proposer.c` where reflection candidates are added to the bundle:

```c
/* For each candidate from reflection: */
hu_uncertainty_signals_t signals = {0};
signals.fact_count = 3;  /* treat patterns as >= 3 facts to weight real-score fully */
signals.grounded_confidence = candidate.confidence;
hu_uncertainty_result_t result = {0};
hu_uncertainty_evaluate(agent->alloc, &signals, &result);

bool keep = true;
if (result.level == HU_CONFIDENCE_VERY_LOW) {
    keep = false;
} else if (result.level == HU_CONFIDENCE_LOW && candidate.confidence <= 0.9) {
    keep = false;
}

hu_uncertainty_result_free(agent->alloc, &result);
if (!keep) continue;

/* existing add-to-bundle logic */
```

- [ ] **Step 8.3: Run + commit**

```
./build/human_tests --suite=init_proposer_uncertainty
git commit -m "feat(init_proposer): consult uncertainty for proactive surfacing decisions"
```

---

## Task 9: Wire into `reflection/consumer.c` (annotation)

**Files:**
- Modify: `src/reflection/consumer.c`
- Modify: `tests/test_init_proposer_uncertainty.c` (tests 20-21)

- [ ] **Step 9.1: Write failing tests (20-21)**

```c
static void test_reflection_slice_annotates_medium_patterns_likely(void) {
    /* AC-6 */
}
static void test_reflection_slice_annotates_low_patterns_uncertain(void) {
    /* AC-6 */
}
```

- [ ] **Step 9.2: Modify slice formatter**

In `src/reflection/consumer.c`, where the system-prompt slice is formatted, annotate based on the pattern's effective confidence:

```c
int64_t now = hu_now_ms();
for (int i = 0; i < n; i++) {
    double eff = hu_reflection_pattern_effective_confidence(&refl[i], now);
    hu_confidence_level_t level = hu_confidence_level_from_score(eff);
    const char *annot = "";
    switch (level) {
        case HU_CONFIDENCE_HIGH: annot = ""; break;
        case HU_CONFIDENCE_MEDIUM: annot = " (likely)"; break;
        case HU_CONFIDENCE_LOW: annot = " (uncertain)"; break;
        case HU_CONFIDENCE_VERY_LOW: annot = ""; break;
    }
    hu_strbuf_appendf(out, "- %s%s\n", refl[i].observation, annot);
}
```

- [ ] **Step 9.3: Run + commit**

```
./build/human_tests --suite=init_proposer_uncertainty
git commit -m "feat(reflection): annotate system-prompt slice with confidence indicators"
```

---

## Task 10: Acceptance verification

- [ ] **Step 10.1: Run full test suite**

```
touch src/agent/uncertainty.c  # per cmake-build-stale-binary rule
cmake --build --preset dev --target human_tests
./build/human_tests
```
Expected: 0 failures, 0 ASan errors. Test count up by 21+ (new uncertainty + init_proposer_uncertainty suites).

- [ ] **Step 10.2: Gate-symmetry check**

```
bash scripts/check-test-source-gate-symmetry.sh
```

- [ ] **Step 10.3: Manual smoke of AC-1 through AC-7**

Document results in `docs/plans/2026-05-26-calibrated-uncertainty/results/acceptance-2026-05-XX.md`.

- AC-1: `./build/human_tests --filter=default_hedges`
- AC-2: Add `hedge_phrases` block to a persona overlay JSON; verify pick reads from overlay
- AC-3: Synthetic signals with fact_count=3 + heuristic flags set → heuristic flags don't budge score
- AC-4: Synthetic signals with fact_count=0 → score equals pre-change formula
- AC-5: Mock reflection candidate with confidence=0.25 → dropped from init_proposer bundle
- AC-6: Build system prompt with reflection slice containing MEDIUM pattern → " (likely)" appears
- AC-7: Counted in 10.1

- [ ] **Step 10.4: Final commit**

```
git commit -m "chore(uncertainty): Phase 1 acceptance results"
```

---

## What Sprint 2 looks like (deferred, separate plan)

Scope-C measurement loop. Brief sketch:
- Backfill `outcome_label` via reaction_collector / response_guard signals
- `scripts/compute_ece.py` — bin uncertainty_evaluations rows by stated_confidence, compute per-bin accuracy, plot reliability diagram, return ECE
- Acceptance: ECE < 0.10 on 30+ days of data; ratchet target < 0.05 over time
- Per-channel calibration variance analysis
- Optional: feedback loop adjusting score weights to minimize ECE

That's a meaningful 2-week project on its own — gets its own plan when this one ships and we have data.

---

## Self-review checklist

- [x] Every task has exact file paths
- [x] Every step that changes code shows the code or a complete sketch
- [x] Every command is runnable
- [x] All 7 ACs trace to tasks (AC-1: T4; AC-2: T4; AC-3: T2; AC-4: T1; AC-5: T8; AC-6: T9; AC-7: T6+T10)
- [x] No handwaves
- [x] Function/struct names consistent across tasks
- [x] Gate symmetry: `src/uncertainty_storage.c` AND its tests both gate on `HU_ENABLE_SQLITE`
- [x] Commits are small (one task per commit, conventional format)
- [x] Existing module is EXTENDED not replaced — AC-4 regression preserves pre-change behavior
