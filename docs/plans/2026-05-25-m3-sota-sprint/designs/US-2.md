# Design for US-2: Bridge B2 verifier — MLX model path resolution

## Approach

US-2 adds a single verifier test (`test_mlx_provider_create_resolves_model_path`) to pin the behavior of `hu_mlx_provider_create` when the model path is present vs. missing. The current implementation (`src/providers/mlx.c:522-550`) **does not validate path existence** — it accepts any non-NULL path and returns `HU_OK` as long as allocation succeeds. This is correct for the config-parse layer (which should not stat paths before handing off to the MLX subsystem), but the test must verify that a missing model is rejected at the **usage boundary** — i.e., when the subprocess tries to invoke `python3 -m mlx_lm.generate --model <path>`.

The design choice is: **validate path existence inside the test fixture**, not in `hu_mlx_provider_create` itself. The create function will remain allocation-only (no I/O); the test will create a temporary fixture directory as a "present" model and pass a nonexistent path for the "missing" case. The verifier contract per the plan (`docs/plans/2026-05-17-m3-mlx-bridge-execution-plan.md`, Phase B2) specifies that a missing path returns `HU_ERR_NOT_SUPPORTED`, matching the dispatcher safety contract.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `tests/test_mlx_provider.c` | Add test function + fixture path helpers | +45 |
| (none — no source change) | Path validation occurs downstream in subprocess | — |

## Implementation steps (for the implementer)

1. **Check if `tests/test_mlx_provider.c` exists.** If not, create it with gate `#ifdef HU_ENABLE_MLX_PROVIDER + __APPLE__ + __arm64__`.

2. **Create two test cases inside a single test function:**
   - `test_mlx_provider_create_resolves_model_path`
   - Subcase (a): call `hu_mlx_provider_create` with a valid fixture path (directory that exists); verify return is `HU_OK` and the returned provider can be freed without error.
   - Subcase (b): call `hu_mlx_provider_create` with a path that does not exist; verify return is `HU_ERR_NOT_SUPPORTED` (NOT `HU_OK`, not `HU_ERR_NOT_FOUND`, specifically `HU_ERR_NOT_SUPPORTED` per dispatcher contract).

3. **Fixture strategy:** Use a temporary directory (via `mkdtemp` or a test fixture dir committed to `tests/fixtures/mlx/`) as the "present" model. For the "missing" case, pass `/nonexistent/path/to/model`.

4. **Ownership check:** After `hu_mlx_provider_create` returns `HU_OK`, call the provider's `deinit` vtable method to verify no leak (ASan will catch double-free or missed free).

5. **Register the test in `tests/test_main.c`** under the `#ifdef HU_ENABLE_MLX_PROVIDER` guard (or inline stub if using the internal-`#ifdef`-wrap pattern from `.claude/rules/test-source-gate-symmetry.md`).

6. **Run:** `cmake --build /Users/sethford/Projects/h-uman/build && ./build/human_tests --filter=mlx_provider_create_resolves`

## Risks

- **Error-code mismatch (MEDIUM/SMALL)**: The plan specifies `HU_ERR_NOT_SUPPORTED` for a missing path, but the implementer might confuse this with `HU_ERR_NOT_FOUND` (resource missing) or `HU_ERR_INVALID_ARGUMENT` (bad input). Mitigation: the test assertion must explicitly check `result == HU_ERR_NOT_SUPPORTED` (not just `result != HU_OK`). The error code is chosen to match the dispatcher safety contract (`test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat`), which expects the MLX provider to transparently fall back when unsupported.

- **Ownership leak under partial init (LOW/MEDIUM)**: If `hu_mlx_provider_create` returns `HU_OK` but the model_path_owned allocation fails, the context is partially initialized. The current code (lines 540-544) handles this by calling `mlx_deinit` before returning OOM. The test should verify that even with a missing path, the returned provider can be freed safely. Mitigation: always call the vtable deinit; ASan will catch the error.

- **Test platform gating (LOW/SMALL)**: The test is gated on `HU_ENABLE_MLX_PROVIDER + __APPLE__ + __arm64__`. On other platforms the test is not compiled. Verify via `cmake --list-presets` that the test is included in the dev preset (which enables MLX) and skipped in minimal/no-mlx builds. Mitigation: the test file uses the symmetric gate pattern (`.claude/rules/test-source-gate-symmetry.md`).

## Test strategy

The test has two explicit paths, no hidden branches:

1. **Happy path (fixture directory)**: Create a temporary directory, pass it to `hu_mlx_provider_create`, expect `HU_OK`.
2. **Error path (nonexistent path)**: Pass `/tmp/definitely_does_not_exist_mlx_model_XXXXXX`, expect `HU_ERR_NOT_SUPPORTED`.

Both cases call `provider.vtable->deinit(provider.ctx, alloc)` to verify no leak.

No real subprocess is invoked (the test runs under `HU_IS_TEST`, so the vtable methods return `HU_ERR_NOT_SUPPORTED` before reaching the subprocess code). The test verifies the **constructor contract**, not the subprocess behavior (that's B1's job).

## Acceptance criteria mapping

- **AC-2.1** → Test function name is `test_mlx_provider_create_resolves_model_path`, lives in `tests/test_mlx_provider.c`.
- **AC-2.2** → Happy-path case: present model → `HU_OK`.
- **AC-2.3** → Error-path case: missing path → `HU_ERR_NOT_SUPPORTED`.
- **AC-2.4** → Both cases call `vtable->deinit` without error; ASan verifies no leak.
- **AC-2.5** → Two test cases (present, missing), both exit cleanly via `HU_ASSERT_*` macros.

## Out of scope

- **Config parser wiring.** AC-2 does not include testing `config->mlx_model_path` JSON parsing or serialization (that's Phase B2 follow-up per the plan). The test passes config directly to `hu_mlx_provider_create`.
- **Subprocess path validation.** AC-2 verifies the constructor contract (allocate + own the path). When the subprocess actually tries to invoke the path, that's B1's test (`test_mlx_chat_subprocess_round_trip`).
- **Error message content.** The test checks the error code, not the log message.

RESULT_tech-lead-US-2=READY
