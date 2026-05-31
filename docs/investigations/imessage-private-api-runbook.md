---
title: iMessage IMCore Private-API — Activation Gate + On-Device Runbook
date: 2026-05-31
feature: Native IMCore backend activation (OFF→SHADOW→LIVE)
status: Runbook — Phases 1-3-core landed; transport/injection/hookup are on-device steps
---

# IMCore Private-API — Activation & On-Device Runbook

What's **landed and CI-verified** vs. what you build + verify **on your own
SIP-off Mac**, plus the activation gate and rollback. Read
[`imessage-private-api-mechanism.md`](imessage-private-api-mechanism.md) first —
this is the operational companion.

## Status ledger

| Piece | State | Verified by |
| --- | --- | --- |
| Protocol foundation (port, line framing, mode parse) | ✅ landed | 14 unit tests |
| Config `channels.imessage.private_api.{enabled,mode}` (default OFF) | ✅ landed | config suite |
| Swift helper dylib (send/reply/tapback/edit/unsend/delete/typing) | ✅ compiles | swiftc + Mach-O inspection |
| Wire-contract JSON builders + routing predicate | ✅ landed | 11 unit tests |
| Daemon TCP server (bind/accept/recv loop) | ⛔ on-device | needs runtime |
| Injection orchestrator (killall/relaunch/env) | ⛔ on-device | needs SIP off |
| Dispatcher hot-path hookup (LIVE branch) | ⛔ on-device | needs the above |

**Default behavior is unchanged**: with `private_api` OFF (the default), none of
this runs and the dispatcher uses the hardened Tier-1 reply path (PR #242).

## Activation model (OFF → SHADOW → LIVE)

Per `~/.claude/rules/feature-gate-requires-measurement.md`. Config:

```json
{ "channels": { "imessage": { "private_api": { "enabled": true, "mode": "shadow" } } } }
```

- **OFF** (default): no injection, no server, zero cost.
- **SHADOW**: inject + connect + build the command and LOG it, but do NOT send via
  IMCore — the Tier-1 path still produces the actual message. Use this to confirm
  injection works and the command JSON is correct against real chats, with zero
  risk to what gets sent. `hu_imessage_private_should_route` returns false in
  SHADOW by design.
- **LIVE**: `should_route` returns true → the daemon sends the command to the
  dylib and IMCore drives the real action.

**Do not flip to LIVE on source defaults.** Set it via the launchd plist env or
config on your machine, and only after the SHADOW + blind-A/B checks below.

## Step 1 — disable SIP (one-time, machine-wide)

```
Shut down → Recovery (hold Power on Apple Silicon) → Utilities ▸ Terminal
csrutil disable
Restart
```
Verify after reboot: `csrutil status` → "System Integrity Protection status: disabled."
Re-enable any time with `csrutil enable` (this disables the whole feature; the
daemon falls back to Tier-1).

## Step 2 — build + sign the dylib

```bash
bash apps/imessage-helper/build.sh --arm64e --sign "Apple Development: <you>"
# → build/imessage-helper/libIMHelper.dylib  (arm64e, __dylib_init constructor)
```
Confirm: `nm libIMHelper.dylib | grep __dylib_init` and `file …` → "Mach-O … arm64e".

## Step 3 — implement the three on-device pieces

These compile against the landed, tested core (builders + predicate +
protocol). Build them in `src/channels/imessage_private/`, guard side effects
with `HU_IS_TEST`, and verify each against a real Messages.app session.

**3a. TCP server** (`server.c`): bind `127.0.0.1:hu_imessage_private_port_for_uid(getuid())`,
listen, accept the dylib. Feed recv bytes into `hu_imsg_line_buf_t`; pop lines
with `hu_imsg_line_buf_next`; parse `{"event":"ready"}` to mark connected and
`{"transactionId":…}` to resolve a pending command. Send commands built by
`hu_imessage_private_build_*` + `"\r\n"`. Track `helper_connected` for the
routing predicate.

**3b. Injection** (`inject.c`): per the blueprint's `injection.rs` pseudocode —
atomic-install the dylib to `~/Library/Application Support/human/private-api/`
(NEVER `cp` over a loaded dylib — see `never-cp-over-running-binary.md`),
`killall Messages`, relaunch `/System/Applications/Messages.app/Contents/MacOS/Messages`
with `DYLD_INSERT_LIBRARIES=<dylib>`, hide after 5s via osascript, retry ≤5.

**3c. Dispatcher hookup** (`daemon_message_router.c`, THREADED/TAPBACK branches):
```c
if (hu_imessage_private_should_route(cfg->private_api.enabled,
                                     cfg->private_api.mode,
                                     hu_imessage_private_server_connected(srv))) {
    /* build with hu_imessage_private_build_send(... parent_guid ...) and
     * send via the server; on error/timeout fall through to Tier-1. */
} /* else: existing Tier-1 path (unchanged) */
```
Keep Tier-1 as the fallback on ANY private-API error — never drop a message.

## Step 4 — verify each action on-device (SHADOW first, then LIVE)

Send yourself test messages and confirm in Messages.app:

| Action | Test | Expected |
| --- | --- | --- |
| send | LIVE, plain reply | blue bubble, no `↩` quote |
| threaded reply | reply to a non-last message | native nested thread bubble (not a text quote) |
| tapback | react "love" to a message | real heart tapback on that bubble |
| edit | edit a just-sent message | "Edited" badge, recipient sees new text |
| unsend | unsend within 2 min | bubble removed both sides |
| typing | trigger a reply | real "…" bubble appears on recipient |

For each: in SHADOW, confirm the daemon log shows the correct command JSON and
the message still goes out via Tier-1; in LIVE, confirm IMCore performed it.
Tail the dylib log: `log stream --predicate 'subsystem == "com.human.imessage-helper"'`.

## Step 5 — the LIVE flip gate (blind A/B)

Do not leave LIVE on for production until a blind A/B (use the
`blind-ab-pipeline` skill) shows native reply/tapback is judged **at least as
human** as the Tier-1 quote — the whole reason this exists. Until then, run LIVE
only in controlled self-tests.

## Rollback

- Fastest: set `private_api.mode` to `off` (or `enabled:false`) and restart the
  daemon → Tier-1 only, no injection.
- Full: `csrutil enable` + reboot → injection impossible regardless of config.
- The injected dylib lives only in the relaunched Messages.app process; quitting
  Messages clears it (the daemon re-injects on next start only if enabled).

## Cross-references
- `imessage-private-api-mechanism.md` — the RE blueprint (selectors, protocol, injection).
- `apps/imessage-helper/README.md` — dylib build + SIP prerequisite.
- `src/channels/imessage_private/{protocol,client}.c` — the landed, tested core.
- `~/.claude/rules/feature-gate-requires-measurement.md` — the OFF→SHADOW→LIVE contract.
- `~/.claude/rules/never-cp-over-running-binary.md` — atomic dylib install.
