---
title: human Threat Model
description: STRIDE threat model and security architecture for human
updated: 2026-03-02
---

# human Threat Model

**Document Version:** 1.0  
**Date:** 2026-03-02  
**Classification:** Internal Use  
**Methodology:** STRIDE, NIST SP 800-53  
**Related:** `docs/security/2026-03-02-security-audit.md`

---

## 1. System Overview

### 1.1 What human Is

human is a C11 autonomous AI assistant runtime optimized for:

- **Minimal binary size:** 267 KB (MinSizeRel build)
- **Minimal memory footprint:** Target &lt; 5 MB peak RSS
- **Minimal dependencies:** libc, optional SQLite and libcurl
- **Cross-platform:** POSIX (macOS, Linux), optional Windows

The system orchestrates AI model providers, messaging channels, tool execution, memory backends, and hardware peripherals under a vtable-driven, modular architecture.

### 1.2 Architecture

All extension work is done by implementing vtable structs and registering them in factory functions. Key components:

| Component         | Path                 | Purpose                                                                   |
| ----------------- | -------------------- | ------------------------------------------------------------------------- |
| **Providers**     | `src/providers/`     | AI model providers (`hu_provider_t`) — 50+ implementations                |
| **Channels**      | `src/channels/`      | Messaging channels (`hu_channel_t`) — CLI, Telegram, Discord, Slack, etc. |
| **Tools**         | `src/tools/`         | Tool execution surface (`hu_tool_t`) — 30+ tools                          |
| **Memory**        | `src/memory/`        | SQLite, markdown, LRU backends, embeddings, vector search                 |
| **Security**      | `src/security/`      | Policy engine, pairing, secrets, sandbox backends                         |
| **Runtime**       | `src/runtime/`       | Native, Docker, WASM, Cloudflare adapters                                 |
| **Peripherals**   | `src/peripherals/`   | Arduino, STM32/Nucleo, Raspberry Pi                                       |
| **Gateway**       | `src/gateway/`       | HTTP/webhook server, WebSocket control protocol                           |
| **Observability** | `src/observability/` | Log and metrics observers                                                 |

### 1.3 Deployment Models

1. **Standalone CLI** — Direct execution via `main.c`, no network exposure
2. **Gateway server** — HTTP server on configurable host/port (default `0.0.0.0:3000`), webhooks, WebSocket control
3. **Embedded** — Peripherals for Arduino, STM32, RPi with serial/GPIO access

### 1.4 Trust Boundaries (Summary)

Trust boundaries exist at:

- **Network perimeter** — External clients → Gateway HTTP/WebSocket
- **Agent boundary** — Gateway/Channels → Agent loop (`src/agent/`)
- **Tool boundary** — Agent → Tool execution
- **Execution boundary** — Tool execution → Operating system (fork/exec, filesystem)
- **External API boundary** — Agent → AI provider (external API over HTTPS)
- **Data boundary** — Agent → Memory store (SQLite, PostgreSQL, etc.)
- **Hardware boundary** — Agent → Peripherals (serial, probe-rs flash)

---

## 2. Assets (What We Protect)

| Asset                                | Location                                         | Sensitivity                                  |
| ------------------------------------ | ------------------------------------------------ | -------------------------------------------- |
| **API keys and OAuth tokens**        | `~/.human/auth.json`, `~/.human/config.json` | CRITICAL — Full provider account access      |
| **User conversation data**           | In-memory, SQLite/memory backends, session state | HIGH — PII, confidential context             |
| **Agent execution state and memory** | `src/memory/`, `src/agent/`                      | HIGH — Context poisoning, inference abuse    |
| **Configuration and policy data**    | `~/.human/config.json`, policy structs         | HIGH — Privilege escalation, scope expansion |
| **Audit logs**                       | `src/security/audit.c`, configured output        | MEDIUM — Tampering enables repudiation       |
| **Hardware peripheral access**       | `src/peripherals/` (Arduino, STM32, RPi)         | HIGH — Physical device control               |
| **Webhook HMAC secrets**             | Config, `HUMAN_WEBHOOK_HMAC_SECRET` env        | CRITICAL — Webhook forgery prevention        |
| **Pairing tokens**                   | In-memory hashes, `hu_pairing_guard_t`           | CRITICAL — Control UI access                 |

