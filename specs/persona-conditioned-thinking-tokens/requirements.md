# Persona-Conditioned Thinking Tokens (PCTT) — Requirements

> **Why this exists.** Today the thinking-filler system emits one of three hardcoded
> strings ("ooh that's a tough one", "let me think about that for a sec", "hm good
> question") for every question that crosses a length threshold, identical across
> every channel and every user. This is the OpenClaw pattern, not the h-uman pattern.
> CLAUDE.md says "persona as compiled architecture, not markdown templates"; the
> current filler module is the opposite of that thesis. Research synthesis (2015-2026)
> attached in the design doc.

## User stories

- **As a user texting with h-uman in iMessage**, I want fast direct replies without
  canned thinking fillers, so that the bot feels like a person who knows me, not
  a chatbot stalling for time.
- **As a user defining a persona**, I want my persona's own filler phrases — drawn
  from how I actually talk — to be the ones used, so the bot speaks in my voice
  on channels where pacing matters.
- **As a user on Discord or voice**, where pauses can feel natural, I want fillers
  that vary turn-over-turn and that fit the channel, so repetition doesn't break
  the illusion.
- **As a developer extending h-uman**, I want filler behavior to be a property of
  the persona (data) rather than a property of the daemon (code), so adding a new
  voice doesn't require a C patch.

## Acceptance criteria

- [ ] **AC-1 (iMessage off):** When the current channel's class is `text_fast`
  (which iMessage and SMS belong to), `hu_conversation_classify_thinking` returns
  `triggered=false` regardless of message length or question shape. Verified by a
  test calling the classifier with an iMessage channel context and a long question;
  expect `false`.

- [ ] **AC-2 (Empty bank → silence):** When the active persona's filler bank for
  the current channel is empty, `hu_conversation_classify_thinking` returns
  `triggered=false`. No hardcoded fallback strings are emitted under any
  circumstance. Verified by a test with an empty persona bank on a non-`text_fast`
  channel; expect `false`.

- [ ] **AC-3 (Recency-aware selection):** When the persona's filler bank for the
  current channel has ≥2 entries, selection is recency-aware: the most recently
  emitted filler in that chat is never selected back-to-back. Verified by a test
  that drives 100 emissions through a 3-entry bank with deterministic seeds and
  asserts no consecutive duplicates. With a 1-entry bank, the same filler may
  repeat (only constraint is "no consecutive duplicates"; AC-2 prevents zero-entry
  emissions).

- [ ] **AC-4 (Hardcoded fillers removed):** The `const char *fillers[…]` arrays at
  [src/context/conversation.c:4191-4211](src/context/conversation.c:4191) are
  deleted along with the `classify_think_type` enum/switch. `grep -rn "ooh that's
  a tough one\|let me think about that for a sec\|hm good question" src/` returns
  zero hits in `src/`. The eval data file `data/eval_blinded_ab.json` is excluded
  from this check (it preserves the historical failure case for evaluation).

- [ ] **AC-5 (Persona schema extension):** `hu_persona_overlay_t` gains a per-channel
  `filler_bank` field (NULL-terminated array of `char *` with explicit count),
  serialized through the existing JSON config format. Round-trip
  `hu_persona_save → hu_persona_load` preserves the bank's contents and order.
  Old configs without `filler_bank` load successfully with an empty bank
  (forward-compatible).

- [ ] **AC-6 (CLI management):** `human persona filler add --channel <name> "<text>"`
  appends; `human persona filler list --channel <name>` round-trips; `human persona
  filler remove --channel <name> --index <n>` removes by zero-based index.
  Persistence is via the same path as other persona mutations (atomic save through
  `hu_persona_save`, per the Phase 0 atomicity contract).

- [ ] **AC-7 (Echo regression eval):** `data/eval_pctt.json` exists and contains a
  scenario: given a corpus of 20+ incoming messages drawn from
  `data/eval_blinded_ab.json`, the bot's emitted filler is **never** byte-identical
  to the most recent incoming message. This is the regression guard for the
  failure mode captured at `data/eval_blinded_ab.json:299-314`.

- [ ] **AC-8 (Channel-aware delay):** Filler delay scaling is read from a channel
  class table:
  - `voice` class: 200-500 ms window post-VAD endpoint (Lala 2019).
  - `text_async` class (Slack, Discord, Telegram): retain the existing dynamic
    delay logic (Gnewuch 2018).
  - `text_fast` class: not applicable — AC-1 forces `triggered=false`.

  The mapping is defined once in a single header table, not scattered across
  channel implementations.

## Non-goals

- **Auto-learning fillers from outbound user messages.** Mining the user's own
  outgoing texts for filler patterns to grow the bank is M3 (private learning)
  territory. Deferred to a follow-up spec. *Manual* CLI add (AC-6) is in scope.
- **Restructuring the trigger heuristic.** We're changing the *output* of
  `hu_conversation_classify_thinking` per channel and per persona; we are not
  redesigning the EMOTIONAL/DECISION/COMPLEX classification. That `classify_think_type`
  function is **removed** by AC-4 — selection from the persona bank is uniform
  across what used to be the three buckets, because the persona itself is the
  source of variation, not a global decision tree.
- **A persona-recording onboarding wizard.** Prompting users for fillers during
  `human onboard` is desirable but out of scope here; empty bank → no filler is
  the contract (AC-2), and that pressure may motivate a follow-up onboarding spec.
- **Backfilling fillers into existing personas.** Existing personas without
  filler banks load fine (AC-5) and simply emit no fillers (AC-2).
- **New eval infrastructure.** AC-7 reuses the existing JSON scenario format.

## Constraints

- **Language/build:** C11, must compile with `-Wall -Wextra -Wpedantic -Werror`,
  ASan clean on the `dev` preset.
- **Memory:** No new heap leaks. The filler bank is owned by `hu_persona_overlay_t`
  and freed in its destructor. Recency state is per-chat and bounded.
- **Test discipline:** All new tests deterministic, no real network, no process
  spawn. Side effects gated by `HU_IS_TEST`.
- **Forward compatibility:** Persona configs from before this change load without
  error (empty banks). Persona configs from this change written by an older binary
  must round-trip without losing the bank (ignored unknown field is acceptable;
  reordering is not).
- **Regression budget:** The existing 9,800+ test suite remains 0-fail, 0-ASan.
  Tests that depended on the hardcoded strings are updated, not deleted.
- **Performance:** Bank lookup must be O(1) in the channel-class table + O(bank
  size) for selection; bank size is bounded ≤ 32 entries per channel per the
  schema. Recency window is bounded ≤ 4 entries per active chat.
- **Security:** Filler strings written to disk via persona save must be subject
  to the same input validation as other persona fields (length bound, UTF-8 sanity).

## Out-of-scope risk callouts (worth a sentence each)

- **Voice channel timing (AC-8 voice branch)** depends on a VAD endpoint signal
  that may not yet be plumbed end-to-end. If no `voice` channel is registered at
  build time, that branch compiles in but is dead; the test for AC-8 voice is
  conditional on a `HU_ENABLE_VOICE` macro.
- **CLI subcommand surface (AC-6)** must coexist with existing `human persona`
  subcommands; collisions and help-text updates required.

