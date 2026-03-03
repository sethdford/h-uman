---
title: seaclaw Security Audit Report
description: Full codebase security audit per OWASP Top 10 and CWE
updated: 2026-03-02
---

# seaclaw Security Audit Report

**Date:** 2026-03-02
**Scope:** Full codebase (~52K lines C, ~415 source + header files)
**Standard:** OWASP Top 10 (2021), CWE, defense-industry readiness
**Methodology:** Manual static analysis across 4 parallel audit streams

---

## Executive Summary

The audit identified **47 unique findings** across 4 security domains. After deduplication, **42 distinct vulnerabilities** remain:

| Severity | Count | Action Required                            |
| -------- | ----- | ------------------------------------------ |
| CRITICAL | 9     | Immediate remediation                      |
| HIGH     | 14    | Remediate before any production deployment |
| MEDIUM   | 12    | Remediate in next sprint                   |
| LOW      | 7     | Track and address in backlog               |

**Key risk themes:**

1. **Missing authentication on critical control surfaces** (WebSocket, webhooks)
2. **Plaintext credential storage** (API keys, OAuth tokens)
3. **Integer overflow in memory allocation** (arena, JSON parser)
4. **Injection vectors** (SQL identifiers, CRLF headers, path traversal)
5. **Timing side channels** in cryptographic comparisons
6. **Default-allow security policies** when unconfigured

---

## CRITICAL Findings (Immediate Action)

### C-01: WebSocket Control Protocol Has No Authentication

- **Files:** `src/gateway/control_protocol.c`, `src/gateway/ws_server.c`
- **OWASP:** A07:2021 — Identification and Authentication Failures
- **CWE:** CWE-306 (Missing Authentication for Critical Function)
- **Description:** WebSocket connections for the control protocol accept any client. `gw->config.auth_token` is stored but never checked during upgrade. No token, pairing, or other auth is enforced.
- **Exploit:** Any attacker on the network can connect via WebSocket and invoke `config.set`, `config.apply`, `sessions.delete`, `exec.approval.resolve` — full control over the agent.
- **Fix:** Check `auth_token` or pairing token on WebSocket upgrade. Require an `Authorization` header or query parameter. Reject upgrade if `require_pairing` is true and no valid token is provided.

### C-02: Webhook Endpoints Accept Unauthenticated Requests

- **Files:** `src/gateway/gateway.c` (~lines 151–154, 291–297)
- **OWASP:** A07:2021 — Identification and Authentication Failures
- **CWE:** CWE-287 (Improper Authentication)
- **Description:** When `hmac_secret` is null or empty, `verify_hmac()` returns `true` without checking signatures. Combined with default `host = "0.0.0.0"`, all webhook endpoints are exposed unauthenticated.
- **Exploit:** Attacker sends forged webhook payloads to `/webhook`, `/telegram`, `/slack/events` — messages processed as legitimate.
- **Fix:** Return `false` when no HMAC secret is configured. Require HMAC for all webhook paths when the gateway is network-exposed.

### C-03: OAuth Tokens and API Keys Stored in Plaintext

- **Files:** `src/auth.c` (~lines 80–100, 117), `src/config.c` (~lines 164–166, 581–592)
- **OWASP:** A02:2021 — Cryptographic Failures
- **CWE:** CWE-312 (Cleartext Storage of Sensitive Information)
- **Description:** Credentials are written to `~/.seaclaw/auth.json` and `~/.seaclaw/config.json` as plaintext JSON. File permissions not explicitly set (depends on umask). `sc_secret_store` exists but is not used for these files.
- **Exploit:** Local access (malware, shared system, backups) exposes all API keys and OAuth tokens.
- **Fix:** Use `sc_secret_store_encrypt` for persisting credentials. Set file mode `0600`. Directory mode `0700`. Never write raw `api_key` values to config.json.

### C-04: HTTP Header CRLF Injection via `http_request` Tool

- **File:** `src/tools/http_request.c` (~lines 58–75)
- **OWASP:** A03:2021 — Injection
- **CWE:** CWE-113 (Improper Neutralization of CRLF in HTTP Headers)
- **Description:** `parse_headers()` builds HTTP headers from JSON without sanitizing `\r\n`. Header names and values with CRLF are sent verbatim.
- **Exploit:** `{"headers": {"X-Custom": "value\r\nX-Injected: evil"}}` produces extra headers — cache poisoning, response splitting, security bypass.
- **Fix:** Reject or strip CR, LF, and NUL in both header names and values before passing to libcurl.

### C-05: Git Tool Path Traversal / Arbitrary File Disclosure

