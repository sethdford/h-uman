---
title: Silent Failures Audit — human Codebase
---
# Silent Failures Audit — human Codebase

This document catalogs silent failures, ignored error codes, and inadequate error handling found across the C gateway, WebSocket server, UI controllers/components, and config/main entry points.

---

## 1. C Gateway (`src/gateway/`)

### 1.1 WebSocket upgrade: wrong HTTP status for all errors

**Location:** `src/gateway/gateway.c` lines 877–880

**Issue:** When `hu_ws_server_upgrade` fails for any reason, the gateway always sends HTTP 429 ("too many connections") and closes the connection. Other errors (e.g. `HU_ERR_PERMISSION_DENIED`, `HU_ERR_INVALID_ARGUMENT`, `HU_ERR_IO`) are misreported as 429.

**Hidden errors:** 401 Unauthorized, 403 Forbidden, 400 Bad Request, 500 Internal Server Error

**User impact:** Clients cannot distinguish auth failures from rate limiting. Debugging is harder.

**Recommendation:** Map `ws_err` to appropriate HTTP status codes:

```c
if (ws_err != HU_OK) {
    int code = 500;
    const char *msg = "{\"error\":\"internal error\"}";
    if (ws_err == HU_ERR_ALREADY_EXISTS) {
        code = 429;
        msg = "{\"error\":\"too many connections\"}";
    } else if (ws_err == HU_ERR_PERMISSION_DENIED) {
        code = (strstr(req, "401") ? 401 : 403);
        msg = "{\"error\":\"unauthorized\"}";
    } else if (ws_err == HU_ERR_INVALID_ARGUMENT) {
        code = 400;
        msg = "{\"error\":\"bad request\"}";
    }
    send_json(client, code, msg);
    close(client);
}
```

---

### 1.2 WebSocket `send()` return value ignored

**Location:** `src/gateway/ws_server.c` lines 424, 499

**Issue:** `send(conn->fd, ...)` return value is cast to `(void)`. Partial sends and failures are ignored.

```c
(void)send(conn->fd, close_frame, 2, MSG_NOSIGNAL);   // line 424
(void)send(conn->fd, pong, 2 + plen, MSG_NOSIGNAL);   // line 499
```

**Hidden errors:** `EAGAIN`/`EWOULDBLOCK`, `EPIPE`, `ECONNRESET`, partial writes

**User impact:** Close frames and pongs may not be sent; peers may not see clean shutdowns.

**Recommendation:** Log send failures and optionally retry or treat as connection loss:

```c
ssize_t n = send(conn->fd, close_frame, 2, MSG_NOSIGNAL);
if (n < 2 && n >= 0)
    /* log partial send */;
else if (n < 0)
    /* log errno */;
```

---

### 1.3 Event bridge: `hu_push_send` return value ignored

**Location:** `src/gateway/event_bridge.c` line 98

**Issue:** `hu_push_send` returns `hu_error_t` but the result is discarded.

```c
hu_push_send(bridge->push, title, body, payload_str);
```

**Hidden errors:** Push delivery failures (FCM/APNS errors, invalid tokens, network issues)

**User impact:** Push notifications may fail without any log or user feedback.

**Recommendation:** Log push failures:

```c
hu_error_t push_err = hu_push_send(bridge->push, title, body, payload_str);
if (push_err != HU_OK)
    /* log hu_error_string(push_err) */;
```

---

### 1.4 Control protocol: OOM in `build_connect_response`

**Location:** `src/gateway/control_protocol.c` lines 56–59

**Issue:** `hu_json_object_new` / `hu_json_array_new` can return NULL, but `server`, `features`, and `methods_arr` are used without checks. `json_set_str(alloc, server, ...)` would dereference NULL.

**Hidden errors:** `HU_ERR_OUT_OF_MEMORY` when allocating server/features/methods_arr

**User impact:** Crash or undefined behavior on OOM during connect handshake.

**Recommendation:** Add NULL checks after each allocation and return `HU_ERR_OUT_OF_MEMORY` on failure.

---

### 1.5 Webhook dispatcher: channel handler return value ignored

**Location:** `src/main.c` lines 527–541

**Issue:** `webhook_fn` is called but its return value (if any) is ignored. Channel handlers may signal errors that are never surfaced.

```c
if (d->channels[i].webhook_fn)
    d->channels[i].webhook_fn(d->channels[i].channel_ctx, d->alloc, body, body_len);
return;
```

**Note:** The webhook vtable may not return an error code; this is a design question. If it does, it should be logged or propagated.

---

