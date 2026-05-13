# Persona-Conditioned Thinking Tokens (PCTT) — Design

## Components

- **`hu_channel_class_t` + table** — `include/human/channel_class.h` (new).
  Enum `{ HU_CHANNEL_CLASS_TEXT_FAST, HU_CHANNEL_CLASS_TEXT_ASYNC,
  HU_CHANNEL_CLASS_VOICE, HU_CHANNEL_CLASS_UNKNOWN }` plus a single static-const
  name→class table (`{"imessage", TEXT_FAST}, {"sms", TEXT_FAST}, {"slack",
  TEXT_ASYNC}, {"discord", TEXT_ASYNC}, {"telegram", TEXT_ASYNC}, {"voice",
  VOICE}, …`). Exposes `hu_channel_class_for_name(const char *name) →
  hu_channel_class_t`. Single place to amend when a new channel ships.

- **`hu_persona_overlay_t::filler_bank`** — new field on the existing per-channel
  overlay at `include/human/persona.h:18-37`. Representation:
  ```c
  char    **filler_bank;        // owned strings
  size_t    filler_bank_count;
  size_t    filler_bank_cap;    // capped at 32 per AC constraint
  ```
  Owned by the overlay; freed in the overlay destructor.

- **`hu_thinking_context_t`** — new struct passed by const pointer to a
  redesigned classify+select call.
  ```c
  typedef struct {
      const hu_persona_t *persona;       // active persona (may be NULL)
      const char         *channel_name;  // e.g. "imessage", "slack"
      const char         *chat_id;       // send_target (≤128 chars)
      size_t              chat_id_len;
      uint32_t            seed;          // existing time-based seed
      // optional/future: VAD endpoint timestamp, user-experience tier
  } hu_thinking_context_t;
  ```

- **`hu_filler_recency_t`** — agent-scoped LRU of `{ chat_id, last_index }`,
  bounded at 32 entries. New field on `hu_agent_t`; lives for the daemon's
  lifetime; no disk persistence. Eviction = LRU by `last_used_seq`.

- **`hu_conversation_select_filler`** — new selection function in
  `src/context/conversation.c`. Replaces the body of the old `classify_think_type`
  + filler-array dispatch. Takes the `hu_thinking_context_t`, returns the chosen
  filler text (pointer into the persona bank — caller copies into `out->filler`).

- **CLI: `human persona filler {add,list,remove}`** — new
  `HU_PERSONA_ACTION_FILLER_*` enum values in `include/human/persona.h`; handlers
  in `src/persona/cli.c` (parse around line 64, dispatch around line 226).

- **Eval scenario** — `data/eval_pctt.json`, driven by a new test
  `tests/test_filler_pctt.c` that loads the JSON, runs the selector for each
  incoming message under a synthetic persona+channel, and asserts no echo.

## Data flow

```
inbound msg
    │
    ▼
daemon.c (~9012) builds hu_thinking_context_t
    │  - persona = agent->persona
    │  - channel_name = ch->channel->vtable->name(ch->channel->ctx)
    │  - chat_id = send_target
    │  - seed = time(NULL)
    ▼
hu_conversation_classify_thinking(ctx, msg, len, out)
    │
    ├─ class = hu_channel_class_for_name(ctx->channel_name)
    │   ├─ TEXT_FAST  → return triggered=false                            [AC-1]
    │   └─ else continue
    │
    ├─ if no persona OR overlay missing OR filler_bank_count == 0:
    │     return triggered=false                                          [AC-2]
    │
    ├─ apply existing length/word trigger heuristic (unchanged shape):
    │     if not triggered: return triggered=false
    │
    ├─ hu_conversation_select_filler(ctx, overlay, recency_state) →
    │     scan bank, pick uniform-at-random from {entries} \ {last_index}
    │     update recency LRU                                              [AC-3]
    │
    ├─ delay_ms ← hu_channel_class_delay(class, msg_complexity)
    │     TEXT_ASYNC: existing dynamic formula
    │     VOICE: 200-500 ms uniform (Lala 2019)                           [AC-8]
    │
    └─ out->filler / out->filler_len / out->delay_ms set; return true
    │
    ▼
daemon emits filler via vtable->send(); usleep(delay_ms); continues turn
```

## Decisions

- **D1 — Filler bank lives on `hu_persona_overlay_t`, not on a new top-level
  array, and not piggybacking on `hu_persona_example_bank_t`.** Overlays are
  already per-channel and already store style-of-speech data
  (`formality`, `style_notes`, `typing_quirks`). Fillers are a style-of-speech
  property. Reusing the example bank would conflate full-conversation examples
  with single-string fillers and force a category tag. **Serves AC-5, AC-2.**

- **D2 — Channel class via a single name→class table, not a new vtable
  function.** Adding `channel_class()` to the 18-pointer vtable forces edits to
  42 channel implementations and 42 test files for a value that is static per
  channel name. A header table is one PR-sized edit and a single grep target
  for "what class is X channel." **Serves AC-1, AC-8.** Risk: a channel renames
  itself and the table goes stale → mitigated by an assertion at registration
  that the channel's `name()` resolves to non-`UNKNOWN` (catchable in
  `test_channel_class.c`).

