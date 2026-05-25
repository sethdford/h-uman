# US-C1.2 — close evidence

## Commits on sprint-49-distribution

- `97e2a71c feat(release,macos): US-C1.2 — .pkg installer build script`
- `a736e53c fix(release,macos): US-C1.2 — launchd absolute log path + sed safety + true e2e test`

## Quality gates

| Gate | Result | Evidence |
|------|--------|----------|
| Implementer reports commit + dry-run | DONE | 97e2a71c; `bash build-pkg.sh --dry-run` exit 0; 11,797/11,797 full suite |
| Verifier (1st, independent) | PASS | 15/15 pkg_builder + 11,797/11,797 + daemon healthy |
| Critic (1st pass) | HAS_FINDINGS CRITICAL=1 HIGH=1 MEDIUM=1 | (CRITICAL) launchd `~/.human/...` silent log loss — tilde not expanded by launchd; (HIGH) sed delimiter+ampersand corruption on `$VERSION`; (MEDIUM) tests grep-not-execute (tests-that-pin-bugs antipattern, repeat of US-C1.1's pattern) |
| Implementer fix | DONE | a736e53c — `/var/log/human/...` absolute, sed `|` delimiter + `&` escape, new `test_pkg_builder_dry_run_succeeds_e2e` |
| Verifier (2nd, independent) | PASS | 16/16 pkg_builder (added e2e) + 11,798/11,798 full suite + plutil-lint OK + log paths now absolute |
| Critic (2nd pass) | CLEAN | All 3 prior findings addressed; no new HIGH/CRITICAL |
| Aspect-panel | SKIPPED | Infrastructure misfire from US-C1.1 not retried per documented retro note |

## Tool-protocol observation (RETRO)

**Critic-trailing-off pattern continues** — critic returns mid-investigation prose without RESULT line until nudged via SendMessage. Discovered workaround: instruct "write the single-line verdict at the TOP of your response, then justify below." Re-review prompt for US-C1.2 used this pattern; critic returned `RESULT_critic=CLEAN` on the first response with no nudge needed.

Recommendation for tune-agent: bake "verdict-first" formatting into critic agent prompt template.

## Decision to close

Verifier PASS (independent re-run), critic CLEAN after addressing 3 real findings (1 CRITICAL that would have caused operators to lose all daemon logs in production). Closing US-C1.2.

## Wave 1 status

| Story | Commits | Status |
|-------|---------|--------|
| US-C1.1 | 3 | CLOSED (verifier PASS, critic CLEAN after 2 CRITICALs fixed) |
| US-C1.2 | 2 | CLOSED (verifier PASS, critic CLEAN after 1 CRITICAL + 1 HIGH + 1 MEDIUM fixed) |

Wave 1 complete. 5 commits total on sprint-49-distribution. Ready for Wave 2 dispatch.
