# Reactive iMessage Recovery — Design

**Status:** Draft (2026-05-24)
**Reads:** `requirements.md`
**Feeds:** `tasks.md`

## Evidence base (the diagnosis we will NOT redo)

From the live 2026-05-24 session (text "Hi" to `+14845661687`):

```
[human] processing batch for +14845661687: "Hi" (group=0)
[human] llm_decides: forwarding to LLM (lean prompt) for +14845661687
[human] classify result: action=0 delay=0 for +14845661687
[director] meta: action=text delay=4s reaction=0 burst=0
          dir=Casual, slightly playful, maybe mention the cat or his current focus to keep it grounded.
[human] director delay: 4000 ms (read after 529ms)
[human] model route: gemini-3.1-flash-lite-preview (tier=reflexive, src=heuristic, thinking=0)
[imessage] IMCore loaded but daemon connection failed (expected on macOS 26+, falling back to AX)
[imessage] typing started via AX compose field
[human] director: text response, sending to LLM
[human] calling agent turn for +14845661687...
[human] agent turn result: err=ok response_len=0
[human] empty assistant response (consecutive=1) — check MLX server, response_guard retry, and cloud fallback logs
[human] w14: enqueued counterfactual rehearsal for +14845661687
```

`response_len=0`, `err=ok`, NO `[http]` log line for the agent call. Director worked (Gemini call succeeded). Agent call appears to have either skipped the LLM or had its streaming response swallowed.

## Three independent bugs, in priority order

### Bug 1: Gemini 3.x thinking-token budget starvation (ROOT CAUSE — confirmed 2026-05-24)
**Locus:** `src/providers/gemini.c` — request body builder (around the `generationConfig` JSON construction in `gemini_chat`).
**Confirmed via live probe 2026-05-24:** With `maxOutputTokens=80` and no `thinkingConfig`, `gemini-3.5-flash` returns `text: ''`, `candidatesTokenCount: 4`, **`thoughtsTokenCount: 72`** — the model burned 72 of 80 tokens on invisible thinking, leaving 4 for visible reply (which were just stop/EOS tokens). Same prompt + `generationConfig.thinkingConfig.thinkingBudget: 0` → `text: "Yeah, just chilling at home, what's up?"`. The h-uman daemon log already says `model route: ... thinking=0 ...` so the routing layer KNOWS to disable thinking; the value just never reaches the Gemini API.
**Fix shape:** In `gemini.c`'s request body construction, when `request->thinking_budget` is set (or routed value is passed through), add:
```json
"generationConfig": {
    ...,
    "thinkingConfig": {"thinkingBudget": <value>}
}
```
Threshold for "disable thinking": value ≤ 0. Higher values = bounded thinking budget. Pro tier might want ~2048.
**Effort:** S (≤1 hr including test).

### Bug 2: `human agent run --once` returns "provider authentication failed" (medium)

### Bug 2: `human agent run --once` returns "provider authentication failed" (medium — possibly subsumed by Bug 1's fix)
**Note:** With Bug 1's fix, the agent CLI may start succeeding even without further work, because `provider authentication failed` may have been a misleading error mapped from a `MAX_TOKENS`-with-empty-content state. Retest after Bug 1's fix lands before investigating further.
**Locus (if still broken after Bug 1):** `src/agent/cli.c` provider creation path, or `src/providers/gemini.c` gemini_load_adc/refresh path.
**Evidence:** Direct Python equivalent of `gemini_refresh_token` returns HTTP 200 with a valid `cloud-platform`-scoped access token. h-uman's daemon `service-loop` mode does NOT hit this bug (Gemini auth works for classifier+director). Only `agent run --once` CLI hits it.
**Hypothesis:** CLI provider wiring differs from daemon wiring; maybe CLI loads gemini without ADC fallback, or `gemini_load_adc` silently fails for HOME/path reasons specific to the CLI invocation environment.

**Decision approach:** Add HOME/cwd logging + ADC-load-result logging in `hu_gemini_create`'s else-branch (gemini.c:1469). Reproduce with `agent run --once`. Log will pinpoint where it diverges from the daemon path.

### Bug 3: MLX serves markdown-bullet thinking, no channel-close markers (medium)
**Locus:** `mlx-server.py` (in `~/Documents/gemma-realtime-1/scripts/`), function `strip_thought_channels` and the `apply_chat_template` render at line 522.
**Evidence:** Both `seth-v3-fused` AND stock `mlx-community/gemma-4-31b-it-4bit` produce identical output: markdown bullet thinking followed by an unmarked final reply. The chat template ends with `<|channel>thought\n<channel|>` (empty thought block) — the model interprets this as "think more" and emits markdown.
**Hypothesis:** The Gemma 4 chat template's `add_generation_prompt=True` rendering is incompatible with the mlx_lm inference path. Either the template should not emit an empty thought block, or the model needs `enable_thinking=False` passed to the template, or `strip_thought_channels` needs to handle the markdown-thinking case.

**Decision approach:** Investigate Gemma 4's official inference recipe. Find the canonical way to suppress the thought channel for short replies. Fix the right one of: (a) chat template, (b) inference call, (c) postprocessor. AC-5 is the test.

## Architectural insight

Every layer in the reply path has reasonable failure semantics:

- agent_turn returns `err=ok` (no error) when content is empty
- director picks "text" (positive intent)
- daemon types nothing into AX (no text to type)
- AX successfully opens compose field (no error)
- response_guard retries silently (no logs)

No single layer is wrong. The dysfunction is in the COMPOSITION — there's no
layer asserting "if the director said text and the AX bridge opened, the
agent_turn MUST produce non-empty content." Adding that contract is AC-3.

This is the **silent-config-gated-subsystems** pattern from
`~/.claude/rules/silent-config-gated-subsystems.md`, applied to the response
layer rather than the config layer. The rule's guidance — emit one log line
naming what's disabled and how to fix — applies here too.

## Design decision: where to add the diagnostic contract (AC-3)

Add in `daemon.c` at the call-site of `hu_agent_turn` (or its streaming
variant). The daemon already knows the director said "text response, sending to
LLM" — it should ASSERT that the response is non-empty AND log richly when not.

**Pseudocode:**
```c
if (director_action == HU_DIRECTOR_ACTION_TEXT && response_len == 0) {
    hu_log_warn("human", NULL,
        "EMPTY-AGENT-RESPONSE provider=%s model=%s tool_calls=%zu "
        "http_status=%d body_bytes=%zu response_guard_retries=%d",
        provider_name, model_name, tool_calls_count,
        last_http_status, last_body_bytes, response_guard_retries);
}
```

This makes the failure surface every time it happens with enough context to
triage. The information already exists in the daemon, it's just not collected
at the assertion site.

## Design decision: regression test shape (AC-6)

Use a **fake provider** that returns the empirically-observed empty-with-tools
shape. The test should fail under today's agent_turn and pass once Bug 1 is
fixed. Pattern matches `tests/test_provider_*.c`.

```c
static void test_agent_turn_empty_with_tools_does_not_return_silently(void) {
    /* Arrange: fake provider returns response with tool_calls + empty content */
    /* Act: call hu_agent_turn */
    /* Assert: either content is non-empty (after tool execution + follow-up turn)
              OR the daemon receives a structured error explaining why */
}
```

## What we are NOT changing

- AX iMessage bridge (works)
- Classifier (works)
- Director (works)
- Persona+memory loading (untouched)
- iMessage polling / chat.db watcher (works)
- The fused MLX model itself (separate M3 sprint)
- Slice 2 (scheduled check-ins) or slice 3 (event-driven outreach)
