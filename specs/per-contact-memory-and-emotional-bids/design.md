# Per-Contact Memory + Emotional Bids (PCMEB) — Design

> Architectural commitments locked from requirements.md feedback:
> 1. Ship-now / roadmap split is preserved — each Wave 3+ extension has a
>    named extension point in this design.
> 2. Topic-stay guard is observability-only; macro flips it to send-block
>    in Wave 3 without touching call sites.
> 3. Response-shape stub-now-wire-once: the dispatcher already routes
>    through `hu_cognition_decide_response_shape()`, even though only the
>    `TEXT` branch is active.
> 4. Privacy invariant is structural: CODEOWNERS + required-test fence on
>    `src/cognition/contact_hints.c` + `tests/test_contact_privacy_isolation.c`.

## Components

- **`hu_relationship_type_t` enum + helpers** — `include/human/persona.h`
  (new). Enum members + `hu_relationship_type_from_string()` (parses the
  existing `char *relationship_type` field on `hu_contact_profile_t`) +
  `hu_relationship_type_to_string()` (canonical form for JSON write).

- **`hu_contact_profile_t::relationship_type_enum`** — new field,
  cached after JSON parse from the existing string field. Reduces strcmp
  noise in hot paths. JSON-writes derive from the string (which remains
  authoritative on disk for forward-compat).

- **`hu_contact_profile_t::top_emoji[]`** — new `char **` + `size_t`
  fields per AC-5. Soft cap 10 entries, parse-time warning on overflow
  matching PR #78 truncation precedent.

- **`hu_contact_profile_t::style_profile`** — new nested substruct
  (`hu_contact_style_profile_t`) per AC-6. Fields: `avg_msg_len_chars`,
  `punctuation_density`, `caps_usage`, `emoji_per_msg`. Nested JSON object
  matching the existing `"communication_patterns"` / `"proactive"` nested
  pattern in the parser.

- **Reuse of existing fields:**
  - `recent_topics[]` (already on `hu_contact_profile_t`) → AC-7 reads
    from this directly. No new field needed.
  - `relationship_type` string (already exists) → AC-1 parses into the
    new enum view.

- **`hu_bid_*` types + `hu_cognition_detect_bid()`** — new
  `include/human/cognition/bid.h` + `src/cognition/bid.c`. Follows the
  `hu_<module>_init/evaluate/build_context` pattern from `rupture_repair.c`
  but with a simpler one-shot classifier shape (no state).

- **`hu_topic_stay_*` types + `hu_cognition_check_topic_stay()`** — new
  `include/human/cognition/topic_stay.h` + `src/cognition/topic_stay.c`.
  Observability-only; logs warning to stderr when pivot detected. Macro
  `HU_TOPIC_STAY_ENFORCE` (default off) flips behavior in Wave 3.

- **`hu_response_shape_*` types + `hu_cognition_decide_response_shape()`** —
  new `include/human/cognition/response_shape.h` +
  `src/cognition/response_shape.c`. Stub returns `HU_SHAPE_TEXT`
  unconditionally this wave; dispatcher already switches on the result.

- **`hu_cognition_contact_hints_build_context()`** — new
  `include/human/cognition/contact_hints.h` +
  `src/cognition/contact_hints.c`. The privacy-sensitive synthesizer.
  Takes a `hu_contact_profile_t *contact`, a `hu_bid_result_t *bid`, a
  `hu_personal_model_t *user_model`, and emits a prompt-block string.
  THIS FILE is the privacy fence — CODEOWNERS rule applies.

- **`hu_persona_build_prompt_with_contact()`** — new wrapper around
  the existing `hu_persona_build_prompt` (in `src/persona/persona.c`),
  takes the additional `contact_id` + `contact_id_len` parameters and
  internally looks up + appends the contact-hints block. Existing
  `hu_persona_build_prompt` keeps its signature for back-compat.

- **CLI: `human persona contact {set-type,show,forget}`** —
  three new actions added to `src/persona/cli.c`, parsed via the same
  `strcmp`-dispatch pattern as PCTT Task 7's `filler`.

