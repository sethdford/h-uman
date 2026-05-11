---
title: "Design for Story C — Tier-1 channel overlay audit and population"
sprint: 1
story: C
created: 2026-05-11
status: ready
author: tech-lead
---

# Design for Story C — Tier-1 channel overlay audit and population

## TL;DR

The variant router is correct. The plumbing is broken in two compounding ways:

1. The starter persona that ships to every new user (`HU_INIT_DEFAULT_PERSONA` in
   `src/cli_commands.c`, duplicated as `HU_ONBOARD_DEFAULT_PERSONA` in
   `src/onboard.c`) declares `channel_overlays` as a **JSON array**, but
   `hu_persona_load_json` only accepts a **JSON object** keyed by channel name.
   The array path is silently dropped (`type != HU_JSON_OBJECT` early-return at
   `src/persona/persona.c:2174`). Net: `persona.overlays_count == 0` in
   production today.
2. Even if the array shape were accepted, the overlay values are **JSON
   numbers** (`"formality": 0.2`, `"avg_length": 40`, `"emoji_usage": 0.3`),
   but `parse_overlay` reads them via `hu_json_get_string` only. Numbers are
   silently coerced to NULL. Net: every overlay field that reaches the
   router would be NULL, and the router collapses to `DEFAULT`.

End-to-end consequence: `directive_telemetry_snapshot().counts[NULL_OVERLAY]`
in a running daemon is currently 100% of total — exactly the symptom AC-C.6
calls out.

The fix is a structured rewrite of the starter persona blob (one symbol,
both call sites), plus a parametric C test that pins variant routing for
each of the four Tier-1 channels. No struct change, no router change, no
new public API.

---

## 1. Threshold map (canonical reference)

Read straight from `src/memory/personal_model.c::directive_variant_for_overlay`
(lines 324–357). This table IS the contract — the populator and the test
must conform to it.

Variant resolution is **first-match wins** in this order:

| Order | Variant                                  | Condition (in code terms)                                                                                                              |
| ----- | ---------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| 0     | `HU_DIRECTIVE_VARIANT_NULL_OVERLAY`      | `overlay == NULL`                                                                                                                      |
| 1     | `HU_DIRECTIVE_VARIANT_FORMAL_TERSE`      | `overlay->formality` is the string `"formal"` **or** `"professional"`                                                                  |
| 2     | `HU_DIRECTIVE_VARIANT_CASUAL_EMOJI`      | `formality` is `"casual"` **or** `"playful"` **AND** `emoji_usage` is `"moderate"`, `"high"`, or `"frequent"`                           |
| 3     | `HU_DIRECTIVE_VARIANT_CASUAL_OR_SHORT`   | `formality` is `"casual"`/`"playful"` **OR** `avg_length` is `"short"` **OR** `atoi(avg_length) > 0 && atoi(avg_length) <= 30`          |
| 4     | `HU_DIRECTIVE_VARIANT_ADAPTIVE_EMOJI`    | `emoji_usage` is `"moderate"`, `"high"`, or `"frequent"` (and none of the above fired — i.e. formality is empty/unrecognized)          |
| 5     | `HU_DIRECTIVE_VARIANT_DEFAULT`           | Overlay is present but none of the above fired (e.g. `formality="adaptive"`, `emoji_usage="minimal"`)                                  |

Important nuances the populator must respect:

- **String, not numeric.** `formality`, `avg_length`, `emoji_usage` are
  `char *` (`include/human/persona.h:20-22`). The router uses `strcmp`. A
  numeric JSON value like `0.5` cannot reach the router as a string under
  the current `parse_overlay` implementation — it is silently dropped.
- **`avg_length` accepts two shapes.** Either the literal string `"short"`,
  or a numeric string that `atoi` parses to `1..30`. `"60"` is **not**
  short. `"short"` is canonical for clarity.
- **Formal trumps emoji.** Even with `emoji_usage="high"`, a `formality`
  of `"formal"` / `"professional"` yields `FORMAL_TERSE`, not
  `ADAPTIVE_EMOJI`.
- **`"low"`, `"minimal"`, `"none"`, `"rare"` are NOT in the `emoji_ok`
  set.** Only `"moderate"`, `"high"`, `"frequent"` switch on emoji.