## 2. WebSocket Server (`src/gateway/ws_server.c`, `src/websocket/`)

### 2.1 Close frame and PONG send failures (see 1.2)

Already covered above.

### 2.2 `on_message` callback cannot report errors

**Location:** `src/gateway/ws_server.c` lines 484–486

**Issue:** `srv->on_message(conn, payload, plen, srv->cb_ctx)` is `void`. If message processing fails (e.g. JSON parse error, control protocol error), the callback has no way to signal failure. The connection stays open with no error feedback.

**Recommendation:** Consider an `on_message_error` callback or a way for the control protocol to send an error frame back to the client when processing fails.

---

## 3. UI Controllers (`ui/src/controllers/chat-controller.ts`)

### 3.1 Empty catch in session rename

**Location:** `ui/src/views/chat-view.ts` lines 394–397

**Issue:** `sessions.patch` failures are swallowed with an empty `catch {}`. The UI still updates `_sessions` optimistically, so the user sees the new title even when the rename failed.

```typescript
try {
  await gw.request("sessions.patch", { key: id, label: title });
  this._sessions = this._sessions.map((s) =>
    s.id === id ? { ...s, title } : s,
  );
} catch {}
```

**User impact:** User believes rename succeeded; server still has old title. State desync.

**Recommendation:** Revert optimistic update and show toast on failure:

```typescript
} catch (e) {
  ScToast.show({
    message: e instanceof Error ? e.message : "Failed to rename session",
    variant: "error",
  });
  this._requestUpdate(); // revert optimistic state
}
```

---

### 3.2 History load failure is silent

**Location:** `ui/src/controllers/chat-controller.ts` lines 164–166

**Issue:** `loadHistory` catches errors with "best-effort" and falls back to cache. No user feedback when the gateway request fails.

```typescript
} catch {
  /* history load is best-effort */
}
```

**User impact:** User may see stale or empty history with no indication that loading failed.

**Recommendation:** Set `errorBanner` or show a toast when history load fails and cache is empty.

---

### 3.3 Copy-to-clipboard shows success even when it fails

**Location:** `ui/src/components/hu-message-actions.ts` lines 93–94

**Issue:** `navigator.clipboard?.writeText(this.content).catch(() => {})` swallows rejections, but `ScToast.show({ message: "Copied to clipboard", variant: "success" })` always runs.

```typescript
navigator.clipboard?.writeText(this.content).catch(() => {});
ScToast.show({ message: "Copied to clipboard", variant: "success" });
```

**User impact:** User sees "Copied" when clipboard write failed (e.g. permission denied).

**Recommendation:** Await the promise and only show success on resolve:

```typescript
try {
  await navigator.clipboard?.writeText(this.content);
  ScToast.show({ message: "Copied to clipboard", variant: "success" });
  this._copied = true;
  // ...
} catch {
  ScToast.show({ message: "Failed to copy", variant: "error" });
}
```

---

### 3.4 Overview view: per-request failures hidden by `.catch(() => ({}))`

**Location:** `ui/src/views/overview-view.ts` lines 367–378

**Issue:** Each `gw.request(...).catch(() => ({}))` or `.catch(() => ({ channels: [] }))` hides individual failures. The outer `catch` only runs if all promises reject. Partial failures (e.g. `health` succeeds, `sessions.list` fails) produce incomplete data with no error indication.

**User impact:** Overview shows partial data; user does not know some sections failed to load.

**Recommendation:** Track per-section errors and show them (e.g. inline error banners or toasts).

---

## 4. UI Components (`ui/src/components/hu-*.ts`)

### 4.1 `querySelector` / `querySelectorAll` without null checks

**Location:** Multiple components

**Issue:** Several components call `querySelector` / `querySelectorAll` and use the result without checking for null, e.g.:

- `hu-chart.ts` line 195: `const wrapper = this.renderRoot.querySelector(".wrapper");` — used in `wrapper` without null check (guarded by `if (wrapper)` on line 196, so this one is OK).
- `hu-dropdown.ts` lines 174–175: `const menu = this.renderRoot.querySelector(...); const items = menu?.querySelectorAll(...)` — `menu?.` handles null.
- `hu-shortcut-overlay.ts` line 188: `const panel = this.renderRoot.querySelector<HTMLElement>(".panel");` — `panel?.querySelectorAll` used; if `panel` is null, `Array.from(panel.querySelectorAll(...))` would throw.

**Recommendation:** Audit all `querySelector`/`querySelectorAll` usages and ensure null/empty results are handled before use.

---

### 4.2 Chart.js load failure is silent

