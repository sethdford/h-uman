# Cross-Channel Synthesis (M2) — Phase 1 Design

**Status:** Draft (post-brainstorm 2026-05-27)
**Owner:** Seth
**Mission:** Closes M2 ("Personal Model") operationally — h-uman knows what happened on iMessage when responding on Telegram, with deterministic privacy gating that prevents family facts bleeding into work channels.
**Sibling specs:** [`2026-05-26-reflection-loop/`](../2026-05-26-reflection-loop/) (reflection patterns are one of the data sources cross-channel context consumes), [`2026-05-26-calibrated-uncertainty/`](../2026-05-26-calibrated-uncertainty/) (uncertainty signals attach to cross-channel surfacings the same way they attach to other facts).

## Why

Recon turned up an unexpected truth: **cross-channel synthesis is already half-shipped**. h-uman has identity resolution with 4-tier confidence, a `cross_channel_ctx` builder gated on `HU_ENABLE_SQLITE` at `src/daemon.c:6525-6815`, contact graph linking platform handles to canonical IDs, `relationship_type` and `dunbar_layer` populated on contact profiles, and Dunbar-aware proactive surfacing at `src/daemon_proactive.c:606`. The "knows about other channels" half is real, in production, and wired into the turn pipeline.

What's not done — and the gap this spec closes — is the **trust property that makes cross-channel safe to ship to users**: deterministic privacy ACL gating that prevents family/partner facts from being included in work-channel context. Today, those facts CAN reach the LLM in coworker conversations; we rely on the LLM to "not mention them." That's not a property; that's a hope. For M4 (ship to users) the property must be structural.

Phase 1 also wires reflection patterns into the existing `cross_channel_ctx` so reflection's M2 contribution actually flows through, and adds provenance annotation ("observed on iMessage, Tuesday") so the LLM can phrase references with channel-awareness when the ACL allows.

## Goals (Phase 1, Scope B from brainstorm)

1. **Privacy ACL** — every persona ships a `cross_channel_acl` map from `relationship_type → allowed_relationship_types`. The context builder DETERMINISTICALLY filters facts/patterns by the ACL before they reach the LLM prompt. The trust property becomes structural, not behavioral.
2. **Reflection pattern integration** — `cross_channel_ctx` consumes reflection patterns where `channel_count > 1` OR `origin_channel != current_channel`, subject to ACL.
3. **Provenance annotation** — each cross-channel item gets a "from <channel>, <relative_time>" tag injected into prompt context.
4. **Sensible defaults** — out-of-box ACL ships with safe defaults: `coworker`/`work` → deny family/partner; `family`/`partner` → allow each other but deny work; `friend` → allow most. User can override per-persona.
5. **Tests pin the property** — a coworker-channel test scenario with family facts in memory MUST produce a `cross_channel_ctx` that contains zero family facts. This is a positive-contract test per `tests-that-pin-bugs.md`.

## Non-goals (Phase 1)

- **Explicit synthesis judge** — "should the LLM mention this cross-channel fact?" stays implicit (LLM-decided). Scope C adds a deterministic governor. We're betting the LLM with structurally-filtered context will behave well; if measurement says otherwise, add the governor later.
- **Surface tracking table** — "we already mentioned this on Telegram, don't repeat on iMessage" — deferred to Scope C.
- **Negative-feedback retire** — user pushes back on a cross-channel reference → fact gets marked unsurfacable. Deferred to Scope C; piggy-backs on reflection's retire-on-contradiction infrastructure when ready.
- **Cross-channel reference phrasing prescriptions** — the LLM picks phrasing from persona overlay style; we don't lock specific templates.
- **Per-fact opt-out** — a way for the user to mark individual facts "never cross-channel." Future.

## Architecture overview

