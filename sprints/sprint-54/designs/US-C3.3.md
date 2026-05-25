# Design for US-C3.3: Provider smoke-check doctor implementation

## Approach
(Provisional) Create a new doctor check vtable registration for provider connectivity validation. The check will attempt to call the configured provider with a minimal 1-token completion, timing out at 10s wall-clock. Network I/O is gated behind `HU_IS_TEST` to silence timeouts in test mode. Failure modes are classified into an enum (not_configured, credentials_missing, credentials_invalid_401, rate_limited_429, unreachable) and mapped to readable diagnostic strings. Mock provider with failure-injection is used in tests to avoid real network calls.

## Files to modify
| File | Change | Est. LOC |
|---|---|---|
| `src/doctor/check_provider.c` | New file; vtable implementation + smoke function | +150 |
| `tests/test_doctor_check_provider.c` | New file; unit tests for PASS + 5 FAIL modes + timeout | +250 |
| `src/doctor/registry.c` | Register check via `hu_doctor_registry_register_defaults` | +5 |
| `CMakeLists.txt` | Add test source to HU_TEST_SOURCES | +3 |

## Implementation steps (for the implementer)
1. Define `hu_doctor_reason_provider_t` enum (6 variants: ok, not_configured, credentials_missing, invalid_401, rate_limited_429, unreachable)
2. Implement `hu_doctor_check_provider_run()` function signature matching vtable
3. Extract configured provider from `agent->config` using existing config accessors
4. Call `hu_provider_create()` to validate provider instantiation
5. Gate actual network I/O behind `#ifdef HU_IS_TEST` — test path returns mock failure, production path makes 1-token call
6. Classify HTTP/network errors into reason enum
7. Return `HU_DOCTOR_NA` when provider not configured, `HU_DOCTOR_FAIL` when configured-but-broken
8. Write 8 tests: PASS case, each of 5 FAIL modes, timeout case, not_configured case
9. Register vtable in registry.c
10. Run `/verify` to confirm all tests pass and no network I/O occurs in test mode

## Risks
- **Provider error classification surface area (MEDIUM/MEDIUM)**: Different providers return different error shapes (HTTP status, errno, custom messages). Mitigation: start with a mapping table for common providers (Gemini, Claude, OpenAI); fail gracefully with "unreachable" as catch-all for unmapped errors. Future providers can extend the mapping.
- **10s timeout flakiness in CI under load (MEDIUM/MEDIUM)**: Slow runners might timeout legitimately. Mitigation: set 10s budget as the contract, but tests run under HU_IS_TEST so no real network calls. Gating prevents timeout complaints in CI.
- **Credential leakage in diagnostic text (HIGH/SMALL)**: If error messages capture full API key from config. Mitigation: aspect-panel review before merge enforces sanitization; never interpolate raw credentials into diagnostic strings.
- **Backward compatibility (LOW/SMALL)**: Adding a new check to registry doesn't break existing checks. Mitigation: register at the end of the list; existing check order is unchanged.

## Test strategy
- Unit tests only; no E2E (smoking provider doesn't require integration).
- PASS case: mock provider, 1-token call succeeds, verdict is HU_DOCTOR_PASS with reason "ok".
- FAIL cases (5 modes):
  - not_configured: no provider in config, verdict HU_DOCTOR_NA
  - credentials_missing: provider configured but no API key, verdict HU_DOCTOR_FAIL + reason
  - credentials_invalid_401: mock provider returns 401, verdict HU_DOCTOR_FAIL + reason
  - rate_limited_429: mock provider returns 429, verdict HU_DOCTOR_FAIL + reason
  - unreachable: mock provider returns ECONNREFUSED, verdict HU_DOCTOR_FAIL + reason
- Timeout case: mock provider delays response > 10s, call returns timeout reason
- Mock provider: use `src/providers/mock.c` failure-injection pattern (need to grep for exact pattern)
- All 8 tests use fixture configs, no real credentials

## Acceptance criteria mapping
- AC-1.1 → implemented by registering vtable in registry.c
- AC-1.2 → covered by test_provider_smoke_pass_case + timeout + 1-token contract verification
- AC-1.3 → covered by test_provider_smoke_fail_* (5 distinct FAIL modes)
- AC-1.4 → HU_IS_TEST gate confirmed in test assertions (no real network I/O)
- AC-1.5 → mock provider pattern verified against src/providers/mock.c
- AC-1.6 → test coverage checklist (8 tests mapping to 6 modes + timeout + not_configured)
- AC-1.7 → aspect-panel review checklist (out-of-scope for this design, but noted as pre-merge gate)

## Out of scope
- New doctor checks beyond this one (stated in sprint anti-goals)
- Provider implementation changes or fixes (this check only validates existing providers)
- Credential rotation or refresh logic (smoke check is point-in-time only)
- Credential caching or persistence (that's onboarding's job, US-C2.3)
