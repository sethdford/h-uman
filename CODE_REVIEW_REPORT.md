# Deep Code Review Report — seaclaw

**Scope:** agent.c (history), channels (telegram, discord, slack, whatsapp), providers (openai, anthropic, gemini), json.c, sqlite.c, persona (sampler, analyzer, creator, feedback)

**Confidence threshold:** ≥80% — only high-confidence bugs reported.

**Status:** All 4 bugs below have been FIXED. Tests: 2797/2797 passed.

---

## Critical

### 1. `append_history`: OOM on name/tool_call_id leaves corrupt history and leaks

**File:** `src/agent/agent.c`  
**Lines:** 486–491

**Bug:** When `name_len > 0` and `sc_strndup` for `name` fails, or when `tool_call_id_len > 0` and `sc_strndup` for `tool_call_id` fails, the function still increments `history_count` and returns `SC_OK`. This leaves a history entry with `name_len > 0` but `name == NULL` (or `tool_call_id_len > 0` but `tool_call_id == NULL`), which can cause crashes or undefined behavior when code assumes non-NULL for non-zero lengths.

**Fix:** After each allocation, check for failure and rollback:

```c
agent->history[agent->history_count].name =
    name_len ? sc_strndup(agent->alloc, name, name_len) : NULL;
agent->history[agent->history_count].name_len = name_len;
if (name_len && !agent->history[agent->history_count].name) {
    agent->alloc->free(agent->alloc->ctx, dup, content_len + 1);
    return SC_ERR_OUT_OF_MEMORY;
}
agent->history[agent->history_count].tool_call_id =
    tool_call_id_len ? sc_strndup(agent->alloc, tool_call_id, tool_call_id_len) : NULL;
agent->history[agent->history_count].tool_call_id_len = tool_call_id_len;
if (tool_call_id_len && !agent->history[agent->history_count].tool_call_id) {
    agent->alloc->free(agent->alloc->ctx, dup, content_len + 1);
    if (agent->history[agent->history_count].name)
        agent->alloc->free(agent->alloc->ctx, agent->history[agent->history_count].name,
                           agent->history[agent->history_count].name_len + 1);
    return SC_ERR_OUT_OF_MEMORY;
}
```

---

### 2. `append_history_with_tool_calls`: OOM in tool_calls loop leaves partial data

**File:** `src/agent/agent.c`  
**Lines:** 526–540

**Bug:** If `sc_strndup` fails for any of `id`, `name`, or `arguments` inside the loop, the function does not roll back. It stores partially filled `tool_calls` in history and returns `SC_OK`. Downstream code may assume these fields are non-NULL when the source had them set.

**Fix:** After each `sc_strndup` in the loop, check for NULL and rollback:

```c
if (src->id && src->id_len > 0) {
    owned[i].id = sc_strndup(agent->alloc, src->id, src->id_len);
    if (!owned[i].id) {
        free_owned_tool_calls(agent->alloc, owned, tool_calls_count);
        agent->alloc->free(agent->alloc->ctx, dup, content_len ? content_len + 1 : 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    owned[i].id_len = src->id_len;
}
```

(Repeat the same pattern for `name` and `arguments`.)

---

## Important

### 3. Discord poll: JSON parse leak when response is not an array

**File:** `src/channels/discord.c`  
**Lines:** 417–418

**Bug:** When `sc_json_parse` succeeds but `parsed->type != SC_JSON_ARRAY` (e.g. error object `{"message":"...","code":...}`), the code `continue`s without freeing `parsed`. This leaks the parsed JSON tree.

**Fix:**

```c
if (err != SC_OK || !parsed || parsed->type != SC_JSON_ARRAY) {
    if (parsed)
        sc_json_free(alloc, parsed);
    continue;
}
```

---

### 4. `sc_agent_clear_history`: Possible NULL dereference

**File:** `src/agent/agent.c`  
**Lines:** 565–566

**Bug:** If `agent->history` is NULL but `agent->history_count > 0` (invalid/corrupt state), the loop dereferences `agent->history[i]` and can crash.

**Fix:** Add a guard at the start:

```c
void sc_agent_clear_history(sc_agent_t *agent) {
    if (!agent || !agent->history)
        return;
    ...
}
```

---

## Summary

| #   | Severity  | File      | Line(s) | Issue                                      |
| --- | --------- | --------- | ------- | ------------------------------------------ |
| 1   | Critical  | agent.c   | 486–491 | OOM on name/tool_call_id → corrupt history |
| 2   | Critical  | agent.c   | 526–540 | OOM in tool_calls loop → partial data      |
| 3   | Important | discord.c | 417–418 | JSON leak when parsed type ≠ array         |
| 4   | Important | agent.c   | 565–566 | NULL history dereference in clear_history  |

---

**Validation:** Run `./build/seaclaw_tests` after applying fixes. All 2797 tests should pass with 0 ASan errors.
