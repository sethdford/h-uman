# Doctor WebSocket Consumer — Requirements

## Problem

`human doctor` today is one-shot: it reads metrics + config files, prints
diagnostics, exits. To see CURRENT activity (a fresh chat event, a tool
call, a cron job dispatch, an error log), operators must `tail -f` the
daemon's log file or query individual control-protocol methods one-by-one.

The gateway already BROADCASTS structured events over WebSocket
(`src/gateway/event_bridge.c`) — they're consumed by the web UI (`ui/`)
but no terminal-side consumer exists. Operators end up using `tail -f`,
which has no schema, no filtering, no fan-out, no replay.

## Goal

Add `human doctor --watch` (alias `human doctor watch`) that connects to
the daemon's WebSocket endpoint, subscribes to events, and prints a
filterable real-time stream to stdout. Optional `--events chat,error`
flag filters by event name.

## What success looks like (testable ACs)

**AC-1 — Connect + subscribe**
Given a running daemon with the gateway up on `127.0.0.1:3006`, when
`human doctor --watch` is invoked, the client opens a WebSocket
connection to `ws://127.0.0.1:3006/ws` within 5 seconds.
*Test:* mock daemon WS server, assert handshake completes.

**AC-2 — Real-time event print**
For each `{"type":"event","event":"<name>","payload":{...},"seq":N}`
frame received, print one line to stdout:
`[<HH:MM:SS>] <event_name> seq=<N> <one-line-summary-of-payload>`
*Test:* inject 3 events, assert stdout has 3 matching lines.

**AC-3 — Event filter**
With `--events chat,error`, only frames whose `event` field matches one
of the comma-separated names are printed. Other frames are silently
dropped.
*Test:* inject events `chat`, `health`, `error`, `cron.job`. With
`--events chat,error`, stdout has exactly 2 lines (the chat + the error).

**AC-4 — Graceful disconnect**
On Ctrl+C, the client sends a WS close frame and exits 0.
On daemon-side disconnect, the client logs "daemon disconnected
(reconnect attempt 1/3)" to stderr and tries to reconnect with
exponential backoff (1s, 2s, 4s). After 3 failures, exit 1.
*Test:* spawn daemon, send 1 event, kill daemon, assert client logs
3 reconnect attempts then exits 1.

**AC-5 — Doctor session log**
By default, append each event as JSONL to
`~/.human/logs/doctor-watch-<YYYYMMDD-HHMMSS>.jsonl`. The path is
printed on startup so operators can grep later.
*Test:* run watch for 2 events, assert the file exists with 2 JSON
lines, schema-correct.

**AC-6 — Reuses existing WS client code if present**
If `src/providers/ws_client.c` (or similar) exists, use it. Otherwise
implement a minimal WS-13 (RFC 6455) handshake inline: HTTP upgrade
request, Sec-WebSocket-Key, parse server response, then read frames.
*Test:* compile-time check that the new file links cleanly without
new external libraries.

**AC-7 — Doctor integration**
`human doctor` (no flag) is unchanged. `human doctor --watch` is the
new entry point. `human doctor --help` lists `--watch` in the usage.
*Test:* `human doctor --help | grep -q -- '--watch'`.

## Out of scope (v1)

- Event replay (subscribing to historical events from before connect).
  Server doesn't store them, so a v2 would need a ring buffer first.
- TLS / wss:// — gateway is localhost-only by default; remote operators
  use SSH tunnel.
- Multi-daemon subscription (one client → many daemons).
- Color output / TUI — keep it grep-friendly.

## Non-goals

- Replacing the current `tail -f` workflow for operators who prefer it.
  Both can coexist.
- Pushing config changes back over WS (control-protocol RPCs are still
  HTTP/POST).

## Dependencies

- Gateway WS endpoint at `ws://127.0.0.1:3006/ws` (already shipped)
- `event_bridge.c` event broadcasting (already shipped)
- The current `hu_doctor_*` command framework (already shipped)

## Risks

- **Reconnect storms**: a client in a `while true; do human doctor --watch
  || sleep 1; done` loop with no backoff at script level could DoS the
  daemon during a brief gateway hiccup. Mitigation: the client itself
  has 3-attempt backoff; script-side wrappers should add their own.
- **Privacy**: event payloads may contain sensitive content (chat text,
  contact handles). The log file at `~/.human/logs/doctor-watch-*.jsonl`
  is user-readable only (mode 0600) but operators must be aware before
  sharing the file.
- **Format drift**: if `event_bridge.c` changes the broadcast envelope
  shape, the consumer must follow. Pin with a contract test that asserts
  both sides agree on the `{type, event, payload, seq}` shape.
