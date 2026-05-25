# Design for US-6: Bridge B5 negative path — malformed safetensors rejected

## Approach

Test `hu_mlx_provider_load_adapter` rejects malformed safetensors files at load time, preventing corrupted adapters from reaching the chat path. The current implementation (src/providers/mlx.c:425) only validates file existence via `access(F_OK)` on `adapters.safetensors`; it does NOT validate the file's format or structure. 

The fix is TWO-FOLD:
1. **Test layer (this story)**: Pin the negative-path contract — a malformed safetensors file must return `HU_ERR_INVALID_ARGUMENT` and leave provider state unchanged (no partial load).
2. **Implementation gap (potential)**: The test may discover that current `mlx_load_adapter` lacks format validation. If so, add a minimal magic-byte check (first 8 bytes) before returning HU_OK. This is pre-flight; the mlx-lm subprocess will re-validate when it actually loads the file, so we don't need to parse the entire safetensors header.

**Why this design:**
- Fixtures are generated at runtime (malformed bytes written to a temp file) rather than committed as binary blobs — cheaper, deterministic, testable without large files.
- Two corruption modes cover the contract: (a) truncated header (missing JSON metadata), (b) corrupted magic (safetensors magic is a fixed 8-byte LE header; wrong bytes fail fast).
- State-unchanged is verified by snapshot-before/after on `hu_mlx_provider_active_adapter_path`, matching the atomic-swap pattern in US-5's existing load path.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `tests/test_mlx_provider.c` | Add `test_mlx_lora_adapter_malformed_safetensors_rejected` with two sub-cases | +60 |
| `src/providers/mlx.c` | Add safetensors magic-byte validation in `mlx_load_adapter` (if test fails) | +15–30 |

## Implementation steps (for the implementer agent)

1. Add test skeleton in `tests/test_mlx_provider.c`:
   - `test_mlx_lora_adapter_malformed_safetensors_rejected()` with two subtests
   - Reuse the `make_tempdir() / rm_rf()` helpers from `test_mlx_load_adapter.c`
2. Create a malformed safetensors fixture (truncated header, 2 bytes long):
   - Write fixture to `tempdir/adapters.safetensors`
   - Call `hu_mlx_provider_load_adapter` with the directory path
   - Assert return code is `HU_ERR_INVALID_ARGUMENT`
   - Assert `hu_mlx_provider_active_adapter_path` is still NULL (no partial state)
3. Create a second fixture (wrong magic, 8 bytes but no valid safetensors header):
   - Repeat assertions
4. Add validation to `src/providers/mlx.c::mlx_load_adapter` if test fails:
   - After `access(F_OK)` check, open the safetensors file and read first 8 bytes
   - Validate magic (safetensors LE header format: length prefix)
   - Return `HU_ERR_INVALID_ARGUMENT` if mismatch; otherwise proceed as before
5. Run `./build/human_tests --filter=malformed` to verify test passes

## Risks

- **Validation level (LOW/SMALL)**: Current code only checks file existence. The test may reveal this. Mitigation: add the 8-byte magic check; it's cheap (one fread) and sufficient because mlx-lm will re-parse the full header on subprocess load.
- **Fixture realism (LOW/SMALL)**: Truncated files are realistic (corruption, partial download); wrong-magic files are less common but still plausible. Both are in the contract scope.
- **Atomic-swap contract (LOW/SMALL)**: Test must verify no partial state after rejection. Mitigation: snapshot `active_adapter_path` before load and assert it's unchanged after error.
- **Backward compatibility (LOW)**: Validation is purely on the error path; existing good adapters are unaffected.

## Test strategy

- Unit test in `tests/test_mlx_provider.c`, colocated with existing `test_mlx_lora_adapter_biases_completion` (US-5)
- Two malformed-file cases (truncated, wrong magic)
- State-unchanged assertion confirms atomic-swap
- No subprocess spawn (HU_IS_TEST gates it)
- Cleanup via rm_rf in teardown

## Acceptance criteria mapping

- **AC-6.1** → `test_mlx_lora_adapter_malformed_safetensors_rejected` function exists ✓
- **AC-6.2** → Test creates malformed safetensors fixtures (truncated, wrong magic) ✓
- **AC-6.3** → `hu_mlx_provider_load_adapter` with malformed path returns `HU_ERR_INVALID_ARGUMENT` ✓
- **AC-6.4** → `hu_mlx_provider_active_adapter_path` unchanged (snapshot before/after) ✓
- **AC-6.5** → Error is specific, no crash, no silent fallback ✓

## Out of scope

- Full safetensors header parsing (mlx-lm already does this)
- Adapter shape validation (lora_A/lora_B tensor names) — deferred to subprocess
- Stress testing with very large malformed files
- Integration with the daemon's adapter-swap admin path (HTTP-level validation is separate)

---

**RESULT_tech-lead-US-6=READY**
