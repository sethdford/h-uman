---
title: Phase 5–7 Implementation Audit Report
---

# Phase 5–7 Implementation Audit Report

**Date:** 2026-03-08  
**Scope:** Tapback reactions (Phase 5), Media/link sharing (Phase 6), A/B response (Phase 7a), Replay learning (Phase 7b), Emotional graph (Phase 7c)

---

## Phase 5: Tapback Reactions

### `include/human/channel.h`

- **ABI compatibility:** `hu_reaction_type_t` enum and `react` vtable entry are appended at the end of the vtable. C designated initializers zero-initialize unspecified fields, so existing channel implementations (telegram, discord, slack, etc.) that omit `.react` will have `react = NULL`. **No ABI break.**
- **Status:** OK

### `src/channels/imessage.c`

- **react implementation:** Correctly guarded: `#if HU_IS_TEST` path records to `last_reaction`/`last_reaction_message_id`; non-test path is a stub that logs when `HU_DEBUG` is set.
- **Test mock:** `hu_imessage_test_get_last_reaction()` exists and is used in `test_imessage_extended.c`. **OK.**

### `src/context/conversation.c` — `hu_conversation_classify_reaction()`

- **Pattern matching:** Uses `str_contains_ci` for content checks. Logic is sound.
- **PRNG:** Uses LCG `s = s * 1103515245 + 12345` (glibc-style). Deterministic given seed. **OK.**
- **from_me guard:** Correctly returns `HU_REACTION_NONE` when `from_me` is true (line 2851).
- **entries/entry_count:** Unused (`(void)entries; (void)entry_count`). Design allows future use; no bug.

### **CRITICAL — NOT INTEGRATED: Reaction flow never invoked**

- `hu_conversation_classify_reaction()` is **only called from tests** (`test_conversation.c`).
- `channel->vtable->react` is **never called** from daemon or agent.
- `hu_channel_loop_msg_t` has no `message_id` field — the poll pipeline does not carry platform message IDs. iMessage poll reads ROWID from chat.db but only uses it for `last_rowid` tracking; it is not passed to the dispatch path.
- **Impact:** The entire tapback feature is unimplemented in production. The vtable, classifier, and iMessage stub exist but are never wired.

---

## Phase 6: Media and Link Sharing

### `src/context/conversation.c` — `hu_conversation_extract_urls()`

- **Protocol filtering:** Only extracts `https://` and `http://`. `javascript:`, `data:`, `vbscript:`, etc. are **not** extracted. **SECURITY OK.**
- **Edge cases:**
  - Malformed URLs: Parsing stops at whitespace or delimiters (` `, `\t`, `\n`, `\r`, `>`, `)`, `]`, `"`, `'`). No explicit length cap — very long URLs (e.g. 100KB) would be extracted. Caller receives pointers into original text; downstream use may need length limits.
- **Risk:** No explicit max URL length. Consider adding `HU_URL_MAX_LEN` (e.g. 2048) for safety.

### `hu_conversation_should_share_link()`

- Logic is sound. Uses `str_contains_ci` for pattern matching. **OK.**

### `hu_conversation_attachment_context()`

- **NOT INTEGRATED:** Only called from `tests/test_conversation.c`. **Never called from daemon or agent_turn.**
- The daemon builds `convo_ctx` from: awareness, style, honesty, anti-repetition, relationship calibration, emotional graph. **Attachment context is never merged.**
- `imessage.c` comment (line 375) says "prompt builder should call hu_conversation_attachment_context()" — but it does not.
- **Impact:** When history contains `[image or attachment]`, `[Photo shared]`, etc., the agent does not receive guidance to acknowledge naturally.

---

## Phase 7a: A/B Response Generation

### `include/human/agent/ab_response.h`

- Struct design is clear. `HU_AB_MAX_CANDIDATES = 3`. **OK.**

### `src/agent/ab_response.c`

- **Evaluation:** Uses `hu_conversation_evaluate_quality()` for each candidate. **OK.**
- **best_idx:** Correctly selects highest `quality_score`. If all candidates have `!response`, loop skips them; `best_idx` remains 0. If `candidates[0].response` is NULL, caller must handle — agent_turn checks `ab_result.candidates[bi].response` before use. **OK.**
- **hu_ab_result_deinit:** Frees all candidate responses. Correctly avoids double-free when caller nulls the selected candidate. **OK.**

### `src/agent/agent_turn.c` — A/B integration

**BUG 1 — Wrong `response_len_out` when A/B selects different candidate**

```c
// Line 909-910
if (response_len_out)
    *response_len_out = resp.content_len;  // WRONG: should be final_len
```

When A/B selects a different candidate, `final_content`/`final_len` are used for the response, but `response_len_out` is set to `resp.content_len`. Caller receives incorrect length.

**BUG 2 — TTS plays wrong content when A/B selects different candidate**

```c
// Line 911
hu_agent_internal_maybe_tts(agent, resp.content, resp.content_len);  // WRONG: should be final_content, final_len
```

When A/B selects a different candidate, TTS plays the original response instead of the selected one.

