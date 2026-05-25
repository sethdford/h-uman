# Critic Report: US-48-4 — Round 1

**Verdict**: RESULT_critic=HAS_FINDINGS severity=HIGH count=1 (+1 MED, +1 LOW)
**Branch**: sprint-48-imessage-aloop-close-impl-US48-4
**Commit reviewed**: 9d3b82c0

## HIGH (blocks closure)
**src/daemon_reaction_poll.c:119-124** — NULL cfg path emits a warning via `hu_log_warn_once` but is NOT tested. Test `test_reaction_collection_disabled_returns_ok` always passes non-NULL cfg. Regression where NULL cfg silently returns HU_OK without logging would not be caught.

**Fix**: add `test_reaction_collection_null_cfg_logs_warning_once` calling `hu_daemon_reaction_poll_tick(NULL, 0, NULL)` and asserting log captures exactly one warning emission.

## MED (deferred)
**tests/test_config_gated_subsystems.c** — test names imply "exactly once" but assertions only check return codes. The heavy-lifting `test_silent_disable_compliance.c` does count log emissions and pins the spirit (lines 128-129, 178-179: `count_substr(buf, "disabled by config") == 1`). If the heavier suite is ever removed, the lighter one masks regressions. **Defer** — stronger tests exist and pass.

## LOW (deferred)
**include/human/core/log.h:73-78** — macro doc example uses one guard name; implementation uses four. Cosmetic.

## Audit completeness (independently verified)
- Variant grep `!cfg->.*\.enable` in src/ → 1 site (daemon_reaction_poll.c, the target). ✓
- Pre-existing audit.c gate (src/security/audit.c:690-695) already has `hu_log_info_once` discipline applied in prior commit; tested in test_silent_disable_compliance.c:190-237. ✓
- No missed sites.

## Cross-agent regression
Files touched: CMakeLists.txt, src/daemon_reaction_poll.c, tests/test_config_gated_subsystems.c (new), tests/test_main.c, tests/test_silent_disable_compliance.c. No memory/, no agent/, no daemon.c tick registration. No cross-story risk.