- **D3 — Recency window is last-1, agent-scoped, in-memory.** AC-3 specifies
  "never selected back-to-back" — that is single-step recency. Storing only
  `last_index` per chat is enough; no ring buffer required. Agent-scoped LRU
  with 32-chat cap is sufficient because the daemon already handles ≤dozens
  of active chats. Lifetime = daemon process; loss on restart is acceptable —
  this is anti-repetition, not memory. **Serves AC-3.**

- **D4 — Replace the function signature with a context struct.** Adding
  `persona`, `channel_name`, `chat_id` as positional arguments is ugly and
  fragile. `hu_thinking_context_t` lets future extensions (VAD endpoint,
  user-experience tier from `hu_personal_model_t`) ride along without further
  signature breaks. Old test call sites (`tests/test_conversation.c:319, 328,
  335, 346-347` per AC-4's grep target) get updated to pass a context with
  `persona=NULL` and `channel_name=NULL` → trigger returns false, matching
  the "no persona = no filler" contract. **Serves AC-1, AC-2, AC-3, AC-8.**

- **D5 — Hand-rolled JSON parser tolerates unknown keys; old configs load.**
  `hu_persona_load_json` uses `hu_json_object_get` per field; missing
  `filler_bank` keys yield empty banks. New banks written by this code path
  reuse `write_json_string_array` at `creator.c:500-510`. Forward and backward
  compatibility verified by a save→reload round-trip test for a persona with
  fillers AND a load test for a pre-existing fixture without fillers. **Serves
  AC-5.**

- **D6 — `classify_think_type` and the hardcoded `fillers[]` arrays are
  deleted, not gated behind a flag.** The three-bucket classifier exists only
  to choose which hardcoded set to read; once selection comes from the persona
  bank, the buckets are dead weight that would shape the schema (forcing
  banks to be sub-categorized by EMOTIONAL/DECISION/COMPLEX) for no observed
  benefit. Future work can reintroduce intent-conditioned banks if eval data
  justifies it. **Serves AC-4** and the non-goal "we are not redesigning the
  EMOTIONAL/DECISION/COMPLEX classification."

- **D7 — Eval scenario is a static JSON corpus driven by a C test, not a new
  harness.** AC-7's regression guard is "emitted filler ≠ most recent incoming
  message." A test reading `data/eval_pctt.json` and asserting this property
  in a tight loop is sufficient. Reuses the existing JSON parser; no new eval
  framework, no LLM-as-a-Judge. **Serves AC-7.** Future TwinVoice-style
  multi-dimensional scoring is a follow-up (out of scope).

- **D8 — CLI nests under `human persona filler`, not `human filler`.** All
  persona mutations already live under `human persona`; treating fillers as a
  separate top-level command would split persona-management UX. Discovery
  hook: extend the existing usage string in `src/main.c:cmd_persona()`.
  **Serves AC-6.**

## Risks

- **Hot-path strcmp on channel name** — `hu_channel_class_for_name` does a
  linear strcmp scan. Called once per inbound message. With 8-10 entries in the
  table this is sub-microsecond; not a real risk, but worth a benchmark
  in the AC-1 test.
- **VAD endpoint for voice delay (AC-8 voice branch)** — the 200-500 ms window
  is measured *from end of user speech*. h-uman's voice channel may not yet
  surface that signal end-to-end. Mitigation: AC-8 voice branch is compiled
  but gated on a `HU_ENABLE_VOICE_VAD_TIMING` macro that defaults off; the
  conditional test for AC-8 voice is `#ifdef HU_ENABLE_VOICE_VAD_TIMING`. The
  text_async branch ships unconditionally.
- **Persona save atomicity already proven** — the
  `tests/test_personal_model_atomic_save.c` contract applies; we are extending
  the same writer (`hu_persona_creator_write`), not introducing a new one.
- **Test-suite regression surface** — the 5 existing test call sites of
  `hu_conversation_classify_thinking` (per `tests/test_conversation.c`) must
  be updated to pass `hu_thinking_context_t`. They should *continue passing*
  after update (persona=NULL → triggered=false is the new contract for those
  tests). If they don't, the test was depending on the hardcoded-filler
  behavior and needs to be redesigned — a finding that should land as a tasks.md
  item, not as a hidden change.
- **Channel-name aliasing** — iMessage might register as "imessage" or
  "iMessage" or "applemessaging" depending on the build. The class table
  uses case-insensitive lookup; verified in `test_channel_class.c`.

## Out of scope (re-asserted from requirements.md)

- Auto-learning fillers from user outbound messages (deferred to follow-up).
- Reintroducing intent buckets (EMOTIONAL/DECISION/COMPLEX).
- Onboarding wizard for filler recording.
- Backfilling existing personas with default fillers.

