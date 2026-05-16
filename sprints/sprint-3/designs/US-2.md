# US-2: Cover Pattern C escape paths — daemon bus broadcast + format.c channel path

## Approach
Two distinct sites, both replace bare legacy-stripper calls with `hu_output_validator_chain_execute`.

**Site A — `src/daemon.c:2089` (`daemon_stream_event_cb`, `HU_AGENT_STREAM_TEXT` branch).** This callback fires per stream chunk for bus broadcast. Read confirms it currently calls only `hu_conversation_strip_channel_tags`. Replace with a chain execute call that uses a chain reachable from `hu_daemon_stream_ctx_t *sc` (extend the ctx with a `const hu_output_validator_chain_t *chain` pointer that the daemon sets at stream-init time from the persona). Allocate the chain inline for this sprint (US-4 will cache it on persona next sprint). On `HU_VALIDATOR_REJECT`, drop the chunk (`return`); on `PASS`/`REWRITE`, copy `final_text` into `ev.message` and free `final_text` via `hu_chain_result_free`.

**Site B — `src/channels/format.c:586` (the `imessage` branch of channel format).** Replace the `strip_markdown → strip_ai_phrases` pair (lines 589–598) with a single call to `hu_output_validator_chain_execute`. Markdown stripping is a pre-format step (formatting, not safety) and stays as-is; only the `strip_ai_phrases` call is the safety-stripper escape path. Refactor: keep `hu_channel_strip_markdown` call, then run the chain over its output instead of `hu_channel_strip_ai_phrases`. The chain must be passed in — extend the channel-format signature to accept `const hu_output_validator_chain_t *chain` (or thread it via channel context).

Both sites need a `hu_validator_context_t` — populate with channel name + provider name from caller-side context.

## Files to modify
- `src/daemon.c:2072–2094` — extend `hu_daemon_stream_ctx_t` (find struct via `grep -n "hu_daemon_stream_ctx_t" src/daemon.c include/`), thread chain pointer, replace strip with chain execute.
- `src/channels/format.c:586–598` — replace stripper pair with chain execute; keep `strip_markdown`.
- `include/human/channels/format.h` (or equivalent) — add chain param to the channel format entry point. Audit all callers.
- `tests/test_pattern_c_paths.c` (new) — inject assistant-closer token "I hope this helps!" through both paths, assert absence in output.

## Risk
- **Signature churn:** adding chain param to `hu_channel_format_*` is a breaking API change. Acceptable internal break; audit `grep -rn "hu_channel_format" src/ tests/`.
- **Bus broadcast semantics:** the bus chunk is partial streaming text — chain validators that expect full responses (e.g., `assistant_closer`) may misfire on partials. Verify: re-read each validator's contract. If full-message-only, gate Site A to non-streaming or final-chunk only.
- **Chain allocation cost per chunk** (streaming): hot path. Acceptable until US-4 caches it; this sprint's E2E test (US-6, deferred) was meant to catch perf, so monitor manually.

## Test strategy
- New `tests/test_pattern_c_paths.c`: feed leak token through each site's wrapper helper, assert clean output.
- Pin existing `tests/test_format.c` cases for imessage formatting — no regression.
- Grep guard for AC: `grep -n "strip_channel_tags\|strip_ai_phrases" src/daemon.c src/channels/format.c` should hit only `format.c:589` (markdown) after change.

## Sequencing
Independent of US-1 and US-3 — can run in parallel. Streaming-chunk concern (Site A) is the only blocker; if validators are full-message-only, narrow Site A to the final assembled message rather than per-chunk.

## Effort estimate
**M** — two sites, signature change for channel format, new test file, ~200 LOC. Streaming validator semantics is the unknown that could push to L.