**Integration:** A/B is wired. `ab_history_entries`/`ab_history_count` are set by daemon before `hu_agent_turn`. Non-selected candidates are freed via `hu_ab_result_deinit`. **OK.**

**Reflection interaction:** A/B runs after reflection. If reflection retries, the retry path does not re-run A/B — acceptable, as the critique-driven retry is a different flow.

---

## Phase 7b: Replay Learning

### `src/persona/replay.c` — `hu_replay_analyze()`

- **Pattern detection:** Uses `str_contains_ci`, `str_starts_with_ci`, `is_positive_signal`, `is_negative_signal`, `is_robotic_tell`. Logic is reasonable.
- **Timestamp parsing:** `parse_timestamp()` supports `"%Y-%m-%d %H:%M"` and `"%H:%M"`. `strptime` with `*p == '\0'` requires exact match; `"12:30:45"` would fail. Minor edge case.
- **Memory:** `hu_replay_result_deinit` frees insights and summary. Free sizes use `observation_len + 1`, `recommendation_len + 1` — correct for `hu_strndup` allocations.
- **hu_conversation_evaluate_quality:** Called with `entries, i + 1` — passes preceding entries as context. **OK.**

### **NOT INTEGRATED**

- `hu_replay_analyze()` is **only called from `tests/test_replay.c`**.
- Not called from daemon, agent_turn, or persona CLI.
- **Impact:** Replay learning is a library function with no production integration.

---

## Phase 7c: Emotional Memory Graph

### `src/memory/emotional_graph.c`

- **hu_egraph_record:** Bounds checks `HU_EGRAPH_MAX_NODES`, `HU_EGRAPH_MAX_EDGES`. Running average: `(old_avg * count + intensity) / (count + 1)`. **OK.**
- **hu_egraph_query:** `topic_cmp_ci` for case-insensitive comparison. **OK.**
- **hu_egraph_populate_from_stm:** Iterates `hu_stm_count(buf)`, uses `hu_stm_get(buf, i)`. Checks `t->emotion_count`, `t->entity_count` before indexing. Entity type check `topic_cmp_ci(ent->type, ent->type_len, "topic", 5)` — **potential bug:** if `ent->type_len < 5`, `topic_cmp_ci` compares min(ent->type_len, 5) = ent->type_len chars. `"top"` (len 3) would not match `"topic"` (len 5). Correct behavior.
- **hu_egraph_build_context:** `tag_idx = (size_t)dom`; checks `tag_idx < HU_EGRAPH_EMOTION_COUNT` before indexing `EMOTION_NAMES`. Negative `dom` becomes large `(size_t)`, so condition fails and fallback to `"neutral"` is used. **OK.**
- **hu_egraph_init:** Takes `hu_allocator_t alloc` by value. Daemon passes `*alloc`. **OK.**
- **hu_strndup(&g->alloc, ...):** `g->alloc` is stored by value; `&g->alloc` is a valid `hu_allocator_t*`. **OK.**

### `src/daemon.c` — Emotional graph integration

- `hu_egraph_init`, `hu_egraph_populate_from_stm`, `hu_egraph_build_context`, `hu_egraph_deinit` are called correctly.
- `egraph_ctx` is merged into `convo_ctx` or used alone. Memory is freed. **OK.**

### `src/agent/agent_turn.c` — `hu_stm_turn_set_primary_topic()`

- Called when `fc_result.primary_topic` is set (lines 100–103). **OK.**

---

## Summary Table

| Category      | Phase 5 | Phase 6 | Phase 7a | Phase 7b | Phase 7c |
| ------------- | ------- | ------- | -------- | -------- | -------- |
| **Bugs**      | 0       | 0       | 2        | 0        | 0        |
| **Security**  | 0       | 0       | 0        | 0        | 0        |
| **Not wired** | 1       | 1       | 0        | 1        | 0        |
| **Risks**     | 0       | 1       | 0        | 0        | 0        |

---

## Recommended Fixes

### Critical (Bugs)

1. **agent_turn.c:910** — Set `*response_len_out = final_len` when A/B path is used.
2. **agent_turn.c:911** — Call `hu_agent_internal_maybe_tts(agent, final_content, final_len)` instead of `resp.content`/`resp.content_len`.

### Integration (Not wired)

3. **Reactions:** Extend `hu_channel_loop_msg_t` with `int64_t message_id`; have iMessage poll populate it; in daemon, after receiving a message, call `hu_conversation_classify_reaction()` and, if not `HU_REACTION_NONE` and `ch->vtable->react`, call `react()` before or instead of full reply.
4. **Attachment context:** In daemon, when building `convo_ctx`, call `hu_conversation_attachment_context(alloc, history_entries, history_count, &att_len)` and merge `att_ctx` into `convo_ctx` when non-NULL.
5. **Replay:** Wire `hu_replay_analyze()` into persona CLI or a post-session hook (e.g. after `hu_agent_turn` when history is available).

### Risk mitigation

6. **URL length:** Add a max URL length (e.g. 2048) in `hu_conversation_extract_urls()` to avoid unbounded extraction.
