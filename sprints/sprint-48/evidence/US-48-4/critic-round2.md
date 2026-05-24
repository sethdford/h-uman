# Critic Re-Review: US-48-4 — Round 2

**Verdict**: RESULT_critic=CLEAN
**Branch**: sprint-48-imessage-aloop-close-impl-US48-4
**Commit added**: fb202dea (round 2)

## HIGH finding from round 1: **CLOSED**

Round 1 flagged: NULL cfg path untested at `src/daemon_reaction_poll.c:119-124`.

Round 2 commit fb202dea adds `test_reaction_collection_null_cfg_logs_warning_once()`:
- 3 invocations with NULL cfg (loop i=0,1,2)
- stderr captured via freopen() to temp file
- Guard reset before test
- Substring match: `count_substr(buf, "cfg is NULL")` ≡ exact production log substring
- Contract assertion: `HU_ASSERT_EQ(n, 1)` — pins one-shot discipline
- Also asserts substring presence; cleans up temp + buffer

## Full suite
11,708/11,708 PASS. Exit 0. New test runs as part of config_gated runner.

## Conclusion
The test is load-bearing, not rubber-stamped. The "exactly once" contract is now pinned with stderr capture + count assertion, matching the discipline of test_silent_disable_compliance.c.
