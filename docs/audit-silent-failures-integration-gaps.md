# Audit: Silent Failures, Integration Gaps, and Unvalidated Paths

**Date:** 2025-03-08  
**Scope:** C11 core, gateway, persona, memory retrieval, daemon, channels

---

## SILENT_FAILURES

### 1. Gateway `send_all()` ignores send failures

**Location:** `src/gateway/gateway.c:364-372`

```c
static void send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0)
            break;
        sent += (size_t)n;
    }
}
```

**Issue:** `send()` failures (n <= 0) are silently ignored. Partial sends leave the client with truncated responses. No logging, no retry, no error propagation.

**Recommendation:** Return `bool` or `sc_error_t`; log failures; consider propagating to caller for 5xx responses.

---

### 2. Gateway `send_response()` returns early on snprintf failure without sending

**Location:** `src/gateway/gateway.c:416-419`

```c
    int n = snprintf(hdr, sizeof(hdr), ...);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(hdr))
        n = (int)(sizeof(hdr) - 1);
    send_all(fd, hdr, (size_t)n);
```

**Issue:** If `snprintf` returns -1 (encoding error), the function returns without sending any response. Client gets a broken connection with no HTTP response.

**Recommendation:** Send a minimal error response (e.g. 500) before returning when `n < 0`.

---

### 3. Gateway cleanup: `sc_ws_server_deinit(&gw->ws)` when `gw` may be NULL

**Location:** `src/gateway/gateway.c:1443`

```c
cleanup:
    ...
    sc_ws_server_deinit(&gw->ws);  // BUG: gw may be NULL
    ...
    if (gw)
        alloc->free(alloc->ctx, gw, sizeof(*gw));
```

**Issue:** When `gw` allocation fails at line 1060, `goto cleanup` runs with `gw == NULL`. `&gw->ws` is undefined behavior (dereferencing NULL).

**Recommendation:** Guard with `if (gw) sc_ws_server_deinit(&gw->ws);`

---

### 4. iMessage SQLite `sqlite3_bind_text` uses NULL instead of SQLITE_STATIC

**Location:** `src/channels/imessage.c:454` (and similar)

```c
sqlite3_bind_text(stmt, 1, contact_buf, -1, NULL);
```

**Issue:** Project rule: "Never use SQLITE_TRANSIENT — use SQLITE_STATIC (null)." Passing `NULL` as the 5th argument is correct for static buffers (SQLITE_STATIC), but the comment in the rule says "null" — verify intent. For stack buffers like `contact_buf` that outlive the statement, `SQLITE_STATIC` is correct. Using `NULL` is equivalent per SQLite docs, but the project explicitly forbids `SQLITE_TRANSIENT`. Double-check other `sqlite3_bind_text` calls for consistency.

**Recommendation:** Use `SQLITE_STATIC` explicitly for clarity and project compliance.

---

### 5. Persona `sc_strdup` without NULL checks for optional fields

**Location:** `src/persona/persona.c` (multiple)

Examples:

- Line 736: `out->biography = sc_strdup(alloc, s);` — no check
- Lines 781-787: motivation fields
- Lines 824-833: humor fields
- Lines 843-851: conflict_style
- Lines 861-873: emotional_range
- Lines 883-891: voice_rhythm
- Lines 901, 916-918: core_anchor, intellectual
- Lines 938-939: backstory_behaviors
- Line 954: sensory

**Issue:** If `sc_strdup` returns NULL (OOM), the persona struct has a NULL field where a string was expected. Later code may dereference without checks. Partial persona state can cause inconsistent behavior.

**Recommendation:** For optional fields, either check and skip on OOM, or abort load and return `SC_ERR_OUT_OF_MEMORY` to keep persona consistent.

---

### 6. Persona `parse_overlay`: `sc_strdup` for formality/avg_length/emoji_usage

**Location:** `src/persona/persona.c:612-619`

```c
    if (s)
        ov->formality = sc_strdup(a, s);
    s = sc_json_get_string(obj, "avg_length");
    if (s)
        ov->avg_length = sc_strdup(a, s);
    ...
```

**Issue:** No NULL check after `sc_strdup`. OOM leaves overlay partially populated.

**Recommendation:** Check each `sc_strdup` result; on failure, return `SC_ERR_OUT_OF_MEMORY` and let caller clean up.

---

