# Sprint 37 — Multi-turn director memory + daemon clear-on-exit test

**Branch:** `sprint-37-multi-turn-director`
**Sprint goals:**

1. **G6 across turns.** Currently G6 (director-string echo) only sees
   the *current* turn's `scene_direction_text`. A model that quotes
   *yesterday's* director text verbatim slips past G6. Fix by adding
   a small ring buffer of recent director strings owned by the agent;
   G6 iterates the buffer.
2. **Daemon clear-on-exit, proven.** Sprint 34 added
   `agent->scene_direction_text = NULL` clear after each turn. No
   test pins this. Add an integration test where turn 2 has no
   director and verify the guard sees NULL, not stale stack memory
   from turn 1.

## Background

Sprint 34 retro:

> "Multi-turn director memory: ring buffer of recent director strings
>  so G6 catches verbatim quotes of previous turns."

Sprint 35 retro:

> "Daemon clear-on-exit test: two-turn integration where second turn
>  has no director set, guard sees NULL not stale memory."

Both are real correctness gaps:

- **Cross-turn miss.** Director text typically changes per turn but
  may be similar across turns (`"casual short"` → `"casual brief"`).
  A model that holds the previous director in its KV cache and quotes
  it in the next turn would slip past G6.
- **Stale-memory risk.** If `daemon.c` ever forgot to clear
  `agent->scene_direction_text` between turns (or if a code path
  raced), the guard would read freed stack memory. We don't have a
  smoke test pinning this.

## Design

### Ring buffer (in `hu_agent_t`)

```c
#define HU_DIRECTOR_HISTORY_MAX     4   /* recent N directors retained */
#define HU_DIRECTOR_TEXT_CAP        256 /* per-entry byte cap */

struct hu_agent {
    /* ... existing fields ... */

    /* Sprint 37 — Ring buffer of recent director strings (excluding the
     * currently-active one in `scene_direction_text`). Owned by the
     * agent, allocated/freed on the agent's allocator. Each entry is
     * a NUL-terminated copy of a past director string truncated to
     * HU_DIRECTOR_TEXT_CAP bytes. G6 iterates the buffer to catch
     * cross-turn director echoes.
     *
     * `director_history_count` is the total push count, capped at
     * HU_DIRECTOR_HISTORY_MAX. Slot indexing is most-recent-first
     * (index 0 = previous turn, 1 = two turns ago, ...). */
    char *director_history[HU_DIRECTOR_HISTORY_MAX];
    size_t director_history_lens[HU_DIRECTOR_HISTORY_MAX];
    size_t director_history_count;
};
```

### Helpers in `agent_internal.h`

```c
/* Push the current director (about to go stale) into the ring buffer.
 * No-op if `text` is NULL or `text_len < HU_GUARD_DIRECTOR_ECHO_MIN_MATCH`
 * (would be skipped by G6 anyway). The agent allocator allocates a copy
 * of up to HU_DIRECTOR_TEXT_CAP bytes; the oldest entry is freed. */
void hu_agent_internal_push_director_history(hu_agent_t *agent,
                                              const char *text, size_t text_len);

/* Free all history entries (called by hu_agent_deinit). */
void hu_agent_internal_free_director_history(hu_agent_t *agent);
```

### API surface

Extend `hu_guard_context_t`:

```c
/* Sprint 37 — past-turn director history (most-recent-first). Same
 * 30-byte threshold as `director_text`. NULL or count = 0 disables. */
const char *const *director_history;
const size_t *director_history_lens;
size_t director_history_count;
```

### Detector

`hu_guard_has_director_echo` extended to iterate history after the
current `director_text`:

```c
static bool hu_guard_has_director_echo(...) {
    /* existing: check ctx->director_text */
    /* new: for i in 0..director_history_count, check history[i] */
}
```

No threshold change. No new report flag — `detected_director_echo`
covers cross-turn matches just as well; the per-source distinction is
not actionable.

### Wiring

Production call sites populate from `agent->director_history`:

```c
guard_ctx.director_history = (const char *const *)agent->director_history;
guard_ctx.director_history_lens = agent->director_history_lens;
guard_ctx.director_history_count = agent->director_history_count;
```

### Daemon integration

