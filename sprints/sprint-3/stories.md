---
title: "Sprint 3 — Track E Phase 2 + Stability"
created: 2026-05-12
status: done
sprint: 3
branch: sprint-3-security-phase2
---

# Sprint 3 — Track E Phase 2 + Stability

## Sprint goal

Address threat model Phase 2 recommendations (constant-time HMAC, default-deny
path policy, secure memory clearing, JSON safety) and resolve the pre-existing
test teardown SIGABRT (exit code 134).

---

## Story A — Constant-time HMAC comparison (H-01)

**Source:** Threat model §8.3 Phase 2, §8.1 item 6.

Webhook HMAC verification uses standard `memcmp`, which leaks timing
information. The pairing guard already has `hu_pairing_guard_constant_time_eq` —
apply the same pattern to webhook signature verification.

- [x] AC-A.1: Webhook HMAC comparison in `src/gateway/gateway.c` uses `hu_constant_time_eq` from shared header
- [x] AC-A.2: `hu_constant_time_eq` extracted to `include/human/security/secure_mem.h` (inline, shared across pairing/secrets/vault/gateway)
- [x] AC-A.3: Computed HMAC digest and hex buffer cleared from stack via `hu_secure_zero` after comparison

---

## Story B — Default-deny path access when policy unset

**Source:** Threat model §8.3 Phase 2, NIST AC-3.

When `hu_security_path_allowed` receives a NULL policy, some tools treat it as
"allow all" rather than "deny all". Change to default-deny.

- [x] AC-B.1: `hu_security_path_allowed(NULL, ...)` already returns false (line 6-7 of security.c)
- [x] AC-B.2: Pre-existing — all callers handle the deny case
- [x] AC-B.3: Pre-existing — test coverage in `test_security.c` and `test_security_extended.c`

---

## Story C — Secure memory clearing (IA-5)

**Source:** Threat model §8.3 Phase 2, NIST SI-16.

Replace `memset` with `explicit_bzero` (or `SecureZeroMemory` on Windows) when
clearing sensitive buffers (API keys, tokens, pairing secrets).

- [x] AC-C.1: Audited — `memset` on struct init (non-sensitive) left as-is; sensitive buffers already use `hu_secure_zero`
- [x] AC-C.2: `hu_secure_zero` extracted to shared `include/human/security/secure_mem.h`; triplicated static copies removed from pairing.c, secrets.c, vault.c
- [x] AC-C.3: Wrapper handles `__STDC_LIB_EXT1__` (memset_s), GCC/Clang (asm barrier), and fallback (volatile loop)

---

## Story D — JSON NULL/bounds hardening

**Source:** Threat model §8.1 item 3, §8.3 Phase 2.

Harden JSON parser and accessor functions against NULL dereference and
integer overflow in arena allocation.

- [x] AC-D.1: `hu_json_object_get` already returns NULL on NULL input (json.c:639)
- [x] AC-D.2: Arena allocator already rejects `size > SIZE_MAX - 7` (arena.c:40-41)
- [ ] AC-D.3: Fuzz harness `fuzz_json_parse` run for 5 min with no new crashes (deferred to CI)

---

## Story E — Test teardown SIGABRT investigation + fix

**Source:** Pre-existing exit code 134 observed across all branches.

The test binary exits with SIGABRT after all tests pass (exit code 134 =
128 + 6). This is a teardown issue, not a test failure. Investigate and fix.

- [x] AC-E.1: Root cause: 5 test files used stdlib `assert()` instead of `HU_ASSERT` — `abort()` → SIGABRT
- [x] AC-E.2: Forward declarations fixed (`int` → `void`); 9851/9851 pass, exit code 0
- [x] AC-E.3: All 51 previously-invisible tests now tracked by `HU_TEST_REPORT()`

---

## Non-goals (deferred to Sprint 4+)

- **Phase 3 items:** PostgreSQL identifier validation, spawn tool allowlist, log
  scrubbing, config path canonicalization — lower priority, deferred.
- **Phase 4 items:** FIPS crypto, signed audit logs, penetration testing — requires
  external resources.
- **Credential encryption at rest (C-03):** Phase 1 item, partially done via
  `hu_secret_store` but persistence migration is a large scope item.
- **GGUF LoRA end-to-end:** Blocked on model availability and `HU_ENABLE_LLAMACPP`.
