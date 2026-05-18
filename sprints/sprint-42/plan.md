# Sprint 42 Plan — Verifiable Privacy

**Scrum Master:** Claude (Sonnet 4.6)
**Branch:** `sprint-42-verifiable-privacy`
**Working directory:** `/Users/sethford/Documents/h-uman/.worktrees/sprint-42-privacy`
**Base SHA:** `5b57ff2b`
**Date:** 2026-05-17
**Total estimate:** 1L + 1L + 1M + 1M + 1S = ~4–5 M-equivalents

---

## §1 Sequencing

### Wave 0 — Parallel, no dependencies
- US-42.1 — DP-SGD training budget (L)
- US-42.2 — Persona key wrap / Keychain (L)
- US-42.3 — Reproducible build (M)

### Wave 1 — After US-42.3 (signing needs reproducible binaries)
- US-42.4 — Binary signature verify (M)

### Wave 2 — After Wave 0 + US-42.4 (doctor composes all four)
- US-42.5 — `--privacy` doctor (S)

Dependency rationale:
- US-42.4 must see a reproducible binary checksum before it can pin expected digests
- US-42.5 calls the dp-sgd budget API, the keychain status API, and the sig-verify API — all must be committed before doctor integration

---

## §2 Wave Assignments

### Wave 0

| Story | Risk | Key files | Implementer | Verifier scope | Critic scope | Aspect-panel | Worktree isolation |
|---|---|---|---|---|---|---|---|
| US-42.1 | HIGH | `src/ml/dp_sgd.c` (NEW), `include/human/ml/dp_sgd.h` (NEW), `tests/test_dp_sgd.c` | general-purpose | Full suite + Opacus oracle comparison script | dp_sgd math + clipping correctness | MANDATORY | YES — `impl/US-42.1` |
| US-42.2 | HIGH | `src/security/persona_key.c` (NEW or extend), `include/human/security/persona_key.h`, `tests/test_persona_key.c` | general-purpose | Full suite + Keychain shim path | crypto API surface, memory hygiene | MANDATORY | YES — `impl/US-42.2` |
| US-42.3 | HIGH | `CMakeLists.txt`, `scripts/check-no-date-time-macros.sh` (NEW), `scripts/check-reproducible-build.sh` (NEW), `.github/workflows/ci.yml` | general-purpose | Two-build sha256 diff (both ubuntu + macos) | Build-infra delta + CI job correctness | Critic-only sufficient | YES — `impl/US-42.3` |

### Wave 1

| Story | Risk | Key files | Implementer | Verifier scope | Critic scope | Aspect-panel | Worktree isolation |
|---|---|---|---|---|---|---|---|
| US-42.4 | HIGH | `src/security/sig_verify.c` (NEW), `include/human/security/sig_verify.h`, `tests/test_sig_verify.c`, release pipeline yaml | general-purpose | Full suite + sig-verify integration test against Wave-0 binary | Signing surface, key custody, failure modes | MANDATORY | YES — `impl/US-42.4` |

### Wave 2

| Story | Risk | Key files | Implementer | Verifier scope | Critic scope | Aspect-panel | Worktree isolation |
|---|---|---|---|---|---|---|---|
| US-42.5 | HIGH | `src/cli/doctor.c` (extend), `tests/test_doctor_privacy.c`, docs/guides/privacy-doctor.md | general-purpose | Full suite + `human --privacy` end-to-end smoke | Doctor output correctness, no false-clean reports | MANDATORY | YES — `impl/US-42.5` |

---

## §3 Implementer Commit Discipline

Every implementer agent's LAST action before reporting DONE MUST be:

```bash
git -C <impl-worktree> add <changed-paths>
git -C <impl-worktree> commit -m "feat(<scope>): <description>"
```

The SM verifies existence of the commit before accepting DONE:

```bash
git log impl/US-42.N ^sprint-42-verifiable-privacy --oneline | grep -q <expected-pattern>
```

A DONE report without a verifiable commit on the story's `impl/US-42.N` branch will be REJECTED and the story re-opened. Working-tree-only state is not DONE. Concurrent agents can and will reset the working tree.

---

## §4 Quality Gates Per Story

A story is closed ONLY when ALL of the following hold:

- [ ] Implementer commit exists on `impl/US-42.N` branch (verified by SM via git log)
- [ ] `/verify` returned `RESULT_verifier=PASS`
- [ ] All AC have evidence (test output, behavior demo, script output)
- [ ] Per-story critic ran immediately after implementer reported DONE — `RESULT_critic=CLEAN` or LOW/INFO only
- [ ] `/aspect-panel` returned PASS or CLEAN (MANDATORY for US-42.1, US-42.2, US-42.4, US-42.5)
- [ ] No `RESULT_critic=HAS_FINDINGS` of HIGH or CRITICAL severity outstanding
- [ ] Tests added: happy path AND error paths AND edge cases
- [ ] Full suite green (`./build/human_tests` — 0 failures, 0 ASan errors)
- [ ] Public API changes documented at call site

