---
title: seaclaw STIG Hardening Checklist
description: DoD STIG hardening checklist for seaclaw deployments
updated: 2026-03-02
---

# seaclaw STIG Hardening Checklist

Checklist for hardening seaclaw deployments per DoD STIG requirements.

## Build Configuration

- [ ] Build with `SC_ENABLE_FIPS_CRYPTO=ON` for FIPS 140-2 compliant crypto
- [ ] Build with `CMAKE_BUILD_TYPE=MinSizeRel` and `SC_ENABLE_LTO=ON`
- [ ] Verify binary with `checksec` shows: Full RELRO, Stack Canary, NX, PIE
- [ ] Generate SBOM: `cmake --build build --target sbom`

## Authentication

- [ ] Set `auth_token` in gateway config (non-empty, min 32 chars)
- [ ] Set `webhook_hmac_secret` for all webhook endpoints
- [ ] Enable `require_pairing` for device pairing
- [ ] Verify pairing code entropy (8 digits, 600s lockout)

## Access Control

- [ ] Configure `allowed_paths` in security policy (default: deny-all)
- [ ] Configure `allowed_commands` for shell/spawn tools
- [ ] Restrict `allowed_tools` to minimum required set
- [ ] Disable `shell` and `spawn` tools if not required
- [ ] Verify CORS is restricted to specific origins

## Cryptography

- [ ] API keys encrypted at rest (sc_secret_store)
- [ ] Auth credentials encrypted at rest
- [ ] TLS 1.2+ enforced for all outbound connections
- [ ] Verify `CURLOPT_SSL_VERIFYPEER` and `CURLOPT_SSL_VERIFYHOST` are set

## Logging and Audit

- [ ] Enable audit logging
- [ ] Verify HMAC chain integrity on audit logs
- [ ] Disable `log_llm_io` and `log_message_payloads` in production
- [ ] Verify no API keys appear in logs
- [ ] Set log file permissions to 0600

## File System

- [ ] Config directory permissions: 0700
- [ ] Config file permissions: 0600
- [ ] Secret key file permissions: 0600
- [ ] SQLite database permissions: 0600
- [ ] Verify `PRAGMA secure_delete=ON` for all SQLite databases

## Network

- [ ] Gateway binds to localhost only (not 0.0.0.0) unless behind reverse proxy
- [ ] WebSocket requires authentication
- [ ] WebSocket validates Origin header
- [ ] Rate limiting configured on gateway
- [ ] Maximum request body size configured

## Sandbox

- [ ] Enable sandbox for tool execution (Landlock, Firejail, or Bubblewrap)
- [ ] Verify seccomp BPF filters are active
- [ ] Restrict network access from sandboxed processes
- [ ] Verify file descriptor inheritance is minimal

## Runtime

- [ ] Disable unused channels
- [ ] Disable unused providers
- [ ] Disable unused tools
- [ ] Set memory limits for agent context
- [ ] Configure maximum conversation length

## Supply Chain

- [ ] Generate and verify SBOM before deployment
- [ ] Pin dependency versions
- [ ] Verify checksums of all dependencies
- [ ] Use reproducible builds where possible
