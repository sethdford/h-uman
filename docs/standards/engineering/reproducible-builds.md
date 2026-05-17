---
title: Reproducible Binary Builds
---

# Reproducible Binary Builds

The shipped `human` binary is **bytewise-reproducible from source**: two independent builds from the same source tree, with the same pinned toolchain and the same `SOURCE_DATE_EPOCH`, produce identical SHA-256 hashes after a narrow set of documented carve-outs.

This is the contract that lets a user or auditor verify a distributed binary matches the published source without trusting the build server. It is the foundation US-8.4 (signed release artifacts) and US-8.5 (privacy doctor SBOM) build on.

Owning story: US-8.3 (Sprint 8). Design: `sprints/sprint-8/designs/US-8.3.md`.

## The contract

Given two builds:

```
SOURCE_DATE_EPOCH=1700000000 cmake -S . -B /tmp/build-A ...
SOURCE_DATE_EPOCH=1700000000 cmake -S . -B /tmp/build-B ...
cmake --build /tmp/build-A --target human
cmake --build /tmp/build-B --target human
```

After the scrub described below, `shasum -a 256 /tmp/build-A/human` must equal `shasum -a 256 /tmp/build-B/human`.

The gate is `scripts/check-reproducible-build.sh`. It is required-check in CI on both `ubuntu-latest` and `macos-latest`.

## What the flags do

All set unconditionally in `CMakeLists.txt`:

- `-ffile-prefix-map=$(CMAKE_SOURCE_DIR)=.` and `-ffile-prefix-map=$(CMAKE_BINARY_DIR)=.` — strip absolute paths from `__FILE__`, debug info, and assertion strings. Without this, every binary embeds the absolute build path.
- `-Werror=date-time` — promote any use of `__DATE__` / `__TIME__` from a warning to a build error. The story originally said `-Wno-date-time`; we deliberately reverse this. A suppressed warning is not a contract. AC-8.3.3 still passes because `-Wdate-time` IS emitted — we additionally fail the build (anti-pattern AP-4 in the design).
- `CMAKE_C_ARCHIVE_CREATE` with `qcD` — deterministic ar (GNU/llvm-ar only; Darwin's BSD ar is probed and skipped, modern Xcode libtool is already deterministic).
- `-Wl,--build-id=none` on Linux — suppress the ELF `.note.gnu.build-id` section, which embeds host-derived random bytes by default.
- `-Wl,-no_uuid` on Darwin — zero the Mach-O `LC_UUID` load command.

`SOURCE_DATE_EPOCH` is read by GCC and Clang automatically. When set, it locks `__DATE__` / `__TIME__` if anything still uses them (defense in depth — `-Werror=date-time` catches new uses).

## Carve-outs (what the diff script scrubs before comparing)

These are the *only* allowed sources of post-build divergence. Each is removed from BOTH copies before `cmp`:

| Carve-out | Platform | Why |
|---|---|---|
| `.note.gnu.build-id` | Linux ELF | Linker version skew (binutils 2.38 → 2.40) can re-add this even with `--build-id=none`. Removed with `objcopy --remove-section`. |
| `LC_UUID` | Darwin Mach-O | Already zeroed by `-Wl,-no_uuid`, but the scrub is no-op-safe. |
| Ad-hoc code signature | Darwin only | `cmake --build` invokes `codesign --sign -` as a post-build step. The signature embeds a hash of the binary that includes itself, so the two copies' signatures inherently differ. Removed with `codesign --remove-signature`. |

We do NOT compare just `.text`. We compare the full file *after* the carve-outs are applied. If even one byte outside the carve-outs differs, the script fails (anti-pattern AP-1 — silent diff acceptance is forbidden).

## CI

Job: `reproducible-build` in `.github/workflows/ci.yml`. Matrix: `ubuntu-latest` + `macos-latest`. NO `continue-on-error` — failure blocks the PR. Two steps:

1. **`scripts/check-no-date-time-macros.sh`** — fast grep for `__DATE__` / `__TIME__` in `src/` and `include/`. Belt-and-suspenders alongside `-Werror=date-time` so the diagnostic is cheap and clean.
2. **`scripts/check-reproducible-build.sh`** — two-shot build into `/tmp/hu-rep-a.*/build/human` and `/tmp/hu-rep-b.*/build/human`, scrub, `cmp`. Exits nonzero on any mismatch.

## Toolchain pinning (Linux)

Reproducibility is property of inputs (sources + toolchain + flags) → outputs. We control sources and flags; the toolchain is pinned implicitly by the `ubuntu-latest` GitHub runner image at any given moment. Major toolchain bumps (GCC 12 → 13, binutils 2.38 → 2.40) can produce diffs in `.eh_frame` or `.note.gnu.property` that aren't covered by our carve-outs. When that happens, the CI job fails — and the fix is either (a) update the carve-out list with a new narrow exception or (b) coordinate a toolchain re-pin sprint.

## Failure recovery — runbook

If `reproducible-build` goes red on a PR not intentionally touching the build:

1. `cmp -l A/human B/human | head -20` — identify byte offsets.
2. `objdump -h A/human` vs `objdump -h B/human` (Linux) or `otool -l A/human` vs `otool -l B/human` (Darwin) — identify which section.
3. Walk the common-culprit list, in order:
   - **(a) Generated file embeds a timestamp.** Grep `CMakeLists.txt` and any generator script for `strftime`, `date +`, `TIMESTAMP`. Either drop the timestamp or have the generator read `SOURCE_DATE_EPOCH`.
   - **(b) New `__DATE__` / `__TIME__` slipped past the gate.** Run `scripts/check-no-date-time-macros.sh` locally. Shouldn't happen given `-Werror=date-time`, but if a TU is built with `-Wno-error=date-time` somewhere, this is the smoking gun.
   - **(c) Toolchain on CI updated.** Check the runner image release notes; cross-check `gcc --version` / `clang --version` in the CI log between the last green run and the failing run.
   - **(d) New dependency embeds its own build ID or timestamp.** Bisect `git log` since the last green run.
4. If diagnosing locally: `scripts/check-reproducible-build.sh --verbose --keep-dirs`. Inspect the kept build dirs.

DO NOT silence the gate. The contract is the deliverable.

## Out of scope (explicit non-goals)

- **Toolchain reproducibility.** We don't reproduce GCC itself; we reproduce the output given a pinned GCC.
- **`human_tests` test binary or fuzz harnesses.** Only the shipped `human` binary is in-contract. Test binaries embed timestamps via the test framework's logging path; out of scope.
- **`HU_ENABLE_LTO=ON` with mixed toolchain.** LTO bytecode is toolchain-private; same GCC produces same LTO output, but mixing GCC and Clang in a two-shot build is not in contract.
- **Stripping debug symbols.** A separate perf/size story.
- **Hermetic Nix/Docker build environment.** Future work; the current design reproduces given the host's installed toolchain.

## Related

- Design: `sprints/sprint-8/designs/US-8.3.md`
- Story: `sprints/sprint-8/stories.md` (search `US-8.3`)
- Quality gate: `~/.claude/rules/quality-gates.md` (anti-pattern AP-1: no silent failures)
- Follow-on: US-8.4 — signing the reproducible binary so users can verify the source SHA matches the signature.
