---
title: Reactive send budget and llm_decides gate split
date: 2026-09-02
status: approved-by-default (autonomous session; defaults stated as assumptions)
---

# Reactive send budget and `llm_decides` gate split

Follow-up to the 2026-09-01 iMessage replay incident (fix: `75bbbfdcf`). The
routing audit found two structural gaps on the reactive reply path in
`src/daemon.c` that the replay guards do not cover:

1. **No send budget.** Nothing counts reactive replies. The channel rate
   limiter (`hu_channel_rate_limiter_*`) has one consumer, the proactive
   throttle. Seventeen sends to one contact in an hour tripped nothing.
2. **`llm_decides: true` disables eight gates at once**, including three that
   are safety, not heuristics: the consecutive-response limiter, the AI-tell
   retry, and the quality-gate retry. Production runs with `llm_decides` on,
   so "I apologize for the delay in responding" reached a real contact with
   no filter in the way.

## Goals

- A reply budget that stops a runaway (a bug, a replay, a loop) within one
  hour without shaping a normal conversation.
- Safety gates stay on regardless of `llm_decides`; heuristic/cost gates keep
  their current behavior.
- Both decisions are pure predicates unit-tested without the daemon
  (`.claude/rules/security-predicate-extraction.md`).
- `src/daemon.c` does not grow (file-size ratchet at 14061 LOC).

## Non-goals

- Persisting the budget across restarts. The replay guards already cover the
  restart case; an in-memory window is enough for a runaway inside one
  process life.
- Reworking the proactive path's budget/quiet hours. Already exists.
- Narrowing the AI-tell retry prompt or the quality scorer.

## Design

### 1. Reply budget (`src/daemon/send_budget.c`)

Sliding one-hour window of reply timestamps, two scopes:

| Scope | Default | Config key (`behavior.*`) |
|---|---|---|
| per contact | 10 replies / hour | `reply_budget_per_contact_hourly` |
| global | 30 replies / hour | `reply_budget_global_hourly` |

`0` disables that scope. Unit is a **reply** (one processed batch), not a
bubble: the two-bubble "sorry just saw this" + answer is one reply.

API (module-level singleton, mirroring `reply_dedup`):

```c
void hu_send_budget_configure(uint32_t per_contact_hourly, uint32_t global_hourly);
bool hu_send_budget_allows(const char *contact, size_t len, int64_t now,
                           hu_send_budget_reason_t *why);   /* pure read */
void hu_send_budget_record(const char *contact, size_t len, int64_t now);
void hu_send_budget_reset(void);                             /* tests */
```

Plus a pure core (`hu_send_budget_t` struct + `_init/_allows/_record`) that
the tests exercise directly and the singleton wraps. Capacity: 128 contacts,
32 timestamps per contact, 256 global timestamps; when a contact slot is
needed the one with the oldest most-recent send is evicted. Timestamps older
than 3600 s are ignored on read and overwritten on write.

Wiring in `src/daemon.c`:

- **Check** right after the reply-dedup check at batch entry. Deny → log
  `reply budget exhausted for <contact> (<n> in last hour, cap <c>) — staying
  silent` and `continue`. Silence is the fail direction (a human who is
  "busy" is normal; a bot that never stops is the tell).
- **Charge** at the post-send `hu_daemon_reply_dedup_mark` site, which runs
  only after a successful reactive send.

Why defaults 10/30: tonight's runaway was ~25 replies to 6 contacts in an
hour, 9 to each of the top two. 10/30 would have cut both. A live rapid chat
rarely exceeds 10 daemon replies to one person in an hour because director
delays and choreography pace it; when it does, going quiet is the safe
failure. Both are operator-tunable.

Trade-offs accepted after critic review:

- **Group chats count as one contact** (the key is the batch/session key).
  Ten daemon replies into one group in an hour trips the per-contact scope.
  Deliberate: a persona posting ten times an hour into a group is louder than
  the human it models. Tunable.
