# US-6: E2E daemon integration test for Site A validator path

## Approach
Build a minimal in-process daemon-shaped scenario that drives the Site A code path (`daemon.c:2095-2122`) end-to-end through a mock provider. Goal: a test that *actually fails* if the chain call is removed — i.e., the test must touch a code path that is unique to that line, not bypassable by a parallel safety net.

Two viable layers:
1. **Full daemon spawn** — `HU_IS_TEST`-guarded test that boots the daemon, registers a mock channel, posts a message, captures channel output. Heavy: requires the daemon to be re-entrant under test, which it isn't today.
2. **Stream callback unit-as-integration** — directly construct a `hu_daemon_stream_ctx_t`, register a stub bus that captures `ev.message`, and invoke `daemon_stream_event_cb` with a `HU_AGENT_STREAM_TEXT` event whose payload is `JORDAN_LEAK_F1`. This is the smallest surface that exercises line 2095 verbatim and is conventional in the existing test suite.

Choose option 2. Rationale: the AC says "spawns a daemon-like state under the test harness" — a real daemon is overkill and would tangle in TLS/socket fixtures. Option 2 satisfies AC-6.3 (deleting line 2095 will short-circuit to the legacy `strip_channel_tags` fallback at line 2127, which does NOT strip `JORDAN_LEAK_F1` — so the test fails). Document this empirically as part of AC-6.3 evidence in the PR.

The mock provider is unused in option 2 because the chunk is injected directly. If AC-6.1 strictly requires a provider, fall back to option 1 with `HU_IS_TEST` guarding — flag to PO during implementation.

Test references `hu_output_validator_chain_execute` and the persona it loads — satisfies US-10's hookify rule.

## Files to create
| File | Purpose | LOC |
|---|---|---|
| `tests/test_daemon_e2e_validator.c` | new suite, 3-4 cases | +180 |
| `tests/fixtures/personas/test_jordan.json` | minimal persona with safety rules | +30 |

## Implementation steps
1. Author the minimal persona fixture (one safety validator that triggers on `JORDAN_LEAK_F1`).
2. Stand up the stream ctx + capturing bus stub. Register a test bus that copies `ev.message` into a heap buffer.
3. Inject a `HU_AGENT_STREAM_TEXT` event with `JORDAN_LEAK_F1` as payload via `daemon_stream_event_cb`.
4. Assert the captured `ev.message` does NOT contain the leak substring.
5. Write the deletion-failure-mode evidence: locally comment out lines 2095-2122, rebuild, observe failure, restore. Document in PR.
6. Wire into `CMakeLists.txt` or test registry per project convention.

## Risks
- **Daemon ctx construction (MED/MED)**: `hu_daemon_stream_ctx_t` may have fields that require live IPC/socket setup. Mitigation: read the struct definition before writing the test (`grep -n "hu_daemon_stream_ctx_t" include/ src/daemon.c`); zero-init what we can and stub the rest. If unreachable, escalate to option 1.
- **AC-6.3 fragility (MED/SMALL)**: the fallback at line 2127 is a `strip_channel_tags` call, which does NOT match `JORDAN_LEAK_F1` (that's an assistant-leak token, not a channel tag). So removing the chain call WILL fail the test. Good. But verify this empirically — don't trust the code on read.
- **Mock-provider AC literalism (LOW/SMALL)**: AC-6.1 mentions a mock provider; option 2 doesn't use one. Document deviation in PR with rationale, or fall back to option 1.
- **Worktree state (LOW)**: depends on US-4 cached chain. If US-4 lands first, test reads `persona->outbound_chain`; if not, builds inline. Design tolerates both.

## Test strategy
- AC-6.1: persona fixture + stream ctx stub.
- AC-6.2: captured bus message excludes the leak substring.
- AC-6.3: manual deletion check documented in PR; consider a CI-skipped `#ifdef HU_DELETE_PROOF` variant.
- AC-6.4: `./build/human_tests --suite=daemon_e2e_validator` green.

## AC mapping
- AC-6.1 → fixture + stream ctx (test setup)
- AC-6.2 → assertion on captured bus payload
- AC-6.3 → PR-documented manual deletion experiment
- AC-6.4 → suite-runs-green in dev preset

## Effort
**M** — fixture + stub + new suite + manual deletion experiment. ~210 LOC. Risk concentrated in stream-ctx construction.