**Location:** `ui/src/components/hu-chart.ts` line 70

**Issue:** `import("https://esm.sh/chart.js@4").catch(() => null)` swallows the error. If the CDN fails, `_chartLoadPromise` resolves to `null` and the chart never renders, with no user feedback.

**Recommendation:** Set an error state (e.g. `_chartLoadError`) and render a fallback message when Chart.js fails to load.

---

### 4.3 Service worker registration failure is silent

**Location:** `ui/index.html` line 39

**Issue:** `navigator.serviceWorker.register("/sw.js").catch(() => {})` — registration failures (e.g. scope issues, network errors) are ignored.

**User impact:** PWA/offline behavior may not work; user has no indication.

**Recommendation:** Log the error in development; optionally show a non-intrusive message in production.

---

## 5. Config / Main (`src/config.c`, `src/main.c`)

### 5.1 Config defaults: OOM in `set_defaults` produces invalid config

**Location:** `src/config.c` lines 26–172

**Issue:** `hu_strdup` and `a->alloc` can return NULL. `set_defaults` does not check. Fields like `default_provider`, `default_model`, `memory_backend`, etc. may be NULL. Later code (e.g. `cfg.default_provider`) can crash or behave incorrectly.

**Hidden errors:** `HU_ERR_OUT_OF_MEMORY` during config initialization

**User impact:** Crash or undefined behavior when memory is tight during startup.

**Recommendation:** Either make `set_defaults` return `hu_error_t` and fail on first OOM, or add fallbacks (e.g. static string literals) for critical fields when `hu_strdup` fails.

---

### 5.2 Plugin loading: error logged but execution continues

**Location:** `src/main.c` lines 570–574, 1691–1696

**Issue:** `hu_plugin_load` failures are logged with `fprintf(stderr, "warning: failed to load plugin: ...")` but execution continues. This is acceptable for optional plugins, but the user may not see stderr in daemon/service mode.

**Recommendation:** Ensure plugin load failures are visible in the configured log/observability path when running as a service.

---

### 5.3 Gateway thread: error logged but not propagated

**Location:** `src/main.c` lines 514–518

**Issue:** `hu_gateway_run` errors are logged to stderr but the main process has no way to react (e.g. exit, restart). The gateway thread just returns.

```c
hu_error_t err = hu_gateway_run(ctx->alloc, ctx->host, ctx->port, &ctx->config);
if (err != HU_OK)
    fprintf(stderr, "[human] gateway thread error: %s\n", hu_error_string(err));
return NULL;
```

**Recommendation:** Consider a shared error flag or callback so the main loop can detect gateway failure and take action (e.g. shutdown, alert).

---

## 6. Additional Patterns

### 6.1 `(void)` casts for intentional ignore

Many `(void)variable` or `(void)func()` uses are intentional (e.g. suppressing unused parameter warnings). The following are more concerning:

- `(void)setsockopt(...)` — `gateway.c` lines 857, 974, 978: setsockopt failures are ignored. SO_LINGER and SO_RCVTIMEO may not be applied.
- `(void)send(...)` — `ws_server.c` (covered above).

### 6.2 Chat controller: cache and abort

- `cacheMessages` / `restoreFromCache`: catch for quota/corruption is acceptable (best-effort cache).
- `abort`: catch for best-effort is acceptable.

---

## Summary Table

| Severity | Location                   | Issue                                          |
| -------- | -------------------------- | ---------------------------------------------- |
| CRITICAL | `chat-view.ts:397`         | Session rename failure swallowed; state desync |
| CRITICAL | `hu-message-actions.ts:93` | Copy success shown when clipboard write fails  |
| HIGH     | `gateway.c:879`            | All WS upgrade errors reported as 429          |
| HIGH     | `event_bridge.c:98`        | `hu_push_send` return value ignored            |
| HIGH     | `control_protocol.c:56-59` | OOM in `build_connect_response` can crash      |
| HIGH     | `config.c:set_defaults`    | OOM produces invalid config                    |
| MEDIUM   | `ws_server.c:424,499`      | `send()` return value ignored                  |
| MEDIUM   | `chat-controller.ts:164`   | History load failure silent                    |
| MEDIUM   | `overview-view.ts:367`     | Partial load failures hidden                   |
| MEDIUM   | `hu-chart.ts:70`           | Chart.js load failure silent                   |
| LOW      | `index.html:39`            | Service worker registration failure silent     |
| LOW      | `main.c:516`               | Gateway thread error not propagated            |

---

_Generated by silent-failure-hunter audit._