---

## 3. Threat Actors

### 3.1 External Attacker

- **Capability:** Network access to gateway (HTTP on `0.0.0.0` by default)
- **Objectives:** Inject forged webhooks, control agent via WebSocket, SSRF via tools
- **Attack paths:** Webhook endpoints, WebSocket control protocol, HTTP gateway

### 3.2 Malicious AI Model

- **Capability:** Prompt injection, tool call manipulation, context poisoning
- **Objectives:** Exfiltrate data, escalate privileges, bypass policy
- **Attack paths:** Tool calls from model output, system prompt leakage, memory injection

### 3.3 Local Attacker

- **Capability:** File system access, process inspection, shared host
- **Objectives:** Read plaintext credentials, modify config, tamper audit logs
- **Attack paths:** `~/.human/` files, config injection, workspace path manipulation

### 3.4 Supply Chain

- **Capability:** Compromised dependencies, build tampering
- **Objectives:** Backdoor binary, credential theft at build time
- **Attack paths:** libcurl, SQLite, CMake/build scripts; no SBOM today

### 3.5 Insider

- **Capability:** Misconfiguration, policy bypass, audit log access
- **Objectives:** Expand tool scope, disable security controls
- **Attack paths:** Config changes, `allowed_commands`, `allowed_paths`, policy flags

---

## 4. STRIDE Analysis

### 4.1 Gateway (`src/gateway/`)

| STRIDE                     | Threat                                                                     | Status         | Notes                                                                                                |
| -------------------------- | -------------------------------------------------------------------------- | -------------- | ---------------------------------------------------------------------------------------------------- |
| **Spoofing**               | WebSocket connections accepted without auth                                | **VULNERABLE** | `gw->config.auth_token` stored but not enforced on upgrade. `slot->authenticated` always false. C-01 |
| **Spoofing**               | Webhook endpoints accept unauthenticated requests when `hmac_secret` unset | **VULNERABLE** | HMAC check only runs when secret configured. C-02                                                    |
| **Spoofing**               | HMAC verification uses non-constant-time comparison                        | **VULNERABLE** | `strncmp(sig_header, hex, 64)` — timing side channel. H-01                                           |
| **Spoofing**               | No Origin validation on WebSocket upgrade                                  | **VULNERABLE** | Cross-site WebSocket from malicious site. H-06                                                       |
| **Tampering**              | Path traversal via URL-encoded segments                                    | **PARTIAL**    | Path validation may not decode `%2e%2e%2f`. M-05                                                     |
| **Tampering**              | TOCTOU in static file serving                                              | **PARTIAL**    | `open` then `fstat` — symlink race. M-06                                                             |
| **Repudiation**            | Webhook path and channel logged                                            | **LOW**        | L-03 — may leak operational data                                                                     |
| **Information Disclosure** | `Access-Control-Allow-Origin: *` on all responses                          | **LOW**        | L-05 — broad CORS                                                                                    |
| **Denial of Service**      | Rate limiting on pairing only                                              | **PARTIAL**    | General HTTP/WebSocket not rate-limited                                                              |
| **Elevation of Privilege** | Unauthenticated WebSocket → full control                                   | **VULNERABLE** | `config.set`, `config.apply`, `sessions.delete`, `exec.approval.resolve`                             |

### 4.2 Tools (`src/tools/`)