```
                Turn arrives on channel B
                          │
                          ▼
              ┌──────────────────────────┐
              │ daemon.c:6525            │
              │ Build cross_channel_ctx  │  (existing path, gated on
              │                          │   HU_ENABLE_SQLITE)
              └────────────┬─────────────┘
                           │
                           ▼
              ┌──────────────────────────┐
              │ hu_cross_channel_collect │  (NEW — extracts candidates)
              │ Returns array of:        │
              │   - facts (from memory)  │
              │   - reflection patterns  │
              │ Each tagged with         │
              │ origin_channel + ts      │
              └────────────┬─────────────┘
                           │
                           ▼
              ┌──────────────────────────┐  ◄── persona.cross_channel_acl
              │ hu_cross_channel_filter  │      (per-relationship_type
              │ (NEW — applies ACL)      │       allow/deny graph)
              │                          │
              │ For each candidate:      │
              │   resolve origin contact │
              │   look up rel_type       │
              │   look up turn rel_type  │
              │   ACL says allow?        │
              └────────────┬─────────────┘
                           │
                           ▼
              ┌──────────────────────────┐
              │ hu_cross_channel_format  │  (NEW — adds provenance)
              │ "from iMessage, Tue:"    │
              │ "from Slack, last week:" │
              └────────────┬─────────────┘
                           │
                           ▼
              ┌──────────────────────────┐
              │ Existing prompt assembly │
              │ at daemon.c:6815+        │
              │ (cross_channel_ctx       │
              │  passed unchanged)       │
              └──────────────────────────┘
```

The architecture is intentionally three pure-data stages (collect → filter → format) with the ACL filter as a testable predicate. This matches `~/.claude/rules/security-predicate-extraction.md`: the privacy property lives in one named pure function, and a unit test pins the truth table.

## The ACL primitive

### Schema (JSON, in persona overlay or root persona JSON)

```json
{
  "cross_channel_acl": {
    "default_policy": "deny_unknown",
    "rules": {
      "coworker":     { "allow": ["coworker", "acquaintance"] },
      "work":         { "allow": ["coworker", "acquaintance"] },
      "acquaintance": { "allow": ["acquaintance", "friend"] },
      "friend":       { "allow": ["friend", "close_friend", "acquaintance"] },
      "close_friend": { "allow": ["close_friend", "friend", "family", "partner"] },
      "family":       { "allow": ["family", "partner", "close_friend"] },
      "partner":      { "allow": ["family", "partner", "close_friend"] }
    }
  }
}
```

### Semantics

A fact F with `origin_contact.relationship_type = R_F` can appear in `cross_channel_ctx` for a turn on channel C with target contact `R_C` if and only if:

```
R_F ∈ acl.rules[R_C].allow
```

Or equivalently: "for this conversation type, this fact's origin relationship type is allowed."

Three edge cases the rules table handles:

