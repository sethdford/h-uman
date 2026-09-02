---
title: daemon.c batch-reply carve-out — reactive turn context + prompt build
date: 2026-09-02
status: slice-A-landed
---

# daemon.c batch-reply carve-out

`src/daemon.c` is 14,058 LOC (ratchet `MAX_BASELINE=14058` in
`scripts/check-file-size-ceiling.sh`, may only shrink). The reactive batch-reply
body of `hu_service_run` runs from the batch `for` at line 4376 to its close at
13635 (~9,260 LOC, one scope, brace depth 4). Earlier carve-outs
(`src/daemon/reactive_gates.c`, `send_budget.c`, `daemon_routing.c`) pulled out
pure predicates; nothing has yet moved a *stretch* of the loop body, because
every candidate region shares dozens of locals with the code around it.

This plan introduces ONE struct for the batch-scoped locals that cross the
5606-7370 boundaries and extracts two slices behind it. No behavior change, no
config / launchd / :8741 touches.

## Re-measured crossing locals (2026-09-02, origin/main 8ec08adab)

Throwaway script: strip comments and strings, track brace depth, match
declarations against a type whitelist, count word-boundary uses that are not
struct-field accesses (`(?<![\w.>])name\b`). Function parameters
(`alloc`, `agent`, `config`, `channels`, `channel_count`) are not counted; they
are passed as parameters. The earlier naive count (27 for 5700-7100) included
struct-field false positives (`a`, `ctx`, `existing`, `message`, `style`).

| Region | LOC | in | out | total | statics inside |
|---|---|---|---|---|---|
| 5606-6036 history + context loading (slice A) | 431 | 8 | 15 | 23 | `style_cache`, `style_cache_count`, `style_seq` |
| 6038-7276 prompt phases to `#undef PHASE6_APPEND` | 1,239 | 13 | 2 | 15 | `ep_hdr`, `error_seed` |
| 6038-7370 prompt phases through awareness merge (slice B) | 1,333 | 15 | 0 | 15 | + `topic_consolidation_debounce`, `topic_debounce_initialized` |
| 5606-7370 both slices | 1,765 | 12 | 13 | 25 | all of the above |

Slice A, in: `ch`, `batch_key`, `key_len`, `combined`, `combined_len`,
`llm_decides` (+ `channels`, `channel_count` as parameters). Out: `contact_ctx`,
`contact_ctx_len`, `convo_ctx`, `convo_ctx_len`, `history_entries`,
`history_count`, `cross_channel_ctx`, `cross_channel_ctx_len`,
`contact_for_tapback`, `ctx_entries`, `ctx_count`; plus `response`,
`response_len`, `err`, `turn_out_state`, which are only *declared* there and
stay declared in daemon.c (moving a declaration downward within the same scope
with no intervening use is a no-op).

Slice B, in: the slice A outputs plus `inner_thought_store`,
`inner_thought_store_ok`, `daemon_turn_counter`, `repair_signal` (function-scope
statics of `hu_service_run`; pass by pointer). Out: `convo_ctx`, `convo_ctx_len`
only, once the region ends at 7370 where `phase6_prefix` has been consumed.

Control flow: no `continue`/`break`/`goto` at batch depth in 5606-7370 (all
exits sit in inner loops, depths 6-12). Both slices are plain `void` calls.

Preprocessor: `#ifndef HU_IS_TEST` opens at 5723 and closes at 9985, so
everything from the contact profile onward is compiled out of `human_tests`.
The whole loop is the `#else` arm of `#ifdef HU_IS_TEST` at 2226, which makes
that inner gate redundant in daemon.c but *meaningful* in the new file: the
extracted function's ungated prefix (clear history, active channel, persona
override, session-store restore) is what the unit test can exercise.

## The struct — `include/human/daemon/reactive_turn.h`

```c
typedef struct hu_daemon_comfort_pending {   /* was an anonymous static local */
    char key[64]; char emotion[64]; char response_type[32];
} hu_daemon_comfort_pending_t;                /* HU_COMFORT_PENDING_MAX = 32 */

typedef struct hu_reactive_turn_ctx {
    /* inputs, set by hu_service_run per batch */
    hu_service_channel_t *ch;
    const char *batch_key; size_t key_len;
    const char *combined;  size_t combined_len;
    bool llm_decides;
    /* loop-lifetime state the slices read/write in place */
    hu_daemon_comfort_pending_t *comfort_pending;   /* HU_COMFORT_PENDING_MAX slots */
    hu_proactive_context_t *proactive_ctx;          /* was the g_proactive_ctx macro */
    /* outputs of slice A (context load) */
    char *contact_ctx; size_t contact_ctx_len;
    char *convo_ctx;   size_t convo_ctx_len;        /* filled by slice B */
    hu_channel_history_entry_t *history_entries; size_t history_count;
    char *cross_channel_ctx; size_t cross_channel_ctx_len;
    const hu_contact_profile_t *contact_for_tapback;
    const hu_channel_history_entry_t *ctx_entries; size_t ctx_count;
} hu_reactive_turn_ctx_t;

void hu_daemon_reactive_context_load(hu_allocator_t *alloc, struct hu_agent *agent,
                                     const struct hu_config *config,
                                     hu_service_channel_t *channels, size_t channel_count,
                                     hu_reactive_turn_ctx_t *rt);
```

Field gates are intentionally unconditional; the *code* keeps every `#if`
verbatim, so under `HU_IS_TEST` some fields are simply never written.

### Pure-move rules (both slices)