- **A denied batch is dropped, not deferred.** The message buffer is a
  per-tick array, so the batch is evaluated once (same as the reply-dedup
  skip); there is no re-logging and no late send when the window frees.
- **The autoresponder path** (DND + allowlisted, `goto skip_llm_this_batch`)
  sends without setting `response`, so it never reaches the dedup mark. It
  charges the budget at its own send site.
- **AI-tell retry hint** now attaches even when there is no prior
  conversation context (the lean prompt under `llm_decides`), so the retry is
  never a blind re-roll.
- **Second miss drops.** (Added later on 2026-09-02.) The filter runs on
  every attempt; `hu_reactive_ai_tell_action(tell, retried)` returns SEND /
  RETRY / DROP. First miss: retry once with the hint. Second miss: free the
  reply and stay silent — silence beats sending a therapy-bot line to a
  friend. The drop is logged with the matched phrase.
- `"I apologize"` alone is NOT a tell: "I apologize for the mistake!" is real
  Seth text in the eval corpus. Only qualified forms are listed.

### 2. Gate split (`src/daemon/reactive_gates.c`)

```c
typedef enum {
    HU_REACTIVE_GATE_RESPONSE_MODE,   /* heuristic */
    HU_REACTIVE_GATE_DROP_OFF,        /* heuristic */
    HU_REACTIVE_GATE_TAPBACK_SKIP,    /* heuristic */
    HU_REACTIVE_GATE_LEAVE_ON_READ,   /* heuristic */
    HU_REACTIVE_GATE_CONSTITUTIONAL,  /* heuristic (LLM cost) */
    HU_REACTIVE_GATE_CONSECUTIVE_LIMIT, /* SAFETY */
    HU_REACTIVE_GATE_AI_TELL_RETRY,   /* SAFETY */
    HU_REACTIVE_GATE_QUALITY_RETRY,   /* SAFETY */
} hu_reactive_gate_t;

bool hu_reactive_gate_active(hu_reactive_gate_t gate, bool llm_decides);
bool hu_reactive_gate_is_safety(hu_reactive_gate_t gate);
const char *hu_reactive_response_ai_tell(const char *response); /* NULL = clean */
```

`hu_reactive_gate_active` returns `true` for safety gates always, and
`!llm_decides` for heuristic gates. The three safety sites in `daemon.c`
call the predicate instead of testing `llm_decides` inline. The heuristic
sites are left as they are (no behavior change; the enum documents them).

The AI-tell phrase list moves out of `daemon.c` into the module and gains
the phrases that leaked tonight: "I apologize", "I understand you're",
"I understand you are", "please clarify", "delay in responding",
"experiencing these feelings". Matching is case-insensitive substring, as
before.

Behavior change under `llm_decides` (production): the consecutive limiter
(3 in a row → silent), the AI-tell retry (one retry with the rewrite hint),
and the quality retry (one retry, only when history is loaded) now run.
Cost: at most one extra local-model call on a flagged reply.

### Error handling

- Budget: NULL/empty contact → allowed (never suppress on a bad key); the
  global scope still applies.
- Gates: unknown enum value → treated as safety (active). Fail closed.

### Testing

- `tests/test_send_budget.c`: allow under limit; deny at per-contact limit;
  window slides (entries older than 3600 s no longer count); global limit
  independent of contact; `0` disables a scope; eviction beyond 128 contacts
  keeps working; NULL contact is allowed.
- `tests/test_reactive_gates.c`: full truth table (8 gates × 2 states);
  each leaked phrase detected; a clean reply returns NULL; case-insensitive.
- Full suite green; `daemon.c` LOC ≤ 14061.

## Related

- `.claude/rules/security-predicate-extraction.md`
- `.claude/rules/file-size-ceiling.md`
- Memory: `project_h-uman_imessage_replay_incident_2026-09-01`