- **Eval scenarios** — `data/eval_persona_tone.json` (25 cases),
  `data/eval_jordan_pcmeb.json` (Jordan's literal transcript).
  Loaded at test time via the existing `HU_TEST_DATA_DIR` define from PR #79.

- **Tests** — 6 new test files, each registered in `tests/test_main.c`.

## Data flow

```
inbound message arrives at daemon
    │
    ▼
src/daemon.c dispatch site
    │
    ├─ contact_profile = hu_persona_find_contact(persona, batch_key, ...)
    │     (already-existing function at src/persona/persona.c:379-398)
    │
    ├─ bid_result = hu_cognition_detect_bid(msg, msg_len)
    │     [AC-8] heuristic classification, <100µs
    │
    ├─ shape = hu_cognition_decide_response_shape(...)
    │     [AC-16] stub returns HU_SHAPE_TEXT unconditionally this wave
    │     switch(shape) {
    │         case HU_SHAPE_TAPBACK: /* Wave 3: route via tapback path */
    │         case HU_SHAPE_SILENCE: /* Wave 3: no-op */
    │         case HU_SHAPE_TEXT:    /* current path — fall through */
    │     }
    │
    ├─ hu_persona_build_prompt_with_contact(persona, contact_profile,
    │                                       bid_result, channel, msg, ...)
    │     ├─ existing hu_persona_build_prompt body
    │     ├─ if contact_profile != NULL:
    │     │     append hu_cognition_contact_hints_build_context(...) block
    │     │         which internally:
    │     │           [AC-3] register hint from relationship_type_enum
    │     │           [AC-5] "top_emoji" hint if non-empty
    │     │           [AC-6] "style with this contact" hint
    │     │           [AC-7] "recent topics" hint from recent_topics[]
    │     │           [AC-9] bid augmentation if gated relationship + bid
    │     │           [AC-10] self-disclosure hint if VULNERABILITY + intimate
    │     │       PRIVACY: lookup is by contact id; no cross-contact reads
    │     └─ output: persona prompt + contact prompt block
    │
    ├─ load_conversation_history(channel, contact_id, limit=10, ...)
    │     [AC-4] bounded last-10 window. Existing vtable; verify the limit
    │     is honored and tighten if not.
    │
    ├─ LLM call assembles: system_prompt + history (≤10) + current message
    │
    ├─ generated response received
    │
    └─ hu_cognition_check_topic_stay(inbound=msg, outbound=response, ...)
          [AC-12] log warning to stderr if pivot detected
          NOT send-blocking; observability only.
          #ifdef HU_TOPIC_STAY_ENFORCE → block + return error instead
          (default off; Wave 3 flips this default)
```

## Decisions

- **D1 — Enum + string coexistence for `relationship_type`.** The existing
  `char *relationship_type` field stays authoritative on disk; the new
  `hu_relationship_type_t relationship_type_enum` is a parse-time cache.
  Round-trip: JSON-parse → string + enum; JSON-write → string only.
  Forward-compat: missing string → enum=UNKNOWN; unknown string value →
  enum=UNKNOWN but string preserved verbatim. Why: avoids breaking any
  existing persona JSONs that have `"relationship_type": "friend"`. Why
  not enum-only: harder migration; loses information if JSON contains a
  type we don't know yet. **Serves AC-1.**

- **D2 — `hu_cognition_contact_hints_build_context()` is the privacy
  chokepoint.** ALL per-contact prompt synthesis goes through this one
  function. It receives `(contact_profile *, bid_result *, user_model *)`
  and emits a string block. The function MUST NOT call out to any other
  contact profile lookup. Code review enforces this via CODEOWNERS;
  `tests/test_contact_privacy_isolation.c` enforces it at runtime by
  distinguishing two contacts and asserting one's data never appears in
  the other's prompt. **Serves AC-11.**

- **D3 — Topic-stay guard is observability-only behind
  `HU_TOPIC_STAY_ENFORCE`.** The macro is undefined by default — runtime
  just logs warnings. Wave 3 will `#define HU_TOPIC_STAY_ENFORCE` and
  switch the call site from "log + continue" to "log + return
  HU_ERR_INVALID_OPERATION." Until then, we accumulate real-world data
  on false-positive rate. **Serves AC-12** and the architectural
  commitment from requirements.md.

- **D4 — Response-shape stub-now-wire-once.** `hu_cognition_decide_response_shape()`
  ships returning `HU_SHAPE_TEXT` unconditionally. The dispatcher
  (`src/daemon.c` near send site) calls it and switches on the result,
  with the TAPBACK / SILENCE branches as compile-time-present but
  runtime-unreachable. Wave 3 replaces the stub function body; no
  caller-side edits. **Serves AC-16.**

- **D5 — `hu_persona_build_prompt_with_contact()` wrapper, not
  signature change.** The existing `hu_persona_build_prompt` is called
  from non-daemon paths (test code, CLI). Adding a `contact_id`
  parameter would force every caller to thread it through. Instead, the
  new wrapper takes the additional parameters and delegates to the
  existing function for the persona half. **Serves AC-3, AC-5, AC-6,
  AC-7, AC-9, AC-10.**

- **D6 — Bid detector heuristic + confidence threshold.** Pure C
  heuristic with a phrase table + emoji table + per-type confidence
  formula. No ML this wave (ruled out by Constraints). Threshold 0.6
  empirically chosen to suppress mundane "I need X" false positives
  while catching emotional "I need cuddles 😩" cases. **Serves AC-8.**

- **D7 — Self-disclosure hint reads from `hu_personal_model_t`,
  not a new store.** The personal_model already tracks "i feel"
  patterns. AC-10's hint synthesizer grep-scans the personal_model's
  fact texts for emotional markers (matching `i feel`, `i've been`,
  `lately`) and pulls up to 3. No new memory layer required. **Serves
  AC-10.**

- **D8 — Eval data via `HU_TEST_DATA_DIR`.** Reuse the build-time
  define from PR #79 (corpus drift guard). Two new JSON files at
  `data/eval_persona_tone.json` and `data/eval_jordan_pcmeb.json`.
  **Serves AC-13, AC-14.**

- **D9 — `top_emoji[]` and `style_profile` are SETTABLE this wave,
  LEARNED in Wave 4.** Wave 1+2 ships the schema, JSON round-trip, CLI
  to set, and prompt-hint surfacing. The auto-population from past
  outbound corpus is Wave 4. This is the cheapest sequence: ship the
  surface area now so Wave 4 only adds the learner. **Serves AC-5,
  AC-6** + the architectural commitment to extension-point readiness.

## Risks

- **`relationship_type` string ambiguity.** Existing JSONs may have
  values like `"friend"` (we have `HU_REL_CLOSE_FRIEND`) or `"coworker"`
  (we have `HU_REL_PROFESSIONAL`). The string → enum mapping must
  canonicalize: `"friend" → HU_REL_CLOSE_FRIEND`, `"coworker" |
  "professional" → HU_REL_PROFESSIONAL`, `"family" → HU_REL_FAMILY`,
  `"acquaintance" → HU_REL_ACQUAINTANCE`. Unknown strings → UNKNOWN,
  log a one-line debug message but don't fail.

- **Privacy lookup mistake.** A future change could pass the wrong
  contact_id (e.g., the user's own id) into the hints builder and
  inadvertently inject another contact's data. AC-11 catches the
  Jordan-vs-Boss case; broader fuzzing not in scope but worth a
  follow-up.

- **Bid detector latency.** Heuristic must stay <100µs/message. Phrase
  table lookup via `strstr` over a small list is sub-µs; emoji match
  similar; overall well within budget. Verify with a microbenchmark in
  the test suite.

- **Topic-stay false positives.** Top-2-noun extraction is naive.
  Likely to flag many legitimate replies as pivots. That's fine for
  observability mode — we collect data, then refine. The macro
  `HU_TOPIC_STAY_ENFORCE` stays off until we have a calibrated
  threshold from real chats.

- **AC-10 self-disclosure may feel forced.** The model is told "share
  back proportionally if natural — don't force it." Without a soft
  prior, the LLM may overshare. Watch in eval; tighten wording if so.

- **CLI confirmation prompts break test automation.** AC-2's
  `forget` action prompts y/N. Tests pass `--yes` to skip. Documented
  in the test.

- **Existing `recent_topics[]` may be semantically different than what
  AC-7 expects.** AC-7 expects "topics talked about recently in this
  thread." Existing field may be looser. Inspect existing uses in
  daemon.c before reusing; if semantics diverge, add a new field
  rather than overload.

## Extension points (for Waves 3–6)

Locking the architectural commitments from requirements.md feedback:

- **Wave 3 — Response shape real logic.** The `hu_cognition_decide_response_shape()`
  function body is the *only* file to change. Caller-side switch
  remains unchanged.

- **Wave 3 — Topic-stay enforcement.** Define `HU_TOPIC_STAY_ENFORCE`
  in a CMake preset (e.g., `enforce` preset). The call site already
  handles both paths via `#ifdef`.

- **Wave 3 — Multi-message fragmentation.** Hook point at the
  send-call site: after the LLM returns a response string, a new
  `hu_cognition_fragment_response()` is called with the response +
  contact's `style_profile.avg_msg_len_chars` + `texts_in_bursts` flag
  (already on contact profile). Returns an array of strings; daemon
  sends each in sequence with timing. Schema is ready (style_profile
  ships in this wave); only the fragmenter is new.

- **Wave 4 — Auto-learn `top_emoji`, `style_profile`, `recent_topics`.**
  A new module `src/cognition/contact_learner.c` runs periodically
  against past chat history (read-only) and populates the fields.
  Hooks: contact profile schema is already complete; learner only adds
  the populator.

- **Wave 4 — Auto-classify `relationship_type`.** Same module; reads
  cadence + emoji-density + disclosure-depth signals → enum.

- **Wave 4 — Rupture/repair connection.** `src/cognition/rupture_repair.c`
  already exists. New function `hu_rupture_evaluate_for_contact(contact,
  recent_messages, out)` runs the existing logic scoped to a single
  contact's thread. Hook point: dispatch site already has contact
  profile in scope.

- **Wave 5 — Proactive recall.** Existing `proactive_checkin`,
  `proactive_channel`, `proactive_schedule` fields on contact profile.
  New module emits scheduled messages; schema already supports it.

- **Wave 5 — Cross-channel coherence.** Contact profile's `contact_id`
  becomes a *canonical id* with channel-specific handle aliases. New
  alias table on the profile.

- **Wave 6 — Privacy dashboard / audit log.** Every call into
  `hu_cognition_contact_hints_build_context()` already passes through
  one chokepoint. Adding an audit log there is a single-site change.
