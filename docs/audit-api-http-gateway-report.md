---
title: API, HTTP, and Gateway Security Audit Report
---
# API, HTTP, and Gateway Security Audit Report

**Date:** 2025-03-08  
**Scope:** All API, HTTP, and gateway code in the human C codebase  
**Auditor:** Automated security audit

---

## Executive Summary

The human codebase demonstrates strong security practices in many areas: path traversal protection, HTTPS enforcement for tools, SSRF protection via `hu_tool_validate_url`, rate limiting, HMAC webhook verification, and secure HTTP client defaults. Several findings require attention, ranging from missing authentication on sensitive endpoints to optional webhook verification and URL validation gaps.

---

## 1. Gateway/HTTP Server (`src/gateway/gateway.c`)

### 1.1 Request Parsing

| File      | Line    | Severity | Description                                                                                                                          |
| --------- | ------- | -------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| gateway.c | 956-957 | **Low**  | `sscanf(line, "%15s %255s", method, path)` — Fixed-width buffers prevent overflow. `path[256]` and `method[16]` are correctly sized. |
| gateway.c | 892-906 | **Info** | Request header parsing uses `strtok` which modifies `req` in place. Body is read separately into `body_buf`. No injection risk.      |
| gateway.c | 243-250 | **Good** | `hu_gateway_path_has_traversal` checks for `..`, `%2e%2e`, `%00`, `%252e%252e` — comprehensive path traversal protection.            |
| gateway.c | 363-368 | **Good** | Static file serving rejects paths with traversal before `open()`. Uses `O_NOFOLLOW` to prevent symlink attacks.                      |

### 1.2 Authentication/Authorization

| File      | Line    | Severity   | Description                                                                                                                             |
| --------- | ------- | ---------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| gateway.c | 655-669 | **High**   | `/v1/chat/completions` and `/v1/models` (OpenAI-compatible API) have **no authentication**. Any client can invoke chat and list models. |
| gateway.c | 484-491 | **Medium** | `/api/status` returns connection count and websocket status with **no auth**. Information disclosure.                                   |
| gateway.c | 454-461 | **Low**    | `/ready` and `/health` are unauthenticated — acceptable for health checks.                                                              |
| gateway.c | 497-551 | **Good**   | `/api/pair` validates pairing code; no auth required (pairing flow).                                                                    |
| gateway.c | 636-644 | **Medium** | Webhook HMAC verification is **optional**. When `hmac_secret` is not configured, webhooks are accepted without signature verification.  |
| gateway.c | 276-302 | **Good**   | WebSocket upgrade requires `Authorization: Bearer <token>` when `auth_token` is set. Origin restricted to localhost.                    |

### 1.3 CORS

| File      | Line    | Severity | Description                                                                                                                       |
| --------- | ------- | -------- | --------------------------------------------------------------------------------------------------------------------------------- |
| gateway.c | 356-364 | **Good** | CORS origin checked via `hu_gateway_is_allowed_origin`. Localhost/127.0.0.1/[::1] allowed; others must be in `cors_origins` list. |
| gateway.c | 393-396 | **Good** | CORS header only echoed when origin is allowed.                                                                                   |

### 1.4 Rate Limiting

| File      | Line    | Severity | Description                                                                |
| --------- | ------- | -------- | -------------------------------------------------------------------------- |
| gateway.c | 447-453 | **Good** | Rate limiting applied before request handling via `hu_rate_limiter_allow`. |
| gateway.c | 848-850 | **Good** | Rate limiter created with configurable window and request count.           |

### 1.5 Request Size Limits

| File      | Line    | Severity | Description                                                                           |
| --------- | ------- | -------- | ------------------------------------------------------------------------------------- |
| gateway.c | 974-982 | **Good** | Content-Length validated; rejects negative, non-numeric, or values > `max_body_size`. |
| gateway.h | 15      | **Good** | `HU_GATEWAY_MAX_BODY_SIZE` = 65536 (64KB).                                            |

### 1.6 Response Content Types

| File      | Line    | Severity | Description                                                              |
| --------- | ------- | -------- | ------------------------------------------------------------------------ |
| gateway.c | 334-354 | **Good** | `mime_for_ext` returns safe content types. Static files limited to 10MB. |
| gateway.c | 396-407 | **Good** | JSON responses use `application/json`.                                   |

### 1.7 Error Responses