| STRIDE                     | Threat                                   | Status         | Notes                                                                         |
| -------------------------- | ---------------------------------------- | -------------- | ----------------------------------------------------------------------------- |
| **Spoofing**               | N/A (tools execute as agent)             | —              | —                                                                             |
| **Tampering**              | CRLF injection in `http_request` headers | **VULNERABLE** | `parse_headers()` does not sanitize `\r\n`. C-04                              |
| **Tampering**              | Path traversal in git tool               | **VULNERABLE** | `paths`, `files`, `branch` not validated with `hu_tool_validate_path()`. C-05 |
| **Tampering**              | Snapshot export/import path traversal    | **VULNERABLE** | No `..` or symlink checks. H-09                                               |
| **Information Disclosure** | Git tool arbitrary file read             | **VULNERABLE** | `{"operation": "diff", "files": "../../../etc/passwd"}`. C-05                 |
| **Denial of Service**      | Large payloads, unbounded allocations    | **PARTIAL**    | Some limits; integer overflow in arena. H-13                                  |
| **Elevation of Privilege** | Spawn tool executes arbitrary commands   | **VULNERABLE** | No allowlist. H-07                                                            |
| **Elevation of Privilege** | Shell tool passes to `/bin/sh -c`        | **LOW**        | L-07 — command injection if policy bypassed                                   |
| **Elevation of Privilege** | `browser_open` buffer over-read          | **VULNERABLE** | `len < 10` before suffix check. H-08                                          |

### 4.3 Providers (`src/providers/`)

| STRIDE                     | Threat                                       | Status         | Notes                                      |
| -------------------------- | -------------------------------------------- | -------------- | ------------------------------------------ |
| **Spoofing**               | N/A (provider is trusted endpoint)           | —              | —                                          |
| **Tampering**              | Malicious model output → tool abuse          | **INHERENT**   | Policy and tool validation are mitigations |
| **Information Disclosure** | API key masking exposes leading chars        | **VULNERABLE** | H-05 — reduces brute-force space           |
| **Information Disclosure** | LLM error messages in logs without scrubbing | **VULNERABLE** | H-10 — API keys in errors                  |

### 4.4 Channels (`src/channels/`)

| STRIDE                     | Threat                          | Status          | Notes                               |
| -------------------------- | ------------------------------- | --------------- | ----------------------------------- |
| **Spoofing**               | Channel auth via OAuth/API keys | **IMPLEMENTED** | Per-channel                         |
| **Tampering**              | Incoming message injection      | **DEPENDS**     | Webhook auth (C-02) is prerequisite |
| **Information Disclosure** | Token in config/logs            | **SEE C-03**    | Plaintext storage                   |

### 4.5 Memory (`src/memory/`)

| STRIDE                     | Threat                                 | Status         | Notes                                                                    |
| -------------------------- | -------------------------------------- | -------------- | ------------------------------------------------------------------------ |
| **Spoofing**               | N/A                                    | —              | —                                                                        |
| **Tampering**              | PostgreSQL identifier injection        | **VULNERABLE** | `schema`, `table_name` interpolated without `PQescapeIdentifier()`. C-06 |
| **Tampering**              | FTS5 query injection                   | **VULNERABLE** | User words in MATCH without escaping `"`. H-11                           |
| **Information Disclosure** | SQLite not `PRAGMA secure_delete=ON`   | **PARTIAL**    | M-10                                                                     |
| **Information Disclosure** | Workspace path from config unvalidated | **VULNERABLE** | H-12 — `../../../etc` possible                                           |

### 4.6 Security (`src/security/`)

| STRIDE                     | Threat                                                          | Status         | Notes                                                                                                |
| -------------------------- | --------------------------------------------------------------- | -------------- | ---------------------------------------------------------------------------------------------------- |
| **Spoofing**               | Pairing code 6–8 digits, limited entropy                        | **VULNERABLE** | H-04 — brute-force over lockout cycles                                                               |
| **Tampering**              | Audit log tampering                                             | **PARTIAL**    | No HMAC chain/signing. L-02 — full commands in audit                                                 |
| **Tampering**              | Sensitive material not securely cleared                         | **VULNERABLE** | `memset()` may be optimized away. H-02                                                               |
| **Information Disclosure** | Secret key file created without `0600`                          | **VULNERABLE** | M-07                                                                                                 |
| **Elevation of Privilege** | Path access control: policy NULL bypass                         | **CONTEXT**    | `file_*` tools: `if (c->policy && !hu_security_path_allowed(...))` — when policy NULL, check skipped |
| **Elevation of Privilege** | `allowed_paths` NULL → `hu_security_path_allowed` returns false | **FIXED**      | `security.c` line 8: `return false` when no allowed_paths                                            |
| **Elevation of Privilege** | Firecracker socket path uses PID 0                              | **VULNERABLE** | All instances share `/tmp/hu_fc_0.sock`. C-07                                                        |

