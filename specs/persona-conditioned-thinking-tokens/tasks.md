# Persona-Conditioned Thinking Tokens (PCTT) — Tasks

> Tasks are sized for single agent-runs (≤~300 LOC each). Every task lists the
> ACs it serves; every AC is covered by ≥1 task. Wave grouping at the bottom
> shows what can dispatch in parallel.

## Tasks

| # | Task | ACs | Owner | Status |
|---|---|---|---|---|
| 1 | Add `include/human/channel_class.h` with `hu_channel_class_t` enum and `hu_channel_class_for_name(const char *name)` declaration. Add `src/channels/channel_class.c` with a static-const name→class table (imessage→TEXT_FAST, sms→TEXT_FAST, slack/discord/telegram/teams/whatsapp/signal/matrix/line→TEXT_ASYNC, voice→VOICE, others→UNKNOWN) and case-insensitive lookup. Add `tests/test_channel_class.c` covering: known names → expected class, case insensitivity, unknown name → UNKNOWN, NULL → UNKNOWN. Wire into CMakeLists. | AC-1, AC-8 | general-purpose | pending |
| 2 | Extend `hu_persona_overlay_t` at `include/human/persona.h:18-37` with `char **filler_bank; size_t filler_bank_count; size_t filler_bank_cap;`. Update overlay destructor in `src/persona/persona.c` to free entries. Update `write_json_string_array`-style emission in `src/persona/creator.c:512` to write a `"filler_bank":[...]` array when count>0. Update `parse_string_array`-style consumption in `src/persona/persona.c:809-855`'s overlay parser to read `filler_bank` (absent → empty). Add `tests/test_persona_filler_roundtrip.c`: (a) round-trip a persona with 5 fillers; (b) load a fixture without `filler_bank` and confirm empty bank + no error. | AC-5 | general-purpose | pending |
| 3 | Add `hu_filler_recency_t` (chat_id[128] + last_index uint16 + last_used_seq uint64) as a 32-entry LRU on `hu_agent_t`. Init at agent creation; free at agent destruction. Add `hu_filler_recency_record(agent, chat_id, len, index)` and `hu_filler_recency_last(agent, chat_id, len)` (returns -1 if absent). Add `tests/test_filler_recency.c`: insert/lookup, LRU eviction at 33rd chat, NULL safety. | AC-3 | general-purpose | pending |
| 4 | Replace the body of `hu_conversation_classify_thinking` at `src/context/conversation.c:4158-4237` with the new context-struct API. Define `hu_thinking_context_t` in `include/human/context/conversation.h`. New signature: `bool hu_conversation_classify_thinking(const hu_thinking_context_t *ctx, const char *msg, size_t msg_len, const hu_channel_history_entry_t *entries, size_t entry_count, hu_thinking_response_t *out);`. Implementation: (a) channel class TEXT_FAST → false; (b) persona NULL or overlay missing or bank empty → false; (c) existing length/word trigger; (d) select via `hu_conversation_select_filler` excluding `recency_last`; (e) record into recency; (f) channel-aware delay (text_async retains existing formula; voice 200-500ms gated on `HU_ENABLE_VOICE_VAD_TIMING`). DELETE `classify_think_type` enum, function, and the hardcoded `fillers[…]` arrays. | AC-1, AC-2, AC-3, AC-4, AC-8 | general-purpose | pending |
| 5 | Update `src/daemon.c:9008-9024` call site to build a `hu_thinking_context_t` from `agent->persona`, `ch->channel->vtable->name(ch->channel->ctx)`, `send_target`, `send_target_len`, and `(uint32_t)time(NULL)`. Pass to new classify API. Behavior unchanged for callers when bank is empty (silent no-filler path). | AC-1, AC-2, AC-3 | general-purpose | pending |
| 6 | Update the 5 existing test call sites in `tests/test_conversation.c:319, 328, 335, 346, 347` to build a `hu_thinking_context_t` with `persona=NULL` (which now implies `triggered=false`). For tests that previously asserted a specific hardcoded filler was emitted, replace with: build a synthetic persona+overlay with a 3-entry filler bank; assert one of the three is chosen; assert no consecutive duplicates over 10 emits. | AC-3, AC-4 | general-purpose | pending |
| 7 | Add `HU_PERSONA_ACTION_FILLER_ADD`, `_LIST`, `_REMOVE` to the action enum in `include/human/persona.h`. Extend `hu_persona_cli_parse` at `src/persona/cli.c:64` to recognize `filler add|list|remove` with `--channel <name>` and `"<text>"` (add) or `--index <n>` (remove). Extend `hu_persona_cli_run` at `src/persona/cli.c:226` with handlers that load persona, mutate overlay's filler_bank, save atomically (reuse existing `hu_persona_save` path). Update usage string in `src/main.c:cmd_persona()` (~lines 1905-1920). Add tests in `tests/test_persona_cli.c` mirroring existing pattern. | AC-6 | general-purpose | pending |
| 8 | Create `data/eval_pctt.json` with ≥20 `{incoming, channel}` entries sourced from `data/eval_blinded_ab.json`'s real_seth corpus (include the original echo failure case at line 300). Add `tests/test_filler_pctt.c`: load JSON; for each entry, build a synthetic persona with a 5-entry filler bank that *includes* the incoming string; run the classifier; assert `emitted_filler ≠ incoming` for every entry. Wire into CMakeLists. | AC-7 | general-purpose | pending |
| 9 | Add `tools/check-no-hardcoded-fillers.sh` (executable bash) that runs `grep -rn "ooh that's a tough one\|let me think about that for a sec\|hm good question" src/` and exits non-zero on hits. Wire into the `dev` CMake preset as a custom target run during `cmake --build --preset dev`. Mirrors AC-4 as a build-time gate that cannot rot. | AC-4 | general-purpose | pending |
| 10 | Run `/verify` against the implemented work. Verifier spawns, runs the dev preset build, runs the full test suite (`./build/human_tests`), runs the new tests by suite name, runs ASan, runs `tools/check-no-hardcoded-fillers.sh`. Reports `RESULT_verifier=PASS` only if all green. | all | verifier | pending |
| 11 | Run `spec-verifier` to confirm implementation satisfies each AC with file:line evidence. | all | spec-verifier | pending |

