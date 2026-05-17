---
plan: docs/plans/2026-03-07-channel-conversation-chaos-tests-design.md
auditor: group-2-channels
audited_at: 2026-05-17
implemented: FULL
proven: PARTIAL
wired: PARTIAL
verdict: SHIPPED_UNWIRED
confidence: HIGH
---

## Plan Summary
Design doc for a channel-level synthetic conversation engine + chaos + pressure + real iMessage harness across all 33 channels. Pairs with the implementation plan of the same date.

## Key Claims (from the plan)
- Claim 1: `hu_<channel>_test_inject_mock` + `hu_<channel>_test_get_last_message` mock APIs added to all channels.
- Claim 2: New `tests/synthetic/channel_*` source files (`channel_harness.h`, `channel_main.c`, `channel_conversation.c`, `channel_chaos.c`, `channel_pressure.c`, `channel_imessage_real.c`).
- Claim 3: New `HU_ENABLE_CHANNEL_TESTS` CMake option + `human_channel_tests` executable.
- Claim 4: CLI flags `--chaos=message|infra|all`, `--real-imessage`, `--concurrency`, `--duration`.
- Claim 5: Real iMessage opt-in path on macOS, polls `~/Library/Messages/chat.db`.

## Evidence

### Implemented? (code exists)
- Mock inject APIs in ~25+ channels (telegram.c:1503, discord.c:1117, slack.c:1444, imessage.c:4300, signal.c:738, whatsapp.c:627, teams.c:699, matrix.c:879, irc.c:329, line.c:328, twilio.c:382, gmail.c:910, facebook.c:316, instagram.c:306, twitter.c:283, lark.c:301, dingtalk.c:291, mattermost.c:733, onebot.c:324, qq.c:312, google_chat.c:289, google_rcs.c:299, tiktok.c:338, web.c:286, maixcam.c:177, mqtt.c:567, nostr.c:556, email.c:332).
- Test_get_last_message in ~26 channels (parallel coverage).
- Harness files all present in tests/synthetic/: channel_harness.h, channel_main.c, channel_conversation.c, channel_chaos.c, channel_pressure.c, channel_imessage_real.c, channel_registry.c.
- CMakeLists.txt:3238 `option(HU_ENABLE_CHANNEL_TESTS ...)` with full target wiring (lines 3238-3293, including HU_HAS_TELEGRAM/DISCORD/SLACK/TIKTOK/IMESSAGE/SIGNAL flags).

### Proven? (tests exist)
- `tests/test_channel_all.c` exists (broad channel coverage).
- However: `human_channel_tests` is an **opt-in build target gated on HU_ENABLE_CHANNEL_TESTS=OFF by default**. It does not run in the standard `./build/human_tests` suite.
- No evidence the harness target is built in CI; not part of `ci.yml` standard matrix per the project CLAUDE.md.

### Wired? (called in runtime path / dispatch)
- Mock APIs are wired into each channel's `HU_IS_TEST` send/poll branches (e.g., imessage.c:4300+ region).
- The `human_channel_tests` executable is an isolated test harness — not wired into production dispatch (correct design).
- WIRED for test purposes; not used in default `human_tests` runner.

## Gaps
- Default CI does not exercise `human_channel_tests`. The harness exists but is dormant.
- Cannot confirm Gemini-driven scenario generation has been run end-to-end without an API key + manual invocation.
- No status report or success-metric file checked in proving the harness has been run against all channels.

## Notes
- This is the DESIGN doc; companion implementation plan tracks task-level execution.
- Marked `status: complete` in frontmatter — supported by code presence but harness is opt-in.