| File      | Line           | Severity | Description                                                                              |
| --------- | -------------- | -------- | ---------------------------------------------------------------------------------------- |
| gateway.c | 375-391        | **Good** | Generic status strings; no stack traces or internal paths in responses.                  |
| gateway.c | 641, 651, etc. | **Good** | Webhook/API errors use generic messages (`invalid signature`, `webhook handler failed`). |

### 1.8 HTTP Method Validation

| File      | Line          | Severity | Description                                                         |
| --------- | ------------- | -------- | ------------------------------------------------------------------- |
| gateway.c | 655, 668      | **Good** | `/v1/chat/completions` requires POST; `/v1/models` requires GET.    |
| gateway.c | 497, 551, 569 | **Good** | Pairing and OAuth routes validate method (POST/GET as appropriate). |
| gateway.c | 664-665       | **Low**  | `/api/status` accepts any method — consider restricting to GET.     |

### 1.9 Fixed-Size Buffers

| File      | Line    | Severity | Description                                                                                                    |
| --------- | ------- | -------- | -------------------------------------------------------------------------------------------------------------- |
| gateway.c | 419-421 | **Low**  | `cors_line[256]` — if origin is very long, `snprintf` truncates. No overflow.                                  |
| gateway.c | 414-416 | **Low**  | `hdr[640]` for response headers — could truncate if body_len is huge; `send_all` would send partial. Unlikely. |

---

## 2. Provider API Clients (`src/providers/*.c`, `src/core/http.c`)

All providers use `hu_http_*` from `src/core/http.c`. Audit of the HTTP layer:

### 2.1 Core HTTP Client (`src/core/http.c`)

| File   | Line    | Severity | Description                                                                                             |
| ------ | ------- | -------- | ------------------------------------------------------------------------------------------------------- |
| http.c | 136-142 | **Good** | `CURLOPT_SSL_VERIFYPEER`, `CURLOPT_SSL_VERIFYHOST` enabled.                                             |
| http.c | 135-137 | **Good** | 120s timeout, low-speed limits.                                                                         |
| http.c | 175-186 | **Good** | curl handle released via `curl_pool_release` on all paths.                                              |
| http.c | 76      | **Good** | Response body capped at `HU_HTTP_MAX_RESPONSE_BODY` (16MB).                                             |
| http.c | 189-194 | **Good** | On curl error, `w.buf` freed before return.                                                             |
| http.c | 330-331 | **Info** | URL passed directly to curl — **caller must validate**. Providers use hardcoded or config-derived URLs. |

### 2.2 Provider URL Handling

| File               | Severity | Description                                                                              |
| ------------------ | -------- | ---------------------------------------------------------------------------------------- |
| providers/\*.c     | **Good** | OpenAI, Anthropic, Gemini, etc. use hardcoded `https://` base URLs or config `base_url`. |
| providers/ollama.c | **Low**  | `base_url` from config can be `http://localhost` — acceptable for local inference.       |

### 2.3 API Key Handling

| File           | Severity | Description                                               |
| -------------- | -------- | --------------------------------------------------------- | ------------------------------------------------------------------- |
| http.c         | 159-163  | **Good**                                                  | Auth header built with `snprintf` into 512-byte buffer; not logged. |
| providers/\*.c | **Good** | Keys passed as `Authorization: Bearer <key>`; not in URL. |

### 2.4 JSON Parsing

| File        | Severity | Description                                                                                               |
| ----------- | -------- | --------------------------------------------------------------------------------------------------------- |
| core/json.c | **Info** | JSON parser used across codebase. Bounds checking in parser implementation should be verified separately. |

---

## 3. SSE Client (`src/sse/sse_client.c`)

| File         | Line    | Severity   | Description                                                                            |
| ------------ | ------- | ---------- | -------------------------------------------------------------------------------------- |
| sse_client.c | 345     | **Medium** | `CURLOPT_TIMEOUT, 0L` — **no timeout** for streaming. Long-lived connections can hang. |
| sse_client.c | 264-274 | **Good**   | Buffer grows dynamically; `HU_SSE_MAX_EVENT_SIZE` (256KB) caps event size.             |
| sse_client.c | 182-193 | **Good**   | Event size check before appending data; overflow prevented.                            |
| sse_client.c | 307-308 | **Good**   | curl handle cleaned up on alloc failure.                                               |
| sse_client.c | 293-361 | **Info**   | No reconnection logic — single-shot connect. Caller must retry.                        |
| sse_client.c | 340     | **Info**   | URL not validated for HTTPS — caller responsibility.                                   |

