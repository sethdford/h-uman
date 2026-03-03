---
title: Security Comparison — SeaClaw vs OpenClaw
description: Security posture comparison between SeaClaw and OpenClaw runtimes
updated: 2026-03-02
---

# Security Comparison: SeaClaw vs OpenClaw

## 1. Executive Summary

**SeaClaw** is a zero-dependency, single-binary AI assistant runtime with encrypted secrets storage, multiple sandbox backends, and secure-by-default configuration. **OpenClaw** is a Node.js-based implementation with 1,200+ npm dependencies, plain-text configuration for API keys, and no built-in sandboxing for tool execution.

## 2. Supply Chain Risk

### SeaClaw

- **0 third-party dependencies** for the minimal core build (libc only). Optional SQLite and libcurl are system libraries.
- Single static binary. No package manager supply chain at runtime.
- SBOM (Software Bill of Materials) generation built-in: `cmake --build . --target sbom` produces a CycloneDX SBOM.
- No transitive dependency graph to audit.

### OpenClaw

- **1,200+ npm packages** in the dependency tree. Each package is a potential attack vector.
- Vulnerable to typosquatting, dependency confusion, and malicious package injection.
- Recent npm supply chain incidents:
  - **event-stream (2018):** Malicious dependency `flatmap-stream` injected into the popular `event-stream` package. ~8 million downloads compromised; targeted Bitcoin wallet credentials in Copay.
  - **ua-parser-js (2021):** Package hijacked; malicious versions deployed cryptominers (XMRig) and credential-stealing trojans. ~7–8 million weekly downloads.
  - **colors.js (2022):** Maintainer sabotage; infinite loop introduced in versions 1.4.1+ causing DoS. 3.3+ billion lifetime downloads affected.

## 3. Secrets Management

### SeaClaw

- ChaCha20-Poly1305 encrypted secrets store (`src/security/secrets.c`). API keys never stored in plain text.
- AEAD-style encryption (ChaCha20 + HMAC-SHA256) with hardware-derived or file-based key. Non-FIPS builds use in-tree ChaCha20; FIPS builds use AES-256-GCM via OpenSSL.
- Secrets decrypted only in memory at use time. Secure zeroing of sensitive buffers.

### OpenClaw

- API keys typically stored in `.env` files or `config.json`. Plain text at rest.
- No encryption for credentials. Relies on filesystem permissions and environment isolation.

## 4. Sandboxing

### SeaClaw

- **7 sandbox backends** for tool execution:
  - **Landlock** (Linux): Kernel LSM filesystem ACLs.
  - **Landlock+seccomp** (Linux): Combined FS ACLs + syscall filtering.
  - **Seatbelt** (macOS): Sandbox profiles via `sandbox-exec`.
  - **Firejail** (Linux): User-space sandbox (`--private=workspace --net=none`).
  - **Bubblewrap** (Linux): User namespaces via `bwrap`.
  - **Docker** (cross-platform): Container isolation.
  - **Firecracker** (Linux): MicroVM isolation via KVM.
- Auto-detection: prefers Landlock → Firejail → Bubblewrap → Docker → none. Secure by default.

### OpenClaw

- No built-in sandboxing. Relies on Docker for isolation when explicitly configured.

## 5. Network Security

### SeaClaw

- HTTPS-only enforcement. HTTP URLs rejected at the tool layer.
- TLS certificate validation for outbound connections.
- Rate limiting and URL allowlisting for tools. Configurable policies.

### OpenClaw

- No enforced HTTPS for outbound tool calls.
- No built-in rate limiting on tool execution.

## 6. Audit and Observability

### SeaClaw

- Built-in audit logging (`src/security/audit.c`). Event types: command execution, file access, config change, auth success/failure, policy violation, security events.
- Observer pattern for instrumentation. Severity levels; configurable minimum severity.
- No sensitive data (secrets, tokens) in audit logs. HMAC integrity for audit records.

### OpenClaw

- Basic application logging. No structured audit trail for security events.

## 7. Skill/Plugin Security

### SeaClaw

- **SkillForge**: Skill discovery from local directories; install from URL. Skills execute within the sandbox when invoked. Explicit enable/disable. No npm-like package supply chain for skills.
- Skills run in the same sandbox as tools (Landlock, Firejail, etc.).

### OpenClaw

- **ClawHub**: Community-curated skills with human moderation. No automated security scanning of skill code. Skills run in the same process as the main application.

## 8. Binary Integrity

### SeaClaw

- Single static binary. SHA-256 checksums for releases.
- Reproducible builds supported. SBOM documents components.

### OpenClaw

- `npm install` pulls packages at install time. No binary integrity verification of dependencies. Lock files reduce but do not eliminate supply chain risk.

## 9. Attack Surface Comparison

| Aspect          | SeaClaw                            | OpenClaw                 |
| --------------- | ---------------------------------- | ------------------------ |
| Dependencies    | 0 (core); 2–3 optional system libs | 1,200+ npm packages      |
| Secrets at rest | Encrypted (ChaCha20/AES-GCM)       | Plain text               |
| Tool sandboxing | 7 backends, default-on             | Optional Docker          |
| Network         | HTTPS-only, URL allowlist          | No enforced HTTPS        |
| Audit trail     | Structured, severity levels        | Basic logging            |
| Supply chain    | SBOM, no package manager           | npm registry, lock files |
| Binary size     | ~280–380 KB                        | ~75 MB+ (node_modules)   |

## 10. Recommendation

For security-conscious deployments, SeaClaw provides significantly stronger defaults with zero additional configuration: encrypted secrets, sandboxed tool execution, HTTPS enforcement, and audit logging out of the box. OpenClaw offers flexibility and a large ecosystem at the cost of a wider attack surface and manual hardening.