### 4.7 Runtime (`src/runtime/`)

| STRIDE                     | Threat                  | Status       | Notes                                                                |
| -------------------------- | ----------------------- | ------------ | -------------------------------------------------------------------- |
| **Spoofing**               | N/A                     | —            | —                                                                    |
| **Tampering**              | Sandbox escape          | **PLATFORM** | Landlock, seccomp, Firecracker, bwrap — implementation bugs possible |
| **Elevation of Privilege** | Firecracker socket race | **SEE C-07** | —                                                                    |

### 4.8 Peripherals (`src/peripherals/`)

| STRIDE                     | Threat                             | Status       | Notes                                   |
| -------------------------- | ---------------------------------- | ------------ | --------------------------------------- |
| **Spoofing**               | Serial/GPIO impersonation          | **PHYSICAL** | Assumes trusted local hardware          |
| **Tampering**              | Arbitrary flash write via probe-rs | **CONTEXT**  | STM32 — full device reprogramming       |
| **Elevation of Privilege** | Peripheral access without policy   | **PARTIAL**  | Policy integration varies by deployment |

### 4.9 Core (`src/core/`)

| STRIDE                     | Threat                                           | Status         | Notes                         |
| -------------------------- | ------------------------------------------------ | -------------- | ----------------------------- |
| **Tampering**              | NULL pointer dereference in `hu_json_string_new` | **VULNERABLE** | C-08                          |
| **Tampering**              | Buffer overread in `hu_json_append_key`          | **VULNERABLE** | C-09                          |
| **Tampering**              | Integer overflow in arena                        | **VULNERABLE** | H-13                          |
| **Tampering**              | Integer overflow in JSON buffer growth           | **PARTIAL**    | M-03                          |
| **Information Disclosure** | Format string risk in `hu_sprintf`               | **PARTIAL**    | M-04 — `fmt` must be constant |

---

## 5. Attack Surface Map

### 5.1 Network

| Surface           | Protocol                                      | Auth                             | Risk                     |
| ----------------- | --------------------------------------------- | -------------------------------- | ------------------------ |
| HTTP gateway      | HTTP/1.1                                      | None (optional HMAC on webhooks) | High                     |
| Webhook paths     | POST /webhook, /telegram, /slack/events, etc. | HMAC when configured             | Critical if unconfigured |
| WebSocket control | WS upgrade                                    | None (auth_token not enforced)   | Critical                 |
| SSE client        | Outbound to providers                         | Provider API keys                | Medium                   |
| WebSocket client  | Outbound                                      | Provider API keys                | Medium                   |
| libcurl HTTP      | HTTPS (enforced for tools)                    | TLS, `CURLOPT_SSL_VERIFYPEER`    | Low                      |

### 5.2 Local

| Surface          | Path                                  | Risk                               |
| ---------------- | ------------------------------------- | ---------------------------------- |
| CLI              | `main.c`, argv                        | Medium — config loading, workspace |
| Config files     | `~/.human/config.json`, `auth.json` | High — plaintext, path injection   |
| SQLite databases | Workspace, memory backends            | Medium — FTS5 injection            |
| Audit logs       | Configured output                     | Medium — tampering, secrets        |
| Secrets store    | `src/security/secrets.c`              | High — key file permissions        |

### 5.3 AI

| Surface           | Vector                 | Risk                              |
| ----------------- | ---------------------- | --------------------------------- |
| Tool calls        | JSON from model output | High — spawn, shell, http_request |
| System prompt     | Leakage via model      | Medium                            |
| Context poisoning | Memory injection       | High                              |
| Prompt injection  | User/model boundary    | Inherent — policy limits scope    |

### 5.4 Hardware

| Surface      | Protocol        | Risk   |
| ------------ | --------------- | ------ |
| Arduino      | Serial JSON     | Medium |
| STM32/Nucleo | probe-rs, flash | High   |
| Raspberry Pi | GPIO, serial    | Medium |

---

