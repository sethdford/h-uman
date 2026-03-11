# Phase 6 Privacy Firewall Audit

**Date:** 2026-03-11  
**Scope:** Human Fidelity Phase 6 — contact isolation, memory scoping, proactive actions

## Summary

Contact A's information must **never** appear in conversation context built for contact B. This document records the audit of all injection points and confirms `contact_id` / `session_id` scoping.

---

## 1. Memory Retrieval in Daemon

### 1.1 `build_callback_context` (src/daemon.c:96–150)

- **Purpose:** Queries memory for past topics relevant to the current message.
- **Scoping:** `memory->vtable->recall(..., session_id, session_id_len, ...)` — session_id is `batch_key` (contact identifier).
- **Call sites:** Lines 680 (proactive), 3421 (inbound batch).
- **Status:** ✅ **Scoped.** Both call sites pass `batch_key` / `cp->contact_id` as session_id.

### 1.2 Agent Memory Session (src/daemon.c:4440–4446)

- **Purpose:** Set agent's memory scope before LLM turn.
- **Scoping:** `agent->memory_session_id = batch_key`, `agent->memory->current_session_id = batch_key`.
- **Status:** ✅ **Scoped.** Memory backend and agent are bound to the current contact's session_key.

### 1.3 Memory Loader (src/agent/memory_loader.c)

- **Path A — retrieval_engine:** `retrieve()` does **not** receive `session_id`. `hu_keyword_retrieve` uses `backend->vtable->list(..., NULL, 0, ...)` — returns all memories.
- **Path B — memory->recall:** Receives `session_id` and passes it to `recall()`.
- **Status:** ⚠️ **Gap when retrieval_engine is used.** Keyword/hybrid/semantic retrieval does not filter by session_id. When no retrieval_engine is configured, `recall` path is used and is correctly scoped.

### 1.4 SQLite Memory Recall (src/memory/engines/sqlite.c:408–548)

- **Scoping:** Post-filter: entries with `session_id` not matching the requested `session_id` are dropped (lines 483–486, 536–539).
- **Status:** ✅ **Scoped.** Core recall path correctly filters by session_id.

---

## 2. Proactive Actions

### 2.1 Proactive Check-in (src/daemon.c:671–680, 823–1350)

- **Context build:** `build_callback_context(alloc, memory, cp->contact_id, strlen(cp->contact_id), ...)`.
- **Superhuman calls:** All use `cp->contact_id` — e.g. `hu_superhuman_topic_absence_list(..., cp->contact_id, ...)`, `hu_superhuman_inside_joke_list(..., cp->contact_id, ...)`.
- **Status:** ✅ **Scoped.** Proactive messages to contact X use only X's context.

---

## 3. Phase 6 Module Scoping

| Module                    | API                                                             | contact_id / session_id | Status |
| ------------------------- | --------------------------------------------------------------- | ----------------------- | ------ |
| `hu_theory_of_mind_*`     | `update_baseline`, `get_baseline`                               | `contact_id`            | ✅     |
| `hu_anticipatory_predict` | Predict emotional states                                        | `contact_id`            | ✅     |
| `hu_self_awareness_*`     | `record_send`, `build_directive_from_memory`, `get_reciprocity` | `contact_id`            | ✅     |
| `hu_social_graph_*`       | `store`, `get`, `build_directive`                               | `contact_id` in SQL     | ✅     |
| `hu_protective_*`         | `memory_ok`, `is_boundary`, `add_boundary`                      | `contact_id`            | ✅     |

---

## 4. Test Coverage

### 4.1 Cross-Contact Isolation Test

**File:** `tests/test_privacy_audit.c`  
**Test:** `cross_contact_isolation_a_memory_not_in_b_context`

1. Store memory for contact A: `"A's secret info"` with `session_id = "contact_a"`.
2. Recall for contact B: `session_id = "contact_b"`, query `"secret"`.
3. **Assert:** A's memory does NOT appear in B's context.
4. **Assert:** Recall for contact A returns A's memory (sanity check).

**Requires:** `HU_ENABLE_SQLITE` (SQLite memory implements session_id filtering).

---

## 5. Recommendations

1. **Retrieval engine session scoping:** Extend `hu_retrieval_options_t` with `session_id` / `session_id_len` and propagate through `hu_keyword_retrieve`, `hu_hybrid_retrieve`, and `hu_semantic_retrieve` (or equivalent vector store filters) so that when retrieval_engine is used, results are scoped to the current contact.
2. **Regression:** Run `run_privacy_audit_tests` in CI to catch cross-contact leakage.

---

## 6. Injection Points Reviewed

| Location                                                                      | Scoping mechanism                          | Verified             |
| ----------------------------------------------------------------------------- | ------------------------------------------ | -------------------- |
| daemon.c `build_callback_context`                                             | session_id param to recall                 | ✅                   |
| daemon.c agent memory_session_id                                              | batch_key before turn                      | ✅                   |
| agent_turn.c memory_loader                                                    | memory_session_id when no retrieval_engine | ✅                   |
| agent_turn.c memory_loader                                                    | retrieval_engine path                      | ⚠️ No session filter |
| daemon.c proactive flows                                                      | cp->contact_id                             | ✅                   |
| hu*superhuman*\* (inside_joke, micro_moment, etc.)                            | contact_id param                           | ✅                   |
| hu_commitment_store_build_context                                             | session_id param                           | ✅                   |
| hu_mood_build_context                                                         | contact_id param                           | ✅                   |
| Phase 6 modules (ToM, anticipatory, self_awareness, social_graph, protective) | contact_id param                           | ✅                   |