Critic runs PER STORY, not batched at sprint end.

---

## §5 Cross-Sprint Coordination

- `human --privacy` doctor slot (US-42.5) is RESERVED for Sprint 42. Sprint-43 owns the `--install` slot.
- **CRITICAL:** US-42.5 design (US-42.5.md) requires a `--install` **no-op stub** (`HU_ERR_NOT_SUPPORTED` + "reserved for sprint-43" message) to be committed in the `cmd_doctor` dispatch order so sprint-43 can add the real implementation without changing precedence. The US-42.5 implementer must add this stub in `src/main.c` and pin it with a precedence test. No overlap with sprint-43 content.
- `src/security/` changes in this sprint: US-42.2 (`persona_crypt.c`, `keystore_darwin.c`) and US-42.4 (`artifact_sign.c`). Sprint-43 must not branch from `sprint-42-verifiable-privacy` until both are merged and the sprint is closed.
- `src/ml/dp_sgd.c` is a NEW file in this sprint — no prior art on the branch. Sprint-43 ML work must rebase after this sprint closes.
- `scripts/check-no-date-time-macros.sh` and `scripts/check-reproducible-build.sh` (US-42.3) may conflict with any concurrent infra work on `.github/workflows/ci.yml`. Check with lead before Wave 0 dispatch if any open infra PRs touch `ci.yml`.
- `include/human/error.h` is touched by BOTH US-42.1 (adds `HU_ERR_PRIVACY_BUDGET_EXHAUSTED`) AND US-42.2 (adds 4 error codes). These are parallel in Wave 0 — **both implementers must be aware of this shared-file conflict**. SM will coordinate the merge order: US-42.1's error code lands first (it has fewer additions); US-42.2 rebases on top before merge.

---

## §6 Top 3 Sprint Risks

### Risk 1 — DP-SGD math regression vs Opacus oracle (US-42.1, HIGH)
Our DP-SGD implementation must produce privacy accounting that matches PyTorch Opacus within tolerance. A subtle clipping-norm or noise-multiplier bug produces silent epsilon over-spend — the model trains, privacy guarantees silently erode. Mitigation: verifier runs comparison script against Opacus oracle (pre-agreed epsilon/delta budget); aspect-panel panel includes correctness + security voters; implementer must include the oracle comparison in AC evidence.

### Risk 2 — Keychain unavailable in CI sandbox (US-42.2, HIGH)
macOS Keychain APIs require an interactive session or a provisioned entitlement. GitHub Actions macOS runners are headless; `SecItemAdd` / `SecItemCopyMatching` fail with `errSecInteractionNotAllowed`. Without a test-mode shim, all Keychain-path tests will be skipped or silently pass empty. Mitigation: implementer MUST provide a `HU_IS_TEST` shim path that exercises the same code paths via an in-memory keystore; verifier must confirm the shim is exercised AND that the production Keychain path compiles clean. Flag as BLOCKER if darwin runner with Keychain entitlement is unavailable for integration test.

### Risk 3 — Signing-key custody for US-42.4 release pipeline
Binary signature verification requires a signing key. Where does the private key live? Who rotates it? If the key is baked into CI secrets without a rotation plan, a single secret leak invalidates all signed binaries. The SM does not own this decision — it is an open question for the stakeholder (see §7). Mitigation: implementer blocks on stakeholder answer before writing release pipeline yaml; test-mode uses a generated ephemeral key.

---

## §7 Open Questions for Stakeholder

These are blockers or decision points that require input BEFORE or DURING Wave dispatch. SM will surface them now rather than mid-sprint.

### Q1 — libsodium as a build dependency (US-42.2)
The persona crypto design likely requires libsodium (NaCl/ChaCha20-Poly1305). This adds a first build dependency beyond libc + optional SQLite + libcurl. Is libsodium acceptable as a mandatory dependency, or must we use Apple Security Framework / CommonCrypto exclusively on Apple platforms? Decision affects `CMakeLists.txt` and cross-platform portability story.

### Q2 — Signing key custody and rotation plan (US-42.4)
Who holds the release signing private key? Options: (a) CI secret (GitHub Actions secret), (b) hardware security module, (c) developer-held key with manual sign step. The SM recommends (b) for a privacy-first product but this is a product + ops decision. Without a decision, US-42.4's release pipeline yaml cannot be finalized.

### Q3 — Reproducible build as a release gate (US-42.3)
Should the reproducible build check be a BLOCKING CI gate on release PRs, or advisory-only for this sprint? Making it blocking gives a hard guarantee before US-42.4 signs binaries; making it advisory lets the sprint ship even if one toolchain variant is non-reproducible. SM recommends blocking — US-42.4 depends on it — but needs PO sign-off.

These three questions are verbatim from the PO's open-questions section in `stories.md` and must be answered before the wave that touches them is dispatched.

---

## §8 DoD Summary Table

