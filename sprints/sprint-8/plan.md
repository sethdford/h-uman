# Sprint 8 Plan — Verifiable Privacy

## Header

| Field                 | Value                                                                             |
|-----------------------|-----------------------------------------------------------------------------------|
| Sprint                | 8                                                                                 |
| Goal                  | Replace theatrical privacy claims with structurally verifiable ones               |
| Scrum Master          | claude-sonnet-4-6 (Scrum Master agent)                                            |
| Branch                | `sprint-8-verifiable-privacy`                                                     |
| Working directory     | `/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-8-privacy`              |
| Sprint base SHA       | `ea02b08e`                                                                        |
| Dates                 | 2026-05-17 — 2026-05-28 (10 working days)                                         |
| Total estimate        | ~18 dev-days across 2 waves (M + M + S in Wave 0; M + S in Wave 1)               |
| Stories               | 5 (2 P0, 2 P1, 1 P2)                                                             |
| Sprint-7 no-touch     | `src/ml/dpo*`, `scripts/finetune-gemma.py`, `src/ml/cli.c`                        |

---

## §1 Sequencing

### Wave Diagram

```
Wave 0 — Parallel (no inter-story deps; all start on day 1)
┌──────────────────────────────────────────────────────────────────┐
│  US-8.1 (DP-SGD + RDP Accounting)          risk=HIGH   est=M    │
│  US-8.2 (Persona Encryption at Rest)       risk=HIGH   est=M    │
│  US-8.3 (Reproducible Binary Builds)       risk=MEDIUM est=S    │
└──────────────────────────────────────────────────────────────────┘
                              │
              all three committed to sprint branch
                              │
                              ▼
Wave 1 — Sequential gate (depends on all of Wave 0)
┌──────────────────────────────────────────────────────────────────┐
│  US-8.4 (Signed Release Artifacts)         risk=HIGH   est=M    │
│    depends on: US-8.3 merged (signing a non-reproducible         │
│    binary is pointless; story §Dependencies)                     │
│    (US-8.1/8.2 outputs are orthogonal; no blocking dep)         │
└──────────────────────────────────────────────────────────────────┘
                              │
               US-8.4 committed to sprint branch
                              │
                              ▼
Wave 2 — Final composition (depends on US-8.2 + US-8.3 + US-8.4)
┌──────────────────────────────────────────────────────────────────┐
│  US-8.5 (Privacy Posture Command)          risk=HIGH   est=S    │
│    depends on: US-8.2 (persona encryption check)                 │
│                US-8.3 (.reprocheck sidecar format)               │
│                US-8.4 (hu_artifact_sign_verify)                  │
│    (US-8.1 DP-ε field is optional; reports NaN if absent)       │
└──────────────────────────────────────────────────────────────────┘
```

**Rationale for 3-wave split (not 2):**
- The backlog story list suggests Wave 1 = US-8.1 + US-8.2 + US-8.3, Wave 2 = US-8.4 → US-8.5.
- The US-8.4 design makes explicit that US-8.4 depends on US-8.3 ("signing a non-reproducible binary is pointless") but does NOT depend on US-8.1 or US-8.2. US-8.5 in turn depends on all three of US-8.2, US-8.3, and US-8.4.
- Collapsing US-8.4 and US-8.5 into one "Wave 2" would require sequencing them within the wave (US-8.4 before US-8.5), which is serial by definition. The 3-wave model makes the sequencing explicit and unambiguous for dispatch.

---

## §2 Wave Assignments

### Wave 0 — Parallel (days 1–5 estimated)

