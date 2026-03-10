---
title: Architecture and Integration Audit Report
---

# Architecture and Integration Audit Report

**Date:** 2025-03-08  
**Scope:** human codebase — dead code, integration gaps, vtable contracts, config, test coverage

---

## 1. Dead Code / Unused Functions

### 1.1 `hu_conversation_extract_urls` — **Dead in production**

| Field              | Value                                                                                                                                                                                                                                                                           |
| ------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Severity**       | Medium                                                                                                                                                                                                                                                                          |
| **Files**          | `include/human/context/conversation.h`, `src/context/conversation.c`                                                                                                                                                                                                          |
| **Type**           | Dead code                                                                                                                                                                                                                                                                       |
| **Description**    | `hu_conversation_extract_urls()` is declared in the header and implemented in conversation.c but **never called from any production path**. Only used in `tests/test_conversation.c`. `hu_conversation_should_share_link()` does not use it — it uses pattern matching instead. |
| **Recommendation** | Either wire it into a production path (e.g., URL extraction for link-sharing prompts) or remove/deprecate it.                                                                                                                                                                   |

### 1.2 Other newer headers — **All used**

- **ab_response.h**: `hu_ab_evaluate`, `hu_ab_result_deinit` — called from `src/agent/agent_turn.c` and tests.
- **emotional_graph.h**: All functions used — `hu_egraph_*` called from `src/daemon.c` and tests.
- **replay.h**: All functions used — `hu_replay_*` called from `src/daemon.c` and tests.
- **conversation.h**: All other public functions are used in daemon, agent_turn, or conversation.c (internal).

---

## 2. Integration Gaps — "Better Than Human" Features

### 2.1 Fully integrated ✅

| Feature                                 | Location                                                              | Status                                    |
| --------------------------------------- | --------------------------------------------------------------------- | ----------------------------------------- |
| `hu_conversation_build_callback()`      | daemon.c:1420                                                         | Wired into convo_ctx                      |
| `hu_conversation_classify_thinking()`   | daemon.c:1742                                                         | Sends filler, then delay, then LLM        |
| `hu_conversation_classify_reaction()`   | daemon.c:1756                                                         | Calls `ch->vtable->react()` when not NONE |
| `hu_ab_evaluate()`                      | agent_turn.c:912                                                      | A/B candidate selection                   |
| `hu_egraph_*`                           | daemon.c:1658–1661                                                    | Emotional graph context in convo_ctx      |
| `hu_replay_analyze()`                   | daemon.c:1845                                                         | Post-turn replay insights                 |
| `hu_conversation_generate_correction()` | daemon.c:2028                                                         | Typo correction fragment (\*meant)        |
| `hu_conversation_apply_typing_quirks()` | daemon.c:1977                                                         | Post-processing quirks                    |
| `hu_conversation_apply_typos()`         | daemon.c:2021                                                         | Typo simulation                           |
| `hu_conversation_calibrate_length()`    | daemon.c:1338 (fallback), conversation.c:733 (inside build_awareness) | Integrated                                |

### 2.2 Partial / potential gaps

| Feature                          | Severity | Description                                                                                                                                                                                                             |
| -------------------------------- | -------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `hu_conversation_extract_urls()` | Medium   | Never called from daemon or agent. Could support link-sharing or URL validation.                                                                                                                                        |
| Reaction + text both sent        | Low      | When `hu_conversation_classify_reaction()` returns non-NONE, daemon sends **both** reaction and full text. Design doc said "instead of (or in addition to)" — current behavior is "in addition to." May be intentional. |
| `message_id` for reactions       | Low      | iMessage must populate `message_id` for `react()` to work. Verified: `hu_channel_message_t` has `message_id`; daemon checks `msg_id > 0` before calling `react()`.                                                      |

---

## 3. Vtable Contract Compliance

### 3.1 Tool vtable (`hu_tool_t`)

**Required:** `execute`, `name`, `description`, `parameters_json`. Optional: `deinit`.

| Check                                                      | Result                                             |
| ---------------------------------------------------------- | -------------------------------------------------- |
| All tools have execute, name, description, parameters_json | ✅ Sampled 20+ tools; all provide these.           |
| `deinit` may be NULL                                       | ✅ e.g. `skill_write`, `diff` use `deinit = NULL`. |

**Note:** `include/human/tool.h` defines the vtable; no violations found.

### 3.2 Channel vtable (`hu_channel_t`)

**Required:** `start`, `stop`, `send`, `name`, `health_check`. Optional: `send_event`, `start_typing`, `stop_typing`, `load_conversation_history`, `get_response_constraints`, `react`.

