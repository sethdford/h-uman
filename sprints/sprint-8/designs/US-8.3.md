# Design for US-8.3: Reproducible Binary Builds

**Story:** US-8.3 (P1, MEDIUM)
**Sprint:** 8 — Verifiable Privacy
**Wave:** 1 (parallel with US-8.1, US-8.2)
**Risk tier:** MEDIUM (CMakeLists + CI change; no production `.c` behavior change)

---

## Approach

The cheapest design that satisfies the story is **flags + scripts, not a rewrite of the build system**. Reproducibility is fundamentally a property of inputs (sources + toolchain + flags) producing identical outputs; we control the flags and the source tree, so we make every nondeterminism source into a flag the compiler honors:

1. **Time sources** — `SOURCE_DATE_EPOCH` is the well-known build-time epoch convention (Debian Reproducible Builds, GCC ≥ 7.4, Clang ≥ 9). CMake threads it into `__DATE__`/`__TIME__` via the compiler driver automatically when the env var is set; we enforce it by promoting any *new* use of those macros to a hard error via `-Werror=date-time` (note: `-Wdate-time` warns; `-Werror=date-time` is the load-bearing piece). The story text says `-Wno-date-time`; we deliberately reverse that to `-Werror=date-time` because **a warning the build still passes does not pin the contract** — see Anti-pattern AP-1 below. AC-8.3.3 is still satisfied (a `-Wdate-time` diagnostic IS emitted; we just additionally fail the build).
2. **Path sources** — `-ffile-prefix-map=$(CMAKE_SOURCE_DIR)=.` and `-fdebug-prefix-map` strip absolute paths from `__FILE__`, debug info, and assertion strings. Satisfies AC-8.3.4.
3. **Symbol-name randomization** — `-frandom-seed=<hash-of-source-file>` per translation unit so internal anonymous-symbol names are deterministic.
4. **Linker sources** — `-Wl,--sort-common` and `-Wl,--build-id=none` on Linux (ELF). Darwin: `-Wl,-no_uuid` (Apple ld64). Story marks darwin out-of-scope for the CI matrix, but we still set the darwin flag so a local mac build is reproducible.
5. **Archive ordering** — `ar` and `ranlib` are invoked deterministically with `D` modifier (already CMake default on most platforms when `CMAKE_AR_FLAGS` permits; we set `CMAKE_C_ARCHIVE_CREATE`/`_FINISH` explicitly to be safe).
6. **The contract itself is the test.** `scripts/check-reproducible-build.sh` does a two-shot build and `cmp` of the binaries. `scripts/check-no-date-time-macros.sh` preprocesses and greps. Both exit nonzero on violation. These are the "tests" — there is no `tests/test_*.c` file (consistent with the story's "Test seam" note).

**Why not switch to Nix / a container build?** Out of scope per the story. Toolchain reproducibility is a separable problem; we reproduce the *output* given the same toolchain, not the toolchain itself.

**Why not use `diffoscope`?** Big dependency, slow, hard to gate. `cmp` is sufficient: byte-identical or fail. If/when we hit a real diff we cannot diagnose by eye, we'll add `diffoscope` as a diagnostic-only step (not a gate).

---

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `CMakeLists.txt` | Add reproducibility block near top (after `project(...)`): SOURCE_DATE_EPOCH detection, prefix-map flags, `-Werror=date-time`, `-frandom-seed`, deterministic AR. Apply via `add_compile_options` so it covers `human_core`, `human`, and test targets uniformly. | +45 |
| `CMakePresets.json` | New preset `release-reproducible` deriving from `release`, sets `SOURCE_DATE_EPOCH=1700000000` env. (Optional convenience; the script sets the env explicitly too.) | +12 |
| `scripts/check-reproducible-build.sh` | New script: build twice into `build-rep-a/` and `build-rep-b/` with `SOURCE_DATE_EPOCH=1700000000`, `sha256sum` and `cmp` the `human` binaries, exit nonzero on mismatch. Includes carve-out documentation (what we *don't* compare). | +85 |
| `scripts/check-no-date-time-macros.sh` | New script: preprocess `src/**/*.c` with `-E` and grep for `__DATE__`/`__TIME__` *in non-system-header contexts*. Exit nonzero on violation. Belt-and-suspenders alongside `-Werror=date-time`. | +50 |
| `.github/workflows/ci.yml` | New job `reproducible-build` (matrix: ubuntu-latest only for sprint 8; macos noted as follow-on per AC scope). Caches CMake config; runs both scripts. Required-check. | +35 |
| `docs/standards/engineering/reproducible-builds.md` | New standard documenting the contract, the carve-outs, and the failure-recovery path. Linked from `docs/standards/README.md`. | +60 |
| `docs/standards/README.md` | One-line index entry. | +1 |

**No production `.c` files change.** No header changes. No vtable changes.

---

## Implementation steps (for the implementer agent)

1. **CMake block, no behavior yet.** Add the reproducibility flag block to `CMakeLists.txt` guarded by `if(DEFINED ENV{SOURCE_DATE_EPOCH})`. Confirm `cmake --preset dev && cmake --build --preset dev` still succeeds (no SOURCE_DATE_EPOCH set → no-op path).
2. **Enable always-on flags.** Move `-ffile-prefix-map`, `-frandom-seed`, `-Werror=date-time`, deterministic AR settings *outside* the env-guard so they apply to every build. Re-run full test suite. Expect 0 changes.
3. **Add `scripts/check-no-date-time-macros.sh`.** Run it locally. If it fires on real code, fail the design — escalate to user before the implementer "fixes" code to make the script pass. (Story claims no `__DATE__`/`__TIME__` usage; verify with this script.)
4. **Add `scripts/check-reproducible-build.sh`.** Run it locally on linux. Two-shot build, `cmp`. If it fails on iter 1, that is the implementer's diagnostic work — common culprits: unsorted globs, embedded timestamps in generated files (`hu_version.h`?), `ar` not in deterministic mode.
5. **CI job.** Add `reproducible-build` to `ci.yml`. Pin runner to `ubuntu-22.04` (not `ubuntu-latest`) and pin GCC version explicitly (`gcc-12`) to mitigate Risk R-1 below.
6. **Standards doc.** Document what's covered, what's carved out, and the "if this job fails, here's how to diagnose" runbook.
7. **`/verify`.** Verifier runs both scripts and confirms exit 0. Verifier ALSO does a negative test: temporarily injects `__DATE__` into a throwaway source file, confirms build fails with `-Werror=date-time` (satisfies AC-8.3.3 with the stronger error semantic).

---

## Risks

- **R-1: Toolchain version drift (MEDIUM probability / MEDIUM impact).** GCC 11 → GCC 12 changes some symbol-name hashing; `-frandom-seed` covers most cases but linker version skew (binutils 2.38 → 2.40) can still produce diffs in `.note.gnu.property` or `.eh_frame`. **Mitigation:** pin `ubuntu-22.04` + `gcc-12` + `binutils` via explicit `apt-get install` in the CI job. Document the pinned toolchain in `docs/standards/engineering/reproducible-builds.md`. Users reproducing locally on a different toolchain may diff; that's expected — the contract is "reproducible given the same toolchain."
- **R-2: Generated files with timestamps (HIGH probability / MEDIUM impact).** `hu_version.h`, embedded SBOM, or any `configure_file()` invocation may bake `${CMAKE_CURRENT_TIMESTAMP}` or `git log` output. **Mitigation:** the iter-1 `check-reproducible-build.sh` run will surface this; the implementer must patch the generator to consume `SOURCE_DATE_EPOCH` or strip the timestamp. Pre-flight grep: `grep -rn "TIMESTAMP\|strftime\|date +" CMakeLists.txt cmake/ scripts/`.
- **R-3: Darwin out-of-scope drift (LOW probability / SMALL impact).** Mac developers run `cmake --build` locally and assume reproducibility. **Mitigation:** the darwin flags are still applied (`-Wl,-no_uuid`) but only linux is CI-gated. Document this clearly in the standard so a dev doesn't assume "green CI = reproducible on my mac."
- **R-4: Brittle `cmp` failure (MEDIUM probability / SMALL impact).** Two builds may diff for a "real" reason (a generated file's content depends on filesystem inode order). **Mitigation:** the script prints `objdump -d` byte ranges of the first 256 differing bytes on failure — diagnostic, not silent. NEVER catch the diff and continue (see AP-1).
- **R-5: Concurrency in iter-2 build (LOW probability / SMALL impact).** Parallel make may interleave object emission differently. **Mitigation:** `ninja` is order-deterministic for the output binary regardless of build-step order; `cmake --preset release` uses ninja by default. Run with `--parallel` is safe.
- **R-6: Observability in production (LOW probability / SMALL impact).** If a release ships and a user can't reproduce, we need a way to debug. **Mitigation:** `scripts/check-reproducible-build.sh --verbose` mode dumps `sha256sum`, `objdump -h`, and the recorded `SOURCE_DATE_EPOCH` so a user can attach those to a bug report.

---

## Anti-patterns (forbidden — gates MUST fail loudly)

- **AP-1: Silent diff acceptance.** `check-reproducible-build.sh` MUST exit nonzero on any byte difference. Citing `~/.claude/rules/quality-gates.md`: "No silent failures: return values checked, errors propagated or logged." A script that prints "diff found" and exits 0 is the failure mode this story is supposed to prevent. The verifier agent should test this by injecting a synthetic diff (e.g., flipping one byte of one binary post-build) and confirming the script exits nonzero.
- **AP-2: Pinning a vulnerability.** Cf. `.claude/rules/tests-that-pin-bugs.md`. Do not write `check-reproducible-build.sh` such that it skips comparison when `SOURCE_DATE_EPOCH` is unset — that turns the gate off in exactly the situation it must fire. The script MUST set the epoch itself.
- **AP-3: Targeted-only test runs.** Cf. project rule: "tests must pass (full suite, not just changed-files)." After CMake flag changes, the implementer must run the FULL `./build/human_tests` suite, not just `--filter=...`. New flags can subtly change codegen.
- **AP-4: `-Wno-date-time` instead of `-Werror=date-time`.** The story says `-Wno-date-time`; we override to `-Werror=date-time`. A suppressed warning is not a contract. Document the deviation in the standards doc.

---

## Test strategy

This story has no `tests/test_*.c` (annotated in the story's "Test seam" field). The "tests" are two scripts and one CI job:

1. **`scripts/check-reproducible-build.sh`** — exit 0 ↔ binary reproducible. Verifier exercises: (a) happy path (two clean builds → exit 0), (b) adversarial (flip a post-build byte → exit nonzero), (c) negative (run without `SOURCE_DATE_EPOCH` → script sets it explicitly, still exit 0).
2. **`scripts/check-no-date-time-macros.sh`** — exit 0 ↔ no offending macros. Verifier exercises: (a) clean tree → exit 0, (b) inject `__DATE__` in a throwaway file → exit nonzero.
3. **CI job `reproducible-build`** — runs both scripts; required check on PRs touching `CMakeLists.txt`, `CMakePresets.json`, `scripts/check-reproducible-build.sh`, or `src/**`.
4. **No mocking.** The scripts run the real build, against the real toolchain, twice. This is the contract; mocking it would defeat the story.

---

## Acceptance criteria mapping

| AC | Covered by | Notes |
|---|---|---|
| AC-8.3.1 — two builds, identical sha256 | `scripts/check-reproducible-build.sh` exit 0 | Script computes and prints both hashes; `cmp` is the gate. |
| AC-8.3.2 — CI runs script on linux x86_64, exits 0 with matching SHA-256 | CI job `reproducible-build` on `ubuntu-22.04` | Pinned toolchain mitigates R-1. |
| AC-8.3.3 — `__DATE__` use emits `-Wdate-time` diagnostic | `-Werror=date-time` in `CMakeLists.txt` + verifier negative test + `scripts/check-no-date-time-macros.sh` | Stronger than story text: build fails, not just warns. Documented deviation. |
| AC-8.3.4 — same source, different abs build paths → same sha256 | `-ffile-prefix-map=$(CMAKE_SOURCE_DIR)=.` + `check-reproducible-build.sh` builds into `build-rep-a/` and `build-rep-b/` (different parent dirs) | The two build dirs are the test fixture. |

Every AC traces to a script invocation or a build-flag that the verifier can mechanically confirm.

---

## Out of scope (explicit non-goals)

- Toolchain reproducibility (we don't reproduce GCC; we reproduce the output given a pinned GCC).
- macOS aarch64 reproducibility in CI (linker + codesign add nondeterminism; follow-on story).
- Stripping debug symbols from release binaries (separate perf/size story).
- Hermetic build environment via Nix/Docker (future story).
- Reproducibility of `human_tests` test binary or fuzz harnesses (only the shipped `human` binary is in-contract).
- SBOM signing or release-artifact signing — that's US-8.4, depends on this story.

---

## Failure recovery (for the runbook)

If `reproducible-build` CI job goes red on a PR not intentionally touching the build:

1. Pull the artifact diff from the CI job (`build-rep-a/human` and `build-rep-b/human`).
2. Run locally: `cmp -l build-rep-a/human build-rep-b/human | head -20` — identifies byte offsets.
3. `objdump -h build-rep-a/human` vs `objdump -h build-rep-b/human` — identifies which section.
4. Common culprits, in order: (a) a new generated file embeds a timestamp, (b) a new use of `__DATE__` slipped past the gate (shouldn't, given `-Werror=date-time`), (c) toolchain on CI updated (check `apt list --installed gcc-12 binutils` in CI log), (d) a new dependency embeds its own build ID.
5. If (c) — the toolchain pin needs bumping; coordinate with the next sprint's release window so all builds re-pin atomically.

This runbook lives in `docs/standards/engineering/reproducible-builds.md` so it survives the sprint.