- **File:** `src/tools/git.c` (~lines 154–173)
- **OWASP:** A01:2021 — Broken Access Control
- **CWE:** CWE-22 (Path Traversal)
- **Description:** The git tool does not use `sc_tool_validate_path()` for `paths`, `files`, `branch`, and similar inputs. These are passed directly to git commands.
- **Exploit:** `{"operation": "diff", "files": "../../../etc/passwd"}` — exposes files outside workspace.
- **Fix:** Validate all path-like inputs with `sc_tool_validate_path()` before passing them to git.

### C-06: PostgreSQL/pgvector Identifier Injection

- **Files:** `src/memory/engines/postgres.c` (~lines 137–142, 250–251, 355–356, 478–492), `src/memory/vector/store_pgvector.c` (~lines 39–42, 78–82)
- **OWASP:** A03:2021 — Injection
- **CWE:** CWE-89 (SQL Injection)
- **Description:** `schema_q`, `table_q`, and `p->table_name` are interpolated into SQL via `%s` without quoting or validation. Attacker-controlled config yields arbitrary SQL execution.
- **Exploit:** Config `schema = "public\"; DROP TABLE memories; --"` → SQL injection.
- **Fix:** Validate identifiers against `[a-zA-Z0-9_]` only. Use `PQescapeIdentifier()`. Reject invalid config values.

### C-07: Firecracker Sandbox Fixed Socket Path (PID 0)

- **File:** `src/security/firecracker.c` (~lines 159–161)
- **OWASP:** A04:2021 — Insecure Design
- **CWE:** CWE-362 (Race Condition)
- **Description:** Socket path uses `(int)0` instead of `getpid()`. All Firecracker instances share `/tmp/sc_fc_0.sock`.
- **Exploit:** Attacker races to bind the socket before the legitimate process — full sandbox control.
- **Fix:** Replace `(int)0` with `getpid()` and add a unique per-sandbox counter.

### C-08: NULL Pointer Dereference in `sc_json_string_new`

- **File:** `src/core/json.c` (~lines 390–399)
- **OWASP:** A04:2021 — Insecure Design
- **CWE:** CWE-476 (NULL Pointer Dereference)
- **Description:** No check for `s == NULL` before `memcpy`. Callers passing `NULL` with `len > 0` trigger undefined behavior.
- **Fix:** Add `if (len > 0 && !s) return NULL;` before memcpy.

### C-09: Buffer Overread in `sc_json_append_key`

- **File:** `src/core/json.c` (~lines 716–721)
- **OWASP:** A04:2021 — Insecure Design
- **CWE:** CWE-125 (Out-of-bounds Read)
- **Description:** When `key == NULL` and `key_len > 0`, the function passes `""` but reads `key_len` bytes past the 1-byte string literal.
- **Exploit:** Out-of-bounds read → information disclosure from process memory.
- **Fix:** Add `if (!key && key_len > 0) return SC_ERR_INVALID_ARGUMENT;`

---

## HIGH Findings (Pre-Production Remediation)

### H-01: Timing Attack on Webhook HMAC Verification

- **File:** `src/gateway/gateway.c` (~line 161)
- **OWASP:** A07:2021 — Identification and Authentication Failures
- **CWE:** CWE-208 (Observable Timing Discrepancy)
- **Description:** `strncmp(sig_header, hex, 64)` is not constant-time. Execution time leaks byte-by-byte match progress.
- **Fix:** Use constant-time XOR comparison (pattern exists in `pairing.c`).

### H-02: Sensitive Material Not Securely Cleared After Use

- **Files:** `src/security/secrets.c`, `src/security/pairing.c`
- **OWASP:** A02:2021 — Cryptographic Failures
- **CWE:** CWE-316 (Cleartext Storage in Memory)
- **Description:** Keys, nonces, and plaintext are cleared with `memset()` which compilers may optimize away.
- **Fix:** Use `explicit_bzero()` (POSIX) or create `sc_secure_zero()` wrapper used for all secret material.

### H-03: Path-Based Access Control Defaults to Allow

- **File:** `src/security/security.c` (~lines 7–9)
- **OWASP:** A01:2021 — Broken Access Control
- **CWE:** CWE-284 (Improper Access Control)
- **Description:** When `allowed_paths` is NULL or empty, `sc_security_path_allowed` returns `true`. Default is open, not closed.
- **Fix:** Return `false` when no allowed paths are configured. Default deny.

### H-04: Pairing Code Has Limited Entropy (6 Digits)

