# Synthetic Pressure Tests Design

**Date:** 2026-03-07
**Status:** Implemented
**Risk:** Medium (new executable, no changes to core src/)

## Motivation

human needs end-to-end validation beyond unit tests. The synthetic test harness:

- Exercises the real binary and gateway under realistic conditions
- Uses Gemini to dynamically generate diverse test scenarios
- Detects regressions in CLI, HTTP, WebSocket, and agent APIs
- Measures latency, throughput, and error rates under load

## Architecture

```
┌──────────────────────────────────────────────────┐
│                human_synthetic                   │
│                                                    │
│  main.c ── orchestrator                           │
│    ├── gemini.c ── Gemini API client              │
│    ├── cli.c ── CLI subprocess tests              │
│    ├── gateway.c ── HTTP endpoint tests           │
│    ├── ws.c ── WebSocket protocol tests           │
│    ├── agent.c ── Agent chat loop tests           │
│    ├── pressure.c ── fork-based load testing      │
│    └── regression.c ── save/load failure data     │
└──────────────────────────────────────────────────┘
```

## Test Layers

| Layer | What it tests | Method |
|-------|---------------|--------|
| CLI | Binary CLI commands (version, status, doctor, etc.) | `hu_process_run` subprocess |
| Gateway HTTP | REST endpoints (health, models, status, chat) | `hu_http_get` / `hu_http_post_json` |
| WebSocket | JSON-RPC control protocol | `hu_ws_connect` / `hu_ws_send` / `hu_ws_recv` |
| Agent | `/v1/chat/completions` end-to-end | HTTP POST to gateway |
| Pressure | Concurrent load on all endpoints | `fork()` worker processes |

## Gemini's Role

1. **Test generation**: Gemini produces JSON arrays of test cases with inputs, expected outputs, and edge cases
2. **Correctness evaluation**: For subjective tests (agent responses), Gemini evaluates whether the output is reasonable

API: `POST https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent`
Response format: Structured JSON via `responseMimeType: "application/json"`

## Gateway Lifecycle

The harness manages the gateway as a child process:
1. Writes a temp config to `/tmp/hu_synth_XXXXXX/.human/config.json` with the desired port
2. Forks and sets `HOME` to the temp dir before `execl`
3. Polls `/health` until ready (up to 15s)
4. Runs tests
5. Sends `SIGTERM` and `waitpid`
6. Cleans up temp directory

## Regression Capture

Failed tests are saved as JSON files in `--regression-dir`:
```json
{
  "name": "test_name",
  "category": "cli|gateway|websocket|agent",
  "input": { ... },
  "actual": "...",
  "verdict": "FAIL",
  "reason": "exit=1 expected=0",
  "latency_ms": 42.5
}
```

Replay with `--replay DIR` to view previously captured failures.

## Build

```bash
cmake -B build -DSC_ENABLE_SYNTHETIC=ON -DSC_ENABLE_ALL_CHANNELS=ON
cmake --build build -j$(nproc)
```

## Usage

```bash
# Full suite
GEMINI_API_KEY=... ./build/human_synthetic --binary ./build/human

# CLI only (no gateway needed)
GEMINI_API_KEY=... ./build/human_synthetic --binary ./build/human --cli-only

# Custom port and count
GEMINI_API_KEY=... ./build/human_synthetic --binary ./build/human --port 4000 --count 50

# With regression capture
GEMINI_API_KEY=... ./build/human_synthetic --binary ./build/human --regression-dir ./failures

# Replay failures
./build/human_synthetic --replay ./failures --verbose

# Pressure only
GEMINI_API_KEY=... ./build/human_synthetic --binary ./build/human --pressure-only --concurrency 8 --duration 30
```

## Metrics Output

```
[synth] CLI          18/20 passed, 2 FAILED  (avg 45.2ms, p99 120.3ms)
[synth] Gateway      19/20 passed, 1 FAILED  (avg 12.1ms, p99 35.7ms)
[synth] WebSocket    14/15 passed, 1 FAILED  (avg 8.3ms, p99 22.1ms)
[synth] Agent         9/10 passed, 1 FAILED  (avg 1250.0ms, p99 3200.0ms)
[synth] Pressure     4 workers, 10s, 1523 reqs, 152.3 req/s, 1520 ok, 3 fail
[synth] Total: 60/65 passed, 5 failed, 0 errors
```

## Security

- `GEMINI_API_KEY` is read from environment only, never logged
- Temp config directories are cleaned up after gateway stops
- No secrets are included in regression files
- Gateway runs on localhost only

## Design Decisions

1. **Separate executable** (not part of `human_tests`): avoids network/Gemini dependency in unit tests
2. **Gemini for generation**: more diverse and realistic than hand-written test matrices
3. **fork-based pressure**: lightweight, no thread library dependency, clean process isolation
4. **Temp config via HOME**: avoids modifying the gateway's CLI arg parsing for a test-only feature
