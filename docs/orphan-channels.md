# Orphan channels

Channels that have implementation files in `src/channels/` but no production
wiring path (no bootstrap call, no config schema, or no daemon poll).

This file is the canonical list. CI lint can grep it; tests can reference it;
agents auditing "what's actually wired e2e" should start here.

| Channel | LOC | What's missing | What it would take | Action |
|---------|----:|----------------|--------------------|--------|
| `signal.c` | ~706 | Config fields (http_url, account, allow_from, group_policy) only the daemon-config slot exists; bootstrap call. | Add 4 fields to `hu_signal_channel_config_t` + parser + 1 block in `bootstrap.c` mirroring `telegram` | Wire (small follow-up) |
| `mattermost.c` | ~700 | Config schema, bootstrap call, daemon-config slot. | Add `hu_mattermost_channel_config_t`, parser, bootstrap, daemon entry. Medium effort. | Experimental — defer until product wants it |
| `maixcam.c` | ~370 | Config schema, bootstrap call, daemon-config slot. AIoT vision board. | Same as above. Niche; gated on hardware availability. | Experimental — defer |
| `web.c` | ~280 | Config schema, bootstrap call, daemon-config slot. Browser-tab "channel". | Replaced by PWA channel for most use cases. | Experimental — likely subsumed by `pwa.c` |
| `cli.c` | ~170 | Production runs `human agent` directly via `cmd_agent`, not through this channel. Used in tests. | Either wire into a `--channel cli` mode or remove the channel facade. | Test-only utility — keep, document |
| `dispatch.c` | ~150 | Multiplex/router channel. Used in tests; production routes through `bootstrap.c` directly. | Could replace per-channel iteration in daemon if performance demands it. | Test-only utility — keep, document |
| `webhook.c` | ~250 | Outbound webhook formatter. The gateway HTTP server uses webhook handlers directly; this channel is unused in production. | Decide: delete or wire as a generic outbound webhook channel. | Cosmetic — document, decide later |
| `twilio_media.c` | ~80 | `send` is a no-op. Inbound hooks NULL. Twilio voice lives in `src/voice/`. | Either implement send or delete. | See FIX 5 |

## Why this matters

A channel exists in the codebase but is never invoked in production = code
that compiles, ships in the binary, but cannot serve a single user message.
That contradicts the project's "no speculative abstractions" principle and
inflates the surface area auditors must reason about.

## How to graduate a channel out of this list

1. Add a config struct in `include/human/config.h`.
2. Add a parser in `src/config_parse_channels.c`.
3. Add a daemon-config slot in `k_daemon_configs` (`src/daemon.c`).
4. Add a bootstrap block in `src/bootstrap.c` mirroring an existing channel.
5. Add an integration test in `tests/test_bootstrap.c` proving the channel is
   created when the config field is set.
6. Remove the entry from this table.

## How to delete a channel

1. Drop the `.c` and `.h` files.
2. Remove from `CMakeLists.txt`.
3. Drop the test file (or the test cases referencing it).
4. Remove from this table.