### 7. Persona feedback: silent skip when realloc or sc_strdup fails

**Location:** `src/persona/feedback.c:174-212`

```c
        if (!new_banks)
            continue;
        ...
        if (!persona.example_banks[bc].channel)
            continue;
        ...
        if (!new_examples)
            continue;
        bank->examples[n].context = sc_strdup(alloc, context);
        ...
        if (bank->examples[n].context && ...) {
            bank->examples_count++;
        } else {
            // frees but doesn't increment — correct, but user gets no feedback
        }
```

**Issue:** On OOM, the example is silently dropped. User feedback is lost with no error or log.

**Recommendation:** Log or return an error so the caller knows feedback application partially failed.

---

### 8. Daemon: `load_conversation_history` return value ignored

**Location:** `src/daemon.c:625-628`, `1107-1110`, `1397-1400`, `1419-1421`

```c
            channels[c].channel->vtable->load_conversation_history(
                channels[c].channel->ctx, alloc, cp->contact_id, strlen(cp->contact_id), 15,
                &entries, &entry_count);
```

**Issue:** Return value is not checked. On failure, `entries`/`entry_count` may be uninitialized or stale. Proactive check-in and context building can use bad data.

**Recommendation:** Check return value; on error, set `entries = NULL`, `entry_count = 0` and skip or handle gracefully.

---

### 9. Daemon: `sc_proactive_build_starter` return value explicitly discarded

**Location:** `src/daemon.c:501-503`

```c
        (void)sc_proactive_build_starter(alloc, memory, cp->contact_id, strlen(cp->contact_id),
                                         &starter, &starter_len);
```

**Issue:** Failure is ignored. Proactive prompt may be built without memory-informed starter.

**Recommendation:** Check return value; if `!= SC_OK`, proceed without starter or log.

---

### 10. Daemon: `memory->vtable->store` return value ignored

**Location:** `src/daemon.c:171-174`

```c
            (void)memory->vtable->store(memory->ctx, key_buf, (size_t)kn, f->object,
                                        strlen(f->object), &cat,
                                        session_id ? session_id : "",
                                        session_id ? session_id_len : 0);
```

**Issue:** Store failures are ignored. Facts may not be persisted without any indication.

**Recommendation:** Check return value; log or increment error metric on failure.

---

### 11. Daemon: `sc_graph_upsert_entity` / `sc_graph_upsert_relation` return values ignored

**Location:** `src/daemon.c:184-189`, `207-212`

```c
                if (sc_graph_upsert_entity(...) == SC_OK &&
                    sc_graph_upsert_entity(...) == SC_OK) {
                    (void)sc_graph_upsert_relation(...);
                }
```

**Issue:** `sc_graph_upsert_relation` result is cast to void. Graph updates can fail silently.

**Recommendation:** Check `sc_graph_upsert_relation`; log or handle failures.

---

### 12. Hybrid retrieval: graph context allocation failure silently skips graph

**Location:** `src/memory/retrieval/hybrid.c:104-124`

When `graph_result.entries` or `graph_result.scores` allocation fails, `graph_ctx` is freed and graph is skipped. Caller receives keyword + semantic results only. No error, no logging.

**Recommendation:** Consider logging when graph context cannot be merged due to allocation failure.

---

### 13. Gateway: `accept()` failure continues without logging

**Location:** `src/gateway/gateway.c:1203-1206`

```c
            int client = accept(fd, (struct sockaddr *)&client_addr, &client_len);
            if (client < 0)
                continue;
```

**Issue:** Accept failures (e.g. EMFILE, ENOMEM) are silently ignored. Under load, this can hide resource exhaustion.

**Recommendation:** Log accept failures with errno when not EAGAIN/EWOULDBLOCK.

---

## INTEGRATION_GAPS

### 1. Persona: wired but conditional

**Status:** Persona is integrated in daemon, agent, gateway (`persona.set`), and demo-gateway. Requires `SC_HAS_PERSONA` and `--with-agent` for the gateway. No gap.

### 2. Graph: wired

**Status:** Graph is used in daemon (`sc_graph_open`), retrieval engine (`sc_retrieval_set_graph`), and hybrid retrieval. No gap.

### 3. Vision: included in daemon

**Status:** `#include "seaclaw/context/vision.h"` in daemon. Usage should be verified (e.g. image handling in agent turn).

### 4. BTH metrics: wired