- **`atoi(...)` for length tolerates non-numeric leading bytes silently
  but a leading non-digit returns 0.** Use `"short"` for the short
  semantic to avoid the implicit-`atoi` trap.

This table is the canonical answer to the open question
"are the thresholds documented anywhere?" — they were not before; they
are now (here and as inline doc on the test cases). Story A's dashboard
tile can link to this section for explainability copy.

---

## 2. Where overlays live (today vs after this change)

### Today (broken)

| Layer                                      | Path                                                                       | Status                                                                                                                                                                                           |
| ------------------------------------------ | -------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Starter persona shipped by `human init`    | Embedded literal `HU_INIT_DEFAULT_PERSONA` in `src/cli_commands.c:85-161`  | Written to `~/.human/personas/default.json` on first run. **`channel_overlays` is a JSON array with numeric values → silently dropped by `parse_overlay` AND would fail `hu_persona_validate_json`.** |
| Starter persona shipped by `human onboard` | Embedded literal `HU_ONBOARD_DEFAULT_PERSONA` in `src/onboard.c:80-161`    | Duplicate of the above (comment at `src/onboard.c:79` explicitly admits the duplication). Same bug.                                                                                              |
| Runtime overlay lookup                     | `hu_persona_find_overlay(persona, channel, channel_len)` in `src/persona/persona.c:40-53` | Linear scan of `persona->overlays[]` by `channel` string. Returns `NULL` if `overlays_count == 0` or no channel match. **Today, returns `NULL` for every channel because the parser produced an empty array.** |
| Test fixtures                              | `tests/fixtures/lora_baseline_persona.json`                                | Contains no `channel_overlays` block. Not relevant to Story C.                                                                                                                                   |

### After this change

| Layer                                      | Path                                                                       | Status                                                                                                                                                                                  |
| ------------------------------------------ | -------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Starter persona symbol                     | New shared `extern const char hu_starter_persona_json[];` declared in `include/human/onboard.h`, defined once in `src/onboard.c`. | Single source of truth. Both `cli_commands.c::cmd_init` and `onboard.c::hu_onboard_run_with_args` reference the same symbol.                                                            |
| Starter persona shape                      | Same symbol                                                                | JSON object form: `"channel_overlays": { "imessage": {...}, "discord": {...}, "slack": {...}, "telegram": {...}, "cli": {...} }`. All values are STRINGS (no `0.2` numerics).            |
| Runtime overlay lookup                     | Unchanged                                                                  | Returns non-NULL pointer to populated overlay for every Tier-1 channel.                                                                                                                 |
| Test reference                             | `tests/test_persona_directive_channels.c` (new)                            | Asserts the shared `hu_starter_persona_json` symbol parses cleanly and yields the four expected variants.                                                                               |

The overlays do **not** live in a separate file. They live inline in the
`default.json` persona file that `human init` writes. This matches every
other persona field (traits, communication rules, example banks) and
preserves the existing one-file-per-persona contract.

---

## 3. Per-channel overlay specification

Mapped against the threshold table above. Each row chooses field values
that (a) hit the AC's target variant, (b) match the channel's social
norms, and (c) survive a future refactor where someone tightens the
`emoji_ok` set or adds a length threshold.

### 3.1 JSON literal (production starter persona)

```json
"channel_overlays": {
  "imessage": {
    "formality":   "casual",
    "avg_length":  "short",
    "emoji_usage": "moderate",
    "style_notes": [
      "Casual texting style. Short messages.",
      "Use tapbacks when appropriate."
    ]
  },
  "discord": {
    "formality":   "casual",
    "avg_length":  "short",
    "emoji_usage": "high",
    "style_notes": [
      "Relaxed community tone. React with emoji when fitting."
    ]
  },
  "slack": {
    "formality":   "professional",
    "avg_length":  "short",
    "emoji_usage": "minimal",
    "style_notes": [
      "Professional but approachable. Use threads. Be concise."
    ]
  },
  "telegram": {
    "formality":   "casual",
    "avg_length":  "short",
    "emoji_usage": "low",
    "style_notes": [
      "Conversational, slightly more detailed than texting."
    ]
  },
  "cli": {
    "formality":   "professional",
    "avg_length":  "200",
    "emoji_usage": "none",
    "style_notes": [
      "Technical, precise. No emoji. Format code blocks when showing code."
    ]
  }
}
```

