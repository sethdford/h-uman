# Design for US-C2.3: Provider Setup Onboarding Step

## Approach

The provider step is the second screen of the onboarding wizard, guiding users to select an AI provider and configure credentials. It reuses the smoke-check implementation from US-C3.3 (provider validation) rather than reimplementing the check inline.

The step presents four options (local-MLX / Anthropic / Gemini / OpenAI) via numeric menu, reads the user's choice, prompts for an API key (if applicable), persists ONLY the provider choice to `hu_onboard_state_t.provider.provider_name` BEFORE validation (crash safety), then validates via smoke-check. The API key is kept in an on-stack buffer, never persisted to state or config. On validation failure, it re-prompts with an actionable error message (e.g., "credentials_invalid_401 — check your API key spelling").

Key design decisions:
1. **Smoke-check reuse**: This story depends on US-C3.3 landing first. We export a reusable function `hu_doctor_check_provider_smoke(provider_name, api_key)` from the doctor module so both stories call the same validation logic.
2. **Menu-driven selection**: `1`/`2`/`3`/`4` for the four providers, `q` to quit. This keeps the UX lightweight and deterministic for testing.
3. **API key is ephemeral**: Read from stdin, passed directly to smoke-check, then erased. NEVER persisted to state or written to config. NEVER logged.
4. **State persistence (minimal)**: Write `state->provider.provider_name` immediately after the menu choice (before validation). Validation failure is REPEAT, not ABORT, so the user's choice persists across re-prompts. Set `provider_smoke_passed=true` only after PASS verdict.
5. **Test injection via user_data**: Same pattern as welcome step — `user_data` can be set to a `hu_onboard_provider_test_input_t` struct (choice, api_key, smoke_result) to inject the entire interaction.
6. **Network I/O guarding**: All smoke-check calls gate on `#ifdef HU_IS_TEST` — in test mode, we use a mock provider instead of hitting the real API.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `include/human/onboard/step_provider.h` | Public header with vtable factory + test-injection struct | +60 |
| `src/onboard/step_provider.c` | Step implementation with menu, credential read, smoke-check reuse | +280 |
| `tests/test_onboard_step_provider.c` | Test cases for all 4 providers + error modes + state persistence | +400 |
| `include/human/doctor/check_provider.h` | **NEW** — exported smoke-check function (US-C3.3 shared boundary) | +30 |
| `src/onboard/dispatcher.c` | Register provider step in step_table | +5 |
| `CMakeLists.txt` | Add step source + test source | +8 |

**Note**: `include/human/doctor/check_provider.h` is a dependency on US-C3.3. That story must create the function; this story reuses it. If US-C3.3 is already landed, this story can import directly. If US-C3.3 is in-flight, coordinate on the signature.

## Implementation steps (for the implementer)

1. **Skeleton vtable** (step_provider.c): create hu_onboard_step_provider_create() returning a vtable with name="provider" and run/enter function pointers; run() returns NEXT as a no-op.
2. **Menu rendering**: implement the 4-option menu (MLX / Anthropic / Gemini / OpenAI), read one keystroke, classify to provider choice. Test with injected choices.
3. **API key input**: for choices that require a key (Anthropic, Gemini, OpenAI — not MLX), prompt "Enter your API key:" and read a full line via fgets() into a stack buffer (256 bytes). Trim trailing whitespace. Do NOT persist this buffer.
4. **State persistence (early)**: write choice to `state->provider.provider_name` IMMEDIATELY after menu selection (before attempting validation). This ensures the choice is saved in case of a crash. Also set `state->provider.provider_smoke_passed = false` initially.
5. **Smoke-check integration**: call `hu_doctor_check_provider_smoke(state->provider.provider_name, api_key_buffer)` (signature TBD by US-C3.3). Handle the result: PASS → set `provider_smoke_passed=true`, return NEXT; FAIL → construct error message from verdict reason, return REPEAT (user stays in this step, can re-enter key or go back).
6. **Key erasure**: after validation (PASS or FAIL), explicitly zero out the api_key buffer via memset(buffer, 0, sizeof(buffer)) to avoid stack leaks.
7. **Test injection**: implement hu_onboard_provider_test_input_t struct with fields (choice, api_key, mock_smoke_result), check user_data at start of run(), bypass all I/O if set.
8. **Add tests**: 8+ test cases covering menu choice validation, API key entry, state persistence of choice (not key), smoke-check PASS/FAIL paths, credential leak detection (grep test output for API key fragments; if injected key "test_api_key_xyz" appears in test output, fail the test).
9. **Register in dispatcher**: add hu_onboard_step_provider_create() call to dispatcher.c's step_table initialization.