| Check                                                   | Result                                                                                                                                       |
| ------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| All channels have start, stop, send, name, health_check | ✅ Sampled 15+ channels; all provide these.                                                                                                  |
| `listen` / `is_configured`                              | Not in vtable. `is_configured` is a **module-level** function (e.g. `hu_imessage_is_configured`) used by `hu_channel_catalog_is_configured`. |

**Note:** CONTRIBUTING.md/AGENTS.md mention "listen" — channels use polling/webhooks for inbound messages; there is no vtable `listen` method.

### 3.3 Provider vtable (`hu_provider_t`)

**Required:** `chat_with_system`, `chat`, `supports_native_tools`, `get_name`, `deinit`.

| Check                              | Result                                                       |
| ---------------------------------- | ------------------------------------------------------------ |
| All providers have required fields | ✅ Sampled openrouter, reliable, gemini, ollama; all comply. |

---

## 4. Config Integration

### 4.1 New features — **Not configurable**

| Feature         | Config field? | Parsed? | Used?                                                                              |
| --------------- | ------------- | ------- | ---------------------------------------------------------------------------------- |
| emotional_graph | ❌            | ❌      | Always on when STM has data                                                        |
| replay          | ❌            | ❌      | Always on (HU_HAS_PERSONA)                                                         |
| ab_response     | ❌            | ❌      | Always on when ab_history_entries set                                              |
| typing_quirks   | ❌            | ❌      | Hardcoded in daemon                                                                |
| typos           | ❌            | ❌      | Hardcoded in daemon                                                                |
| proactive       | ✅ (persona)  | ✅      | `proactive_checkin`, `proactive_channel`, `proactive_schedule` in persona contacts |

### 4.2 Config fields — **No orphaned fields found**

Searched `config_parse.c`, `config_merge.c`; no config keys for emotional_graph, replay, or ab_response. These features are effectively always-on when their preconditions are met.

### 4.3 Recommendation

| Severity | Issue                                                 | Recommendation                                                                                                                               |
| -------- | ----------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| Low      | emotional_graph, replay, ab_response not configurable | Consider adding `agent.emotional_graph_enabled`, `agent.replay_enabled`, `agent.ab_candidates` (0 = disabled) if users need to disable them. |
| Low      | typing_quirks / typos not configurable                | Consider persona-level or channel-level config for quirks and typo probability.                                                              |

---

## 5. Test Coverage Gaps

### 5.1 Newer headers — coverage summary

| Header            | Public functions                                                          | Tested?   | Notes                  |
| ----------------- | ------------------------------------------------------------------------- | --------- | ---------------------- |
| ab_response.h     | `hu_ab_evaluate`, `hu_ab_result_deinit`                                   | ✅        | test_ab_response.c     |
| emotional_graph.h | All 6 functions                                                           | ✅        | test_emotional_graph.c |
| replay.h          | `hu_replay_analyze`, `hu_replay_result_deinit`, `hu_replay_build_context` | ✅        | test_replay.c          |
| conversation.h    | 25+ functions                                                             | Mostly ✅ | test_conversation.c    |

### 5.2 Untested or lightly tested public functions

| Function                                 | Severity | Notes                                         |
| ---------------------------------------- | -------- | --------------------------------------------- |
| `hu_conversation_extract_urls`           | Low      | Has tests but function is dead in production. |
| `hu_conversation_calibrate_relationship` | Low      | Tested in test_conversation.c.                |
| `hu_conversation_classify_group`         | Low      | Tested in test_conversation.c.                |

All public functions in the audited headers have at least one test. No critical untested functions.

---

## 6. Summary Table

| Issue                                                     | Severity | Type          | Files                          |
| --------------------------------------------------------- | -------- | ------------- | ------------------------------ |
| `hu_conversation_extract_urls` never called in production | Medium   | Dead code     | conversation.c, conversation.h |
| emotional_graph, replay, ab_response not configurable     | Low      | Config gap    | config.c, config_merge.c       |
| typing_quirks / typos not configurable                    | Low      | Config gap    | daemon.c                       |
| Channel vtable: no `listen` (docs mention it)             | Low      | Documentation | AGENTS.md, CONTRIBUTING.md     |

---

## 7. Recommendations (prioritized)

1. **Medium:** Wire `hu_conversation_extract_urls()` into a production path (e.g., link-sharing prompt context) or remove/deprecate it.
2. **Low:** Add config toggles for emotional_graph, replay, ab_response if product requires them.
3. **Low:** Align AGENTS.md/CONTRIBUTING.md with actual channel vtable (start, stop, send, name, health_check — no `listen` in vtable).