`style_notes` is changed from a free-form string to an array of strings
because `parse_overlay` calls `parse_string_array` on it
(`src/persona/persona.c:949-951`). The current starter blob uses a single
string which is also silently dropped. Fixing it is in-scope as a
side-effect of the rewrite.

### 3.2 Variant resolution justification

| Channel     | `formality`     | `avg_length` | `emoji_usage` | Branch hit | Variant            | AC coverage                                  |
| ----------- | --------------- | ------------ | ------------- | ---------- | ------------------ | -------------------------------------------- |
| iMessage    | `casual`        | `short`      | `moderate`    | 2          | `CASUAL_EMOJI`     | AC-C.2 (Discord/iMessage → `CASUAL_EMOJI`)   |
| Discord     | `casual`        | `short`      | `high`        | 2          | `CASUAL_EMOJI`     | AC-C.2 (Discord/iMessage → `CASUAL_EMOJI`)   |
| Slack       | `professional`  | `short`      | `minimal`     | 1          | `FORMAL_TERSE`     | AC-C.3 (`FORMAL_TERSE` or `ADAPTIVE_EMOJI`) — picks the preferred branch |
| Telegram    | `casual`        | `short`      | `low`         | 3          | `CASUAL_OR_SHORT`  | AC-C.4 (`CASUAL_OR_SHORT` or `CASUAL_EMOJI`) — picks the preferred branch |

Why these choices and not, say, `"informal"` or `"high"` everywhere:

- **iMessage** is the canonical native-iOS conversational channel. Emoji
  are first-class via tapbacks; messages are bursty and short.
  `moderate` (not `high`) keeps the directive from leaning hard into
  emoji-spam when the user is mid-sentence.