| Story | Title | Risk | New Files (key) | Implementer | Verifier scope | Critic scope | Aspect-Panel | Worktree isolation |
|-------|-------|------|-----------------|-------------|----------------|--------------|--------------|-------------------|
| US-8.1 | DP-SGD + RDP Accounting | HIGH | `include/human/ml/dp_sgd.h`, `src/ml/dp_sgd.c`, `tests/test_dp_sgd.c`, `tests/fixtures/dp_accountant_oracle.json`, CMakeLists.txt (+2 lines) | `general-purpose` | Run `./build/human_tests --suite=dp_sgd`; confirm oracle fixture cross-checks pass for all 4 cases within stated tolerances; confirm AC-8.1.1 through AC-8.1.5 green; 0 ASan errors | All 5 ACs; adversarial assertions in AC-8.1.2 and AC-8.1.5 assert BLOCKED not ACCEPTED; no `local_rdp` re-implementation in test file; no modification to `src/ml/learner*.c` or `include/human/ml/learner.h` | MANDATORY | YES |
| US-8.2 | Encryption-at-Rest for Personas | HIGH | `include/human/persona_crypt.h`, `src/persona/persona_crypt.c`, `src/persona/persona_crypt_keystore_linux.c`, `src/persona/persona_crypt_keystore_darwin.c`, `tests/test_persona_encryption.c`, `tests/fixtures/persona_plaintext_sample.json` | `general-purpose` | Run `./build/human_tests --suite=persona_encryption`; confirm AC-8.2.1 through AC-8.2.5 green; confirm `src/persona/persona.c` is UNMODIFIED; confirm `HU_ERR_LEGACY_REFUSED` assertion is in test (not `HU_OK`); 0 ASan errors | All 5 ACs; adversarial assertions AC-8.2.2 and AC-8.2.4 assert BLOCKED; `#error` guard present when `HU_HAS_LIBSODIUM` undefined; atomic write sequence has all 7 steps (including dirfd fsync and rename); `sodium_memzero` on transient key buffer | MANDATORY | YES |
| US-8.3 | Reproducible Binary Builds | MEDIUM | `CMakeLists.txt` (flag block), `CMakePresets.json` (+preset), `scripts/check-reproducible-build.sh`, `scripts/check-no-date-time-macros.sh`, `.github/workflows/ci.yml` (+job), `docs/standards/engineering/reproducible-builds.md` | `general-purpose` | Run `scripts/check-reproducible-build.sh` and confirm exit 0; run `scripts/check-no-date-time-macros.sh` and confirm exit 0; inject `__DATE__` in throwaway file and confirm build fails (`-Werror=date-time`); run `./build/human_tests` full suite green | No `.c` behavior change; flag block covers all four nondeterminism sources (time, paths, symbol-seed, linker); CI job pinned to `ubuntu-22.04` + explicit gcc-12; no `cmp` exit-code swallow on diff | Critic-only sufficient | YES |

### Wave 1 — After all of Wave 0 committed (day 6 estimated)

| Story | Title | Risk | New Files (key) | Implementer | Verifier scope | Critic scope | Aspect-Panel | Worktree isolation |
|-------|-------|------|-----------------|-------------|----------------|--------------|--------------|-------------------|
| US-8.4 | Signed Release Artifacts | HIGH | `include/human/signing/release_pubkey.h`, `include/human/artifact_sign.h`, `src/security/artifact_sign.c`, `tests/test_artifact_sign.c`, `tests/test_artifact_sign_predicate.c`, `tests/fixtures/signing/*`, `scripts/sign-artifacts.sh`, `scripts/gen-release-keypair.sh`, `.github/workflows/release.yml` (+step) | `general-purpose` | Run `./build/human_tests --suite=artifact_sign`; confirm AC-8.4.1 through AC-8.4.5 green; run `scripts/sign-artifacts.sh` round-trip test; confirm `git grep 'BEGIN.*KEY'` returns zero; 0 ASan errors | All 5 ACs; adversarial assertions AC-8.4.2 and AC-8.4.3 assert exact return codes (`HU_ERR_SECURITY_DENIED` / `HU_ERR_NOT_FOUND`), not `rc != HU_OK`; private key absent from repo (CI guard added); predicate truth table exhaustive (8 cases for 3 booleans); `rc = HU_ERR_SECURITY_DENIED` initialization at top of verify function | MANDATORY | YES |

### Wave 2 — After US-8.4 committed (day 8 estimated)

| Story | Title | Risk | New Files (key) | Implementer | Verifier scope | Critic scope | Aspect-Panel | Worktree isolation |
|-------|-------|------|-----------------|-------------|----------------|--------------|--------------|-------------------|
| US-8.5 | Privacy Posture Command | HIGH | `include/human/privacy_posture.h`, `src/security/privacy_posture.c`, `tests/test_privacy_posture.c`, `tests/test_doctor_privacy_cli.c`, `src/main.c` (+90 LOC branch) | `general-purpose` | Run `./build/human_tests --suite=privacy_posture`; confirm AC-8.5.1 through AC-8.5.5 green; run `human doctor privacy` (or `human doctor --privacy`) on a fixture state dir and confirm exit code = 1 when sig missing; 0 ASan errors | All 5 ACs; adversarial test #5 (mixed persona state → false) and #16 (exit code = 1 on missing sig) assert BLOCKED; no leaf predicate returns `true` on the error branch; aggregator calls all four leaves unconditionally (test #15); `tests/test_privacy_posture.c` references `hu_privacy_posture_check`, `hu_privacy_personas_encrypted`, `hu_privacy_sbom_present` as production symbols | MANDATORY (HIGH risk: if the command lies it actively destroys trust) | YES |