## Risks

- **API key leak via logging** (MEDIUM/LARGE): if error messages or test output capture the key, credentials are exposed. Mitigation: strict no-logging of the key itself; only log the provider name and the verdict reason (which comes from smoke-check and is already sanitized); aspect-panel review required; test includes a credential-leak check.
- **API key left on stack** (LOW/MEDIUM): if the step crashes before memset, the key might be recoverable from a core dump. Mitigation: memset(buffer, 0, sizeof(buffer)) immediately after validation; in production builds, stack is typically not dumped.
- **Race between keystroke and line-read** (LOW/SMALL): if we read a single keystroke but the user pastes a multi-line key, characters are lost. Mitigation: always read full lines via fgets() for the key, not single-char reads.
- **Smoke-check timeout stalls wizard** (MEDIUM/MEDIUM): a 10s timeout on provider.complete() could block the user. Mitigation: in HU_IS_TEST mode, use a mock that returns immediately; in production, show a spinner + timeout hint.
- **Reuse boundary with US-C3.3** (MEDIUM/MEDIUM): if US-C3.3 exports a function with a different signature than expected, this story fails at link time. Mitigation: explicitly document the expected signature; US-C3.3 author confirms before proceeding.

## Test strategy

- **Credential leak detection**: each test case with an injected key must verify the key does NOT appear in the test output or logs. Use a regex match on captured stderr to ensure the injected key string never surfaces.
- **State persistence**: verify that `state->provider.provider_name` is written even if validation fails; `provider_smoke_passed` is only set to true after PASS.
- **Menu classification**: 4 choices map to correct provider names.
- **API key reads**: multi-line input, trailing whitespace trimming, empty key rejection.
- **Smoke-check integration**: call is made with correct params; verdict is correctly interpreted; REPEAT is returned on FAIL, NEXT on PASS.
- **Edge cases**: ctrl-C during input (graceful), invalid choice (re-prompt), key too long (truncate or reject with guidance).

## Acceptance criteria mapping

- AC-1.1 → files created (step_provider.h/c, test file)
- AC-1.2 → step registered in dispatcher's step_table at HU_ONBOARD_STEP_PROVIDER
- AC-1.3 → reuses hu_doctor_check_provider_smoke from US-C3.3; no re-implementation
- AC-1.4 → menu invites user to choose from 4 providers (1/2/3/4 keys)
- AC-1.5 → persists provider CHOICE to state.provider.provider_name; API key is ephemeral (not persisted)
- AC-1.6 → validates via smoke-check; re-prompts (REPEAT) on failure with diagnostic
- AC-1.7 → test injection via user_data callback (struct hu_onboard_provider_test_input_t)
- AC-1.8 → aspect-panel review required (security-sensitive credential handling)

## Out of scope

- Credential refresh or expiration handling (future story)
- Provider dynamic discovery (static registration only)
- Multi-factor auth (out of scope; doc-link only)
- Config.json write (later step or wizard finalization)

## Key open question for US-C3.3

**Confirm the signature of the reusable smoke-check function.** This design assumes:

```c
hu_doctor_check_result_t hu_doctor_check_provider_smoke(
    const char *provider_name,    /* "gemini", "anthropic", "openai", "mlx_local" */
    const char *api_key           /* NULL for mlx_local */
);
```

If US-C3.3 uses a different signature (e.g., requires a config struct, or uses a registry context), this step's design changes the call site accordingly. Coordinate before finalizing.