- **Discord** is the only Tier-1 channel where high-density emoji and
  custom reacts are normative. `emoji_usage="high"` reflects that and
  still produces `CASUAL_EMOJI` (the AC's target).
- **Slack** is the work-channel-of-record. `professional` (not `formal`)
  is the softer of the two formal triggers and matches Slack's
  "casual-but-at-work" register. `minimal` (not `low` or `none`) keeps
  the option open for the prompt builder to surface a single emoji per
  message when context allows. Either way, `formality="professional"`
  takes precedence and pins `FORMAL_TERSE`.
- **Telegram** is text-heavy and conversational but not emoji-dense
  outside of stickers (which the persona system doesn't render).
  `casual` + `low` deliberately drops out of the `emoji_ok` set so
  the router lands on `CASUAL_OR_SHORT` rather than `CASUAL_EMOJI`,
  giving the AC's preferred branch and producing a different prompt
  variant from iMessage/Discord (avoids variant monoculture across the
  four Tier-1 channels — Story A's tile will show this clearly).

### 3.3 In-memory struct literal (test fixture)

The same four overlays expressed as C struct literals for the new test
file. String literals (not allocator-owned strings) — `hu_persona_overlay_t`
fields are non-const `char *` but the router only reads them.

```c
static const hu_persona_overlay_t tier1_overlays[] = {
    {
        .channel     = (char *)"imessage",
        .formality   = (char *)"casual",
        .avg_length  = (char *)"short",
        .emoji_usage = (char *)"moderate",
    },
    {
        .channel     = (char *)"discord",
        .formality   = (char *)"casual",
        .avg_length  = (char *)"short",
        .emoji_usage = (char *)"high",
    },
    {
        .channel     = (char *)"slack",
        .formality   = (char *)"professional",
        .avg_length  = (char *)"short",
        .emoji_usage = (char *)"minimal",
    },
    {
        .channel     = (char *)"telegram",
        .formality   = (char *)"casual",
        .avg_length  = (char *)"short",
        .emoji_usage = (char *)"low",
    },
};
```

This is the exact pattern already in use at
`tests/test_personal_model.c:2187-2261` and
`tests/test_persona.c:1288-1295`. No allocator, no JSON, no file I/O —
the struct is plain data.

---

## 4. Test design

### 4.1 New file: `tests/test_persona_directive_channels.c`

Drives `hu_personal_model_build_prompt_with_overlay` (not the static
`directive_variant_for_overlay`) for each of the four Tier-1 overlays in
turn. The variant assertion is **the telemetry counter**, which is the
public, deterministic, single-source-of-truth signal:

- `acknowledgment_directive_for_overlay` is a `static` function in
  `src/memory/personal_model.c:359`. It is unreachable from a test
  without exposing it. Same for `directive_variant_for_overlay`.
- Both increment `s_directive_counts[v]` via `HU_DIRECTIVE_INC` before
  returning. The counter is **per-variant**, accessible via
  `hu_personal_model_directive_telemetry_snapshot`.
- Asserting `snap.counts[EXPECTED_VARIANT] == 1` after a single fire is
  isomorphic to asserting `directive_variant_for_overlay(overlay) ==
  EXPECTED_VARIANT`. The PO's AC text ("hard assertions on the return
  value") is satisfied because the counter increment IS the observable
  side-effect of that return value being computed.

Test cases (matching the AC structure):

```c
/* AC-C.1: starter persona overlays load and are reachable. */
static void persona_directive_starter_persona_loads_four_tier1_overlays(void);

/* AC-C.2: Discord → CASUAL_EMOJI. */
static void persona_directive_discord_overlay_fires_casual_emoji(void);

/* AC-C.2: iMessage → CASUAL_EMOJI. */
static void persona_directive_imessage_overlay_fires_casual_emoji(void);

/* AC-C.3: Slack → FORMAL_TERSE. */
static void persona_directive_slack_overlay_fires_formal_terse(void);

/* AC-C.4: Telegram → CASUAL_OR_SHORT. */
static void persona_directive_telegram_overlay_fires_casual_or_short(void);

/* AC-C.6: after firing all four Tier-1 overlays in a single batch,
 * null_overlay count is 0, total is 4, and each expected bucket has
 * the right count. This is the "telemetry has zero null_overlay for
 * these channels after run" assertion called out in the user query. */
static void persona_directive_tier1_batch_yields_zero_null_overlay(void);
```

Skeleton for one test (the others follow the same shape — only the
overlay literal and expected variant change):

```c
static void persona_directive_discord_overlay_fires_casual_emoji(void) {
    hu_personal_model_directive_telemetry_reset();

    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* Minimal model — fact_count == 0 is fine. The directive wording
     * does not depend on facts; only the overlay routing matters. */

    hu_persona_overlay_t overlay = {
        .channel     = (char *)"discord",
        .formality   = (char *)"casual",
        .avg_length  = (char *)"short",
        .emoji_usage = (char *)"high",
    };

    char buf[2048];
    (void)hu_personal_model_build_prompt_with_overlay(&m, &overlay,
                                                      buf, sizeof(buf));

    hu_directive_telemetry_t s;
    hu_personal_model_directive_telemetry_snapshot(&s);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_CASUAL_EMOJI], 1ULL);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_NULL_OVERLAY], 0ULL);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_FORMAL_TERSE], 0ULL);
    HU_ASSERT_EQ((unsigned long long)s.total, 1ULL);
}
```

The batch test (`persona_directive_tier1_batch_yields_zero_null_overlay`)
loops the array from §3.3, fires once per overlay, then asserts:

```c
HU_ASSERT_EQ(s.total, 4ULL);
HU_ASSERT_EQ(s.counts[HU_DIRECTIVE_VARIANT_NULL_OVERLAY], 0ULL);
HU_ASSERT_EQ(s.counts[HU_DIRECTIVE_VARIANT_CASUAL_EMOJI], 2ULL);     /* discord + imessage */
HU_ASSERT_EQ(s.counts[HU_DIRECTIVE_VARIANT_FORMAL_TERSE], 1ULL);     /* slack */
HU_ASSERT_EQ(s.counts[HU_DIRECTIVE_VARIANT_CASUAL_OR_SHORT], 1ULL);  /* telegram */
HU_ASSERT_EQ(s.counts[HU_DIRECTIVE_VARIANT_DEFAULT], 0ULL);
HU_ASSERT_EQ(s.counts[HU_DIRECTIVE_VARIANT_ADAPTIVE_EMOJI], 0ULL);
```

That single test discharges AC-C.6 with one snapshot — exactly the
"telemetry has zero null_overlay for these channels after run"
assertion called out in the user query.

The AC-C.1 starter-persona test loads `hu_starter_persona_json` via
`hu_persona_load_json` and asserts `hu_persona_find_overlay(&p, "<channel>", strlen("<channel>")) != NULL`
for each of the four channels, plus `hu_persona_validate_json` returns
`HU_OK` (regression guard against the malformed-array bug returning).

Suite label: `HU_TEST_SUITE("persona_directive_channels")` — case-insensitive
substring match makes `./build/human_tests --suite=persona_directive`
work as specified in AC-C.5.

### 4.2 Why not exercise via `hu_persona_build_prompt`?

`hu_persona_build_prompt` is the persona-level builder; it formats the
overlay fields into a system prompt block. The directive routing is in
`hu_personal_model_build_prompt_with_overlay`, which is the per-turn
personal-model builder. Both can be reached, but the personal-model path
is the one that fires the telemetry counter (`HU_DIRECTIVE_INC` is only
inside `acknowledgment_directive_for_overlay`, which is only called from
`hu_personal_model_build_prompt_with_overlay` at `src/memory/personal_model.c:563`).
Use the path that increments the counter so the AC-C.6 assertion has
something to read.

---

## 5. Suite registration

Two edits, both mechanical, following the existing pattern.

### 5.1 `CMakeLists.txt`

Append the new file to the persona test source list at line 2702:

```cmake
list(APPEND HU_TEST_SOURCES tests/test_persona.c tests/test_circadian.c
    tests/test_relationship.c tests/test_replay.c tests/test_style_clone.c
    tests/test_life_sim.c tests/test_persona_mood.c
    tests/test_persona_feedback.c tests/test_persona_cli.c
    tests/test_voice_maturity.c tests/test_style_learner.c
    tests/test_temporal.c tests/test_inner_world.c
    tests/test_persona_eval.c
    tests/test_persona_directive_channels.c)
```

### 5.2 `tests/test_main.c`

Two edits, mirroring the existing `run_persona_tests` pattern:

- Forward declaration block (near line 99 — alphabetically grouped):

  ```c
  void run_persona_directive_channels_tests(void);
  ```

- Invocation block (near line 643, immediately after `run_persona_tests();`):

  ```c
  run_persona_directive_channels_tests();
  ```

No other registration is needed. The framework auto-discovers tests
inside `run_persona_directive_channels_tests()` via `HU_RUN_TEST`.

---

## 6. Test fixture decision

**In-memory struct literals. No JSON fixture file.**

Rationale:

| Option                                       | Pros                                              | Cons                                                                                                              |
| -------------------------------------------- | ------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| **In-memory struct literals (chosen)**       | No file I/O. No allocator. Deterministic. Matches the existing pattern at `tests/test_personal_model.c:2187-2261`. Drops the build-time dependency on a fixture path. | None material.                                                                                                    |
| Loadable JSON fixture (`tests/fixtures/tier1_overlays.json`) | Exercises the parser end-to-end.                | Adds a file the parser must keep aligned with. Duplicates the starter persona — drift risk.                       |
| Load the starter persona via `hu_starter_persona_json` symbol | One source of truth.                  | Used for AC-C.1 (load+find-overlay) but overkill for AC-C.2/3/4 routing assertions — too much surface for a pure threshold pin. |

The new test does both:

- **Routing assertions (AC-C.2/3/4/6)**: in-memory struct literals.
- **Loading assertion (AC-C.1)**: parse the shared
  `hu_starter_persona_json` symbol via `hu_persona_load_json`, then
  iterate `hu_persona_find_overlay` for each Tier-1 channel ID.

This split keeps each test cheap to read and modify in isolation.

---

## 7. Production overlay placement

**Critical for AC-C.6 in spirit:** if overlays only live in test
literals, a running daemon still emits `null_overlay` 100% of the time
and the production telemetry that the Story A dashboard tile reads stays
at zero in every non-NULL bucket. The starter persona must ship the
fixed overlays.

### 7.1 Files modified

| File                            | Change                                                                                                                                                                                                                          |
| ------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/onboard.c`                 | Replace `static const char HU_ONBOARD_DEFAULT_PERSONA[]` (lines 80-161) with `const char hu_starter_persona_json[]` (extern-visible) holding the object-shape JSON from §3.1. Drop the comment that admits duplication.         |
| `src/cli_commands.c`            | Remove the duplicate `HU_INIT_DEFAULT_PERSONA` literal (lines 85-161). Replace `HU_INIT_DEFAULT_PERSONA` references at lines 231-232 with `hu_starter_persona_json` and use `strlen()` instead of `sizeof(...) - 1`.             |
| `include/human/onboard.h`       | Add `extern const char hu_starter_persona_json[];` declaration (or place in a new tiny header `include/human/persona/starter.h` if the maintainer prefers; the function this serves is "shared C-literal" — header is trivial). |

### 7.2 Why centralize

- One canonical JSON, one place to update, one place to validate. The
  current duplication has already led to drift (`HU_ONBOARD_DEFAULT_PERSONA`
  has a comment at `src/onboard.c:79` admitting it). After this change,
  the only way the two paths can diverge is by removing the shared
  symbol — a far louder mistake than silent literal drift.
- Story A's dashboard surfaces `directive_telemetry`. If a user runs
  `human init` (path 1) or `human onboard` (path 2), they should see
  the same telemetry distribution. Sharing the symbol guarantees this.
- The test in §4 can include the shared symbol directly via a single
  `extern const char hu_starter_persona_json[];` declaration, pinning
  the AC-C.1 load-from-production-blob assertion to the exact bytes the
  user will receive.

### 7.3 Why this is in scope

The Story C out-of-scope list excludes "Modifying `human init` or the
onboarding wizard to **generate overlays interactively**". The change
here is **not interactive** — it's a static-data correction to the
shipped blob. The blob already contains a `channel_overlays` section
(broken). We are converting it to a correct shape; we are not adding new
user flow. AC-C.1 plus AC-C.6 jointly require this fix; without it,
AC-C.6's "production telemetry shows non-zero" cannot be satisfied.

### 7.4 Existing-user upgrade behavior

Users who already ran `human init` against the broken blob have a
`~/.human/personas/default.json` on disk with the malformed array shape.
After this change, the daemon still loads their existing file (silently
producing zero overlays) — no regression vs today. They can recover by
deleting the file and re-running `human init`, or hand-editing.

An auto-migrator is **out of scope** for this sprint. If we decide to
add one later, it lives in `src/persona/persona.c` as a one-shot fixup
in `hu_persona_load_json` (detect array shape → emit a warning →
attempt object-shape coercion). Not Story C work.

---

## 8. Files to modify (summary table)

| File                                              | Change                                                                                                                              | LOC      |
| ------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- | -------- |
| `src/onboard.c`                                   | Rewrite starter persona literal (object shape, string values) and export as `hu_starter_persona_json`.                              | ±80      |
| `src/cli_commands.c`                              | Remove duplicate literal, reference shared symbol via `strlen`.                                                                     | −80, +5  |
| `include/human/onboard.h`                         | Add `extern const char hu_starter_persona_json[];`.                                                                                 | +3       |
| `tests/test_persona_directive_channels.c` (new)   | Six tests as specified in §4.1. Suite label `"persona_directive_channels"`.                                                         | +220     |
| `tests/test_main.c`                               | Forward decl + invocation.                                                                                                          | +2       |
| `CMakeLists.txt`                                  | Append new test file to `HU_TEST_SOURCES` at line 2702.                                                                             | +1       |

Net diff: ~+150 LOC. No struct changes. No new public APIs in
`include/human/memory/personal_model.h` (the existing telemetry export
is sufficient).

---

## 9. Risks

### 9.1 Backward-compatibility (LOW / SMALL)

- Concrete failure: a test elsewhere asserts on the exact bytes of
  `HU_INIT_DEFAULT_PERSONA` or `HU_ONBOARD_DEFAULT_PERSONA`.
- Probability: low. `rg "HU_INIT_DEFAULT_PERSONA|HU_ONBOARD_DEFAULT_PERSONA" tests/`
  returns zero matches (verified during this design pass).
- Mitigation: rename the symbol explicitly to `hu_starter_persona_json`
  so any latent reference fails to compile rather than silently picking
  up the wrong blob.

### 9.2 Validator drift (LOW / SMALL)

- Concrete failure: `hu_persona_validate_json` already rejects the
  starter persona today (because of the array-shape `channel_overlays`),
  but no test exercises that fact. After this fix, validation passes.
  If any test did rely on the current "starter blob is invalid"
  behavior, it breaks.
- Probability: very low. `rg "validate.*starter\|validate.*default\.json" tests/`
  is empty.
- Mitigation: the new AC-C.1 test asserts `hu_persona_validate_json`
  returns `HU_OK` for the shared symbol — turns this from latent risk
  into a positive guard.

### 9.3 Telemetry counter contamination (LOW / MEDIUM)

- Concrete failure: another test in the same process leaves
  `s_directive_counts` non-zero, and the AC-C.6 batch test reads stale
  counts.
- Probability: low. Every existing test that touches telemetry already
  calls `hu_personal_model_directive_telemetry_reset()` first; the new
  tests do the same.
- Mitigation: every test in `tests/test_persona_directive_channels.c`
  calls `_reset()` at the top, before any model init. AC-C.6's batch
  test asserts both per-bucket counts AND `total == 4`, so a stray
  increment from elsewhere would surface as a `total` mismatch (loud
  failure).

### 9.4 String-pool ownership in struct literals (LOW / SMALL)

- Concrete failure: `hu_persona_overlay_t.channel` etc. are non-const
  `char *`. A future `hu_persona_deinit`-style call against a literal
  overlay would attempt to free read-only memory.
- Probability: low. Test never calls deinit on these literals (matches
  existing pattern at `tests/test_persona.c:1286-1311` which uses the
  same trick).
- Mitigation: stack-allocate the overlay inside each test (not file
  scope), so reuse outside the test function is structurally
  impossible.

### 9.5 Implicit-`atoi` trap in `avg_length` (LOW / SMALL)

- Concrete failure: someone later changes `"short"` to a numeric string
  like `"40"` (today's broken value), and `atoi("40")` is 40 which is
  >30, so `short_length` becomes false and the variant flips.
- Probability: low; we are explicitly setting `"short"` everywhere.
- Mitigation: §1 of this design names the trap. The threshold table
  goes inline as a comment block at the top of the new test file. Any
  reviewer who later wants to change a value reads the threshold
  before editing.

### 9.6 Per-channel norm choice (MEDIUM / SMALL)

- Concrete failure: a user disagrees with `emoji_usage="high"` on
  Discord or `formality="professional"` on Slack and re-files an issue.
- Probability: medium — these are stylistic calls.
- Mitigation: the values are user-editable in `~/.human/personas/default.json`
  by design. The starter is a starting point, not an authoritative
  brand voice. Document this in the JSON file via a `style_notes`
  array entry per channel.

### 9.7 Observability for the production claim

After this change ships, an operator can verify production health by:

```bash
# Run the agent for a few turns across the four Tier-1 channels, then:
curl -s "$HUMAN_GATEWAY/metrics/directive_telemetry" | jq .
```

Expected: `counts.null_overlay == 0` for those four channels (in the
running daemon, not just the test). Story A's dashboard tile will
surface this without curl. This is the end-to-end proof of AC-C.6
"production telemetry shows non-zero" beyond the test-suite assertion.

---

## 10. Implementation steps (for the implementer agent)

In order. Each step is reversible. Run tests after each step.

1. **Add `extern const char hu_starter_persona_json[];`** to
   `include/human/onboard.h`. Compile — should still pass, no
   definitions yet.
2. **Move the persona literal** out of `src/onboard.c` and define
   `const char hu_starter_persona_json[] = "{ ... }";` in the same file
   (or a new `src/persona/starter.c`; either is fine — `onboard.c`
   is fine for minimal churn). Rewrite the literal to the §3.1 shape.
   Update `hu_onboard_run_with_args` to reference the new symbol and
   use `strlen(hu_starter_persona_json)` instead of `sizeof(...)-1`.
3. **Remove the duplicate literal** from `src/cli_commands.c` and
   replace references with `hu_starter_persona_json` + `strlen`. Include
   `human/onboard.h` if not already.
4. **Run `cmake --build build && ./build/human_tests`** — the full
   suite should still pass; nobody references the old symbol names.
5. **Create `tests/test_persona_directive_channels.c`** with the six
   tests from §4.1, all calling `_reset()` then
   `hu_personal_model_build_prompt_with_overlay`. Use the §3.3 struct
   literals.
6. **Wire the suite** — append to `CMakeLists.txt:2702`, add forward
   decl + call in `tests/test_main.c`.
7. **Verify**:

   ```bash
   cmake --build build -j$(nproc)
   ./build/human_tests --suite=persona_directive
   ```

   Expect 0 failures, 0 ASan errors, six tests run.
8. **Run the full suite**:

   ```bash
   ./build/human_tests
   ```

   Expect 0 failures, 0 ASan errors, all 9,800+ tests pass.
9. **Run `scripts/agent-preflight.sh`** to catch any cross-cutting
   regressions (docs, tokens, etc.).
10. **Manual smoke** (optional but recommended for the "production
    telemetry" claim): rebuild, delete `~/.human/personas/default.json`,
    run `human init` to recreate it, then `human persona validate default`
    — should return success (this exercises the validator path that
    silently rejects the current array shape).

---

## 11. Acceptance criteria mapping

| AC     | Verified by                                                                                                                                                    |
| ------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| AC-C.1 | `persona_directive_starter_persona_loads_four_tier1_overlays` — loads `hu_starter_persona_json` via `hu_persona_load_json`, asserts non-NULL `hu_persona_find_overlay` for each of `"telegram"`, `"discord"`, `"imessage"`, `"slack"`. Also asserts `hu_persona_validate_json` returns `HU_OK`. |
| AC-C.2 | `persona_directive_discord_overlay_fires_casual_emoji` + `persona_directive_imessage_overlay_fires_casual_emoji` — each asserts `counts[CASUAL_EMOJI] == 1` after a single fire. |
| AC-C.3 | `persona_directive_slack_overlay_fires_formal_terse` — asserts `counts[FORMAL_TERSE] == 1`. (Picks the preferred of `FORMAL_TERSE` / `ADAPTIVE_EMOJI`.)         |
| AC-C.4 | `persona_directive_telegram_overlay_fires_casual_or_short` — asserts `counts[CASUAL_OR_SHORT] == 1`. (Picks the preferred of `CASUAL_OR_SHORT` / `CASUAL_EMOJI`.) |
| AC-C.5 | `./build/human_tests --suite=persona_directive` runs the suite labeled `"persona_directive_channels"` (case-insensitive substring match). All six tests pass. 0 ASan errors. |
| AC-C.6 | `persona_directive_tier1_batch_yields_zero_null_overlay` — fires the four overlays in one batch and asserts `counts[NULL_OVERLAY] == 0`, `counts[DEFAULT] == 0`, `total == 4`, plus per-bucket counts match §3.2. Production-side claim discharged by the §7 starter-persona fix (see §9.7 observability paragraph). |

---

## 12. AC amendments / open questions

### 12.1 Proposed amendment: include the starter-persona fix in AC-C.6's scope

The PO AC-C.6 reads: "the variant-specific counts are non-zero —
verified by a test assertion or by inspecting the snapshot struct in
the test". This is satisfied at the test level by §4.1's batch test.

But the user-query brief asks: "if overlays only live in test
fixtures, AC-C.6 'production telemetry shows non-zero' is technically
not satisfied". This is correct. To satisfy that production reading,
we MUST fix the starter persona blob in `src/cli_commands.c` and
`src/onboard.c` as in §7. I treat this as in-scope per §7.3, and AC-C.6
is satisfied by the test assertion plus the static data fix.

**No AC change is required** — the PO ACs as written are achievable and
the static data fix is a necessary implementation detail. If a sprint
auditor wants stronger language, propose adding:

> AC-C.7 (suggested addition, not required): The starter persona blob
> written by `human init` and `human onboard` parses without warnings
> under `hu_persona_validate_json` and yields a non-empty `overlays`
> array — verified by a C test that reads the shared
> `hu_starter_persona_json` symbol and asserts both conditions.

This is already covered by §4.1's `persona_directive_starter_persona_loads_four_tier1_overlays`,
so the suggestion is documentary, not behavioral.

### 12.2 Contradiction discovered (not blocking)

The CURRENT starter persona in `cli_commands.c` and `onboard.c` is
silently invalid in two ways (array shape + numeric values). This was
not in the PO's dependency list. It does not contradict the AC — it
is in fact the **reason** the AC exists. Calling it out here so the
implementer knows the fix is structural, not just additive.

### 12.3 Resolved open question

User query: "The variant routing thresholds are implementation-defined —
are they documented anywhere?"

Resolution: they were not. §1 of this document is the canonical
reference. The thresholds are also re-stated as a comment block at the
top of `tests/test_persona_directive_channels.c` so the table travels
with the test.

---

`RESULT_tech-lead=DESIGN_READY`
