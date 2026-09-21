# src/security/ — Security Subsystem (High Risk)

The security module enforces access control, sandboxing, pairing, secrets management, and adversarial defense. **Every change here has high blast radius.**

## Architecture

```
security.c           Top-level security initialization and coordination
policy.c             Security policy enforcement (hu_security_policy_t)
policy_engine.c      Advanced policy evaluation engine
```

## Access Control

```
pairing.c            Device pairing guard (code-based authentication)
vault.c              Credential vault (encrypted at rest)
secrets.c            Secret store (AEAD encryption, hex encoding)
audit.c              Security audit logging
replay.c             Replay attack detection
detect.c             Adversarial input detection
adversarial.c        Adversarial defense strategies
moderation.c         Content moderation enforcement
cot_audit.c          Chain-of-thought audit trail
```

## Sandbox Backends

```
sandbox.c            Sandbox abstraction and dispatch
landlock.c           Linux Landlock LSM (filesystem restriction)
landlock_seccomp.c   Landlock + seccomp combined
seccomp.c            Linux seccomp-bpf (syscall filtering)
bubblewrap.c         Bubblewrap (bwrap) container sandbox
firejail.c           Firejail application sandbox
docker.c             Docker-based isolation
firecracker.c        Firecracker microVM sandbox
seatbelt.c           macOS Seatbelt (sandbox-exec)
appcontainer.c       Windows AppContainer sandbox
wasi_sandbox.c       WASI sandboxing for WebAssembly
noop_sandbox.c       No-op sandbox (for testing)
```

## Key Types

```c
hu_security_policy_t     — autonomy level, workspace dir, allowed commands/paths, rate limits, sandbox
hu_autonomy_level_t      — LOCKED, SUPERVISED, ASSISTED, AUTONOMOUS
hu_command_risk_level_t  — LOW, MEDIUM, HIGH
hu_pairing_guard_t       — device pairing (code generation, attempt validation, lockout)
hu_secret_store_t        — AEAD-encrypted secret storage
hu_rate_tracker_t        — sliding window rate limiter
```

## Key Functions

- `hu_security_path_allowed` — check if a file path is within allowed directories
- `hu_policy_validate_command` — validate shell command against policy
- `hu_policy_command_risk_level` / `hu_tool_risk_level` — risk classification
- `hu_pairing_guard_attempt_pair` — validate pairing code (constant-time comparison)
- `hu_secret_store_encrypt` / `hu_secret_store_decrypt` — AEAD encrypt/decrypt

## Rules (Mandatory)

- **Deny-by-default** for all access boundaries
- **Never** log secrets, raw tokens, or sensitive payloads
- **Never** silently broaden permissions or capabilities
- **HTTPS-only** for all outbound URLs
- **Constant-time comparison** for all secret/token equality checks
- **Rate limiting** — always check and enforce
- Include **threat/risk notes** in every commit message touching this module
- Pairing: never weaken lockout thresholds or bypass code validation
- AEAD: use stack buffers for decrypt, then `dupe()`
- Secret store: always check encrypt/decrypt return values

## Required Tests

- Boundary/failure-mode test for every change
- Test lockout, invalid input, privilege escalation paths
- Test sandbox enforcement (allowed vs. denied operations)
- Use `HU_IS_TEST` for crypto or network operations
- No real secrets in test fixtures — use `"test-key"`, `"example.com"`