---

## 4. WebSocket Client (`src/websocket/websocket.c`)

| File        | Line    | Severity   | Description                                                                                                                               |
| ----------- | ------- | ---------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| websocket.c | 159-169 | **Good**   | `ws_parse_url` accepts only `ws://` and `wss://`.                                                                                         |
| websocket.c | 215-218 | **Medium** | When `HU_HAS_TLS` is not defined, `wss://` returns `HU_ERR_NOT_SUPPORTED`, but `ws://` is accepted — **unencrypted connections allowed**. |
| websocket.c | 98-129  | **Good**   | `hu_ws_parse_header` validates payload_len against `HU_WS_MAX_FRAME_PAYLOAD` (4MB).                                                       |
| websocket.c | 414-418 | **Good**   | `hdr.payload_len > HU_WS_MAX_MSG` returns error.                                                                                          |
| websocket.c | 456-461 | **Good**   | Fragmented message size checked against `HU_WS_MAX_MSG`.                                                                                  |
| websocket.c | 500-516 | **Good**   | `hu_ws_close` sends close frame, frees frag_buf, closes socket.                                                                           |

---

## 5. Channel HTTP Integrations

### 5.1 Webhook Signature Verification

| Channel      | File                | Severity   | Description                                                                                            |
| ------------ | ------------------- | ---------- | ------------------------------------------------------------------------------------------------------ |
| Gateway      | gateway.c:636-644   | **Medium** | Single HMAC for all webhooks when `webhook_hmac_secret` configured. When not set, **no verification**. |
| Meta (FB/IG) | meta_common.c:14-47 | **Good**   | `hu_meta_verify_webhook` validates `X-Hub-Signature-256` with app secret.                              |
| Telegram     | —                   | **Info**   | No request signing; uses secret token in URL path. Gateway HMAC is optional layer.                     |
| Slack        | —                   | **Info**   | Uses `X-Slack-Signature`; channel-specific. Gateway HMAC is optional.                                  |

### 5.2 Outbound URLs

| Channel                                    | Severity | Description                                              |
| ------------------------------------------ | -------- | -------------------------------------------------------- |
| lark.c, teams.c, google_chat.c, dingtalk.c | **Good** | Webhook URLs validated for `https://` prefix before use. |
| telegram.c                                 | **Good** | Uses `https://api.telegram.org`.                         |
| meta_common.c                              | **Good** | Uses `https://graph.facebook.com`.                       |

### 5.3 Token Handling

| Channel | Severity | Description                                              |
| ------- | -------- | -------------------------------------------------------- |
| All     | **Good** | Tokens in Authorization header or form body; not logged. |

---

## 6. Tool HTTP Calls

### 6.1 web_fetch (`src/tools/web_fetch.c`)

| File        | Line    | Severity | Description                                                                     |
| ----------- | ------- | -------- | ------------------------------------------------------------------------------- |
| web_fetch.c | 228-232 | **Good** | `hu_tool_validate_url` enforces HTTPS and blocks private IPs (SSRF protection). |
| web_fetch.c | 252-262 | **Good** | Uses `hu_http_get`; response freed on all paths.                                |

### 6.2 web_search (`src/tools/web_search.c`)

| File                           | Line       | Severity                                                                                                                    | Description |
| ------------------------------ | ---------- | --------------------------------------------------------------------------------------------------------------------------- | ----------- |
| web_search_providers/\*.c      | **Good**   | Brave, Tavily, Exa, Firecrawl, Perplexity, DuckDuckGo use hardcoded `https://` URLs.                                        |
| web_search_providers/searxng.c | **Medium** | `base_url` from `SEARXNG_BASE_URL` env — **not validated**. If env is `http://169.254.169.254/`, request goes there (SSRF). |
| web_search_providers/jina.c    | **Info**   | Jina fetches user-provided URLs via `https://s.jina.ai/<encoded_url>`. Jina proxy validates; no direct SSRF.                |

### 6.3 http_request (`src/tools/http_request.c`)

| File           | Line    | Severity | Description                                                    |
| -------------- | ------- | -------- | -------------------------------------------------------------- |
| http_request.c | 128-132 | **Good** | `hu_tool_validate_url` — HTTPS only, no private IPs.           |
| http_request.c | 30, 246 | **Info** | `allow_http` stored but **never used** in execute — dead code. |

