# Deploy Verification Asserts Descendant, Not SHA-Equality

After deploying the daemon, verify with `scripts/verify-deploy.sh <my-commit>`
— never a hand-rolled "installed SHA equals my SHA" check.

## The hazard

Multiple sessions deploy to the same `~/.local/bin/human-daemon` within
minutes of each other, and the provenance guard
(`scripts/check-install-provenance.sh`) explicitly ALLOWS that when each
install is a descendant of the previous one. An equality contract therefore
false-FAILs on healthy forward motion.

Observed 2026-07-27: a verifier dispatched with "installed binary embeds
78a059e67" reported RESULT_verifier=FAIL because a sibling session had
redeployed `cc81f5f1c` — a *descendant carrying the same fix* — twenty
minutes later. The chain `e6b6af640 → 78a059e67 → cc81f5f1c` was exactly
what the guard is designed to produce; the contract was wrong, not the
deploy. Diagnosing the false FAIL cost a git-lineage investigation that
`merge-base --is-ancestor` answers in one call.

## Why the obvious fix is wrong

❌ **"Pin the expected SHA and redeploy if it doesn't match."** You would
un-deploy the sibling's newer work — the exact incident class the
provenance guard exists to prevent (2026-07-26: a 70-day-stale binary
un-deployed weeks of fixes).

❌ **"Just check the service is running."** A running service says nothing
about *which* code runs. The 2026-07-27 near-miss started because prod ran
a pristine older build while the session believed its fix was live.

## The right shape

```bash
scripts/verify-deploy.sh <my-commit> [-m "distinctive literal from my change"]
```

The script asserts, in order:
1. installed `HU_BUILD_SHA` **contains `<my-commit>` as ancestor-or-self**
2. installed SHA is itself **on origin/main** (not a stray local branch build)
3. optional `-m` markers appear in the binary (content-level proof)
4. the launchd service has a live pid
5. `doctor` reports 0 errors

It prints `RESULT_deploy_verify=PASS|FAIL` — hand that line to verifier
dispatches instead of re-deriving the contract in each prompt.

## When this applies / does NOT

- APPLIES: any "is my change deployed?" check on the shared daemon, and any
  verifier/critic dispatch whose contract mentions an installed binary SHA.
- DOES NOT apply: pre-push build checks (`ground-truth-over-proxy-signals.md`)
  or the install-time direction guard (that's `check-install-provenance.sh`,
  which gates candidate-vs-installed at install, not after).

## Related

- `scripts/check-install-provenance.sh` — install-time twin: refuses
  non-descendant installs; this rule is the post-install read side.
- `.claude/rules/never-cp-over-running-binary.md` — install mechanics.
- `.claude/rules/cmake-build-stale-binary.md` — the build-side staleness trap.
- `.claude/rules/session-worktree-isolation.md` — why siblings deploying
  concurrently is normal here, not an anomaly.
