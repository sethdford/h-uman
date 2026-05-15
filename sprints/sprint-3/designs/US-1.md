# US-1: Wire structured-output (JSON schema) per persona opt-in

## Approach
Add a single `bool structured_output_enabled` field on `hu_persona_t` (NOT on overlay). Rationale: the JSON-schema response shape is provider-orthogonal and persona-global; per-channel toggling would require per-channel schemas, which the sprint scopes out (Non-goal: Anthropic / OpenAI structured output). Field defaults to `false` so loaded JSON without the key is a no-op.

At the request construction site `src/agent/agent_turn.c:4067` (`hu_chat_request_t req`), after `req` is initialized but before any provider dispatch, branch on `agent->persona && agent->persona->structured_output_enabled`. When true, set `req.response_format = "json_schema"`, `req.response_format_len = 11`, and `req.response_schema` / `req.response_schema_len` from a static const schema string defined in a new helper `src/agent/structured_output.c::hu_structured_output_default_schema()`. The schema is intentionally minimal — `{"type":"object","properties":{"message":{"type":"string"}},"required":["message"]}` — sufficient to activate Gemini's `responseSchema` layer without forcing prose-shape changes. Mirror the same branch in `src/agent/agent_stream.c:1258`.

JSON parsing: extend `src/persona/persona_loader.c` (or wherever fields are parsed — locate via `grep "core_values" src/persona/`) to read the optional `structured_output_enabled` boolean. Absent key → false.

## Files to modify / create
- `include/human/persona.h` — add `bool structured_output_enabled;` near end of `hu_persona_t` (~line 443, before `chronotype`).
- `src/persona/` JSON loader — parse new key (find via `grep -rn "calibrated" src/persona/`).
- `src/agent/structured_output.c` (new, ~30 LOC) + `include/human/agent/structured_output.h`.
- `src/agent/agent_turn.c:~4067` and `src/agent/agent_stream.c:~1258` — set `response_format` / `response_schema` when opt-in.
- `tests/test_structured_output.c` (new) — fixture persona with flag true, mock provider captures `hu_chat_request_t`, asserts `response_format == "json_schema"`.

## Risk
- **Provider compatibility:** non-Gemini providers must ignore `response_format`. Verify by reading `src/providers/anthropic.c`, `openai.c` — they already key off `response_format == "json_schema"`; if any treats it as an error, gate the wiring to gemini-only providers.
- **Persona serialization:** if persona is written back to disk via `hu_persona_save` (search), the new bool must round-trip. Add round-trip test.
- **Schema-trap leaks:** a too-restrictive schema could over-constrain prose. Mitigated by minimal schema.

## Test strategy
- New `tests/test_structured_output.c` (3 cases): opt-in sets fields; opt-out leaves NULL; NULL persona is safe.
- Existing `tests/test_persona_*` must still pass (no field-presence regression).
- Mock provider pattern: copy from any existing test that captures a `hu_chat_request_t` (search `grep -rn "hu_chat_request_t" tests/`).

## Sequencing
**First.** US-4 (deferred next sprint) caches the chain on persona — it will inherit this field's lifecycle. US-3 is independent. US-2 is independent.

## Effort estimate
**M** — field plumbing + 2 call-site edits + 1 new test file + JSON loader edit. ~150 LOC. Most risk is in the loader / serialization round-trip.