---

## 7. Config API (`src/config.c`, `src/config_merge.c`)

### 7.1 Config Loading

| File           | Line    | Severity | Description                                                                                                            |
| -------------- | ------- | -------- | ---------------------------------------------------------------------------------------------------------------------- |
| config_merge.c | 236-255 | **Good** | `load_json_file` uses fixed paths (`~/.human/config.json`, `./.human/config.json`). No path traversal in filename. |
| config_merge.c | 245     | **Good** | Config file size capped at 65536 bytes.                                                                                |
| config_merge.c | 318-326 | **Good** | Config directory permissions tightened if too permissive.                                                              |

### 7.2 Config Value Validation

| File              | Severity | Description                                                            |
| ----------------- | -------- | ---------------------------------------------------------------------- |
| config_validate.c | **Info** | Schema validation for known keys; unknown keys ignored or strict-fail. |
| config_parse.c    | **Info** | Provider `base_url` accepted as-is; no HTTPS validation.               |

### 7.3 File Paths from Config

| File           | Severity | Description |
| -------------- | -------- | ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| config_merge.c | 288-299  | **Good**    | `control_ui_dir`, `workspace_dir` from config. Path traversal in these could affect static file serving — gateway checks `hu_gateway_path_has_traversal` on URL path, not filesystem path. |
| gateway.c      | 363-368  | **Good**    | `serve_static_file` uses `base_dir` + `url_path`; traversal in `url_path` rejected.                                                                                                        |

---

## 8. WebSocket Server (`src/gateway/ws_server.c`)

| File        | Line    | Severity | Description                                                                               |
| ----------- | ------- | -------- | ----------------------------------------------------------------------------------------- |
| ws_server.c | 468-476 | **Good** | Frame parsing validates header; `total > conn->recv_len` breaks loop. No buffer overflow. |
| ws_server.c | 447-453 | **Good** | Buffer full with no complete frame closes connection.                                     |
| ws_server.c | 304-314 | **Info** | Token comparison uses constant-time XOR; timing-safe.                                     |
| ws_server.c | 306     | **Low**  | `tok_len != exp_len` short-circuits — acceptable for length check.                        |

---

## 9. Summary of Findings by Severity

### High

- **OpenAI-compatible API unauthenticated** — `/v1/chat/completions` and `/v1/models` accept requests without auth.

### Medium

- **Webhook HMAC optional** — When `webhook_hmac_secret` is not set, webhooks are accepted without verification.
- **SearXNG base_url SSRF** — `SEARXNG_BASE_URL` env not validated; can point to private IPs or HTTP.
- **SSE no timeout** — Streaming connections have no timeout; can hang indefinitely.
- **WebSocket ws:// allowed** — When TLS not available, unencrypted `ws://` is accepted.
- **`/api/status` unauthenticated** — Returns connection count.

### Low

- **`allow_http` dead code** — In http_request tool.
- **`/api/status` method** — Accepts any HTTP method; consider GET-only.
- **OAuth state PRNG** — Uses `time() ^ (uintptr_t)oauth_ctx`; weak but acceptable for CSRF token.

### Good Practices Observed

- Path traversal protection on gateway paths and static files
- HTTPS enforcement for web_fetch, http_request, web_search (except searxng base)
- SSRF protection via `hu_tool_validate_url` (private IP block)
- Rate limiting, body size limits, CORS checks
- curl SSL verification, timeouts, handle cleanup
- Webhook HMAC when configured
- Secure token handling (no logging, headers not URLs)
- WebSocket auth and origin restriction when configured

---

## 10. Recommendations

1. **Add authentication to OpenAI-compatible endpoints** — Require `Authorization: Bearer` or API key header when `auth_token` or pairing is configured.
2. **Require webhook HMAC when webhooks enabled** — Fail startup or reject webhook requests if `webhook_hmac_secret` is not set.
3. **Validate SearXNG base URL** — Use `hu_tool_validate_url` or equivalent for `SEARXNG_BASE_URL` before making requests.
4. **Add SSE connection timeout** — Use `CURLOPT_CONNECTTIMEOUT` and consider a max stream duration.
5. **Consider WSS-only for WebSocket client** — Reject `ws://` when TLS is available; document `ws://` as localhost-only when TLS is disabled.
6. **Restrict `/api/status` to GET** — Or add auth for sensitive fields.
7. **Remove or use `allow_http`** — In http_request tool; currently dead code.
