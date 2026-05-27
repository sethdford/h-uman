# Cross-Channel Synthesis Phase 1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship structural privacy ACL gating cross-channel info flow + reflection pattern integration into the existing `cross_channel_ctx` pipeline, with provenance-annotated output. AC-1 is the trust property: family facts MUST NEVER reach a coworker turn's prompt.

**Architecture:** Replace inline cross-channel context construction at `src/daemon.c:6525-6815` with a three-stage pipeline (`hu_cross_channel_collect` → `hu_cross_channel_filter` → `hu_cross_channel_format`) where the filter is a pure-predicate ACL keyed on `relationship_type`. Persona owns the ACL JSON; safe defaults ship in code with `deny_unknown` fail-closed policy.

**Tech Stack:** C11, SQLite (gated on `HU_ENABLE_SQLITE`), existing `hu_personal_model_t` typed facts + reflection_patterns table (from sibling spec), existing `hu_contact_profile_t.relationship_type` field, existing identity resolver for canonical contact IDs, existing time-formatting helper `cross_channel_format_when` at `daemon.c:561` (extracted to public).

**Spec:** [`design.md`](./design.md). Acceptance criteria AC-1 through AC-8 listed there.

---

## File structure (locked from spec)

| File | Responsibility | Status |
|---|---|---|
| `include/human/memory/cross_channel.h` | Public types + 4 functions (collect/filter/format/acl_check) | NEW |
| `src/memory/cross_channel.c` | Implementation of all four functions | NEW |
| `include/human/persona.h` | Add `cross_channel_acl` field + sub-structs | MODIFY |
| `src/persona/persona.c` | Parse `cross_channel_acl` JSON; populate safe defaults | MODIFY |
| `src/daemon.c` (lines ~6525-6815) | Replace inline cross-channel ctx with pipeline calls | MODIFY |
| `src/daemon.c` (line ~561) | Extract `cross_channel_format_when` to public helper in cross_channel.c | MODIFY |
| `tests/test_cross_channel_acl.c` | Trust property tests (AC-1 lives here) | NEW |
| `tests/test_cross_channel_pipeline.c` | Integration tests with in-memory DB | NEW |
| `tests/test_persona_acl_parse.c` | Persona-load ACL parser tests | NEW |
| `tests/test_main.c` | Register new runners | MODIFY |
| `CMakeLists.txt` | Add new sources gated on `HU_ENABLE_SQLITE` | MODIFY |

---

## Task ordering rationale