---

## §3 Implementer Commit Discipline

**This rule is non-negotiable. DONE reports without a verifiable commit will be rejected and the story will re-open.**

Every implementer's last action MUST be:

```bash
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/sprint-8-privacy \
    add <specific-files-changed> \
 && git -C /Users/sethford/Projects/h-uman/.claude/worktrees/sprint-8-privacy \
    commit -m "feat(<scope>): <description>"
```

**Why `git -C` explicitly:** the cwd-resolution failure mode has burned us before. Claude Code agents reset their working directory between bash invocations. An implementer that omits `-C` and runs `git commit` will commit to whatever branch the shell happens to have checked out — which may be a completely different sprint or the main branch. Using the absolute worktree path via `-C` is the only safe pattern.

**Verification after each DONE report:** the Scrum Master runs:

```bash
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/sprint-8-privacy \
    log sprint-8-verifiable-privacy ^ea02b08e --oneline | head -20
```

If the implementer's commit is not in that list, the DONE report is rejected. A working-tree-only change that has not been committed could be wiped at any moment by a concurrent agent running `git reset --hard`.

**Commit message format** (Conventional Commits, enforced by `.githooks/commit-msg`):

```
feat(ml): add RDP moments accountant and dp-sgd step (US-8.1)
feat(persona): add encryption-at-rest with libsodium (US-8.2)
feat(build): reproducible binary builds with SOURCE_DATE_EPOCH (US-8.3)
feat(security): ed25519 artifact signing and verify (US-8.4)
feat(doctor): privacy posture command with four live checks (US-8.5)
```

---

## §4 Quality Gate Per Story

The following checklist applies to every story before it is closed. The Scrum Master will not accept a DONE report that cannot answer YES to every item.

| Gate | US-8.1 | US-8.2 | US-8.3 | US-8.4 | US-8.5 |
|------|--------|--------|--------|--------|--------|
| Committed to `sprint-8-verifiable-privacy` via `git -C` | req | req | req | req | req |
| `/verify` returned `RESULT_verifier=PASS` | req | req | req | req | req |
| Per-story critic ran immediately after DONE (not batched) | req | req | req | req | req |
| Critic returned CLEAN or LOW/INFO only (no HIGH/CRITICAL open) | req | req | req | req | req |
| `/aspect-panel` returned PASS or CLEAN | MANDATORY | MANDATORY | not req | MANDATORY | MANDATORY |
| Full test suite green (`./build/human_tests`, 10,326+ tests) | req | req | req | req | req |
| 0 ASan errors | req | req | req | req | req |
| Test file references production symbols (not re-implementations) | req | req | n/a (no `.c` test) | req | req |
| Adversarial ACs assert dangerous case is BLOCKED | 8.1.2, 8.1.5 | 8.2.2, 8.2.4 | n/a | 8.4.2, 8.4.3 | 8.5.3, 8.5.4 |
| Sprint-7 no-touch files unchanged | req | req | req | req | req |
| Evidence document at `sprints/sprint-8/evidence/US-8.N-evidence.md` | req | req | req | req | req |

**Aspect-panel is MANDATORY for US-8.1, US-8.2, US-8.4, US-8.5 (all HIGH risk).** The stories.md marks US-8.5 as HIGH risk because if the posture command lies, it actively destroys trust — this is no less dangerous than a wrong crypto implementation. Critic-only is sufficient for US-8.3 (MEDIUM risk, no production `.c` behavior change).

**Per-story critic batching is forbidden.** Sprint 1 shipped a regex bug into the production publish path because the critic was run at sprint end, after the next implementer had already built on top of broken code. Each story's critic runs the moment the implementer reports DONE, while the diff is fresh and before Wave 1 dispatch.

---

## §5 Sprint-7 Coordination — No-Touch Files

The following files belong to Sprint 7's active work surface. No Sprint-8 implementer may modify them for any reason, including "cleaning up," "fixing a merge conflict," or "adding a forward declaration for convenience."

```
src/ml/dpo*
scripts/finetune-gemma.py
src/ml/cli.c
```

