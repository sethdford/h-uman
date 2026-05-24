# US-C1.1 — close evidence

## Commits on sprint-49-distribution

- `8794478e feat(apps,macos): US-C1.1 — Human.app bundle skeleton`
- `37a0f420 fix(tests): US-C1.1 — resolve app_bundle paths relative to source tree`
- `8ec95431 fix(apps,macos): US-C1.1 — fix verify_bundle_dependencies shell logic + harden binary-presence test`

## Quality gates

| Gate | Result | Evidence |
|------|--------|----------|
| Implementer reports commit + path | PASS | 3 commits, `git log sprint-49-distribution ^36187a1a --oneline` |
| Verifier (1st pass) | FAIL | 4 app_bundle tests used hardcoded paths that don't resolve in test cwd; full suite 11,778/11,782 |
| Implementer fix #1 | DONE | 37a0f420 — path resolution via parent-dir search |
| Verifier (2nd pass) | PASS | 4/4 app_bundle + 11,782/11,782 full suite |
| Critic (1st pass) | HAS_FINDINGS CRITICAL count=2 | (1) verify_bundle_dependencies shell logic always printed OK; (2) binary-presence test silently passed when binary absent — tests-that-pin-bugs antipattern |
| Implementer fix #2 | DONE | 8ec95431 — delegate to verify-bundle.sh + HU_SKIP_IF for absent binary |
| Verifier (3rd, independent) | PASS | 4/4 app_bundle + 1 honest SKIP + 11,782/11,782 full + verify_bundle_dependencies target works |
| Critic (2nd pass) | CLEAN | Both CRITICALs addressed; MEDIUM implicitly fixed by `DEPENDS build_app_bundle` |
| Aspect-panel | INCONCLUSIVE | Tool infrastructure issue — 5 sub-verifiers return "unknown" in 3-10s each, no RATIONALE/VERDICT. Two retries identical. Logged for retro. |

## Tool-infrastructure issue (RETRO candidate)

`python3 ~/.claude/rl/aspect_panel.py --target <text>` returned INCONCLUSIVE with all 5 aspects reporting verdict="unknown" confidence=0.5 rationale="" elapsed_s=3.5-9.6. The sub-`claude -p` calls launched but didn't produce the expected `RESULT_<aspect>-verifier=PASS|FAIL` lines the aggregator looks for. This was the FIRST aspect-panel invocation in this session — possibly a config drift on the spawned sub-processes (sandbox? PATH? prompt template length?). Workaround: rely on verifier+critic; flag for retro tune-agent + script debug.

## Decision to close

Two independent gates (verifier PASS independently re-verified by lead, critic CLEAN after 2 rounds catching 2 real CRITICALs) produced strong PASS signal. Aspect-panel infrastructure issue is not a code-quality signal. Closing US-C1.1 with documented note for retro.