- **File:** `src/security/pairing.c` (~lines 39–47)
- **OWASP:** A07:2021 — Identification and Authentication Failures
- **CWE:** CWE-330 (Insufficiently Random Values)
- **Description:** 6-digit code = ~1M combinations. 5-attempt lockout for 300s. Brute-forceable in ~2 days after lockout cycling.
- **Fix:** Use 8+ digits or alphanumeric. Implement exponential backoff. Consider one-time pairing URL/token.

### H-05: API Key Masking Exposes Leading Characters

- **File:** `src/providers/api_key.c` (~lines 76–86)
- **OWASP:** A04:2021 — Insecure Design
- **CWE:** CWE-532 (Sensitive Info in Log File)
- **Description:** `sc_api_key_mask()` reveals first 4 characters. Combined with known provider key formats, reduces brute-force space.
- **Fix:** Show only last 4 characters (`****...XXXX`) or use `[REDACTED]`.

### H-06: WebSocket Upgrade Without Origin Validation

- **File:** `src/gateway/ws_server.c` (~lines 221–272)
- **OWASP:** A05:2021 — Security Misconfiguration
- **CWE:** CWE-346 (Origin Validation Error)
- **Description:** No `Origin` header check on WebSocket upgrade. Any website can open a cross-site WebSocket connection.
- **Fix:** Validate `Origin` against allowed origins list before upgrading.

### H-07: Spawn Tool Allows Arbitrary Executable Execution

- **File:** `src/tools/spawn.c` (~lines 41–76, 112–127)
- **OWASP:** A03:2021 — Injection
- **CWE:** CWE-78 (OS Command Injection)
- **Description:** `spawn` accepts any `command` path and `args` with no allowlist. A compromised model can run `{"command": "/bin/sh", "args": ["-c", "rm -rf /"]}`.
- **Fix:** Add optional command allowlist. Restrict to known-safe commands in high-assurance deployments.

### H-08: `browser_open` Buffer Over-Read

- **File:** `src/tools/browser_open.c` (~lines 22–30)
- **OWASP:** A04:2021 — Insecure Design
- **CWE:** CWE-125 (Out-of-bounds Read)
- **Description:** `strncmp(host + len - 10, ".localhost", 10)` without checking `len >= 10`. Underflow on short hosts → `SIZE_MAX` offset.
- **Fix:** Add `if (len >= 10)` guard before each suffix comparison.

### H-09: Unvalidated Snapshot Export/Import Paths

- **File:** `src/memory/lifecycle/snapshot.c` (~lines 81–95, 106–114)
- **OWASP:** A01:2021 — Broken Access Control
- **CWE:** CWE-22 (Path Traversal)
- **Description:** Export/import accept `path` from caller with no `..` or symlink checks. Arbitrary file read/write.
- **Fix:** Canonicalize with `realpath()`, ensure inside workspace, reject `..`.

### H-10: LLM Error Messages Logged Without Scrubbing

- **Files:** `src/observability/log_observer.c` (~lines 73–76), `src/agent/agent.c` (~line 769)
- **OWASP:** A09:2021 — Security Logging and Monitoring Failures
- **CWE:** CWE-532 (Sensitive Info in Log File)
- **Description:** Provider error messages (which may contain API keys) are written to logs without sanitization.
- **Fix:** Apply `sc_scrub_sanitize_api_error()` before logging all error text.

### H-11: FTS5 Query Built from User Input Without Escaping

- **File:** `src/memory/engines/sqlite.c` (~lines 189–206)
- **OWASP:** A03:2021 — Injection
- **CWE:** CWE-89 (SQL Injection)
- **Description:** FTS5 MATCH query wraps user words in double quotes without escaping embedded `"`. Breaks FTS5 syntax.
- **Fix:** Escape `"` as `""` inside FTS5 phrase literals.

### H-12: Config Workspace Path Injection

- **File:** `src/config.c` (~lines 602–608)
- **OWASP:** A01:2021 — Broken Access Control
- **CWE:** CWE-22 (Path Traversal)
- **Description:** `workspace` from JSON used as `workspace_dir` without validation. A crafted config can point to `../../../etc`.
- **Fix:** Canonicalize, reject paths outside `$HOME` or explicit allowed base.

### H-13: Integer Overflow in Arena Allocation

- **File:** `src/core/arena.c` (~lines 22–24, 34–40)
- **OWASP:** A04:2021 — Insecure Design
- **CWE:** CWE-190 (Integer Overflow)
- **Description:** `size + 7` can overflow near `SIZE_MAX`. `used + aligned` can overflow. `sizeof(arena_block_t) + block_size` can overflow.
- **Fix:** Add overflow checks before each arithmetic operation.