**Status:** `bth_metrics` is used in daemon and agent_turn. No gap.

### 5. UI vs C gateway

**Status:** `persona.set` exists in control_protocol, cp_admin, and demo-gateway. Method catalog matches. No identified gap.

---

## RACE_CONDITIONS

### 1. Gateway: `gw->ws.conn_count` and `gw->ws.conns` in poll loop

**Location:** `src/gateway/gateway.c:1170-1178`, `ws_server.c:345`, `437`

**Issue:** The main poll loop reads `gw->ws.conns[i].active` and `conn_count`. WebSocket `on_close` runs from `sc_ws_server_read_and_process` in the same thread, so there is no concurrent modification from another thread. The poll loop and WebSocket processing are single-threaded. **No race.**

### 2. OAuth pending and rate entries

**Location:** `src/gateway/gateway.c` — `oauth_pending_store`, `oauth_pending_lookup`, `oauth_pending_remove`; `rate_entries`

**Issue:** These are used from `http_worker_fn`, which runs in thread pool workers. The main loop does not touch them. Multiple workers can access `oauth_pending` and `rate_entries` concurrently without locks.

**Recommendation:** Add a mutex for `oauth_pending_*` and rate limiting structures, or document single-writer assumptions if they hold.

### 3. Signal handler: `g_stop_flag`

**Location:** `src/daemon.c:816-820`

**Issue:** `volatile sig_atomic_t g_stop_flag` is set in a signal handler. This is correct for a simple stop flag. No issue.

---

## UNVALIDATED_INPUT

### 1. Gateway `/v1/chat/completions` body

**Location:** `src/gateway/gateway.c:489-491`

Body is passed to `sc_openai_compat_handle_chat_completions`. Validation should be confirmed inside that function (JSON parse, size limits).

### 2. Gateway webhook body

**Location:** `src/gateway/gateway.c:906-911`

Body is passed to `on_webhook(channel, body, body_len, ctx)`. HMAC is verified when configured. Channel name comes from path. Ensure `on_webhook` implementations validate/sanitize body.

### 3. Gateway `/api/pair` code

**Location:** `src/gateway/gateway.c:531-538`

Code is extracted from JSON and length-checked. Pairing guard validates format. Adequate.

### 4. Path traversal

**Location:** `src/gateway/gateway.c:243-249`, `358-359`

`sc_gateway_path_has_traversal` and `sc_gateway_path_is` are used for static file serving. Traversal is rejected. Good.

---

## INCONSISTENT_STATE

### 1. Persona load_json partial state on OOM

**Location:** `src/persona/persona.c`

If `sc_strdup` fails for an optional field (e.g. biography, motivation), the persona has mixed valid and NULL fields. Code paths that assume non-NULL may crash or behave oddly.

**Recommendation:** Validate all optional `sc_strdup` results; on failure, call `sc_persona_deinit` and return `SC_ERR_OUT_OF_MEMORY`.

### 2. Persona feedback partial example

**Location:** `src/persona/feedback.c:196-212`

When `sc_strdup` fails for context/incoming/response, the example is not added but allocated strings are freed. Bank state remains consistent. The only inconsistency is that the user's feedback is dropped without notification.

### 3. Daemon cleanup order

**Location:** `src/daemon.c`, `src/gateway/gateway.c`

Gateway cleanup order is correct (bridge, pairing, rate limiter, pool, ws, ctrl, body_buf, fd, gw). Fix the `gw` NULL guard for `sc_ws_server_deinit`.

---

## Summary

| Category           | Count | Critical                                         |
| ------------------ | ----- | ------------------------------------------------ |
| SILENT_FAILURES    | 13    | 3 (gateway cleanup crash, send_all, persona OOM) |
| INTEGRATION_GAPS   | 0     | —                                                |
| RACE_CONDITIONS    | 1     | 1 (oauth/rate from worker threads)               |
| UNVALIDATED_INPUT  | 0     | —                                                |
| INCONSISTENT_STATE | 2     | 1 (persona OOM)                                  |

**Immediate fixes:**

1. Guard `sc_ws_server_deinit(&gw->ws)` with `if (gw)` in gateway cleanup.
2. Add NULL checks for persona `sc_strdup` on optional fields, or fail load on OOM.
3. Replace `sqlite3_bind_text(..., NULL)` with `SQLITE_STATIC` where appropriate for project compliance.