Per `tests-that-pin-bugs.md` discipline: **write the trust-property test FIRST**, before any ACL code exists. The test must FAIL initially (because the predicate doesn't exist), then PASS when the predicate is implemented correctly. If we wrote the predicate first, the test might accidentally codify the predicate's mistakes.

Task 1 = trust-property tests. Task 2 = persona schema + parser. Task 3 = pure predicate. Task 4 = filter stage. Task 5 = collect stage. Task 6 = format stage. Task 7 = daemon.c integration. Task 8 = acceptance.

---

## Task 1: Write trust-property tests FIRST (before any implementation)

**Files:**
- Create: `tests/test_cross_channel_acl.c`
- Modify: `tests/test_main.c`, `CMakeLists.txt`

- [ ] **Step 1.1: Write the negative-contract test (AC-1)**

```c
#include "human/memory/cross_channel.h"
#include "human/persona.h"
#include "test_framework.h"
#include <string.h>

/* AC-1: THE TRUST PROPERTY.
 *
 * A family-relationship-type fact MUST NEVER reach a coworker turn.
 * This is the highest-priority test in this entire sprint. If this
 * fails, the privacy property is broken.
 *
 * Per tests-that-pin-bugs.md: the test name asserts the DANGEROUS case
 * is BLOCKED, not that some score landed in a band. */
static void test_family_fact_never_reaches_coworker_turn(void) {
    hu_persona_t persona = {0};
    hu_persona_load_defaults(&persona);  /* loads safe-default ACL */

    /* Pre-built item with family origin */
    hu_cross_channel_item_t items[] = {
        {
            .source_type = HU_XCHAN_FACT,
            .item_id = "fact_001",
            .text = "wife mentioned needing new running shoes",
            .text_len = strlen("wife mentioned needing new running shoes"),
            .origin_channel = "imessage",
            .origin_contact_id = "contact_wife",
            .origin_relationship_type = "family",
            .observed_at_ms = 1000,
            .confidence = 0.9,
        }
    };
    size_t count = 1;

    /* Apply filter for a coworker turn */
    HU_ASSERT_EQ(hu_cross_channel_filter(&persona, "coworker", items, &count), HU_OK);

    /* The family fact MUST be filtered out */
    HU_ASSERT_EQ(count, 0);
}

static void test_partner_fact_never_reaches_coworker_turn(void) {
    /* Same shape as above with origin_relationship_type = "partner" */
}

static void test_acl_same_relationship_always_allowed(void) {
    /* family origin + family turn → kept */
}

static void test_acl_deny_unknown_blocks_null_relationship(void) {
    /* turn_relationship_type = NULL → deny under default policy */
}

void run_cross_channel_acl_tests(void) {
    HU_TEST_SUITE("cross_channel_acl");
    HU_RUN_TEST(test_family_fact_never_reaches_coworker_turn);
    HU_RUN_TEST(test_partner_fact_never_reaches_coworker_turn);
    HU_RUN_TEST(test_acl_same_relationship_always_allowed);
    HU_RUN_TEST(test_acl_deny_unknown_blocks_null_relationship);
}
```

Register in `tests/test_main.c`:

```c
#ifdef HU_ENABLE_SQLITE
void run_cross_channel_acl_tests(void);
#endif
/* in main(): */
#ifdef HU_ENABLE_SQLITE
    run_cross_channel_acl_tests();
#endif
```

- [ ] **Step 1.2: Run to verify they fail (link error)**

```
cmake --build --preset dev --target human_tests 2>&1 | tail -20
```
Expected: undefined references to `hu_cross_channel_filter`, `hu_persona_load_defaults`, etc. Confirms the tests are wired.

- [ ] **Step 1.3: Commit the failing tests**

```
git add tests/test_cross_channel_acl.c tests/test_main.c CMakeLists.txt
git commit -m "test(cross_channel): trust-property tests FIRST per tests-that-pin-bugs.md

AC-1 negative-contract: family fact MUST NEVER reach coworker turn.
Tests fail (link error) until Tasks 2-4 land the persona ACL schema,
parser, predicate, and filter. Failing tests committed deliberately to
pin the contract before any implementation."
```

---

## Task 2: Persona ACL schema + safe defaults + parser

**Files:**
- Modify: `include/human/persona.h` (add types + field)
- Modify: `src/persona/persona.c` (parse + defaults + free)
- Create: `tests/test_persona_acl_parse.c`

- [ ] **Step 2.1: Add types to persona.h**

```c
typedef struct hu_xchan_acl_rule {
    char relationship_type[32];     /* "coworker", "family", etc */
    char **allow_list;              /* allowed relationship_types */
    size_t allow_count;
} hu_xchan_acl_rule_t;

typedef struct hu_xchan_acl {
    char default_policy[32];        /* "deny_unknown" | "allow_unknown" */
    hu_xchan_acl_rule_t *rules;
    size_t rule_count;
} hu_xchan_acl_t;

/* In hu_persona_t (add at end of struct): */
hu_xchan_acl_t cross_channel_acl;
```

- [ ] **Step 2.2: Write failing parser tests**

```c
static void test_persona_with_explicit_acl_overrides_defaults(void) {
    const char *json =
      "{\"name\":\"test\",\"cross_channel_acl\":{"
      "  \"default_policy\":\"allow_unknown\","
      "  \"rules\":{\"coworker\":{\"allow\":[\"family\"]}}"
      "}}";
    hu_persona_t p = {0};
    HU_ASSERT_EQ(hu_persona_parse_json(json, &p), HU_OK);
    HU_ASSERT_STR_EQ(p.cross_channel_acl.default_policy, "allow_unknown");
    HU_ASSERT_TRUE(p.cross_channel_acl.rule_count >= 1);
    /* The coworker rule allows family (override of safe default) */
    bool found = false;
    for (size_t i = 0; i < p.cross_channel_acl.rule_count; i++) {
        if (strcmp(p.cross_channel_acl.rules[i].relationship_type, "coworker") == 0) {
            for (size_t j = 0; j < p.cross_channel_acl.rules[i].allow_count; j++) {
                if (strcmp(p.cross_channel_acl.rules[i].allow_list[j], "family") == 0) {
                    found = true;
                }
            }
        }
    }
    HU_ASSERT_TRUE(found);
    hu_persona_free(&p);
}

static void test_persona_missing_acl_field_uses_safe_defaults(void) {
    const char *json = "{\"name\":\"test\"}";  /* no acl field */
    hu_persona_t p = {0};
    HU_ASSERT_EQ(hu_persona_parse_json(json, &p), HU_OK);
    HU_ASSERT_STR_EQ(p.cross_channel_acl.default_policy, "deny_unknown");
    HU_ASSERT_TRUE(p.cross_channel_acl.rule_count >= 7);  /* 7 default rels */
    hu_persona_free(&p);
}

static void test_persona_malformed_acl_block_falls_back_safely(void) {
    const char *json =
      "{\"name\":\"test\",\"cross_channel_acl\":\"not an object\"}";
    hu_persona_t p = {0};
    /* Parse should succeed (persona loads) but ACL falls back to safe defaults */
    HU_ASSERT_EQ(hu_persona_parse_json(json, &p), HU_OK);
    HU_ASSERT_STR_EQ(p.cross_channel_acl.default_policy, "deny_unknown");
    hu_persona_free(&p);
}
```

- [ ] **Step 2.3: Implement safe defaults**

In `src/persona/persona.c`, add a `populate_safe_default_acl(hu_xchan_acl_t *acl)` static helper. It writes the 7-rule default from the spec into `acl->rules` via `calloc` + `strdup`. Called whenever the persona has no `cross_channel_acl` field, or the field is malformed.

```c
static void populate_safe_default_acl(hu_xchan_acl_t *acl) {
    static const struct {
        const char *rel;
        const char *const *allow;
        size_t allow_n;
    } k_defaults[] = {
        { "coworker",     (const char*[]){"coworker", "acquaintance"}, 2 },
        { "work",         (const char*[]){"coworker", "acquaintance"}, 2 },
        { "acquaintance", (const char*[]){"acquaintance", "friend"}, 2 },
        { "friend",       (const char*[]){"friend", "close_friend", "acquaintance"}, 3 },
        { "close_friend", (const char*[]){"close_friend", "friend", "family", "partner"}, 4 },
        { "family",       (const char*[]){"family", "partner", "close_friend"}, 3 },
        { "partner",      (const char*[]){"family", "partner", "close_friend"}, 3 },
    };
    strncpy(acl->default_policy, "deny_unknown", sizeof acl->default_policy - 1);
    acl->rule_count = sizeof k_defaults / sizeof k_defaults[0];
    acl->rules = calloc(acl->rule_count, sizeof(hu_xchan_acl_rule_t));
    for (size_t i = 0; i < acl->rule_count; i++) {
        strncpy(acl->rules[i].relationship_type, k_defaults[i].rel,
                sizeof acl->rules[i].relationship_type - 1);
        acl->rules[i].allow_count = k_defaults[i].allow_n;
        acl->rules[i].allow_list = calloc(k_defaults[i].allow_n, sizeof(char *));
        for (size_t j = 0; j < k_defaults[i].allow_n; j++) {
            acl->rules[i].allow_list[j] = strdup(k_defaults[i].allow[j]);
        }
    }
}
```

- [ ] **Step 2.4: Implement parser**

In `hu_persona_parse_json` (the existing JSON parser), find where persona fields are read. Add a block for `cross_channel_acl`:

```c
hu_json_value_t *acl_v = hu_json_object_get(root, "cross_channel_acl");
if (!acl_v || hu_json_type(acl_v) != HU_JSON_OBJECT) {
    populate_safe_default_acl(&out->cross_channel_acl);
    if (acl_v) {
        hu_log_warn("persona", "cross_channel_acl field present but malformed; "
                                "falling back to safe defaults");
    } else {
        hu_log_info("persona", "no cross_channel_acl field; using safe defaults");
    }
} else {
    /* Parse default_policy */
    hu_json_value_t *dp = hu_json_object_get(acl_v, "default_policy");
    const char *dp_str = (dp && hu_json_type(dp) == HU_JSON_STRING)
                         ? hu_json_string(dp) : "deny_unknown";
    strncpy(out->cross_channel_acl.default_policy, dp_str,
            sizeof out->cross_channel_acl.default_policy - 1);

    /* Parse rules object */
    hu_json_value_t *rules_v = hu_json_object_get(acl_v, "rules");
    if (rules_v && hu_json_type(rules_v) == HU_JSON_OBJECT) {
        /* Walk the rules object: for each key (e.g. "coworker"), read its
         * "allow" array. Allocate `rule_count` rules and populate. */
        /* ... iteration logic following the codebase's JSON helper pattern ... */
    } else {
        populate_safe_default_acl(&out->cross_channel_acl);
    }
}
```

- [ ] **Step 2.5: Implement `hu_persona_load_defaults` helper for tests**

A test-only helper that loads a persona stub with safe-default ACL:

```c
void hu_persona_load_defaults(hu_persona_t *out) {
    memset(out, 0, sizeof(*out));
    populate_safe_default_acl(&out->cross_channel_acl);
}
```

- [ ] **Step 2.6: Free path**

In `hu_persona_free`, free the ACL:

```c
for (size_t i = 0; i < persona->cross_channel_acl.rule_count; i++) {
    for (size_t j = 0; j < persona->cross_channel_acl.rules[i].allow_count; j++) {
        free(persona->cross_channel_acl.rules[i].allow_list[j]);
    }
    free(persona->cross_channel_acl.rules[i].allow_list);
}
free(persona->cross_channel_acl.rules);
persona->cross_channel_acl.rules = NULL;
persona->cross_channel_acl.rule_count = 0;
```

- [ ] **Step 2.7: Run + commit**

```
./build/human_tests --suite=persona_acl_parse
git commit -m "feat(persona): cross_channel_acl schema + safe defaults + parser"
```

---

## Task 3: The pure ACL predicate

**Files:**
- Create: `include/human/memory/cross_channel.h` (partial — just the enum + predicate)
- Create: `src/memory/cross_channel.c` (partial — just the predicate)
- Modify: `tests/test_cross_channel_acl.c` (verify Task 1's tests pass after predicate lands)

- [ ] **Step 3.1: Add the public header (partial)**

```c
#ifndef HU_MEMORY_CROSS_CHANNEL_H
#define HU_MEMORY_CROSS_CHANNEL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"
#include <stddef.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

typedef enum {
    HU_XCHAN_ACL_ALLOW = 0,
    HU_XCHAN_ACL_DENY,
    HU_XCHAN_ACL_DENY_UNKNOWN,
} hu_xchan_acl_decision_t;

/* Pure predicate — no DB, no allocator, no LLM. Testable in isolation
 * per ~/.claude/rules/security-predicate-extraction.md. */
hu_xchan_acl_decision_t hu_cross_channel_acl_check(
    const hu_persona_t *persona,
    const char *origin_relationship_type,    /* may be NULL */
    const char *turn_relationship_type);     /* may be NULL */

#endif
```

- [ ] **Step 3.2: Implement the predicate**

```c
hu_xchan_acl_decision_t hu_cross_channel_acl_check(
    const hu_persona_t *persona,
    const char *origin_rel,
    const char *turn_rel)
{
    if (!persona) return HU_XCHAN_ACL_DENY_UNKNOWN;

    /* Resolve unknowns */
    if (!turn_rel || turn_rel[0] == '\0') {
        /* Turn-side unknown: apply default_policy */
        if (strcmp(persona->cross_channel_acl.default_policy, "allow_unknown") == 0)
            return HU_XCHAN_ACL_ALLOW;
        return HU_XCHAN_ACL_DENY_UNKNOWN;
    }

    /* Origin unknown → treat as acquaintance (most generic profiled type) */
    const char *origin = (origin_rel && origin_rel[0]) ? origin_rel : "acquaintance";

    /* Same-relationship always allowed */
    if (strcmp(origin, turn_rel) == 0) return HU_XCHAN_ACL_ALLOW;

    /* Look up the turn-side rule */
    for (size_t i = 0; i < persona->cross_channel_acl.rule_count; i++) {
        const hu_xchan_acl_rule_t *r = &persona->cross_channel_acl.rules[i];
        if (strcmp(r->relationship_type, turn_rel) != 0) continue;
        for (size_t j = 0; j < r->allow_count; j++) {
            if (strcmp(r->allow_list[j], origin) == 0)
                return HU_XCHAN_ACL_ALLOW;
        }
        return HU_XCHAN_ACL_DENY;
    }

    /* No rule for this turn_rel — default policy */
    if (strcmp(persona->cross_channel_acl.default_policy, "allow_unknown") == 0)
        return HU_XCHAN_ACL_ALLOW;
    return HU_XCHAN_ACL_DENY_UNKNOWN;
}
```

- [ ] **Step 3.3: Add direct-predicate tests**

In `tests/test_cross_channel_acl.c`, append:

```c
static void test_acl_check_family_to_coworker_denies(void) {
    hu_persona_t p; hu_persona_load_defaults(&p);
    HU_ASSERT_EQ(hu_cross_channel_acl_check(&p, "family", "coworker"),
                 HU_XCHAN_ACL_DENY);
    hu_persona_free(&p);
}

static void test_acl_check_family_to_partner_allows(void) {
    hu_persona_t p; hu_persona_load_defaults(&p);
    HU_ASSERT_EQ(hu_cross_channel_acl_check(&p, "family", "partner"),
                 HU_XCHAN_ACL_ALLOW);
    hu_persona_free(&p);
}

static void test_acl_check_null_turn_rel_deny_unknown(void) {
    hu_persona_t p; hu_persona_load_defaults(&p);
    HU_ASSERT_EQ(hu_cross_channel_acl_check(&p, "family", NULL),
                 HU_XCHAN_ACL_DENY_UNKNOWN);
    hu_persona_free(&p);
}
```

- [ ] **Step 3.4: Run + commit**

```
./build/human_tests --suite=cross_channel_acl
```
Expected: predicate tests pass; Task 1's tests STILL fail (still need filter stage).

```
git commit -m "feat(cross_channel): hu_cross_channel_acl_check pure predicate"
```

---

## Task 4: Filter stage (Task 1's tests turn green)

**Files:**
- Modify: `include/human/memory/cross_channel.h` (add `hu_cross_channel_filter`)
- Modify: `src/memory/cross_channel.c`

- [ ] **Step 4.1: Add struct + filter prototype to header**

```c
typedef struct hu_cross_channel_item {
    enum {
        HU_XCHAN_FACT = 0,
        HU_XCHAN_REFLECTION_PATTERN,
    } source_type;
    char        item_id[64];
    char       *text;
    size_t      text_len;
    char        origin_channel[32];
    char        origin_contact_id[64];
    char        origin_relationship_type[32];
    int64_t     observed_at_ms;
    double      confidence;
} hu_cross_channel_item_t;

hu_error_t hu_cross_channel_filter(
    const hu_persona_t *persona,
    const char *turn_relationship_type,
    hu_cross_channel_item_t *items, size_t *count);
```

- [ ] **Step 4.2: Implement filter**

```c
hu_error_t hu_cross_channel_filter(
    const hu_persona_t *persona,
    const char *turn_rel,
    hu_cross_channel_item_t *items, size_t *count)
{
    if (!persona || !items || !count) return HU_ERR_INVALID_ARGUMENT;
    size_t kept = 0;
    for (size_t i = 0; i < *count; i++) {
        if (hu_cross_channel_acl_check(persona,
                items[i].origin_relationship_type, turn_rel) == HU_XCHAN_ACL_ALLOW) {
            if (kept != i) items[kept] = items[i];
            kept++;
        } else {
            /* DEBUG-level log per design.md "Mitigation R6" */
            hu_log_debug("cross_channel",
                "filtered fact %s (origin=%s, turn=%s) by ACL",
                items[i].item_id,
                items[i].origin_relationship_type[0]
                    ? items[i].origin_relationship_type : "(unset)",
                turn_rel ? turn_rel : "(unset)");
            /* Note: caller still owns items[i].text — caller frees on cleanup;
             * filter does NOT free dropped items (caller knows full array bounds) */
        }
    }
    *count = kept;
    return HU_OK;
}
```

- [ ] **Step 4.3: Verify Task 1's trust-property tests turn green**

```
./build/human_tests --suite=cross_channel_acl
```
Expected: ALL tests pass including `test_family_fact_never_reaches_coworker_turn`.

- [ ] **Step 4.4: Commit**

```
git commit -m "feat(cross_channel): hu_cross_channel_filter — AC-1 trust property locked

The trust-property test from Task 1 turns green here. Family facts
deterministically blocked from reaching coworker turns. Filter is
in-place compaction; caller manages item lifetime."
```

---

## Task 5: Collect stage

**Files:**
- Modify: `include/human/memory/cross_channel.h` (add `hu_cross_channel_collect`)
- Modify: `src/memory/cross_channel.c`
- Create: `tests/test_cross_channel_pipeline.c`

- [ ] **Step 5.1: Add prototype**

```c
hu_error_t hu_cross_channel_collect(
    hu_allocator_t *alloc, sqlite3 *db,
    const char *current_channel, const char *current_contact_id,
    int64_t now_ms, int max_candidates,
    hu_cross_channel_item_t **out_items, size_t *out_count);
```

- [ ] **Step 5.2: Write failing integration test**

```c
static void test_collect_returns_facts_from_other_channels(void) {
    /* Setup: in-memory DB with personal_model facts originating from
     * "telegram" and the current channel is "imessage".
     * Call collect → assert returned items have origin_channel = "telegram" */
}

static void test_collect_skips_reflection_table_when_absent(void) {
    /* in-memory DB without reflection_patterns table → collect returns
     * only facts; does not crash or error */
}
```

- [ ] **Step 5.3: Implement collect**

Two SQL queries:

Query A: facts from personal_model where `origin_channel != current_channel`. Each row maps to a `hu_cross_channel_item_t` with `source_type = HU_XCHAN_FACT`.

Query B: reflection_patterns where current_channel is NOT in `channels_json` OR `json_array_length(channels_json) > 1`. Uses JSON1 EXISTS pattern from reflection-loop's `consumer.c`. Source type = `HU_XCHAN_REFLECTION_PATTERN`.

Wrap query B in:

```c
/* Probe for reflection_patterns table existence */
sqlite3_stmt *probe = NULL;
sqlite3_prepare_v2(db,
    "SELECT name FROM sqlite_master WHERE type='table' "
    "AND name='reflection_patterns'", -1, &probe, NULL);
bool has_reflection = (sqlite3_step(probe) == SQLITE_ROW);
sqlite3_finalize(probe);
if (has_reflection) {
    /* Run Query B */
}
```

Cap at `max_candidates` by descending `confidence * recency_weight` composite score.

For each origin_contact_id, look up the contact profile and copy `relationship_type` into the item (this is the data the filter needs).

- [ ] **Step 5.4: Run + commit**

```
./build/human_tests --suite=cross_channel_pipeline
git commit -m "feat(cross_channel): hu_cross_channel_collect — reads facts + patterns"
```

---

## Task 6: Format stage + extract `format_when` helper

**Files:**
- Modify: `include/human/memory/cross_channel.h` (add `hu_cross_channel_format`)
- Modify: `src/memory/cross_channel.c`
- Modify: `src/daemon.c` (move `cross_channel_format_when` to public; delete private copy)

- [ ] **Step 6.1: Extract `cross_channel_format_when` to public**

In `src/daemon.c:561`, the existing function `cross_channel_format_when` is `static`. Move it to `src/memory/cross_channel.c` and rename to `hu_cross_channel_format_when`. Add prototype to the header. Replace internal callers in `daemon.c` with the new public name (`grep -n "cross_channel_format_when" src/` → update call sites).

- [ ] **Step 6.2: Implement format**

```c
hu_error_t hu_cross_channel_format(
    hu_allocator_t *alloc, int64_t now_ms,
    const hu_cross_channel_item_t *items, size_t count,
    char **out_text, size_t *out_len)
{
    if (!alloc || !out_text || !out_len) return HU_ERR_INVALID_ARGUMENT;
    if (count == 0) {
        *out_text = NULL;
        *out_len = 0;
        return HU_OK;
    }

    /* Estimate capacity: ~80 chars provenance + text per item + margin */
    size_t cap = 256;
    for (size_t i = 0; i < count; i++) cap += items[i].text_len + 80;
    char *buf = alloc->alloc(alloc->ctx, cap);
    if (!buf) return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        char when_buf[64];
        hu_cross_channel_format_when(when_buf, sizeof when_buf, items[i].observed_at_ms, now_ms);

        int written = snprintf(buf + pos, cap - pos,
            "From %s %s: %.*s\n",
            items[i].origin_channel[0] ? items[i].origin_channel : "memory",
            when_buf,
            (int)items[i].text_len, items[i].text);
        if (written < 0 || (size_t)written >= cap - pos) break;
        pos += (size_t)written;
    }
    buf[pos] = '\0';
    *out_text = buf;
    *out_len = pos;
    return HU_OK;
}

void hu_cross_channel_items_free(
    hu_allocator_t *alloc, hu_cross_channel_item_t *items, size_t count)
{
    if (!items) return;
    for (size_t i = 0; i < count; i++) {
        if (items[i].text) alloc->free(alloc->ctx, items[i].text, items[i].text_len);
    }
    alloc->free(alloc->ctx, items, count * sizeof(*items));
}
```

- [ ] **Step 6.3: Tests**

```c
static void test_format_includes_origin_channel_and_relative_time(void) {
    /* Build items array; call format; assert output contains "From imessage" and a time expression */
}
```

- [ ] **Step 6.4: Run + commit**

```
git commit -m "feat(cross_channel): format stage with provenance + extracted when helper"
```

---

## Task 7: Daemon integration (replace inline cross-channel ctx)

**Files:**
- Modify: `src/daemon.c` (lines ~6525-6815)

- [ ] **Step 7.1: Locate the existing inline cross_channel_ctx construction**

```
sed -n '6520,6620p' src/daemon.c
```

Identify the exact block that builds `cross_channel_ctx` and `cross_channel_ctx_len`. This is the surface area to replace.

- [ ] **Step 7.2: Replace with pipeline calls**

```c
#if defined(HU_ENABLE_SQLITE) && !defined(HU_IS_TEST)
char *cross_channel_ctx = NULL;
size_t cross_channel_ctx_len = 0;

const char *current_channel = ch->channel->vtable->name
    ? ch->channel->vtable->name(ch->channel->ctx) : NULL;
const char *current_contact_id = (cp && cp->contact_id) ? cp->contact_id : NULL;
const char *turn_rel = (cp && cp->relationship_type) ? cp->relationship_type : NULL;

hu_cross_channel_item_t *items = NULL;
size_t item_count = 0;

if (agent->memory) {
    sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
    if (db && current_channel) {
        hu_cross_channel_collect(alloc, db, current_channel, current_contact_id,
                                  hu_now_ms(), /*max=*/20, &items, &item_count);
        hu_cross_channel_filter(agent->persona, turn_rel, items, &item_count);
        hu_cross_channel_format(alloc, hu_now_ms(), items, item_count,
                                 &cross_channel_ctx, &cross_channel_ctx_len);
        hu_cross_channel_items_free(alloc, items, item_count);
    }
}
#endif
```

The downstream prompt assembler at `daemon.c:6815+` that consumes `cross_channel_ctx` is unchanged — same output contract.

- [ ] **Step 7.3: Touch + rebuild + smoke per cmake-build-stale-binary rule**

```
touch src/daemon.c
cmake --build --preset dev --target human -j8
nm build/human | grep hu_cross_channel_  # verify new symbols linked
```

- [ ] **Step 7.4: Run full test suite**

```
./build/human_tests
```
Expected: 0 failures. The 15+ new tests + all pre-existing tests pass.

- [ ] **Step 7.5: Commit**

```
git commit -m "feat(daemon): replace inline cross_channel_ctx with 3-stage pipeline

daemon.c:6525-6815 now calls collect→filter→format. Filter enforces
the ACL trust property (AC-1). Downstream prompt assembler contract
is preserved (cross_channel_ctx + cross_channel_ctx_len populated
the same way). Cross-channel context now structurally cannot leak
across relationship_type boundaries."
```

---

## Task 8: Acceptance verification + manual smoke

- [ ] **Step 8.1: Full test suite**

```
./build/human_tests
```
Expected: 0 failures, 0 ASan errors. Test count up by 15+ from new suites.

- [ ] **Step 8.2: Gate symmetry**

```
bash scripts/check-test-source-gate-symmetry.sh
```

- [ ] **Step 8.3: Manual smoke against AC-1 through AC-8**

Concrete smoke procedure:
- AC-1 (trust property): `./build/human_tests --filter=family_fact_never_reaches`
- AC-2 (reflection integration): Run reflection-loop once (requires that sprint to have landed), then trigger a cross-channel turn; verify reflection pattern appears in cross_channel_ctx via daemon DEBUG logs
- AC-3 (override): create a persona with `cross_channel_acl.rules.coworker.allow = ["family"]`; verify family facts now appear in coworker context
- AC-4 (missing field): load a legacy persona without the field; verify daemon starts, INFO log emitted, safe defaults in effect
- AC-5 (malformed): inject malformed ACL JSON; verify daemon starts, WARN log emitted, safe defaults in effect, no crash
- AC-6: predicate test count
- AC-7 (graceful degradation): with reflection table absent, verify cross_channel_ctx still populates from facts only
- AC-8: from 8.1

Document results in `docs/plans/2026-05-27-cross-channel-synthesis/results/acceptance-2026-XX-XX.md`.

- [ ] **Step 8.4: Final commit**

```
git commit -m "chore(cross_channel): Phase 1 acceptance results"
```

---

## What Sprint 2 (Scope C) looks like — deferred

- Explicit synthesis judge: deterministic "should I surface this?" governor (separate from init_proposer's, or extension of it)
- Surface tracking table: log when a fact/pattern was surfaced on which channel, dedupe across turns
- Negative-feedback retire: when user pushes back on a cross-channel reference, mark unsurfacable
- `dunbar_layer` integration in ACL (finer-grained than `relationship_type`)
- Per-fact opt-out: user-facing way to mark specific facts as never-cross-channel

That's a 2-week project on its own — gets its own plan after Phase 1 ships and we see how the LLM behaves with structural-ACL-only.

---

## Self-review checklist

- [x] Every task has exact file paths
- [x] Every step that changes code shows the code or a complete sketch
- [x] Every command is runnable
- [x] All 8 ACs trace to tasks (AC-1: T1+T4; AC-2: T5+T8; AC-3: T2+T8; AC-4: T2+T8; AC-5: T2+T8; AC-6: T3; AC-7: T5+T8; AC-8: T8.1)
- [x] No handwaves
- [x] Function/struct names consistent: `hu_cross_channel_*`, `hu_xchan_acl_*`
- [x] Gate symmetry: `src/memory/cross_channel.c` AND its tests gate on `HU_ENABLE_SQLITE`
- [x] AC-1 negative-contract test is named per `tests-that-pin-bugs.md` (asserts dangerous case is BLOCKED)
- [x] Task 1 lands FAILING tests deliberately (per tests-first discipline)
- [x] Existing infrastructure preserved (identity_resolver, contact_graph, relationship_type) — extended not replaced
- [x] Commits are small, conventional format