## 6. Trust Boundaries

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        UNTRUSTED EXTERNAL                                   │
│  Network clients, webhook senders, malicious websites (Origin)              │
└───────────────────────────────────┬─────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  TRUST BOUNDARY 1: GATEWAY                                                   │
│  src/gateway/gateway.c, ws_server.c, control_protocol.c                      │
│  Mitigations: HMAC (when configured), pairing (optional)                      │
│  Gaps: WebSocket auth not enforced, webhook HMAC optional                   │
└───────────────────────────────────┬─────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  TRUST BOUNDARY 2: AGENT LOOP                                                │
│  src/agent/agent.c, context, planner, dispatcher                             │
│  Mitigations: Policy engine, autonomy level, rate limiting                   │
└───────────────────────────────────┬─────────────────────────────────────────┘
                                    │
                    ┌───────────────┼───────────────┐
                    ▼               ▼               ▼
┌───────────────────────┐ ┌─────────────────┐ ┌─────────────────────────────┐
│ TB3: TOOL EXECUTION   │ │ TB4: AI PROVIDER │ │ TB5: MEMORY STORE            │
│ src/tools/*           │ │ src/providers/*  │ │ src/memory/*                 │
│ Mitigations:          │ │ Mitigations:     │ │ Mitigations:                │
│ - hu_tool_validate_   │ │ - HTTPS only     │ │ - Parameterized queries      │
│   path (some tools)   │ │ - API key in     │ │ Gaps: pg identifier inj,     │
│ - Policy command      │ │   config         │ │ FTS5, snapshot paths         │
│   allowlist           │ │ Gaps: Plaintext  │ │                              │
│ Gaps: spawn, git,     │ │ credentials      │ │                              │
│ http_request CRLF     │ │                  │ │                              │
└───────────┬───────────┘ └────────┬────────┘ └─────────────────────────────┘
            │                      │
            ▼                      ▼
┌───────────────────────┐ ┌─────────────────┐
│ TB6: OPERATING SYSTEM │ │ TB7: EXTERNAL    │
│ fork/exec, filesystem │ │ API (OpenAI,     │
│ Mitigations:          │ │ Anthropic, etc.) │
│ - Landlock, seccomp   │ │                  │
│ - Path allowlists     │ └─────────────────┘
│ - Sandbox backends    │
│ Gaps: Policy NULL     │
│ bypass in some tools │
└───────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│  TB8: HARDWARE PERIPHERALS                                                   │
│  src/peripherals/arduino.c, stm32.c, rpi.c                                   │
│  Assumes local physical access; probe-rs/flash are high privilege           │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 7. NIST SP 800-53 Control Mapping

### AC — Access Control

| Control                 | Implementation                                           | Status                                     |
| ----------------------- | -------------------------------------------------------- | ------------------------------------------ |
| AC-2 Account Management | Pairing guard, token hashes                              | Partial — WebSocket not enforced           |
| AC-3 Access Enforcement | `hu_security_path_allowed`, `hu_policy_validate_command` | Partial — policy NULL bypass in file tools |
| AC-6 Least Privilege    | Sandbox backends (Landlock, seccomp, bwrap, Firecracker) | Implemented                                |
| AC-17 Remote Access     | Gateway, WebSocket                                       | **Gap** — auth optional                    |

### AU — Audit and Accountability

| Control                              | Implementation                                                                  | Status      |
| ------------------------------------ | ------------------------------------------------------------------------------- | ----------- |
| AU-2 Auditable Events                | `src/security/audit.c` — command_execution, file_access, auth, policy_violation | Implemented |
| AU-3 Content of Audit Records        | Timestamp, event_id, actor, action, result                                      | Implemented |
| AU-9 Protection of Audit Information | **Gap** — no HMAC chain, no append-only                                         | Partial     |
| AU-11 Audit Record Retention         | Configurable; file-based                                                        | Implemented |

### IA — Identification and Authentication

| Control                                | Implementation                     | Status                                 |
| -------------------------------------- | ---------------------------------- | -------------------------------------- |
| IA-2 Identification and Authentication | Pairing guard, HMAC webhooks       | **Gap** — WebSocket, webhooks optional |
| IA-5 Authenticator Management          | Token generation, secure zero      | Partial — `memset` clearing (H-02)     |
| IA-6 Authentication Feedback           | Pairing lockout (5 attempts, 600s) | Implemented                            |

### SC — System and Communications Protection

| Control                           | Implementation                                           | Status          |
| --------------------------------- | -------------------------------------------------------- | --------------- |
| SC-8 Transmission Confidentiality | HTTPS for tools (`hu_validate_url`), libcurl TLS         | Implemented     |
| SC-13 Cryptographic Protection    | ChaCha20+HMAC in `hu_secret_store`, HMAC-SHA256 webhooks | Implemented     |
| SC-28 Protection at Rest          | **Gap** — credentials plaintext (C-03)                   | Not implemented |
| SC-39 Process Isolation           | Landlock, seccomp, Firecracker, bwrap, WASI              | Implemented     |

### SI — System and Information Integrity

| Control                            | Implementation                                                    | Status                                        |
| ---------------------------------- | ----------------------------------------------------------------- | --------------------------------------------- |
| SI-10 Information Input Validation | `hu_tool_validate_path`, URL validation                           | Partial — gaps in git, http_request, snapshot |
| SI-11 Error Handling               | Error codes, no sensitive data in errors                          | Partial — H-10 LLM errors                     |
| SI-16 Memory Protection            | `explicit_bzero` pattern in pairing; **gap** — `memset` elsewhere | Partial                                       |

### CM — Configuration Management

| Control                       | Implementation                    | Status                          |
| ----------------------------- | --------------------------------- | ------------------------------- |
| CM-6 Configuration Settings   | Config schema, validation         | Partial — workspace path (H-12) |
| CM-7 Least Functionality      | Tool allowlist, command allowlist | Implemented                     |
| CM-11 User-Installed Software | N/A — minimal deps                | N/A                             |

### MP — Media Protection

| Control                 | Implementation                                         | Status  |
| ----------------------- | ------------------------------------------------------ | ------- |
| MP-6 Media Sanitization | **Gap** — no `secure_delete` for SQLite (M-10)         | Partial |
| MP-6 File permissions   | **Gap** — auth.json, config.json not 0600 (C-03, M-07) | Partial |

### PE — Physical and Environmental Protection

| Control                             | Implementation                          | Status  |
| ----------------------------------- | --------------------------------------- | ------- |
| PE-2 Physical Access Authorizations | Peripheral access assumes trusted local | Assumed |

---

## 8. Residual Risks

### 8.1 Areas Where Mitigations Are Not Complete

1. **Authentication optional on all network surfaces** — WebSocket and webhooks accept unauthenticated requests when not explicitly configured. Defense/mandatory-auth deployments require configuration discipline.

2. **Plaintext credential storage** — API keys and OAuth tokens in `auth.json` and `config.json`. `hu_secret_store` exists but is not used for persistence.

3. **Memory safety in core** — Integer overflow (arena, JSON), NULL dereference, buffer overread. AddressSanitizer in CI helps; formal verification or safer allocator not in place.

4. **Injection vectors** — CRLF in HTTP headers, SQL identifiers in PostgreSQL, FTS5 escaping, path traversal in git/snapshot/config.

5. **Audit log transparency** — No cryptographic integrity; logs can be modified. Full command strings may include secrets (L-02).

6. **Timing side channels** — HMAC comparison not constant-time (H-01). Pairing uses `hu_pairing_guard_constant_time_eq` — pattern exists but not applied to webhooks.

### 8.2 Accepted Risks (with Justification)

| Risk                            | Justification                                                                    |
| ------------------------------- | -------------------------------------------------------------------------------- |
| Malicious AI model abuse        | Inherent to LLM agents. Mitigated by policy, command allowlist, autonomy levels. |
| Peripheral physical access      | Assumes trusted deployment; physical security is operator responsibility.        |
| ChaCha20 for secrets (non-FIPS) | Binary size constraint; FIPS path would require AES-256-GCM and evaluation.      |
| CORS `*`                        | Convenience for control UI; acceptable when gateway is behind VPN or localhost.  |

### 8.3 Recommendations for Further Hardening

1. **Phase 1 (Immediate):** Enforce WebSocket auth; require HMAC for webhooks when gateway is network-exposed; encrypt credentials at rest; CRLF sanitization in http_request; git tool path validation.

2. **Phase 2 (Short-term):** Constant-time HMAC comparison; default-deny path access when policy unset; Firecracker socket path fix; JSON NULL/bounds checks; secure memory clearing.

3. **Phase 3 (Medium-term):** PostgreSQL identifier validation; FTS5 escaping; spawn tool allowlist; log scrubbing; config path canonicalization.

4. **Phase 4 (Defense certification):** Formal threat model maintenance; FIPS 140-2 crypto evaluation; signed/tamper-proof audit logging; penetration testing; SBOM generation.

---

## 9. Security Controls Summary Table

| Control                               | NIST ID     | Status              | Notes                                                                   |
| ------------------------------------- | ----------- | ------------------- | ----------------------------------------------------------------------- |
| WebSocket authentication              | IA-2        | **Not implemented** | auth_token stored but not checked on upgrade (C-01)                     |
| Webhook HMAC verification             | IA-2        | Partial             | Implemented when secret configured; optional by default (C-02)          |
| Webhook HMAC constant-time comparison | IA-6        | **Not implemented** | strncmp used (H-01)                                                     |
| WebSocket Origin validation           | AC-17       | **Not implemented** | H-06                                                                    |
| Credential encryption at rest         | SC-28       | **Not implemented** | Plaintext in auth.json, config.json (C-03)                              |
| Path access control                   | AC-3        | Implemented         | hu_security_path_allowed; returns false when no allowed_paths           |
| Path access when policy NULL          | AC-3        | **Bypass**          | file\_\* tools skip check when c->policy is NULL                        |
| Command allowlist                     | AC-6        | Implemented         | hu_policy_validate_command, allowed_commands                            |
| Tool path validation                  | SI-10       | Partial             | hu_tool_validate_path used by file_read, file_write; git, snapshot gaps |
| HTTPS enforcement for tools           | SC-8        | Implemented         | hu_validate_url, net_security.c — HTTP rejected except localhost        |
| libcurl TLS verification              | SC-8        | Implemented         | CURLOPT_SSL_VERIFYPEER, CURLOPT_SSL_VERIFYHOST in http.c                |
| Pairing guard                         | IA-2        | Implemented         | 6–8 digit code, lockout, constant-time token comparison                 |
| Audit logging                         | AU-2, AU-3  | Implemented         | command_execution, file_access, auth, policy_violation                  |
| Audit integrity                       | AU-9        | **Not implemented** | No HMAC chain or signing                                                |
| Sandbox backends                      | AC-6, SC-39 | Implemented         | Landlock, seccomp, bwrap, Firecracker, WASI                             |
| Secret store (hu_secret_store)        | SC-13       | Implemented         | ChaCha20+HMAC; not used for auth.json persistence                       |
| Secure memory clearing                | IA-5        | Partial             | pairing.c uses volatile/asm; secrets.c uses memset (H-02)               |
| Private IP blocking                   | SC-8        | Implemented         | hu_is_private_ip, hu_validate_url for tools                             |
| SQLite parameterized queries          | SI-10       | Implemented         | Prevents most SQL injection                                             |
| PostgreSQL identifier validation      | SI-10       | **Not implemented** | C-06                                                                    |
| FTS5 query escaping                   | SI-10       | **Not implemented** | H-11                                                                    |
| HTTP header CRLF sanitization         | SI-10       | **Not implemented** | C-04                                                                    |
| Firecracker socket path               | SC-39       | **Vulnerable**      | PID 0 shared path (C-07)                                                |

---

## 10. Revision History

| Date       | Version | Author | Changes                                            |
| ---------- | ------- | ------ | -------------------------------------------------- |
| 2026-03-02 | 1.0     | —      | Initial threat model per NIST SP 800-53 and STRIDE |

---

_This document is intended for defense industry security review and compliance alignment. It reflects the state of the human codebase as of 2026-03-02 and should be updated when significant architectural or security changes occur._
