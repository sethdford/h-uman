---
plan: docs/plans/2026-03-07-channel-conversation-chaos-tests-plan.md
auditor: group-2-channels
audited_at: 2026-05-17
implemented: FULL
proven: PARTIAL
wired: PARTIAL
verdict: SHIPPED_UNWIRED
confidence: HIGH
---

## Plan Summary
12-task implementation plan to build the channel chaos/conversation/pressure/real-iMessage harness. Companion to the design doc of the same date.

## Key Claims (from the plan)
- Task 1: iMessage mock inject API at `src/channels/imessage.c` ~lines 23-34/103-109/378-381.
- Task 2: Same pattern applied to telegram, discord, slack, signal.
- Task 3: Same pattern applied to Tier 2/3 channels.
- Task 4-10: New harness files in `tests/synthetic/`.
- Task 11: CMake `HU_ENABLE_CHANNEL_TESTS` integration.
- Task 12: Integration verification.

## Evidence

### Implemented? (code exists)
- Task 1 (iMessage): `hu_imessage_test_inject_mock` at src/channels/imessage.c:4300; extended variants `_ex`, `_ex2`, `_full` at 4307, 4314, 4338; `get_last_message` at 4402.
- Task 2 (telegram/discord/slack/signal): all 4 have inject + get_last (telegram.c:1503/1523, discord.c:1117/1137, slack.c:1444/1464, signal.c:738/758). Discord and Slack also have `_full` variants (telegram.c:1532, discord.c:1146, slack.c:1473).
- Task 3 (Tier 2/3): Implemented in whatsapp, teams, matrix, irc, line, facebook, instagram, twitter, google_chat, google_rcs, lark, dingtalk, mattermost, onebot, qq, twilio, web, gmail, tiktok, mqtt, nostr, email, maixcam. (See sibling design doc evidence file.)
- Task 4-10: All harness files present (`tests/synthetic/channel_harness.h`, `channel_main.c`, `channel_conversation.c`, `channel_chaos.c`, `channel_pressure.c`, `channel_imessage_real.c`, `channel_registry.c`).
- Task 11: CMakeLists.txt:3238-3293 `HU_ENABLE_CHANNEL_TESTS` option + `human_channel_tests` target + HU_HAS_<CHANNEL> conditional flags.

### Proven? (tests exist)
- Unit tests for inject APIs exist (referenced by Task 1 step 6 pointing to `tests/test_channel_all.c`).
- Harness itself is the test infrastructure — but it is opt-in and runs against Gemini API (requires GEMINI_API_KEY).
- No checked-in evidence (regression dumps, log artifacts) showing the harness has been run end-to-end.

### Wired? (called in runtime path / dispatch)
- Mock APIs wired into each channel under `HU_IS_TEST` (correct).
- `human_channel_tests` is an opt-in executable, not part of default `./build/human_tests`.
- Not invoked by `ci.yml` per project CLAUDE.md (which lists `c build + 10,000+ tests` but no channel harness gate).

## Gaps
- Tasks 4-10 may have partial gaps in chaos/pressure/imessage_real coverage (would require reading those files to confirm full feature parity with design doc).
- Real iMessage e2e path requires macOS + Messages.app + manual target — never run in CI.
- Default build does not produce `human_channel_tests`; only available with `-DHU_ENABLE_CHANNEL_TESTS=ON`.

## Notes
- Plan marked `status: complete`. Code presence corroborates that.
- Verdict SHIPPED_UNWIRED because harness target is dormant in default CI/build flow.