## Dependencies

```
1 (channel_class) ──┐
                    ├─► 4 (selector) ──┬─► 5 (daemon)
2 (overlay schema) ─┤                  ├─► 6 (test_conversation update)
3 (recency on agent)┘                  └─► 8 (eval scenario)
                    │
2 ──► 7 (CLI)
1 ──► 4 (already shown)
9 (grep guard) — independent, runs anytime after 4
10 (verify) — runs after 1-9
11 (spec-verifier) — runs after 10
```

### Dispatch waves (parallel-safe groups)

- **Wave 1 (3 in parallel):** Tasks 1, 2, 3 — independent surfaces (channel
  table, persona schema, agent recency state).
- **Wave 2 (3 in parallel):** Tasks 4, 7, 9 — Task 4 needs 1+2+3; Task 7 needs
  2; Task 9 is independent. All three can run once their prerequisites land.
- **Wave 3 (3 in parallel):** Tasks 5, 6, 8 — all need Task 4 in main.
- **Wave 4 (sequential):** Task 10 (verify), then Task 11 (spec-verify).

## CRITIC checkpoints

Per the agent-team-os, the critic agent reviews each wave's closures for:

- **After Wave 1:** cross-cutting type bleed (does `filler_bank` accidentally
  leak into non-overlay code paths? does `channel_class.c` introduce a global
  init order dependency?).
- **After Wave 2:** ensure Task 4's deletion of `classify_think_type` doesn't
  break any reachable test or doc reference outside the 5 known call sites.
- **After Wave 3:** end-to-end on a real persona — does iMessage + populated
  bank actually emit nothing (AC-1 wins over AC-3 emit) vs. iMessage + empty
  bank also emit nothing (AC-2)? The interaction must be tested.
- **After Wave 4:** verify the eval scenario in Task 8 catches a synthetic
  regression: introduce a one-line bug that makes the selector echo, confirm
  test fails, revert.

## Quality gates (from CLAUDE.md global rules)

- Each task ends with `RESULT_verifier=PASS` evidence (Task 10 aggregates).
- No `--no-verify` commits.
- Memory: every overlay destructor and agent destructor freed; ASan clean.
- Hardcoded filler grep: zero hits in `src/` (Task 9 enforces).
- Existing 9,800+ test suite remains 0-fail.

## Out of scope (re-asserted)

- Auto-learning fillers from outbound messages → follow-up spec.
- Onboarding wizard for filler recording → follow-up spec.
- Intent-conditioned filler banks → follow-up if eval data motivates.
- TwinVoice-style multi-dimensional persona-tone scoring → follow-up.