If an implementer encounters a naming collision or include dependency involving these files, the correct response is to stop, document the conflict in their DONE report, and escalate to the Scrum Master. The conflict is resolved by coordination with the Sprint-7 team, not by unilateral edits.

**US-8.1 specific:** `src/ml/dp_sgd.c` is entirely new; it does NOT wire into `src/ml/learner_cpu.c` or `src/ml/learner.c`. Both of those files stay exactly as they are. The naming collision mitigation documented in the US-8.1 design (new type `hu_dp_rdp_accountant_t`, new function namespace `hu_dp_accountant_rdp_*`) resolves the coexistence problem without any touch to the Sprint-7 surface.

---

## §6 Sprint-Level Risks

### Risk 1: DP math correctness (US-8.1) — HIGH probability of subtle error, LARGE impact

The RDP moments accountant requires implementing Mironov–Talwar–Zhang 2019 Theorem 4 correctly. A factor-of-2 error in the sub-sampled Gaussian mechanism (e.g., confusing `clip_norm` with `2 * clip_norm` for sensitivity) produces a noise multiplier σ that is half what it should be. The user receives a printed ε that is roughly 4× lower than the true privacy spend — a silent lie about the product's core privacy guarantee.

**Sprint-level mitigation:** the oracle JSON fixture cross-checked against Opacus 1.5.x is the primary guard. A factor-of-2 error shifts all oracle cases by a factor that far exceeds the stated 5% tolerance. The aspect-panel runs a security-focused verifier pass specifically looking for this class of calibration error. The implementer must read the three reference papers cited in the design (Abadi 2016, Mironov 2017, Mironov–Talwar–Zhang 2019) before writing any math code.

### Risk 2: Persona encryption silent downgrade (US-8.2) — MEDIUM probability, LARGE impact

The most dangerous failure mode in US-8.2 is not a crypto bug but a control-flow bug: `hu_persona_load_legacy` falling through to plaintext load after migration, because the sentinel check logic is wrong or the classify predicate is confused. This passes all "encryption works" tests while leaving users with unencrypted data they believe is protected.

**Sprint-level mitigation:** the `hu_persona_classify_bytes` pure predicate is the single source of truth for "is this file encrypted." Both `load_encrypted` and `load_legacy` call it — no second copy of the decision. The adversarial test `test_load_legacy_refuses_encrypted_file_with_legacy_refused_error` pins both the test name and the assertion to the BLOCKED case (`HU_ASSERT_EQ(err, HU_ERR_LEGACY_REFUSED)` AND `out` untouched). Aspect-panel runs a security verifier pass looking for `if (err) return HU_OK` anti-pattern in the legacy loader.

### Risk 3: US-8.5 Wave-2 dependency stubs (US-8.5) — LOW probability in a clean sprint, LARGE impact if Wave 0/1 slips

US-8.5 depends on headers and sidecar formats delivered by US-8.2 (persona crypt), US-8.3 (`.reprocheck` sidecar), and US-8.4 (`hu_artifact_sign_verify`). If any Wave-0 or Wave-1 story slips past its expected commit deadline, the US-8.5 implementer must use forward-declared extern stubs to keep building, then rebase before final commit. Rebasing on top of three concurrent merges is the highest-probability source of merge conflicts in this sprint.

**Sprint-level mitigation:** the US-8.5 implementer does not start until all three upstream stories have confirmed commits on the sprint branch (the design's Step 1 is an explicit gate: "If not, do not start"). If Wave-0 or Wave-1 slips by more than 2 business days, the Scrum Master escalates to the user to decide whether to defer US-8.5 to Sprint-9 rather than ship a composition layer built on stubs. US-8.5 is P2 — the sprint succeeds on the P0/P1 stories alone.

---

## §7 Standup Checkpoints

Given the sprint structure (parallel Wave 0 → serial Wave 1 → serial Wave 2), the Scrum Master will call a standup at:

1. **End of day 3** — Wave 0 mid-point check: are all three implementers making progress? Any blockers (libsodium availability, CMake flag collisions, merge conflicts with main)?
2. **Before Wave 1 dispatch** — confirm all three Wave-0 commits are on the sprint branch and all three per-story critics have run. Only dispatch US-8.4 after this gate.
3. **Before Wave 2 dispatch** — confirm US-8.4 commit is on the sprint branch and its per-story critic has run. Only dispatch US-8.5 after this gate.
4. **Sprint close** — all 5 stories DONE, sprint auditor invoked, retro filed, close tag created.

---

RESULT_scrum-master=PLAN_READY
