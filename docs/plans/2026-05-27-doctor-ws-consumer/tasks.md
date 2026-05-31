# Doctor WebSocket Consumer — Tasks

## T1: Pure helpers (~30 LOC, ~30 min)
**AC:** AC-3 (filter), AC-2 (format) — partial
**Files:** `src/doctor/ws_consumer.c`, `include/human/doctor/ws_consumer.h`
**Effort:** S (30 min)
**Done when:**
- `hu_doctor_ws_event_matches_filter(name, filter_csv)` returns bool;
  NULL filter = true; comma-split tokens; trim whitespace
- `hu_doctor_ws_format_event_line(alloc, name, payload_json, seq)`
  returns one-line string; default formatter handles unknown event
  types; specialized formatters for chat/agent.tool/error/health/cron.job
- Unit tests in `tests/test_doctor_ws_consumer.c` pin every branch
- 5+ specific assertions, no tautologies

## T2: WS handshake (~50 LOC, ~60 min)
**AC:** AC-1 (connect)
**Files:** `src/doctor/ws_consumer.c` (new internal helpers)
**Effort:** M (1 hr)
**Done when:**
- TCP connect to `host:port`
- HTTP GET upgrade with valid `Sec-WebSocket-Key` (16 random bytes,
  base64-encoded)
- Parse 101 Switching Protocols response, verify
  `Sec-WebSocket-Accept` is `base64(sha1(key + GUID))`
- Returns the connected fd or HU_ERR_IO
- HU_IS_TEST stub returns a fake fd that reads from an injected fixture
- Test: mock server accepts handshake, asserts client sent correct headers

## T3: Frame parser (~60 LOC, ~60 min)
**AC:** AC-2 (event print)
**Files:** `src/doctor/ws_consumer.c`
**Effort:** M (1 hr)
**Done when:**
- Reads WS frames per RFC 6455 (FIN bit + opcode + payload length 7/16/64 bit)
- Handles text (0x1), close (0x8), ping (0x9), pong (0xA)
- Auto-replies to ping with pong (keep-alive)
- Strips masking key if present (server-side frames don't mask, but be
  defensive)
- Returns one parsed-text-payload string per call OR signals close
- Tests: parse a chat event, a close frame, a ping frame

## T4: Watch loop + reconnect (~40 LOC, ~30 min)
**AC:** AC-4 (graceful disconnect)
**Files:** `src/doctor/ws_consumer.c`
**Effort:** S (30 min)
**Done when:**
- `hu_doctor_ws_watch(alloc, cfg)` opens connection, loops reading
  frames, formats + prints, logs to JSONL
- On disconnect: log to stderr, sleep `2^(N-1)` seconds, reconnect
- After cfg->max_reconnect_attempts failures: return HU_ERR_IO
- SIGINT handler sends WS close + returns HU_OK
- Test: mock server sends 3 events then closes; client reconnects 3
  times then exits 1

## T5: CLI wiring (~30 LOC, ~30 min)
**AC:** AC-7 (doctor integration)
**Files:** `src/doctor/ws_consumer_cli.c`, modify `src/app/main.c` or
wherever `doctor` is dispatched (likely `cmd_doctor`)
**Effort:** S (30 min)
**Done when:**
- `human doctor --watch` invokes `hu_doctor_ws_watch` with defaults
- `--events <csv>` flag plumbs through to `cfg.event_filter`
- `--host`, `--port` flags also plumb (for testing against non-default daemon)
- `human doctor --help` mentions `--watch` in usage
- Test: argv parsing pinned

## T6: JSONL session log (~20 LOC, ~15 min)
**AC:** AC-5 (session log)
**Files:** `src/doctor/ws_consumer.c`
**Effort:** XS (15 min)
**Done when:**
- Opens `~/.human/logs/doctor-watch-<YYYYMMDD-HHMMSS>.jsonl` with
  mode 0600 at startup
- Each received event is appended as `{"ts":..., "event":..., "payload":..., "seq":...}\n`
- File path printed to stderr on startup so operators can find it
- Test: pin file existence + contents after a 3-event run

## T7: Contract test with event_bridge.c (~20 LOC, ~30 min)
**AC:** Risk "format drift"
**Files:** `tests/test_doctor_ws_contract.c`
**Effort:** S (30 min)
**Done when:**
- Test calls `hu_control_send_event` (the producer) with a known event
  + payload, captures the broadcast text via a mock WS server
- Then feeds the captured text into `hu_doctor_ws_parse_event` (the
  consumer)
- Asserts roundtrip: producer-emitted event_name + payload == parsed values
- If event_bridge.c changes the envelope shape, this test catches it
  before the watcher silently misses events

## Implementation order

T1 → T2 → T3 → T4 → T5 → T6 → T7

T1 + T7 land first (pure helpers + contract): fast feedback, no
network risk.
T2-T6 land sequentially because each builds on the previous.

## Estimated total effort

3.5 hours including tests + verification. One focused session.

## Definition of Done

- 7 ACs pass per test runner
- `./build/human_tests --filter=ws_consumer` shows all-green
- `./build/human doctor --watch --events chat,error` connects to a
  running daemon and prints events
- `~/.claude/rules/never-cp-over-running-binary.md` rule applied to
  the install step
- Commit message threat/risk notes: this client opens a TCP socket to
  the loopback gateway — no new attack surface; no auth added (gateway
  already enforces require_pairing for non-public endpoints)
