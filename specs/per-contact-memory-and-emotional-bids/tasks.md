# Per-Contact Memory + Emotional Bids (PCMEB) — Tasks

> 11 implementation tasks + 3 verification tasks across 5 dispatch waves.
> Every AC has ≥1 task; every task maps to ≥1 AC. Wave dependencies and
> agent-budget notes at the bottom.

## Implementation tasks

| # | Task | ACs | Owner | Status |
|---|---|---|---|---|
| 1 | Add `hu_relationship_type_t` enum to `include/human/persona.h` with members `{UNKNOWN, INTIMATE, CLOSE_FRIEND, FAMILY, PROFESSIONAL, ACQUAINTANCE}`. Add helpers `hu_relationship_type_from_string(const char*)` and `hu_relationship_type_to_string(hu_relationship_type_t)` in `src/persona/persona.c`. Map string variants: `"friend" → CLOSE_FRIEND`, `"coworker" \| "professional" → PROFESSIONAL`, `"family" → FAMILY`, `"acquaintance" → ACQUAINTANCE`, `"intimate" → INTIMATE`, anything else → UNKNOWN. Add `hu_relationship_type_t relationship_type_enum` field to `hu_contact_profile_t`. Populate the enum at parse time from the existing string. Add `tests/test_relationship_type.c` with: known strings → expected enums, NULL → UNKNOWN, unknown string → UNKNOWN, round-trip persona save→load preserves both the string AND the resulting enum. | AC-1 | general-purpose | pending |
| 2 | Extend `hu_contact_profile_t` in `include/human/persona.h` with: `char **top_emoji; size_t top_emoji_count; size_t top_emoji_cap;` (matching `filler_bank` pattern) AND a nested `hu_contact_style_profile_t style_profile` substruct with fields `size_t avg_msg_len_chars; float punctuation_density; float caps_usage; float emoji_per_msg`. Update the JSON writer at `src/persona/creator.c:1189-1249` to emit `"top_emoji": [...]` (only when count>0) and a nested `"style_profile": {...}` object (only when any field is non-zero). Update the parser at `src/persona/persona.c:2085-2214` to read both (absent → defaults). Soft-cap top_emoji at 10 entries with parse-time stderr warning matching PR #78 pattern. Free both in `free_contact_profile()` at `src/persona/persona.c:161-185`. Add round-trip test `tests/test_contact_profile_pcmeb_schema.c`: write a contact with 5 top_emoji and a populated style_profile, save+load, verify all fields preserved. | AC-5, AC-6 | general-purpose | pending |
| 3 | Add three new actions to `src/persona/cli.c` under a new `contact` subcommand (mirror the `filler` subcommand pattern from PCTT Task 7). Actions: `set-type <persona> <contact_id> <type>`, `show <persona> <contact_id>`, `forget <persona> <contact_id> [--yes]`. Add `HU_PERSONA_ACTION_CONTACT_SET_TYPE`, `_CONTACT_SHOW`, `_CONTACT_FORGET` enum values + parse + run handlers. `set-type` accepts type strings: `intimate`, `close-friend`, `family`, `professional`, `acquaintance`, `unknown` and updates the existing string field (the parse-time enum cache is auto-populated on next load). `show` prints the full profile (name, relationship_type string + parsed enum, top_emoji, style_profile, recent_topics). `forget` zeros learned fields (sets relationship_type string to NULL/UNKNOWN, frees top_emoji, zeros style_profile, frees recent_topics) but preserves `contact_id` + `name`. Prompts `y/N` confirmation unless `--yes` flag. All three save atomically via the existing persona save path. Update usage string in `src/main.c:cmd_persona()`. Tests in `tests/test_persona_cli.c`: parse-correctness for each subcommand (4 cases) + a run-correctness test for `set-type` that round-trips through disk and reloads. | AC-2, AC-15 | general-purpose | pending |
| 4 | New module: `include/human/cognition/bid.h` + `src/cognition/bid.c`. Define `hu_bid_type_t {HU_BID_NONE=0, HU_BID_VULNERABILITY, HU_BID_EXCITEMENT, HU_BID_DISTRESS}` and `hu_bid_result_t { hu_bid_type_t type; float confidence; }`. Implement `hu_error_t hu_cognition_detect_bid(const char *msg, size_t msg_len, hu_bid_result_t *out)`. Internal: phrase tables per type + emoji tables per type. Confidence formula: phrase-match contributes 0.4, emoji-match contributes 0.3, additional emotional emoji adds 0.15 each (cap 1.0); confidence < 0.6 returns HU_BID_NONE. Hard negatives: `"i need to grab milk"` / `"i need a coffee"` / `"i feel like a snack"` must NOT trigger VULNERABILITY — implement via a mundane-noun suppression list checked after `"i need"` / `"i feel"`. Test file `tests/test_cognition_bid.c`: ≥10 positive cases per type + ≥5 hard negatives per type. Include the literal `"I need cuddles 😩"` case asserting VULNERABILITY at confidence ≥ 0.6. Microbenchmark test confirms <100µs/message on a 100-call loop (use `clock_gettime` or h-uman's existing timing utility). | AC-8 | general-purpose | pending |
| 5 | New module: `include/human/cognition/topic_stay.h` + `src/cognition/topic_stay.c`. Define `hu_topic_stay_result_t { bool pivot_detected; float confidence; char top_noun_a[64]; char top_noun_b[64]; }`. Implement `hu_error_t hu_cognition_check_topic_stay(const char *inbound, size_t inbound_len, const char *outbound, size_t outbound_len, hu_topic_stay_result_t *out)`. Heuristic: extract up to 2 "content nouns" from `inbound` (≥4 chars, not in a stoplist of {the, and, you, are, for, was, this, that, with, have, what, your, just, like, when, will, can, all, get, got}); if neither appears in the first sentence of `outbound` (defined as text up to first `.!?\n`), set `pivot_detected = true` with confidence = 0.7. Log a single-line warning to stderr when `pivot_detected` AND `HU_TOPIC_STAY_ENFORCE` is undefined: `WARN: topic-stay pivot detected: inbound nouns=[%s,%s] absent from outbound first sentence`. Add `#ifdef HU_TOPIC_STAY_ENFORCE` branch that returns `HU_ERR_INVALID_OPERATION` instead — this is the Wave 3 flip path; do not define the macro in any preset yet. Test `tests/test_cognition_topic_stay.c`: assert literal Jordan case (inbound=`"let's do a sleepover tomorrow night"`, outbound=`"tbh lucky! Enjoy the break! ✨"`) returns `pivot_detected=true`. Also assert non-pivot case (inbound=`"sleepover tomorrow?"`, outbound=`"yes! sleepover sounds great"`) returns `pivot_detected=false`. | AC-12 | general-purpose | pending |
| 6 | New module: `include/human/cognition/response_shape.h` + `src/cognition/response_shape.c`. Define `hu_response_shape_t {HU_SHAPE_TEXT=0, HU_SHAPE_TAPBACK, HU_SHAPE_SILENCE}`. Implement `hu_response_shape_t hu_cognition_decide_response_shape(const char *msg, size_t msg_len, const hu_contact_profile_t *contact)`. This wave: ALWAYS return `HU_SHAPE_TEXT` (stub). Add the call site in `src/daemon.c` near the dispatch site — find the LLM-call section (around 4518-4531 per exploration) and add a switch statement on the returned shape with three cases (TAPBACK and SILENCE are present-but-unreachable; TEXT falls through to the current path). Add a one-line comment at the function noting "Wave 3 will replace this stub with real decision logic." Test `tests/test_cognition_response_shape.c`: assert stub returns TEXT for any input including NULL inputs; assert the daemon dispatch path correctly takes the TEXT branch. | AC-16 | general-purpose | pending |
| 7 | New module: `include/human/cognition/contact_hints.h` + `src/cognition/contact_hints.c`. This is **the privacy fence** — only this file synthesizes per-contact prompt blocks. Implement `hu_error_t hu_cognition_contact_hints_build_context(hu_allocator_t *alloc, const hu_contact_profile_t *contact, const hu_bid_result_t *bid, const hu_personal_model_t *user_model, char **out, size_t *out_len)`. The block is built section-by-section with `snprintf` into a `hu_json_buf_t` (or equivalent growing buffer): (a) register hint from `contact->relationship_type_enum` per AC-3 spec text; (b) top_emoji hint if `contact->top_emoji_count > 0`; (c) style_profile hint if any style_profile field is non-zero; (d) recent_topics hint if `contact->recent_topics_count > 0`; (e) bid augmentation if `bid->type != HU_BID_NONE` AND `contact->relationship_type_enum` ∈ {INTIMATE, CLOSE_FRIEND, FAMILY} — emit the AC-9 string with bid_type lowercased; (f) self-disclosure hint if `bid->type == HU_BID_VULNERABILITY` AND `relationship_type_enum` ∈ {INTIMATE, CLOSE_FRIEND} AND `user_model` has emotional facts (heuristic grep on `i feel\|i've been\|lately` in personal_model facts; pull up to 3 most recent). NULL contact → empty output, return HU_OK. Add to `src/persona/persona.c` a new wrapper `hu_persona_build_prompt_with_contact(...)` that calls the existing `hu_persona_build_prompt` and then appends the contact_hints block; declare in `include/human/persona.h`. Test `tests/test_cognition_contact_hints.c`: ≥5 cases covering each section's presence/absence with controlled fixtures. **CRITICAL:** the implementation must NOT call any other function that takes a `hu_persona_t *` for lookups — accept only the `contact` parameter that was already looked up by the caller. Add a comment block at the top of `contact_hints.c` reading: `/* PRIVACY FENCE: this file is the ONLY synthesizer of per-contact prompt blocks. Cross-contact data lookups are FORBIDDEN here. Any change to this file requires CODEOWNERS review. */` | AC-3, AC-5, AC-6, AC-7, AC-9, AC-10 | general-purpose | pending |
| 8 | Wire `hu_persona_build_prompt_with_contact` into the daemon dispatch path at `src/daemon.c` (LLM-call section around 4518-4531 per exploration). Look up the contact via `hu_persona_find_contact(agent->persona, batch_key, key_len)` (existing function at `src/persona/persona.c:379-398`) BEFORE the LLM call. Call `hu_cognition_detect_bid(...)` on the inbound message. Call `hu_cognition_decide_response_shape(...)` and switch (Task 6 wires the switch skeleton; this task adds the bid + contact lookups). Pass contact + bid result to `hu_persona_build_prompt_with_contact`. After the response is generated, call `hu_cognition_check_topic_stay(inbound, response)` for observability. Also: VERIFY that the existing `load_conversation_history` call honors `limit=10` (AC-4 spec) — if it currently passes a larger limit, tighten to 10 and document. Add an integration test `tests/test_daemon_pcmeb_integration.c` that constructs a fake channel + persona + contact, sends one inbound message, and asserts: the prompt the LLM was about to receive contains the expected register hint, the bid result was recorded, the topic-stay check ran. Use `HU_IS_TEST` guards on any side effects. | AC-4 | general-purpose | pending |
| 9 | Cross-contact privacy isolation test. New file `tests/test_contact_privacy_isolation.c`. Construct two contacts on one synthetic persona: Jordan with `relationship_type="intimate"` (parsed to INTIMATE), `top_emoji=["🥺","💕"]`, `recent_topics=["PM rollout"]`; and "Acme Boss" with `relationship_type="professional"` (parsed to PROFESSIONAL), `top_emoji=[]`, `recent_topics=["Q3 review"]`. For each contact: build the contact_hints block via `hu_cognition_contact_hints_build_context`. Assert: Jordan's block contains `🥺` AND `PM rollout` AND the INTIMATE register text; does NOT contain `Q3 review` OR `professional`. Boss's block contains `Q3 review` AND the PROFESSIONAL register text; does NOT contain `🥺` OR `PM rollout` OR the intimate hint. Each assertion must use a clear `HU_ASSERT` with a descriptive failure message naming the leaking field. **Mark this test file as a privacy fence** with a header comment matching `contact_hints.c`'s. | AC-11 | general-purpose | pending |
| 10 | Create `data/eval_persona_tone.json` with the schema `{ "cases": [{ "label", "contact": { "relationship_type", "top_emoji", "style_profile", "recent_topics", "name", "contact_id" }, "incoming_message", "expected_bid_type", "expected_register_hint_substring", "expected_augmentation_present" }, ...] }`. Populate with 25 cases: 5 contacts (one per non-UNKNOWN relationship_type) × 5 message types (vulnerability, excitement, distress, info-request, neutral). New test `tests/test_persona_tone_eval.c` loads the JSON via the `HU_TEST_DATA_DIR` define + `hu_json_parse`; for each case constructs the contact, runs `hu_cognition_detect_bid` on the incoming, asserts bid type matches expected; calls `hu_cognition_contact_hints_build_context`, asserts the expected register hint substring is present; asserts augmentation presence matches the gated flag. No LLM calls. Test must run in <1s total across all 25 cases. | AC-13 | general-purpose | pending |
| 11 | Create `data/eval_jordan_pcmeb.json` carrying the literal Jordan messages quoted in the spec preamble: morning + cuddles + tired-explanation message block, the sleepover proposal, the "no class" message. Plus the two literal h-uman replies (`"I'm in! What's the plan? 🍿"` and `"tbh lucky! Enjoy the break! ✨"`) for the topic-stay regression assertion. Schema: `{ "incoming": [{ "text", "expected_bid_type" }, ...], "outbound_pivots": [{ "inbound", "outbound", "expected_pivot": true }, ...] }`. New test `tests/test_jordan_pcmeb_regression.c` asserts: (a) `"I need cuddles 😩"` → bid=VULNERABILITY, conf≥0.6; (b) `"let's do a sleepover tomorrow night"` → bid=NONE; (c) `"I don't have class today and tomorrow 🙌🏻"` → bid=EXCITEMENT; (d) with synthetic contact relationship_type=INTIMATE, contact_hints block for the cuddles message contains the AC-9 augmentation string; (e) with synthetic contact relationship_type=PROFESSIONAL, that augmentation is ABSENT; (f) `hu_cognition_check_topic_stay(inbound="let's do a sleepover tomorrow night", outbound="tbh lucky! Enjoy the break! ✨")` returns `pivot_detected=true`. | AC-14 | general-purpose | pending |

## Verification tasks

| # | Task | ACs | Owner | Status |
|---|---|---|---|---|
| 12 | Run `/verify` (verifier agent) against the merged result. Build dev preset clean, run full test suite (must show 10,247 baseline + ~50 new tests pass), ASan clean, all 6 new suites resolved (`relationship_type`, `contact_profile_pcmeb_schema`, `cognition_bid`, `cognition_topic_stay`, `cognition_response_shape`, `cognition_contact_hints`, `contact_privacy_isolation`, `persona_tone_eval`, `jordan_pcmeb_regression`, `daemon_pcmeb_integration` — note suite names may differ slightly per project convention). Capture verbatim output for each assertion. Output `RESULT_verifier=PASS|FAIL|INCONCLUSIVE`. | all | verifier | pending |
| 13 | Run spec-verifier against `specs/per-contact-memory-and-emotional-bids/requirements.md` to confirm each of the 16 ACs has file:line evidence in the implementation. Output `RESULT_spec-verifier=PASS|FAIL`. | all | spec-verifier | pending |
| 14 | Run focused critic on the PCMEB merged result. Check for: cross-contact privacy bleed (try to find a code path that calls `contact_hints_build_context` with the wrong contact_id), bid detector false positives on a real-text fuzz sample, topic-stay false positives in casual chat, register hint phrasing that might over-constrain the LLM, missing free() in the new types, race conditions if the daemon ever becomes multi-threaded. Severity-tag and recommend fixes; user re-scores. | all | critic | pending |

## Dependencies

```
Wave 1 (3 in parallel) — schema + CLI:
  T1 (relationship enum) ──┐
  T2 (top_emoji+style) ────┤
  T3 (contact CLI) ────────┘ depends on existing schema only

Wave 2 (3 in parallel) — cognition modules (independent surfaces):
  T4 (bid detector)     ──┐
  T5 (topic-stay)       ──┤
  T6 (response-shape)   ──┘

Wave 3 (1 sequential, 1 parallel) — integration:
  T7 (contact_hints synthesizer + privacy fence) → needs T1, T2, T4
  T9 (privacy isolation test)                    → needs T7

Wave 4 (1 in parallel with T9, 1 sequential) — wiring + eval prep:
  T8 (daemon integration)  → needs T4, T5, T6, T7
  T10 (persona-tone eval)  → needs T4, T7

Wave 5 (1 sequential after all impl tasks):
  T11 (Jordan regression)  → needs T4, T5, T7

Wave 6 (verification):
  T12 (/verify)          → needs all impl
  T13 (spec-verifier)    → needs T12
  T14 (critic)           → parallel with T13
```

### Dispatch waves (parallel-safe groups)

- **Wave 1 (parallel: T1, T2, T3)** — schema + CLI. Each agent in own worktree, merges Wave 0 (post-PCTT main). ~150-250 LOC each.
- **Wave 2 (parallel: T4, T5, T6)** — cognition modules, fully independent. ~150-300 LOC each. Note T4 (bid detector) is the most lexicon-heavy; budget more tool-uses for it.
- **Wave 3 (parallel: T7, T8 wait)** — T7 is the BIG task (~400 LOC). After T7 ships, T8 picks up.
- **Wave 4 (parallel: T8, T9, T10)** — integration + privacy test + eval suite.
- **Wave 5 (sequential: T11)** — Jordan regression. Needs all upstream landed.
- **Wave 6 (sequential: T12 → T13, T14 parallel)** — verification.

## Agent-budget notes

- **Sonnet ~40-tool-use limit** (lesson from PCTT) — design each task to fit in ≤30 tool uses where possible, or pre-instruct the agent to STOP and report at a checkpoint so SendMessage continuation is cheap.
- **T7 (contact_hints) is the budget-risk task.** Six sections × ~50 LOC each + tests + privacy fence comments + JSON buffer plumbing. Likely ≥40 tool uses. The agent's prompt should explicitly say: "If you near the budget after sections a-d are done, STOP and report current state with `git status` — section e-f and tests will be a continuation."
- **Wave 1 merge preamble** — each agent must merge post-PCTT main first (commits `60b06e9c`, `baf3f78a`, `bca99e81`).

## Quality gates (from global rules)

- Build clean with `cmake --preset dev` (-Wall -Wextra -Wpedantic -Werror).
- ASan clean across full suite.
- Topology check still passes.
- Existing 10,247-test baseline retained; new tests bring total to ~10,300+.
- Grep guard (PCTT Task 9) continues to pass — no hardcoded fillers introduced.
- Privacy isolation test (T9 / AC-11) is REQUIRED for merge.
- CODEOWNERS entries (delivered as part of T7 or a sibling: see follow-up note below) on `src/cognition/contact_hints.c` and `tests/test_contact_privacy_isolation.c`.

## CODEOWNERS follow-up (sibling task, not blocking PCMEB merge)

If `.github/CODEOWNERS` does NOT exist in the repo, create it as part of T7 with entries pointing the new privacy-sensitive files to the same reviewers as the rest of the persona surface (or self-assign if no other owners exist). If CODEOWNERS already exists, append the entries. This is a low-LOC additive change; T7's agent can handle it.

## Out of scope (signposted, NOT in this spec)

- Auto-learning relationship_type, top_emoji, style_profile, recent_topics — Wave 4.
- Real response-shape logic (TAPBACK / SILENCE) — Wave 3.
- Multi-message fragmentation — Wave 3.
- Topic-stay send-blocking enforcement — Wave 3 flip of HU_TOPIC_STAY_ENFORCE.
- Proactive recall / bot-initiates-first — Wave 5.
- Cross-channel coherence (Jordan in iMessage + Slack as one identity) — Wave 5.
- ML-based bid detection — Wave 4+.
- Privacy audit dashboard / user-facing controls — Wave 6.
