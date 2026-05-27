# Doctor WebSocket Consumer — Design

## Architecture

```
┌────────────────────────┐    WS frames     ┌─────────────────────┐
│  human-daemon (gw)     │ ───────────────▶ │ human doctor --watch│
│  event_bridge.c        │                  │ src/doctor/         │
│  hu_control_send_event │                  │   ws_consumer.c     │
└────────────────────────┘                  └────────┬────────────┘
                                                     │
                                            stdout + jsonl log
                                                     ▼
                                            ┌────────────────────┐
                                            │ operator terminal  │
                                            │ + replay file      │
                                            └────────────────────┘
```

## File layout

| File | Purpose |
|---|---|
| `include/human/doctor/ws_consumer.h` | Public surface (3 functions) |
| `src/doctor/ws_consumer.c` | Implementation (~200 LOC) |
| `src/doctor/ws_consumer_cli.c` | Argparse + main loop for the `--watch` subcommand (~80 LOC) |
| `tests/test_doctor_ws_consumer.c` | Unit tests for each AC (~150 LOC) |
| `tests/fixtures/ws_consumer/` | Sample frame fixtures + mock server |

## Public API

```c
/* include/human/doctor/ws_consumer.h */

typedef struct hu_doctor_ws_config {
    const char *host;            /* default "127.0.0.1" */
    uint16_t port;               /* default 3006 */
    const char *path;            /* default "/ws" */
    const char *event_filter;    /* NULL = all events; else comma-separated */
    const char *log_path;        /* NULL = auto-generate under ~/.human/logs/ */
    uint32_t max_reconnect_attempts; /* default 3 */
    bool quiet_stdout;           /* default false — set for tests */
} hu_doctor_ws_config_t;

/* Opens a WS connection, subscribes to events, prints + logs each
 * matching event until SIGINT or max_reconnect_attempts exhausted.
 * Returns HU_OK on clean exit, HU_ERR_IO on reconnect-failed, or
 * HU_ERR_INVALID_ARGUMENT for bad args. */
hu_error_t hu_doctor_ws_watch(hu_allocator_t *alloc,
                               const hu_doctor_ws_config_t *cfg);

/* Pure (testable): format a single event into one human-readable line.
 * Caller frees with alloc->free. Returns NULL on format error.
 * Output: "[HH:MM:SS] <event_name> seq=<N> <summary>" */
char *hu_doctor_ws_format_event_line(hu_allocator_t *alloc,
                                      const char *event_name,
                                      const char *payload_json,
                                      uint64_t seq);

/* Pure (testable): check whether an event name matches the comma-separated
 * filter string. NULL filter means "match all". */
bool hu_doctor_ws_event_matches_filter(const char *event_name,
                                        const char *filter_csv);
```

## Wire format (already established by event_bridge.c)

Server sends frames of the shape:
```json
{"type":"event","event":"<name>","payload":<json>,"seq":<u64>}
```

Where `<name>` is one of: `chat`, `agent.tool`, `error`, `health`,
`cron.job`. (Source: src/gateway/event_bridge.c lines 27-68.)

Client doesn't send anything after the WS handshake — purely a
subscriber. (If we ever need server-side filtering, that's v2.)

## Reconnect protocol

1. Initial connect attempt
2. On disconnect, log "daemon disconnected (reconnect attempt N/MAX)"
3. Sleep `2^(N-1)` seconds (1s, 2s, 4s for MAX=3)
4. Retry connect
5. On success, reset N to 1 and resume
6. After MAX failures, return HU_ERR_IO

Total max delay budget: 1 + 2 + 4 = 7 seconds across 3 attempts.
Fast enough that operators notice; slow enough not to DoS the gateway.

## Event-line summary format

Each event type gets a one-line summary (~80 chars max). Defaults to
`<event_name>` only; specialized formatters for common types:

| Event | Summary template |
|---|---|
| `chat` | `<role> <contact>: "<first 40 chars>..."` |
| `agent.tool` | `<tool_name>(<args_compact>) -> <ok/err>` |
| `error` | `<severity>: <message>` |
| `health` | `<component> <status>` |
| `cron.job` | `id=<id> kind=<kind> elapsed=<ms>ms <result>` |
| (unknown) | `<event_name>` + raw JSON (truncated to 80) |

## CLI integration

In `src/agent/cli.c` or wherever `doctor` is dispatched:

```c
/* Parse --watch flag */
if (parse_flag(argv, argc, "--watch")) {
    hu_doctor_ws_config_t cfg = {0};  /* defaults via hu_doctor_ws_config_default() */
    cfg.event_filter = parse_flag_value(argv, argc, "--events");
    return hu_doctor_ws_watch(alloc, &cfg);
}
/* else existing one-shot doctor */
```

## Testing strategy

**Unit tests (no network):**
- `hu_doctor_ws_format_event_line` for each event type
- `hu_doctor_ws_event_matches_filter` with NULL, "chat", "chat,error", etc.

**Integration tests (loopback mock):**
- Spawn a mock WS server bound to a random localhost port
- `hu_doctor_ws_watch` connects, server sends 3 events, client prints 3
- Server closes the connection mid-stream, client retries 3 times

**HU_IS_TEST guards:**
- Real `connect()` syscall gated behind `!HU_IS_TEST`
- Tests use an injected `int fd` instead of opening one

## Decision points (need user input before implementation)

**DECISION-1**: WS handshake implementation
- **Option A** — Roll our own RFC 6455 client (~100 LOC, no deps)
- **Option B** — libcurl `CURLOPT_CONNECT_ONLY` + websocketpp (deps grow)
- **Option C** — Reuse mock WS code from `tests/fixtures/` if present

**Recommendation**: Option A. h-uman's philosophy is zero deps beyond
libc; rolling our own is in keeping. The WS handshake is ~50 LOC, the
frame parser is another ~50.

**DECISION-2**: Output format
- **Option A** — Human-readable single-line (`[HH:MM:SS] event ...`)
- **Option B** — Raw JSONL (one frame per line, for `jq` piping)
- **Option C** — Both: human-readable to stdout, raw JSONL to log

**Recommendation**: Option C. Operators in the terminal want
human-readable; replay/analysis wants structured. Cost is small.

**DECISION-3**: Filter syntax
- **Option A** — Exact-match comma-separated (`--events chat,error`)
- **Option B** — Glob (`--events "agent.*,error"`)
- **Option C** — Negation (`--events "!cron.job"` = all but cron)

**Recommendation**: Option A for v1. Glob can land in v2 if anyone
asks. Negation is footgun-y (operators forget `!` and accidentally
filter EVERYTHING).

## Risks

- **DoS via reconnect storm**: mitigated by max=3 + script-side
  wrapping discipline
- **Format drift**: pin with a contract test that runs both
  `event_bridge.c::format_event` and
  `ws_consumer.c::parse_event` against shared fixtures
- **TLS expectation**: ws:// is plaintext; if anyone runs the gateway
  exposed beyond loopback, this client has no auth. Add a config
  warning if `host != "127.0.0.1"` and TLS is not used