### H-14: Diagnostics Flags Can Log Sensitive Data

- **File:** `src/config.c` (~lines 657–660)
- **OWASP:** A09:2021 — Security Logging and Monitoring Failures
- **CWE:** CWE-532
- **Description:** `log_llm_io`, `log_message_payloads`, etc. dump raw content to logs including PII and secrets.
- **Fix:** Scrub all payloads before logging. Add warnings for production use.

---

## MEDIUM Findings

### M-01: `realloc(ptr, 0)` Handling in Tracking Allocator

- **File:** `src/core/allocator.c` (~lines 73–95) | CWE-415 | Treat `new_size == 0` as free.

### M-02: Integer Truncation in libcurl Body Size

- **File:** `src/core/http.c` (~lines 354, 435, 592) | CWE-190 | Reject bodies exceeding `LONG_MAX`.

### M-03: Integer Overflow in JSON Buffer Growth

- **File:** `src/core/json.c` (~lines 520–521, 493–494, 195–198) | CWE-190 | Add overflow checks before growth arithmetic.

### M-04: Format String Risk in `sc_sprintf`

- **File:** `src/core/string.c` (~lines 80–96) | CWE-134 | Document `fmt` must be constant. Consider compiler attributes.

### M-05: Path Traversal Protection Too Narrow (URL Encoding)

- **File:** `src/gateway/gateway.c` (~lines 215–217) | CWE-22 | Decode URL-encoded segments before validation. Canonicalize.

### M-06: TOCTOU in Static File Serving

- **File:** `src/gateway/gateway.c` (~lines 226–241) | CWE-367 | Use `open(O_NOFOLLOW)` + `fstat()` on fd.

### M-07: Secret Key File Created Without Restrictive Permissions

- **File:** `src/security/secrets.c` (~lines 102–109) | CWE-732 | Use `open()` with mode `0600` then `fdopen()`.

### M-08: Static Buffers in WASI and Firecracker Sandboxes

- **Files:** `src/security/wasi_sandbox.c` (~lines 70–76), `src/security/firecracker.c` (~lines 75–78) | CWE-362 | Move to per-call or per-context buffers.

### M-09: Content-Length Parsed with `atoi`

- **File:** `src/gateway/gateway.c` (~line 484) | CWE-20 | Use `strtol()` with range checks.

### M-10: SQLite Security Settings Not Applied

- **Files:** `src/memory/engines/sqlite.c`, `lucid.c`, `lancedb.c` | CWE-359 | Set `PRAGMA secure_delete=ON`, `journal_mode=WAL`, `foreign_keys=ON`.

### M-11: No Explicit TLS Verification for libcurl

- **File:** `src/core/http.c` (`curl_setup_common`) | CWE-295 | Explicitly set `CURLOPT_SSL_VERIFYPEER` and `CURLOPT_SSL_VERIFYHOST`.

### M-12: HTTP Timeout Not Configurable (300s Default)

- **File:** `src/core/http.c` (~line 169) | CWE-400 | Make configurable. Default 60–120s for providers.

---

## LOW Findings

### L-01: `SQLITE_TRANSIENT` Used Despite Project Rule (sqlite.c, lucid.c, lancedb.c)

### L-02: Audit Log Can Include Full Command Strings with Secrets (security/audit.c)

### L-03: Gateway Logs Webhook Path and Channel (gateway/gateway.c)

### L-04: Seccomp BPF ERRNO Encoding Arch-Dependent (security/seccomp.c)

### L-05: `Access-Control-Allow-Origin: *` on All Responses (gateway/gateway.c)

### L-06: SSE Client URL/Header Not Validated by Library (sse/sse_client.c)

### L-07: Shell Tool Passes Commands to `/bin/sh -c` (tools/shell.c)

---

## OWASP Top 10 Coverage Matrix

| OWASP Category                    | Findings                                | Status                                  |
| --------------------------------- | --------------------------------------- | --------------------------------------- |
| **A01** Broken Access Control     | C-05, H-03, H-09, H-12, M-05            | Multiple path traversal + default-allow |
| **A02** Cryptographic Failures    | C-03, H-02, H-05, M-07                  | Plaintext secrets + weak clearing       |
| **A03** Injection                 | C-04, C-05, C-06, H-07, H-11, M-04      | CRLF, SQL, command, FTS5                |
| **A04** Insecure Design           | C-07, C-08, C-09, H-08, H-13, M-01–M-03 | Memory safety, integer overflow         |
| **A05** Security Misconfiguration | H-06, L-05                              | CORS, Origin validation                 |
| **A06** Vulnerable Components     | —                                       | No third-party vulns identified         |
| **A07** Auth Failures             | C-01, C-02, H-01, H-04                  | Missing auth, timing attacks            |
| **A08** Software/Data Integrity   | —                                       | No deserialization vulns identified     |
| **A09** Logging Failures          | H-10, H-14, L-02, L-03                  | Secrets in logs                         |
| **A10** SSRF                      | L-06                                    | Limited; HTTPS enforcement helps        |