| Story | Wave | Risk | Aspect-panel | Full suite required | Commit branch |
|---|---|---|---|---|---|
| US-42.1 | 0 | HIGH | MANDATORY | YES | `impl/US-42.1` |
| US-42.2 | 0 | HIGH | MANDATORY | YES | `impl/US-42.2` |
| US-42.3 | 0 | MEDIUM | Critic-only | YES | `impl/US-42.3` |
| US-42.4 | 1 | HIGH | MANDATORY | YES | `impl/US-42.4` |
| US-42.5 | 2 | HIGH | MANDATORY | YES | `impl/US-42.5` |

---

## §9 Load-Bearing Design Constraints from Tech Lead Designs

These constraints are non-negotiable for each implementer. Deviating from them risks math voids or security holes that the aspect-panel must catch.

### US-42.1 Constraints (from `designs/US-42.1.md`)
- `hu_dp_sgd_step()` MUST accept per-sample gradients as SEPARATE ROWS — the existing `learner_cpu.c` "per-sample" is actually per-batch. The CPU backend must be REWRITTEN to surface per-sample gradients before passing to the canonical step.
- Alpha grid MUST include 256 as the upper bound (not 64) — prior evidence shows argmin saturates at grid edge for tight budgets.
- All RDP math computed in log-space to prevent overflow; use `logsumexp` for the subsampled Gaussian sum.
- Opacus oracle fixture values in `tests/fixtures/dp_accountant_oracle.json` are PLACEHOLDERS — implementer must regenerate against a real Opacus run and pin exact values before the AC-42.1.2 test goes green.
- Single-summed-gradient call shapes at the `hu_dp_sgd_step()` API MUST be rejected with `HU_ERR_INVALID_ARGUMENT`.
- CMake gate: `src/ml/dp_sgd.c` and `tests/test_dp_sgd.c` under `HU_ENABLE_ML` using the internal-`#ifdef`-wrap-with-stub-runner pattern.

### US-42.2 Constraints (from `designs/US-42.2.md`)
- Migration shred sequence: step 6 (`shred_and_unlink` on the plaintext backup) MUST execute AFTER the `.migration_done` sentinel is written, BEFORE returning `HU_OK`. The prior design omitted this step.
- Linux keyfile creation MUST use bounded `O_CREAT | O_EXCL` retry loop (≤3 attempts) — never recurse on `EEXIST`.
- `include/human/error.h` shared with US-42.1 — merge coordination required (see §5).
- libsodium fallback path to `include/human/crypto.h` ChaCha20+HMAC-SHA256 must be documented in `include/human/persona/crypto.h` via `HU_PERSONA_CRYPTO_BACKEND`.

### US-42.3 Constraints (from `designs/US-42.3.md`)
- Ship `-Werror=date-time` (NOT `-Wno-date-time` as story text suggests) — the codebase is already `-Werror`; the weaker flag would silently allow regressions.
- DO NOT add `-frandom-seed` — `-ffile-prefix-map` already canonicalizes per-TU seed input; a constant `-frandom-seed` would cause static-local symbol collisions. CMake comment MUST document this prohibition.
- Linker flags (`-Wl,--build-id=none` on linux, `-Wl,-no_uuid` on darwin) apply to `human` target ONLY — `human_tests` keeps LC_UUID so ASan symbolication works.
- Two scripts, NOT one: `scripts/check-no-date-time-macros.sh` (fast preprocess gate) and `scripts/check-reproducible-build.sh` (slow two-build sha256 diff).

### US-42.4 Constraints (from `designs/US-42.4.md`)
- Fail-closed init MANDATORY: `hu_error_t rc = HU_ERR_SECURITY_DENIED;` as baseline; ONLY the successful libsodium call path overwrites `rc = HU_OK`. No other path may set `HU_OK`.
- Signature footer format is FIXED (80 bytes): magic `"HUSG"` (4B) + version uint32 LE (4B) + signed-length uint64 LE (8B) + 64B Ed25519 detached sig.
- Truth table MUST cover all four rows: `HU_OK` → false, `HU_ERR_NOT_FOUND` → true, `HU_ERR_SECURITY_DENIED` → true, any other error → true.
- Test key generated deterministically from a FIXED SEED — not random, not CI-generated secret.

### US-42.5 Constraints (from `designs/US-42.5.md`)
- Each sub-check result struct MUST have a non-empty `evidence_path`. Aggregator refuses to emit `ok=true` if `evidence_path` is empty — prevents stubs from silently lying. Test MUST inject a stub returning YES with empty evidence and assert the doctor refuses it.
- Dispatch order in `src/main.c::cmd_doctor` (verbatim): (1) subcommands, (2) `--install` no-op stub [sprint-43 reserved], (3) `--privacy`, (4) `--fix`, (5) default. Do not change the existing `--fix` path.
- `build-reproducible` sub-check reads the compile-time constant `HU_BUILD_REPRODUCIBLE`; if that constant is absent the build MUST fail with `#error`. This means US-42.3 must land before US-42.5 is compiled.

---

_Plan status: FINAL_