1. The moved body is byte-identical except indentation. Inputs are aliased at
   the top under their historical names (`const char *batch_key = rt->batch_key;`)
   and outputs are written back at the bottom (`rt->contact_ctx = contact_ctx;`),
   inside the same `#if` gate the local was declared under.
2. daemon.c unpacks the struct into the historical locals right after the call
   (`char *convo_ctx = rt.convo_ctx;`). The ~700 downstream uses of
   `convo_ctx`/`history_entries`/... are left alone; migrating them to `rt.*` is a
   later script-driven mechanical pass (slice C), not part of A or B.
3. Early `continue`/`break` at batch depth become an enum return the caller
   switches on (`HU_REACTIVE_STEP_CONTINUE` → `continue;`). Neither A nor B
   needs one; the convention is here for the slices after 7370.
4. Statics move only if `grep -rn '<name>' src include tests` proves the region
   is the sole user. `daemon.c` macros used by the region are moved verbatim
   (`HU_STYLE_CACHE_CAP`, `HU_STYLE_RELEARN_INTERVAL`) or expanded once
   (`daemon_contact_activity_record(...)` → `hu_daemon_contact_activity_record(rt->proactive_ctx, ...)`).
5. Every `#if`/`#ifndef` line inside the region is kept exactly; a gate that
   *spans* the region boundary (5723) is closed in the new file and re-opened
   in daemon.c at the same point.

## Slice A — `src/daemon/daemon_reactive_context.c` (LANDED)

Moves daemon.c 5606-6036: `hu_agent_clear_history`, active-channel set,
per-contact/per-channel persona override, session-store history restore, the
context declarations, contact profile + inner-world merge, BTH style learning
(statics `style_cache*` move), `load_conversation_history`, F27 comfort-pending
consume, `ctx_entries`/`ctx_count`, and the 2b cross-channel awareness block.

Steps (each a build+suite cycle):

1. Add the header above; give `comfort_pending` its named type in daemon.c
   (`static hu_daemon_comfort_pending_t comfort_pending[HU_COMFORT_PENDING_MAX];`)
   and delete the local `#define HU_COMFORT_PENDING_MAX`.
2. Create the .c with the moved body; register in `CMakeLists.txt` next to
   `src/daemon/reactive_gates.c`.
3. Replace 5606-6036 in daemon.c with: fill `rt`, call, re-declare
   `response/response_len/err`, unpack the outputs, re-open `#ifndef HU_IS_TEST`
   and keep `turn_out_state` + `ctx_entries`/`ctx_count` unpack inside it.
4. `tests/test_daemon_reactive_context.c` drives the ungated prefix: a fake
   session store returning two entries lands as two `agent->history` rows with
   the right roles; the channel vtable name becomes `agent->active_channel`;
   pre-existing history is cleared; every output starts NULL/0.
5. Build (`cmake --preset dev`, -Werror), full suite, all `scripts/check-*.sh`,
   lower `MAX_BASELINE`, confirm the clone ratchet did not rise.

Result: daemon.c 14058 → 13664 LOC (−394 net: 431 moved, 37 for the call,
unpack and re-declarations); see the slice A commit for the suite line.

## Slice B — `src/daemon/daemon_reactive_prompt.c` (NEXT)

Moves daemon.c 6038-7370 (post-slice-A numbering shifts by -412): the Phase 6
prefix builder (`PHASE6_APPEND` macro + `#undef`, life sim, mood, ToM,
anticipatory, self-awareness, life chapter, social graph, timezone, humor,
inner thoughts, anti-sycophancy, repair, evolved opinions, feeds, visual,
relationship dynamics), then `hu_conversation_build_awareness` and the prefix
merge that produces `convo_ctx`.

```c
void hu_daemon_reactive_prompt_build(hu_allocator_t *alloc, struct hu_agent *agent,
                                     hu_inner_thought_store_t *inner_thought_store,
                                     bool inner_thought_store_ok,
                                     uint32_t *daemon_turn_counter,
                                     hu_repair_signal_t *repair_signal,
                                     hu_reactive_turn_ctx_t *rt);
```

Checklist before moving: `grep` proves `ep_hdr`, `error_seed`,
`topic_consolidation_debounce`, `topic_debounce_initialized` have no other user;
`daemon_turn_counter++` and `repair_signal` writes go through the pointers;
`hu_daemon_detect_emotion` (used at 6300) stays reachable via its header; the
`#undef PHASE6_APPEND` moves with the macro. After B lands, the daemon.c unpack
of `convo_ctx`/`convo_ctx_len` moves below the B call and the A-time unpack of
`history_entries`/`ctx_entries` stays where it is (B reads them from `rt`).

Expected: daemon.c ≈ 12,310 LOC; `MAX_BASELINE` lowered again.

## Verification (per slice)

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/<wt>
cmake --preset dev && cmake --build --preset dev -j8          # -Werror, must link
$W/build/human_tests 2>/dev/null | grep 'Results:'            # 0 failures, 0 ASan
for s in file-size-ceiling clone-ratchet no-new-root-files sqlite-includer-ratchet \
         test-source-gate-symmetry test-references untested layer-topology; do
  bash $W/scripts/check-$s.sh; done
```

`grep -rn hu_daemon_reactive_context_load src --include='*.c' | grep -v daemon_reactive_context.c`
must show the daemon.c caller (integration-done contract).

## Related

- `docs/superpowers/specs/2026-09-02-reactive-send-budget-and-gate-split-design.md` — peer carve-outs this copies the shape of
- `.claude/rules/file-size-ceiling.md`, `.claude/rules/clone-ratchet.md`
- `docs/plans/2026-05-29-ddd-bounded-contexts/phase-E2-daemon-service-lifecycle.md`
