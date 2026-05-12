---
title: "Sprint 3 — Track E Phase 2 + Stability"
created: 2026-05-12
status: ready
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

- [ ] AC-A.1: Webhook HMAC comparison in `src/gateway/` uses constant-time compare
- [ ] AC-A.2: Existing `hu_pairing_guard_constant_time_eq` extracted to shared utility or duplicated with test
- [ ] AC-A.3: Unit test verifies constant-time path is exercised

---

## Story B — Default-deny path access when policy unset

**Source:** Threat model §8.3 Phase 2, NIST AC-3.

When `hu_security_path_allowed` receives a NULL policy, some tools treat it as
"allow all" rather than "deny all". Change to default-deny.

- [ ] AC-B.1: `hu_security_path_allowed(NULL, ...)` returns false (deny)
- [ ] AC-B.2: All callers of `hu_security_path_allowed` handle the deny case
- [ ] AC-B.3: Existing tests updated; new test for NULL-policy → deny

---

## Story C — Secure memory clearing (IA-5)

**Source:** Threat model §8.3 Phase 2, NIST SI-16.

Replace `memset` with `explicit_bzero` (or `SecureZeroMemory` on Windows) when
clearing sensitive buffers (API keys, tokens, pairing secrets).

- [ ] AC-C.1: Audit for `memset(..., 0, ...)` on security-sensitive buffers
- [ ] AC-C.2: Replace with `explicit_bzero` wrapper (`hu_secure_zero`)
- [ ] AC-C.3: Wrapper compiles on macOS, Linux, and Windows (ifdef)

---

## Story D — JSON NULL/bounds hardening

**Source:** Threat model §8.1 item 3, §8.3 Phase 2.

Harden JSON parser and accessor functions against NULL dereference and
integer overflow in arena allocation.

- [ ] AC-D.1: `hu_json_object_get` / `hu_json_array_get` return NULL on NULL input
- [ ] AC-D.2: Arena allocator rejects allocations where `size + alignment` would overflow
- [ ] AC-D.3: Fuzz harness `fuzz_json_parse` run for 5 min with no new crashes

---

## Story E — Test teardown SIGABRT investigation + fix

**Source:** Pre-existing exit code 134 observed across all branches.

The test binary exits with SIGABRT after all tests pass (exit code 134 =
128 + 6). This is a teardown issue, not a test failure. Investigate and fix.

- [ ] AC-E.1: Root cause identified (double-free, atexit handler, assert, etc.)
- [ ] AC-E.2: Fix applied; `./build/human_tests` exits 0 when all tests pass
- [ ] AC-E.3: CI green with exit code 0

---

## Non-goals (deferred to Sprint 4+)

- **Phase 3 items:** PostgreSQL identifier validation, spawn tool allowlist, log
  scrubbing, config path canonicalization — lower priority, deferred.
- **Phase 4 items:** FIPS crypto, signed audit logs, penetration testing — requires
  external resources.
- **Credential encryption at rest (C-03):** Phase 1 item, partially done via
  `hu_secret_store` but persistence migration is a large scope item.
- **GGUF LoRA end-to-end:** Blocked on model availability and `HU_ENABLE_LLAMACPP`.