1. **R_C unknown or NULL** (anonymous channel, untyped contact): apply `default_policy`. Default ships as `"deny_unknown"` — fail closed. Safer than fail open.
2. **R_F unknown or NULL** (fact originated from a contact we don't have a profile for): the fact is treated as relationship_type = `"acquaintance"` (the most generic profiled relationship).
3. **R_C == R_F**: same-relationship-type cross-channel is always allowed regardless of explicit rule (an iMessage-with-family fact reaching a Telegram-with-family turn is fine; both are family-context).

### Why "deny_unknown" default

Asymmetric loss: a family inside-joke reaching a coworker conversation costs more (deeply damages trust, embarrassing) than a benign acquaintance fact failing to cross over (mild — user can just repeat the info if needed). Per `~/.claude/rules/classifier-score-plus-flag-gate.md` reasoning ("asymmetric loss → conservative threshold"), fail closed.

### Pure predicate

```c
typedef enum {
    HU_XCHAN_ACL_ALLOW = 0,
    HU_XCHAN_ACL_DENY,
    HU_XCHAN_ACL_DENY_UNKNOWN,    /* default policy fired */
} hu_xchan_acl_decision_t;

hu_xchan_acl_decision_t hu_cross_channel_acl_check(
    const hu_persona_t *persona,
    const char *origin_relationship_type,    /* may be NULL */
    const char *turn_relationship_type);     /* may be NULL */
```

The persona owns the ACL. Pure inputs/outputs. Testable without forking, without DB, without LLM. Direct application of `~/.claude/rules/security-predicate-extraction.md`.

## Components

### `include/human/memory/cross_channel.h` — NEW

```c
typedef struct hu_cross_channel_item {
    /* Source */
    enum {
        HU_XCHAN_FACT = 0,
        HU_XCHAN_REFLECTION_PATTERN,
    } source_type;
    char        item_id[64];                /* fact_id or pattern_id */

    /* Content */
    char       *text;                       /* fact statement or pattern observation */
    size_t      text_len;

    /* Provenance */
    char        origin_channel[32];         /* "imessage", "telegram", etc */
    char        origin_contact_id[64];      /* canonical contact ID */
    char        origin_relationship_type[32]; /* "family", "coworker", or NULL */
    int64_t     observed_at_ms;

    /* Confidence (consumed by hedging downstream — see calibrated-uncertainty) */
    double      confidence;
} hu_cross_channel_item_t;

/* Stage 1: collect candidates. Pure-read; no ACL applied yet. */
hu_error_t hu_cross_channel_collect(
    hu_allocator_t *alloc, sqlite3 *db,
    const char *current_channel, const char *current_contact_id,
    int64_t now_ms, int max_candidates,
    hu_cross_channel_item_t **out_items, size_t *out_count);

/* Stage 2: filter via ACL. Pure predicate. Mutates the array in place. */
hu_error_t hu_cross_channel_filter(
    const hu_persona_t *persona,
    const char *turn_relationship_type,    /* of the current-turn contact */
    hu_cross_channel_item_t *items, size_t *count);

/* Stage 3: format for prompt injection. */
hu_error_t hu_cross_channel_format(
    hu_allocator_t *alloc, int64_t now_ms,
    const hu_cross_channel_item_t *items, size_t count,
    char **out_text, size_t *out_len);

/* The ACL predicate, separately exposed for testing. */
hu_xchan_acl_decision_t hu_cross_channel_acl_check(
    const hu_persona_t *persona,
    const char *origin_relationship_type,
    const char *turn_relationship_type);

void hu_cross_channel_items_free(
    hu_allocator_t *alloc, hu_cross_channel_item_t *items, size_t count);
```

### `src/memory/cross_channel.c` — NEW

Implements the four functions. Reads from:
- `hu_personal_model_t` typed facts (where `origin_channel != current_channel`)
- `reflection_patterns` table (where `channels_json` does not contain current_channel, OR `json_array_length > 1`)

ACL implementation: a small static-allocated lookup, JSON-parsed into `hu_persona_t` at persona load time. Hot-path lookup is O(N) over rules array (typically N=7); negligible.

### `include/human/persona.h` — MODIFY

Add `cross_channel_acl` field to `hu_persona_t`:

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

/* In hu_persona_t: */
hu_xchan_acl_t cross_channel_acl;
```

### `src/persona/persona.c` — MODIFY

Parse `cross_channel_acl` JSON block when loading persona. Free during persona destroy. If field missing, populate with the safe-default ACL (the schema above shipped in code).

### `src/daemon.c:6525-6815` — MODIFY

Replace inline `cross_channel_ctx` construction with calls to the new three-stage API:

```c
#if defined(HU_ENABLE_SQLITE) && !defined(HU_IS_TEST)
hu_cross_channel_item_t *items = NULL;
size_t item_count = 0;

hu_cross_channel_collect(alloc, db, current_channel, current_contact_id,
                          now_ms, /*max=*/20, &items, &item_count);

const char *turn_rel = (cp && cp->relationship_type) ? cp->relationship_type : NULL;
hu_cross_channel_filter(agent->persona, turn_rel, items, &item_count);

hu_cross_channel_format(alloc, now_ms, items, item_count,
                         &cross_channel_ctx, &cross_channel_ctx_len);

hu_cross_channel_items_free(alloc, items, item_count);
#endif
```

This collapses the existing inline logic into the testable pipeline while preserving the call-site contract (still produces `cross_channel_ctx` / `cross_channel_ctx_len` for the existing prompt assembler at lines 6815+).

### `tests/test_cross_channel_acl.c` — NEW

The trust-property test. The single highest-importance test file in this sprint:

```c
static void test_family_fact_never_reaches_coworker_turn(void) {
    /* Setup: persona with default ACL; family fact in memory;
     * turn is on Slack with relationship_type="coworker".
     * Run the full collect → filter → format pipeline.
     * Assert: the formatted output does NOT contain the family fact's text. */
}

static void test_acl_deny_unknown_blocks_null_relationship(void) {
    /* turn_relationship_type == NULL → DENY_UNKNOWN under default policy */
}

static void test_acl_same_relationship_always_allowed(void) {
    /* family ↔ family allowed even without explicit rule */
}

static void test_acl_persona_override_widens_default(void) {
    /* Persona with explicit { "coworker": { "allow": ["family"] } } 
     * → family fact reaches coworker turn (user explicitly opted in) */
}

/* Plus tests covering each rule in the default ACL */
```

### `tests/test_cross_channel_pipeline.c` — NEW

Integration test with in-memory DB:
- Insert facts originating from multiple channels with multiple `relationship_type`s
- Call `hu_cross_channel_collect` → assert candidates returned
- Call `hu_cross_channel_filter` with various `turn_relationship_type` → assert correct filtering
- Call `hu_cross_channel_format` → assert provenance strings present ("from imessage")

## Failure handling

- **Persona has no `cross_channel_acl` field:** Use code-shipped safe-default ACL. Logged once at persona load via `silent-config-gated-subsystems.md` pattern: "persona <id> has no cross_channel_acl; using safe defaults."
- **Persona has malformed ACL JSON:** Reject parse, log error, persona still loads with safe-default ACL. Don't fail-open on bad config.
- **Origin contact's `relationship_type` unknown for a fact** (rare — orphan fact, contact deleted): treat as "acquaintance" (the most generic profiled type). Logged at DEBUG.
- **Turn's `relationship_type` unknown:** default_policy fires (`deny_unknown` ships safe). Logged at DEBUG.
- **`hu_cross_channel_collect` returns >max_candidates:** truncate at max=20 by descending recency × confidence (composite score).
- **Reflection table not yet present** (reflection sprint hasn't fully landed): `hu_cross_channel_collect` skips reflection source silently (only pulls from facts). Test exists for this graceful-degradation path.

## Provenance string format

Per `hu_cross_channel_format`:

```
"From iMessage on Tuesday: <observation>"
"From Slack last month: <observation>"
"From multiple channels: <observation>"  (when channel_count > 1)
```

Relative time format reuses any existing helper (`grep -n "format_when\|relative_time\|days_ago" src/`). Per the recon, `cross_channel_format_when` already exists at `src/daemon.c:561` — extract to `src/memory/cross_channel.c` as a public helper for reuse.

## Testing strategy

**Unit (`tests/test_cross_channel_acl.c`):**
1. `test_family_fact_never_reaches_coworker_turn` — THE trust property (highest priority)
2. `test_partner_fact_never_reaches_coworker_turn`
3. `test_coworker_fact_can_reach_acquaintance_turn` (default policy allows it)
4. `test_acl_deny_unknown_blocks_null_relationship`
5. `test_acl_allow_unknown_lets_null_through` (override default_policy = "allow_unknown")
6. `test_acl_same_relationship_always_allowed`
7. `test_acl_persona_override_widens_default`
8. `test_acl_malformed_persona_falls_back_to_safe_default`

**Integration (`tests/test_cross_channel_pipeline.c`):**
9. `test_pipeline_filters_during_collect_then_format`
10. `test_reflection_pattern_with_channel_count_2_appears_when_acl_allows`
11. `test_reflection_pattern_skipped_when_table_absent` (graceful degradation)
12. `test_provenance_format_includes_origin_channel_and_relative_time`

**Persona load (`tests/test_persona_acl_parse.c`):**
13. `test_persona_with_explicit_acl_overrides_defaults`
14. `test_persona_missing_acl_field_uses_safe_defaults`
15. `test_persona_malformed_acl_block_falls_back_safely`

**Gate symmetry:**
- `src/memory/cross_channel.c` gated on `HU_ENABLE_SQLITE` per existing memory module conventions
- All test files match the gating

## Sprint sequencing (~2 weeks)

**Week 1 — ACL primitive (lock the trust property first):**
- Day 1: Test fixtures (`test_cross_channel_acl.c`) writing the negative-contract tests FIRST per `tests-that-pin-bugs.md`
- Day 2: `hu_persona_t.cross_channel_acl` field + parser + safe defaults
- Day 3: `hu_cross_channel_acl_check` pure predicate + remaining ACL tests
- Day 4: `hu_cross_channel_filter` Stage 2 + persona-load tests

**Week 2 — Pipeline + integration:**
- Day 5-6: `hu_cross_channel_collect` Stage 1 (read from personal_model + reflection_patterns)
- Day 7: `hu_cross_channel_format` Stage 3 + provenance helper extraction from daemon.c
- Day 8: Replace inline cross_channel_ctx construction in daemon.c:6525-6815
- Day 9-10: Integration tests + acceptance verification + manual smoke (force a coworker conversation with family facts pre-loaded; verify nothing family-tagged surfaces)

## Acceptance criteria

- **AC-1 (TRUST PROPERTY):** A coworker-channel test scenario with family facts pre-loaded in memory produces `cross_channel_ctx` that contains ZERO family-relationship-type facts. (Negative-contract test pins this.)
- **AC-2:** A reflection pattern with `channel_count > 1` from the reflection sprint appears in `cross_channel_ctx` when the ACL allows.
- **AC-3:** Per-persona ACL override successfully widens or narrows defaults (test 7 above).
- **AC-4:** Persona without explicit `cross_channel_acl` field loads cleanly with safe-default ACL and emits one-shot log.
- **AC-5:** Malformed ACL JSON in a persona file logs an error, the persona STILL loads with safe defaults (no fail-open, no crash).
- **AC-6:** `hu_cross_channel_acl_check` is a pure predicate testable without database, allocator, or persona load.
- **AC-7:** Existing `cross_channel_ctx` callers at `daemon.c:6815+` continue to work — manual smoke with reflection disabled produces sensible context (graceful degradation).
- **AC-8:** All 15+ tests pass, 0 ASan errors, gate-symmetry check passes.

## Risks

- **R1 — Default ACL too restrictive, user experience feels "the agent forgot what we talked about."** *Mitigation:* the defaults explicitly allow same-relationship-type unconditionally + safe-by-default transitive trust within each rel-type cluster (family ↔ partner ↔ close_friend). If field measurements show too many false-blocks, widen defaults in a follow-up. Asymmetric loss favors starting tight.
- **R2 — `relationship_type` populated inconsistently in production.** *Mitigation:* the safe default (`deny_unknown`) means a fact with unset `relationship_type` is treated as acquaintance (the most generic), and a turn with unset `relationship_type` triggers `deny_unknown` (skip cross-channel context this turn). Failure mode is "less cross-channel context", not "leaked family info."
- **R3 — Pipeline refactor at `daemon.c:6525-6815` breaks existing context flow.** *Mitigation:* the new pipeline preserves the call-site output contract (still produces `cross_channel_ctx` / `cross_channel_ctx_len`). The existing prompt assembler at lines 6815+ is unchanged. Manual smoke without ACL changes is AC-7.
- **R4 — Reflection table doesn't exist yet at runtime.** *Mitigation:* `hu_cross_channel_collect` queries reflection only inside an `if (reflection_table_exists)` guard. Tests pin this; reflection landing later doesn't require redeploying this sprint.
- **R5 — Identity resolution returns wrong `relationship_type` for a fact.** *Mitigation:* identity resolution already has 4-tier confidence (NONE/LOW/MEDIUM/HIGH). Cross-channel filter only trusts MEDIUM or HIGH for the `relationship_type` lookup. LOW or NONE → treat as acquaintance (safe default).
- **R6 — User feels the ACL is opaque ("why didn't h-uman bring that up?").** *Mitigation:* DEBUG-level log every ACL denial. Future scope-C work could add an explicit "you have N facts that are walled off from this conversation by your privacy ACL — review at /xchan-acl" hint.

## Open questions

1. **Should `dunbar_layer` affect the ACL?** Currently the ACL keys on `relationship_type` only. Dunbar layer is finer-grained (e.g., "family" but specifically layer 1 = partner vs layer 5 = extended). *Default:* keep Phase 1 simple, key on `relationship_type` only. If measurement shows partner-vs-family distinctions matter, add a `dunbar_max_layer` field to ACL rules in a follow-up.
2. **What about facts with no contact origin** (e.g., a system-generated reminder)? *Default:* treat as `relationship_type = NULL` → fails `deny_unknown`. Genuinely safer than allowing. If this blocks legitimate system-facts from crossing over, add an explicit `HU_XCHAN_SYSTEM_FACT` source type that bypasses ACL.
3. **Per-channel ACL granularity** (vs per-relationship-type)? E.g., "iMessage with mom" should allow X but "iMessage with brother" should deny Y. Per-channel-per-contact is much more complex; defer until measurement shows real users want it.

## Related rules

- `~/.claude/rules/security-predicate-extraction.md` — `hu_cross_channel_acl_check` is the canonical pure-predicate shape; testable without forking/DB/persona-load.
- `~/.claude/rules/tests-that-pin-bugs.md` — `test_family_fact_never_reaches_coworker_turn` is a POSITIVE contract (asserts the dangerous-case is blocked); name matches assertion.
- `~/.claude/rules/silent-config-gated-subsystems.md` — persona-loaded-without-ACL emits one-shot log naming the field that would enable customization.
- `~/.claude/rules/classifier-score-plus-flag-gate.md` — the score-AND-flag composition pattern; ACL is the flag-only side of the gate (no continuous score for now).
- `~/.claude/rules/audit-verify-before-allege.md` — recon during brainstorming verified what infrastructure existed before claiming "missing"; same discipline applied here.
- `.claude/rules/c-backend.md` — vtable conventions; ACL is data, not a vtable; the three-stage pipeline functions follow `hu_<module>_<action>` naming.