Just before turning over the current `scene_direction_text` (i.e.
NULLing it out at end of turn), push it into the ring buffer:

```c
hu_agent_internal_push_director_history(agent,
    agent->scene_direction_text, agent->scene_direction_text_len);
agent->scene_direction_text = NULL;
agent->scene_direction_text_len = 0;
```

The push is idempotent on NULL — safe to always call.

## Stories

### S1 — `hu_agent_t` ring buffer fields

Add the three fields documented above. `hu_agent_init` zeroes them
(memset already covers this since the struct is `memset`-initialized).

### S2 — Helper functions in `agent.c`

`hu_agent_internal_push_director_history`:

- Allocates a copy via `agent->alloc->alloc(.., min(text_len, CAP))`.
- Truncates safely (NUL-terminates).
- Frees the oldest slot if buffer is full.
- Shifts entries down one slot (slot 0 = most recent).
- Updates `director_history_count` (saturates at MAX).

`hu_agent_internal_free_director_history`:

- Frees all non-NULL slots.
- Zeroes the lens array.
- Sets count = 0.

`hu_agent_deinit` calls the free helper before main cleanup.

### S3 — Extend `hu_guard_context_t` and `hu_guard_has_director_echo`

- New context fields (3, all const-pointers / size_t).
- Detector iterates history slots after current `director_text`.
- Same 30-byte minimum match.
- Returns true on first match (any source).

### S4 — Wire all 3 production call sites

- `agent_stream.c` (×2), `agent_turn.c` (×1) populate the new
  context fields from `agent->director_history*`.
- No log change (reuses `director=N` flag).

### S5 — Daemon: push director into history before NULLing

- `src/daemon.c`: before clearing `agent->scene_direction_text`,
  call `hu_agent_internal_push_director_history`.

### S6 — Tests

Unit tests in `test_response_guard.c`:

- `guard_g6_history_catches_previous_director` — ctx has empty
  `director_text` (current turn, no director) but history slot 0 is
  the previous director, response quotes 30+ bytes of it → REJECT.
- `guard_g6_history_skips_below_threshold` — history entry < 30 bytes
  → no fire.
- `guard_g6_history_orthogonal_to_current` — ctx has both current
  director and history entry; response quotes only history → REJECT.
- `guard_g6_history_zero_count_disables` — `director_history_count = 0`
  → no enforcement.

Helper tests in same file:

- `agent_director_history_push_basic` — push 1, count = 1, slot 0
  matches.
- `agent_director_history_push_overflow_evicts_oldest` — push 5,
  count saturates at MAX = 4, oldest evicted.
- `agent_director_history_push_truncates_long` — push 1KB, slot 0
  contains first CAP bytes, NUL-terminated.
- `agent_director_history_push_null_is_noop` — NULL/short skipped.
- `agent_director_history_free_zeroes_count` — free helper resets
  count and frees slots.

Integration tests in `test_response_guard_retry.c`:

- `agent_g6_history_cross_turn_rejects_and_retries` — push a
  director into history (simulating "previous turn"), then run a
  turn with NO current director, mock provider quotes the historical
  director, G6 fires via history path, retry succeeds.
- `agent_clear_on_exit_no_stale_memory` — turn 1 sets a stack-local
  director string. After the turn, stack frame goes out of scope.
  Turn 2 starts with no director set. Mock provider returns a string
  that would have matched the (now-freed) turn 1 director. G6 must
  NOT fire from stale memory — the agent's own ring buffer is fine
  (heap-owned copy), but `agent->scene_direction_text` itself must
  be cleared.

## Definition of Done

- All 6 stories shipped.
- Dev build clean with `-Wall -Wextra -Wpedantic -Werror`.
- Full dev test suite passes 10322 + N (N ≥ 11), 0 ASan errors.
- Clear-on-exit test runs with ASan and reports zero stale-memory
  reads.
- Branch tagged `v-sprint-37-close` and cherry-picked to `h-uman` main.

## Out of scope (deferred)

- **Quality gate `MARGINAL → REJECT`** (Sprint 28 carry-over).
- **Per-channel length thresholds.**
- **Widen G7 lookahead 30 → 60 bytes.**
- **Extend G8 to `persona->biography`.**
- **CI/cron schedule for `audit-imessage-leaks.sh`.**
