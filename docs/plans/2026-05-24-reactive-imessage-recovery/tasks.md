# Reactive iMessage Recovery — Tasks

**Status:** Draft (2026-05-24)
**Reads:** `requirements.md`, `design.md`

Each task has a "Done when" criterion mapping back to a requirements AC.
Tasks are sequenced — earlier tasks unblock later ones.

## T1: Fix Bug 1 — wire thinkingConfig.thinkingBudget into Gemini request body
**AC:** AC-1, AC-3
**Files:** `src/providers/gemini.c` (request body construction in `gemini_chat` and `gemini_stream_chat`); `include/human/provider.h` (add `thinking_budget` field to `hu_chat_request_t` if not present); `src/agent/agent_turn.c` or wherever the model_router `thinking` value flows into the chat request (verify it's propagated).
**Effort:** S (≤1 hr including test)
**Root cause (confirmed 2026-05-24):** Gemini 3.x defaults to thinking-enabled with a large invisible thinking budget. h-uman sends `maxOutputTokens` but no `thinkingConfig`. With small max_tokens, thinking burns the entire budget → empty visible content → daemon logs `response_len=0`. **The daemon ALREADY knows to disable thinking** (log: `model route: ... thinking=0 ...`) but the value never reaches the Gemini API.
**Done when:**
  1. Sending "hello" to `+14845661687` produces a real iMessage reply on Seth's phone, verified twice with different opening messages.
  2. Daemon log shows `agent turn result: err=ok response_len=<N>` with `N > 0`.
  3. The new `gemini-3.5-flash` is wired in the model_router as the conversational tier (replacing `gemini-3-flash-preview` per the 2026-05-24 update to CLAUDE.md).
**Implementation sketch:** Add to gemini.c's request body construction, conditional on thinking_budget being explicitly set:
```c
if (request->thinking_budget_set) {
    hu_json_value_t *tc = hu_json_object_new(alloc);
    hu_json_object_set_number(alloc, tc, "thinkingBudget", request->thinking_budget);
    hu_json_object_set(alloc, generation_config, "thinkingConfig", tc);
}
```

## T1b: (Was T1 — now de-prioritized) Add diagnostic logging to agent_turn empty-response path
**AC:** AC-3
**Files:** `src/daemon.c` (at the call-site of `hu_agent_turn`)
**Effort:** S (≤30 min)
**Done when:** If response_len=0 ever happens again, the log line names provider/model/finishReason/thoughtsTokenCount/candidatesTokenCount so future debugging takes minutes not hours.
**Why deprioritized:** T1 fixes the actual root cause. T1b is defense-in-depth so if a future provider or model exhibits the same starvation pattern, it's discoverable in seconds.

## T4: Provider parity probe — Gemini Vertex agent CLI auth (Bug 2)
**AC:** AC-4
**Files:** `src/providers/gemini.c::gemini_load_adc` (add diagnostic), `src/agent/cli.c` (compare provider wiring vs daemon's)
**Effort:** M (1-3 hrs)
**Done when:** `./build/human agent run --once --prompt "hi"` succeeds with non-empty output. Bug pinned by a unit test (T6).
**Note:** Can run in parallel with T2/T3 — different code paths.

## T5: MLX response extraction for stock Gemma 4 (Bug 3)
**AC:** AC-5
**Files:** Out-of-repo `~/Documents/gemma-realtime-1/scripts/mlx-server.py` — investigate the apply_chat_template render and strip_thought_channels postprocessor
**Effort:** M-L (2-6 hrs; investigating Gemma 4 inference recipe)
**Done when:** With `mlx_local.model = mlx-community/gemma-4-31b-it-4bit` and `default_provider = mlx_local`, AC-1 verification passes. Either the chat template no longer emits empty thought block, OR strip_thought_channels extracts the final unbulleted line as the reply, OR a different mlx_lm sampling config produces proper channel-close markers.
**Note:** Lowest priority — both Gemini and OpenAI paths can satisfy AC-1+AC-2 without this. But required for the privacy thesis (local model talking to Seth).

## T6: Regression tests pinning each bug
**AC:** AC-6
**Files:** `tests/test_agent_turn_empty_response.c` (new), `tests/test_gemini_cli_adc.c` (new), `tests/test_mlx_response_extraction.c` (new — or shell test invoking mlx-server.py)
**Effort:** M (2-4 hrs total across 3 tests)
**Done when:** Each test exists, fails against the broken state, passes against the fix. CI runs them. Following `~/.claude/rules/tests-that-pin-bugs.md` — test names ARE claims, assertions enforce the correct behavior.

## T7: Provider parity AC-2 verification
**AC:** AC-2
**Files:** None — config edit + restart + live text
**Effort:** S (≤30 min, mostly waiting for restart)
**Done when:** After T3 lands, swap `default_provider` to a second value (mlx_local or anthropic, depending on T5/T3 completion order), restart, send text, observe reply. Both providers produce real replies. Documented in this directory (`ac2-verification.md`).

## T8: Update `~/.claude/projects/-Users-sethford-Projects-h-uman/memory/MEMORY.md`
**AC:** none (cleanup)
**Files:** Memory index + the `reactive_imessage_blocked.md` memory
**Effort:** XS (5 min)
**Done when:** After T7, update the memory to reflect "reactive iMessage RECOVERED 2026-MM-DD via spec docs/plans/2026-05-24-reactive-imessage-recovery/." Don't delete — historical context is valuable for future debugging.

---

## Out of this spec (deferred to follow-up specs)

- **Slice 2: Scheduled check-ins to Seth.** Add Seth as a persona contact in `~/.human/personas/seth.json` with `proactive_checkin: true, proactive_channel: "imessage", contact_id: "+18018285260", proactive_schedule: "0 10 * * *"`. Restart daemon. Wait for 10am. Verify Seth's phone receives a check-in. Estimated effort: S (≤1 hr). Cannot start until AC-1 passes.

- **Slice 3: Event-driven outreach.** Wire `hu_proactive_throttle` + `hu_proactive_budget` (already at `src/daemon.c:493`) to fire on feed/calendar/research-job events with "schedule as floor + governor as ceiling" policy. Deserves its own `/spec` covering: which events qualify, quiet-hours respect, dedup with scheduled jobs, persona-level kill switch. Estimated effort: L (multi-session). Cannot start until slice 2 ships and runs for a week's worth of dogfooding data.

- **M3 personalization repair.** Fix `seth-v3-fused` (or a successor LoRA) so it preserves base instruction-following. Out of scope here.

- **`agent CLI vs service-loop` config-reload symmetry.** `service-loop` doesn't honor SIGHUP for config reload (`g_reload_requested` is checked only by `agent CLI` at `src/agent/cli.c:1084`). This is a separate paper cut found during 2026-05-24 investigation. Worth its own small task at some point.
