---
plan: docs/plans/2026-05-11-init-03-apple-fm-provider.md
auditor: group-10-init-series
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: FULL
verdict: PARTIAL
confidence: HIGH
---

## Plan Summary
Promote `src/providers/apple.c` from a localhost OAI-compat HTTP shim
into a real `hu_provider_t` backed by Apple's `FoundationModels.framework`
through a stable C ABI exposed by `HumanKit`. Adds Unix-domain-socket
transport, in-process `dlopen` of `libHumanFoundationModelsBridge.dylib`,
strict-local mode (refuse PCC fallback), and native tool-calling via
`FoundationModels.Tool`.

## Key Claims (from the plan)
- Three transports: TCP-loopback (existing), Unix domain socket, in-process C ABI via `dlopen`
- Strict-local mode that refuses Apple Private Cloud Compute fallback
- iOS path via in-process Swift bridge
- Native tool calling (FoundationModels.Tool ↔ `hu_tool_t` registry)
- Auth via per-user file-mode-checked socket path replacing the bearer token

## Evidence

### Implemented? (code exists)
- `src/providers/apple.c` exists (676 LOC) — but it is the pre-existing localhost OAI-compat HTTP shim the plan acknowledges as "Reality check": still posting JSON to `http://127.0.0.1:11435/v1/chat/completions`.
- `include/human/providers/apple.h` exists with `hu_apple_config_t.base_url` only — no UDS path, no strict-local flag, no dlopen surface.
- Swift side: `apps/shared/HumanKit/Sources/HumanOnDevice/OnDeviceProvider.swift` and `OnDeviceChatAdapter.swift` exist (LanguageModelSession bridge), as the plan describes.
- No `libHumanFoundationModelsBridge.dylib` build target, no `dlopen` path in `apple.c`.
- No Unix-domain-socket transport, no strict-local-mode gating against PCC.

### Proven? (tests exist)
- `tests/test_apple_provider.c` exists (357 LOC) — pins the existing TCP-loopback OAI-compat behavior, not the new transports or strict-local mode.
- No tests for UDS transport, dlopen bridge, or strict-local refusal.

### Wired? (called in runtime path / dispatch)
- Apple provider registered in `src/providers/factory.c` at lines 187–200 under names `apple`, `apple-intelligence`, etc. Existing TCP path is wired.

## Gaps
- The five "real gaps" the plan claims to close are still open: iOS path, native tool calling, strict-local mode, loopback TCP attack surface (UDS), direct C ABI via dlopen.
- `OnDeviceRouter.swift` still ignores `tools[]` per the plan's own Reality Check; no fix landed.
- No `hu_provider_t.load_adapter` implementation — vtable still leaves it NULL.

## Notes
This is the only Init plan whose subject (the Apple provider) already
existed at design time, so "implemented" reflects the pre-existing shim,
not the plan's deliverables. None of the plan's net-new contributions
have landed.