---

## Defense Industry Readiness Assessment

### Strengths

1. Compile with `-Wall -Wextra -Wpedantic -Werror` — strong baseline
2. AddressSanitizer in CI — catches many memory bugs
3. `SC_IS_TEST` guards — good isolation in tests
4. Parameterized SQLite queries — prevents most SQL injection
5. HTTPS enforcement policy — good network security stance
6. Deny-by-default design intent in security policy
7. ChaCha20 + HMAC for secrets — solid crypto primitives

### Critical Gaps for Defense Certification

1. **Authentication is optional everywhere** — WebSocket, webhooks, and gateway all work unauthenticated by default. Defense requires mandatory auth.
2. **No secure credential storage** — API keys and tokens in plaintext on disk. Needs integration with OS keychain or encrypted secrets store.
3. **No audit trail integrity** — Audit logs can be tampered with. Consider signed/append-only audit logging.
4. **No FIPS 140-2 compliance path** — ChaCha20 is not FIPS-approved. Consider AES-256-GCM for government/defense.
5. **No formal threat model** — Document threat model per NIST SP 800-53 or equivalent.
6. **Integer overflow in allocators** — Memory corruption from crafted inputs. Critical for systems handling untrusted data.
7. **Default-allow security policies** — Must flip to default-deny everywhere.

---

## Remediation Priority Matrix

### Phase 1: Immediate (Week 1) — Block exploits

| ID   | Finding                          | Effort |
| ---- | -------------------------------- | ------ |
| C-01 | WebSocket authentication         | Medium |
| C-02 | Webhook HMAC mandatory           | Low    |
| C-03 | Encrypt credentials at rest      | Medium |
| C-04 | CRLF header sanitization         | Low    |
| C-05 | Git tool path validation         | Low    |
| H-01 | Constant-time HMAC comparison    | Low    |
| H-03 | Default-deny path access control | Low    |

### Phase 2: Short-term (Weeks 2–3) — Harden surfaces

| ID   | Finding                         | Effort |
| ---- | ------------------------------- | ------ |
| C-06 | PostgreSQL identifier injection | Medium |
| C-07 | Firecracker socket path fix     | Low    |
| C-08 | JSON NULL pointer checks        | Low    |
| C-09 | JSON append_key bounds check    | Low    |
| H-02 | Secure memory clearing          | Medium |
| H-06 | WebSocket Origin validation     | Low    |
| H-08 | browser_open bounds fix         | Low    |
| H-09 | Snapshot path validation        | Low    |
| H-13 | Arena integer overflow checks   | Medium |
| M-05 | URL-decode path traversal check | Medium |
| M-07 | Secret file permissions         | Low    |

### Phase 3: Medium-term (Weeks 3–5) — Production hardening

| ID        | Finding                  | Effort |
| --------- | ------------------------ | ------ |
| H-04      | Stronger pairing codes   | Medium |
| H-05      | API key masking fix      | Low    |
| H-07      | Spawn tool allowlist     | Medium |
| H-10      | Log scrubbing for errors | Medium |
| H-11      | FTS5 query escaping      | Low    |
| H-12      | Config path validation   | Low    |
| M-01–M-12 | All medium findings      | High   |

### Phase 4: Defense certification prep

- Formal threat model document
- FIPS 140-2 crypto evaluation
- Signed/tamper-proof audit logging
- Penetration testing by external firm
- STIG/SCAP compliance checks
- Supply chain security (SBOM generation)

---

## Methodology Notes

This audit was conducted via manual static analysis across 4 parallel streams:

1. **Core memory safety** — `src/core/`, `include/seaclaw/`
2. **Security/crypto/auth** — `src/security/`, `src/auth.c`, `src/gateway/`
3. **Input surfaces/injection** — `src/tools/`, `src/gateway/`, `src/channels/`, `src/sse/`, `src/websocket/`
4. **Data/config/providers** — `src/memory/`, `src/providers/`, `src/config.c`, `src/observability/`, `src/agent/`

Findings were deduplicated across streams. Dynamic analysis (fuzzing, pen testing) was not performed and is recommended as a follow-up.
