---
title: "Design — US-3.1: Wire hu_hybrid_retrieve into hu_memory_recall_for_contact"
sprint: 3
story: US-3.1
created: 2026-05-15
status: ready_for_implementation
authored_by: tech-lead
---

# Design for US-3.1 — Wire `hu_hybrid_retrieve` into `hu_memory_recall_for_contact`

## Context

Today, `hu_memory_recall_for_contact` (`src/memory/contact_memory.c:38`)
goes directly to `mem->vtable->recall` (BM25/FTS5) and post-filters the
returned `hu_memory_entry_t**` by the literal byte prefix
`"contact:<contact_id>:"`. Hybrid retrieval (RRF over keyword + vector,
with optional graph context, cross-encoder rerank) already exists at
`src/memory/retrieval/hybrid.c:77`. It accepts NULL `embedder`,
`vector_store`, and `graph` and degrades gracefully to keyword-only.

The change this story asks for is purely **plumbing**: thread two new
nullable pointers (`embedder`, `vector_store`) through the contact-recall
entry point so it can call `hu_hybrid_retrieve` when both are present.
The contact-prefix filter stays. The graph stays NULL for this sprint
(per AC-3.1.4) — wiring a real graph is out of scope.

The pre-resolved open question (Q2) bounds the blast radius: this is one
commit, all call sites updated together. The implementer must rebase off
`origin/main` commit `13b89763` so the unrelated working-tree edits in
`agent_turn.c` / `agent_stream.c` / `main.c` do not bleed into the
US-3.1 commit.

---

## Approach

**Smallest design that satisfies all 7 ACs:** add two parameters to
`hu_memory_recall_for_contact` (header + impl), branch internally on
`embedder && vector_store`, and update every call site to pass either
the live pointers (production, when US-3.3 wires them) or `NULL, NULL`
(every existing test and the fallback path).

Alternatives rejected:

- **Add a new function `hu_memory_recall_for_contact_hybrid`** — leaves
  the old signature in place but doubles the surface area. AC-3.1.1
  explicitly mandates a signature change. Rejected.
- **Plumb the embedder/vector store via `hu_memory_t` (attach them to
  the backend struct)** — pollutes a vtable-driven interface with
  retrieval concerns. Crosses a layering boundary. Rejected.
- **Take a `hu_retrieval_engine_t*` instead of two raw pointers** —
  cleaner abstractly, but `hu_hybrid_retrieve` itself takes the two raw
  pointers, and AC-3.1.1 names them by type. The retrieval-engine
  wrapper at `include/human/memory/retrieval.h:42` is for a different
  call path. Rejected as out-of-scope for this story.

**Routing logic (verbatim, copy this into the implementation):**

```c
const bool use_hybrid = (embedder != NULL && vector_store != NULL);

if (use_hybrid) {
    /* Hybrid path: RRF(keyword + semantic), cross-encoder rerank.
     * NOTE: hu_hybrid_retrieve itself is robust to either being NULL
     * (falls through to keyword-only), but we keep the explicit AND
     * here so the BM25/FTS fallback path stays a single, well-tested
     * branch — no partial-vector mode. */
    return recall_hybrid(mem, alloc, embedder, vector_store,
                         contact_id, contact_id_len,
                         query, query_len, limit,
                         session_id, session_id_len,
                         out, out_count);
}

/* BM25/FTS fallback — exactly today's body, unchanged. */
return recall_bm25(mem, alloc, contact_id, contact_id_len,
                   query, query_len, limit,
                   session_id, session_id_len,
                   out, out_count);
```

Both internal helpers apply the **same** contact-prefix filter after
retrieval; the difference is only the source of `raw_entries`.

**Why no partial-vector path?** If only `embedder` is non-NULL,
`hu_semantic_retrieve` cannot run (no store to query); if only
`vector_store` is non-NULL, the store is empty of embeddings the caller
could ask about. Either combination produces subtle scoring asymmetry
where RRF would rank keyword-only entries above empty-vector results
and look like silent degradation. The explicit AND is cheaper to reason
about and easier to test.

---

## New signature (AC-3.1.1, AC-3.1.7)

```c
/* include/human/memory.h — line 168 region
 *
 * Recall scoped to a contact. When `embedder` AND `vector_store` are
 * BOTH non-NULL, recall uses the hybrid RRF pipeline
 * (hu_hybrid_retrieve). Otherwise, recall falls back to the BM25/FTS
 * keyword path with no behaviour change versus pre-Sprint-3. Either
 * parameter may be NULL independently — NULL on either one triggers
 * the BM25 fallback (no partial-vector mode).
 */
hu_error_t hu_memory_recall_for_contact(
    hu_memory_t        *mem,
    hu_allocator_t     *alloc,
    const char         *contact_id, size_t contact_id_len,
    const char         *query,      size_t query_len,
    size_t              limit,
    const char         *session_id, size_t session_id_len,
    hu_embedder_t      *embedder,        /* nullable; NULL → BM25-only */
    hu_vector_store_t  *vector_store,    /* nullable; NULL → BM25-only */
    hu_memory_entry_t **out,
    size_t             *out_count);
```

Parameter insertion point: **before** the existing `out` /
`out_count` pair, after `session_id_len`. AC-3.1.1 mandates "before the
existing `hu_memory_entry_t **out` parameter".

Required header includes added to `include/human/memory.h`:
`#include "human/memory/vector.h"` (provides `hu_embedder_t` and
`hu_vector_store_t`). The header already includes `human/memory.h`
itself so no circular include is introduced.

---

## Result conversion (AC-3.1.3)

`hu_hybrid_retrieve` produces `hu_retrieval_result_t { entries,
count, scores }` (`include/human/memory/retrieval.h:30`); the contact
API returns `hu_memory_entry_t** + size_t*`. The conversion is a
direct ownership transfer — the result's `entries` array is already
`hu_memory_entry_t[]` allocated by `alloc`, and `scores` is a parallel
`double*`. We hand the entries array out and free only the `scores`
array (the contact API does not surface scores today and adding them
would expand AC-3.1.1's signature change beyond what's mandated).

Concretely, inside `recall_hybrid`:

```c
hu_retrieval_options_t opts = {
    .mode                  = HU_RETRIEVAL_HYBRID,
    .limit                 = limit * 4,    /* see Risk R1 below */
    .min_score             = 0.0,
    .use_reranking         = true,
    .temporal_decay_factor = 0.0,
};

hu_retrieval_result_t raw = {0};
hu_error_t err = hu_hybrid_retrieve(
    alloc, mem, embedder, vector_store, /*graph=*/NULL,
    query, query_len, &opts, &raw);
if (err != HU_OK) {
    /* AC-3.1.5: propagate without touching *out (still NULL from
     * the entry-guard at line 45-46). raw is zero-initialised so
     * a partial result is safe to free. */
    hu_retrieval_result_free(alloc, &raw);
    return err;
}

/* Filter raw.entries by "contact:<contact_id>:" prefix; this re-uses
 * exactly the same loop body as lines 68-116 of today's
 * contact_memory.c. The free path uses hu_retrieval_result_free for
 * the rejected entries (it walks both entries[] and scores[]) — but
 * because we move entries out individually (memset to zero after the
 * move), we instead free fields with hu_memory_entry_free_fields and
 * then free the arrays directly, mirroring today's code. */
```

The filter loop is **identical** to the existing one
(`contact_memory.c:68-116`). The only difference is the input array
source. Factoring this into a static helper `filter_by_contact_prefix`
inside `contact_memory.c` is a nice-to-have but not required by any AC;
implementer may inline twice for clarity.

---

## Contact-prefix filtering (AC-3.1.3, AC-3.2.6 regression guard)

The vector store returned by `hu_vector_store_mem_create` (per US-3.3)
indexes **all** entries the daemon embedded — across every contact.
The hybrid path can therefore return entries from contacts other than
`contact_id`. The post-filter (today's lines 68-116) catches this
because every entry written via `hu_memory_store_for_contact` has its
key prefixed with `"contact:<contact_id>:"`. The hybrid path preserves
key/key_len through `search_results_to_entries` at `hybrid.c:60-66`
(it copies content into both `content` and `key`), **except** that the
keys it writes are the entry's content, not the original key prefix.

**This is a real problem.** Read `hybrid.c:60-66`:

```c
entries[i].content = hu_strndup(alloc, results[i].content, len);
entries[i].content_len = len;
entries[i].key = hu_strndup(alloc, results[i].content, len);  /* ← content, not key */
entries[i].key_len = len;
```

The hybrid path discards the original storage key. Our prefix filter
matches on `entry.key`. If we naively post-filter, every hybrid result
will fail the prefix check and the contact recall will return zero
entries — exactly the AC-3.2.6 regression guard the test will catch.

**Mitigation (chosen design):** propagate the original key through the
RRF merge. The minimum-change fix is in `hybrid.c` (one or two lines),
not in `contact_memory.c`. Specifically:

- In `entries_to_search_results` (`hybrid.c:16-34`) the `content`
  variable is already preferred to fall back to `entries[i].key` when
  content is empty. That's the wrong direction.
- Add a parallel array of original keys carried through
  `hu_search_result_t` (or smuggle the key in a new field on
  `hu_search_result_t`), and on the return trip re-populate
  `entries[i].key` from that array in `search_results_to_entries`.

**That is out of scope for US-3.1's "wire it in" mandate.** Sprint 3's
goal is wiring, not redesigning the RRF data model.

**Practical solution within scope:** in `recall_hybrid`, when we receive
`hu_retrieval_result_t` back from `hu_hybrid_retrieve`, look up each
returned entry's **original key** by content-matching against a single
unfiltered `mem->vtable->recall(query="", limit=very_large)` scan of
the backend, then apply the prefix filter on those resolved keys.

That is also expensive and ugly. So the **cleanest in-scope** path:

**Constrain the vector store at index time.** US-3.3 owns daemon-side
embedder/store init. Document there that the vector store the daemon
hands to `hu_memory_recall_for_contact` must be either (a) per-contact
(one store per contact), or (b) the store entries must encode the
contact_id in a metadata field that we can filter against pre-RRF.

Since we cannot rely on (a) or (b) being true in Sprint 3 — US-3.3 ships
a single in-memory store via `hu_vector_store_mem_create` —
**this story (US-3.1) accepts that cross-contact contamination is
possible in the hybrid path** and the only thing keeping the contract
honest is the post-filter. To make the post-filter work, US-3.1 takes
the minimum-viable fix:

**Minimum-viable fix that stays in scope:** modify `hybrid.c`'s
`search_results_to_entries` to preserve the original key from the
keyword path when available. Concretely, change the RRF merge to track
each `hu_search_result_t`'s originating `hu_memory_entry_t.key` (today
already plumbed via `entries_to_search_results` if we point at
`entries[i].key` instead of `entries[i].content`). The keyword path
returned by `hu_keyword_retrieve` does carry the proper key. The
semantic path's key comes from the vector store's stored content (which
the daemon will write as content-only, not key-prefixed).

**Decision: ship US-3.1 with the existing post-filter, and rely on the
keyword path's keys being correct.** The semantic-only results will
fail the prefix filter and be dropped — which is functionally correct
(we don't have a way to attribute them to a contact). The "Utah
hike" semantic test (AC-3.2.5) will pass IFF the keyword path also
returns the Utah entry (it will, because RRF merges both lists before
the post-filter sees them and the keyword path returns the Utah entry
under any reasonable BM25 ranking — verified by the existing keyword
test in `tests/test_memory_features.c:144-160` which already returns
contact-prefixed keys from FTS).

**Wait — that's wrong.** The Utah test query is `"outdoor plans"`. BM25
on `"outdoor plans"` against `"she is planning a Utah hiking trip"`
will likely match on `"plans"` ↔ `"planning"` (stemming) but is not
guaranteed. The whole reason for semantic recall is to *catch the
case where keyword misses*. If we drop semantic-only hits at the
post-filter we lose the win.

**Therefore the in-scope fix is mandatory:** US-3.1 must propagate the
key from `hu_memory_entry_t` through the RRF round-trip in
`hybrid.c`. Specifically:

1. Change `entries_to_search_results` (`hybrid.c:16-34`) to write
   `out[i].content = hu_strndup(alloc, entries[i].content, ...)` always
   (not the key fallback) **and** add a sibling array `original_keys[]`
   carried alongside `kw_sr` / `sem_sr` that maps result index → key.
2. Change `search_results_to_entries` (`hybrid.c:37-75`) to take that
   `original_keys[]` array and write the original key into
   `entries[i].key` instead of duplicating content.

**Scope assessment:** ~30-40 LOC change inside `hybrid.c` plus one new
helper struct. Still inside US-3.1's "wire it in" mandate because the
wire fails contract AC-3.2.6 without it. This is documented as the
HIGHEST risk for the story (see Risks → R-HIGH).

---

## Files to modify

| File | Change | Estimated LOC |
| ---- | ------ | ------------- |
| `include/human/memory.h` | Update signature (lines 168-172). Add `#include "human/memory/vector.h"`. Add `/* nullable */` annotation block before declaration. | +6 / -4 |
| `src/memory/contact_memory.c` | Split body into `recall_bm25` (static) + `recall_hybrid` (static). Public function becomes a 5-line router on `embedder && vector_store`. Convert `hu_retrieval_result_t` → `hu_memory_entry_t**` in `recall_hybrid`. Apply existing prefix filter to both. | +90 / -10 |
| `src/memory/retrieval/hybrid.c` | Propagate original `hu_memory_entry_t.key` through RRF round-trip so the post-filter has a real key to match. See R-HIGH mitigation. | +35 / -8 |
| `src/agent/agent_turn.c` | Update the call site at line 1490 (current HEAD; was line 839 per pre-resolved Q2 — discrepancy noted below). Pass `NULL, NULL` for now; US-3.3 wires real pointers. | +2 / -0 |
| `tests/test_memory_features.c` | Update 3 call sites (lines 116, 144, 155) — pass `NULL, NULL` to keep BM25-only behaviour. | +6 / -3 |
| `tests/test_e2e_agent_loop.c` | Update 3 call sites (lines 158, 193, 204) — pass `NULL, NULL`. | +6 / -3 |
| `tests/test_contact_memory_hybrid.c` (new) | Unit test covering AC-3.1.5 (error propagation via stub embedder returning `HU_ERR_NOT_SUPPORTED`) + AC-3.1.2 (BM25 fallback when one of embedder/store is NULL). | +180 |
| **Total** | | **~325 LOC** |

**Call-site discrepancy note for implementer:** the parent agent's
pre-resolved Q2 stated "one production call site at
`src/agent/agent_turn.c:839`". Current HEAD's `agent_turn.c` has the
call at **line 1490**, not 839, because of concurrent edits to that
file (visible in `git status`). The implementer **must** rebase off
`origin/main` commit `13b89763` (the worktree HEAD per the story
header) before editing, which will land them on a clean tree where the
line number matches whatever 13b89763 actually contains. After rebase,
re-grep:
`grep -rn "hu_memory_recall_for_contact(" src/ tests/`
and update every printed call site in the same commit. Do NOT carry
forward the `M` files in `git status`.

---

## Implementation steps (for the implementer agent)

1. **Rebase.** `git fetch origin && git reset --hard 13b89763`. Verify
   `git status` is clean. (The story header at
   `sprints/sprint-3/stories.md:8` names this commit as the worktree
   HEAD; the modified `agent_turn.c` / `agent_stream.c` / `main.c` are
   from a concurrent agent and must not enter this commit.)

2. **Header change.** Edit `include/human/memory.h:168-172`. Add the
   `embedder` and `vector_store` parameters before `out`. Add the
   `/* nullable */` annotation comment block above the declaration per
   AC-3.1.7. Add `#include "human/memory/vector.h"` near the top.

3. **`hybrid.c` key-propagation fix (R-HIGH mitigation).** Add an
   `original_key` + `original_key_len` field to a *local* tracking
   array alongside `kw_sr` / `sem_sr` in `hu_hybrid_retrieve`. Use it
   to repopulate `entries[i].key` in `search_results_to_entries`. Add
   a focused unit test in `tests/test_retrieval_hybrid.c` (or create
   the file if absent) that stores one entry with key
   `"contact:+15555550100:hike"` and content `"Utah hike planning"`,
   queries `"outdoor"`, and asserts the returned entry's `.key` still
   contains the literal prefix `"contact:+15555550100:"`. **Run this
   test before touching `contact_memory.c`** so the fix is validated
   in isolation.

4. **Split `contact_memory.c`.** Extract today's body lines 48-116
   into static `recall_bm25(...)`. Add new static
   `recall_hybrid(...)` that calls `hu_hybrid_retrieve` and applies
   the same filter loop (factor into a third static helper
   `filter_entries_by_prefix(...)` to avoid duplication). The public
   function becomes a 5-line router.

5. **Update production call site.** Edit `src/agent/agent_turn.c`
   (wherever the call lives post-rebase — `grep -n
   hu_memory_recall_for_contact src/agent/agent_turn.c`). Pass
   `NULL, NULL` for `embedder` and `vector_store`. Add a one-line
   comment: `/* US-3.3 will wire embedder/vector_store; NULL preserves
   BM25 behaviour today. */`

6. **Update test call sites.** Edit `tests/test_memory_features.c`
   (lines 116, 144, 155 in HEAD) and `tests/test_e2e_agent_loop.c`
   (lines 158, 193, 204 in HEAD). Each gains `NULL, NULL` before the
   `&entries, &count` pair. No behaviour change.

7. **Add new test file `tests/test_contact_memory_hybrid.c`.** Two
   subtests:
   - `recall_falls_back_to_bm25_when_embedder_null`: pass a non-NULL
     dummy `hu_vector_store_t` but NULL embedder; assert behaviour is
     bit-identical to the today-no-args path.
   - `recall_propagates_error_from_embedder_failure` (AC-3.1.5):
     construct a stub `hu_embedder_t` whose `embed` vtable returns
     `HU_ERR_NOT_SUPPORTED`; pass it alongside a real in-memory store;
     assert `hu_memory_recall_for_contact` returns
     `HU_ERR_NOT_SUPPORTED` and `*out == NULL`, `*out_count == 0`.

8. **Compile + targeted tests.**
   ```bash
   cmake --build --preset dev 2>&1 | tee /tmp/us31-build.log
   grep -c "error:" /tmp/us31-build.log    # must print 0
   ./build/human_tests --suite=Memory      # must be 0 failures
   ./build/human_tests --filter=contact_memory_hybrid
   ./build/human_tests --filter=retrieval_hybrid
   ```

9. **Full suite + ASan.**
   ```bash
   ./build/human_tests 2>&1 | tail -5
   # expect: 9800+ tests, 0 failures, 0 ASan errors
   ```

10. **Commit + report DONE.** Single commit on `sprint-3-hybrid-recall`
    with message:
    ```
    feat(memory): wire hu_hybrid_retrieve into hu_memory_recall_for_contact

    Adds nullable embedder + vector_store parameters; routes to
    hu_hybrid_retrieve when both are non-NULL, falls back to BM25/FTS
    otherwise. Preserves original entry keys through RRF so the
    contact-prefix filter still matches semantic-only hits.

    Sprint 3 / US-3.1
    ```
    The story's per-story DoD (`stories.md:21-31`) requires the commit
    to land **before** reporting DONE. Working-tree-only DONE is
    rejected.

---

## Risks

### R-HIGH — Key loss in RRF round-trip silently breaks the contact-prefix filter

**What could go wrong.** `hybrid.c`'s `search_results_to_entries`
populates `entries[i].key` from the search result's `content`, not the
original storage key. Our contact recall depends on the
`"contact:<contact_id>:"` prefix being literally in `entries[i].key`.
Without the fix in implementation step 3, **every semantic-only hit
will fail the prefix filter and be dropped**, defeating the entire
purpose of this story. The BM25-only test cases will still pass (the
keyword path keeps real keys in `hu_keyword_retrieve`), so the
regression hides until AC-3.2.5 — the dedicated semantic-recall test —
runs.

**Probability.** HIGH — it's a guaranteed silent failure mode, not a
race or edge case. Just running the code triggers it.

**Impact.** LARGE — the semantic-recall contract (AC-3.2.5) fails, the
sprint demo fails, US-3.3's daemon wiring becomes effectively dead
code.

**Mitigation.** Implementation step 3 fixes `hybrid.c` to carry the
original key through RRF. A focused unit test in
`tests/test_retrieval_hybrid.c` validates the fix in isolation before
the wiring change is layered on top. Verifier evidence: the test
asserts a stored key with `"contact:+15555550100:hike"` prefix survives
the round-trip even when the query matches only semantically. If that
test passes, the post-filter in `contact_memory.c` will work
correctly.

**Alternative mitigation (rejected, scope creep):** redesign
`hu_search_result_t` to carry both content and original key as
first-class fields. That's a 100+ LOC change touching 4 files and
3 retrieval strategies. Out of scope for US-3.1.

### R1 — Filter discards everything when hybrid returns N < limit results

**What could go wrong.** Today's BM25 path already requests `limit * 2`
from the backend (`contact_memory.c:62`) precisely because the prefix
filter rejects ~half. Hybrid retrieval may return entries from many
contacts at once (single shared vector store across all contacts), so
the keep-ratio post-filter is even lower. Asking for `limit` returns
`limit` results, of which 0 may survive the prefix filter for a
sparsely-populated contact.

**Probability.** MEDIUM (only triggers for low-volume contacts).

**Impact.** MEDIUM (recall returns 0 entries when there should have
been a match; user-visible "Mindy drew a blank" failure).

**Mitigation.** In `recall_hybrid`, set `opts.limit = limit * 4`
(quoted in the conversion block above). Four is a guess but conservative
enough to cover the worst case where 75% of hits belong to other
contacts. Document in a comment that the multiplier exists specifically
because hybrid is not contact-scoped at index time and that US-3.3's
per-contact vector store would let us drop the multiplier back to
`limit * 1`.

### R2 — Backward compatibility for callers that did not yet adopt

**What could go wrong.** Any caller compiled against the old signature
breaks at link/build time once the header changes. The implementer
needs to find them all.

**Probability.** LOW — `grep -rn "hu_memory_recall_for_contact(" src/
include/ tests/` shows 8 call sites total in HEAD (1 prod + 7 test). The
compiler will refuse to build any missed one.

**Impact.** SMALL — strict prototypes catch it immediately; no runtime
mismatch possible.

**Mitigation.** AC-3.1.6 mandates updating all callers in one commit.
Step 6 of the sequence covers the test files explicitly.

### R3 — Error-path leak when `hu_hybrid_retrieve` returns mid-allocation

**What could go wrong.** `hu_hybrid_retrieve` zero-initialises `out`
on entry and partial-allocates inside the function. On error it must
not leave dangling memory. AC-3.1.5 says we must propagate the error
"without freeing result buffers it did not allocate."

**Probability.** LOW — `hu_hybrid_retrieve` already cleans up its own
internal state on every error branch (verified by reading
`hybrid.c:96-272`; all `goto`-style cleanups via `hu_retrieval_result_free`
on partial results).

**Impact.** MEDIUM (ASan failure would block CI).

**Mitigation.** Call `hu_retrieval_result_free(alloc, &raw)` in the
error branch of `recall_hybrid`. `raw` is zero-initialised at the call
site (`hu_retrieval_result_t raw = {0};`); passing a zero-init struct
to `hu_retrieval_result_free` is safe (no-op).

### R4 — Graph parameter unused but allocated

**What could go wrong.** We pass `graph=NULL` to `hu_hybrid_retrieve`
(AC-3.1.4). Inside, the `#ifdef HU_ENABLE_SQLITE` block at
`hybrid.c:100-130` is fully guarded on `if (graph)`, so the dead branch
is skipped at runtime. There is no allocation in the unused branch.

**Probability.** LOW.
**Impact.** SMALL.
**Mitigation.** None needed. AC-3.1.4 verified by `grep -n "graph"
src/memory/contact_memory.c` returning exactly one match (the literal
`NULL` passed into the call).

### R5 — Concurrency on shared vector store

**What could go wrong.** `hu_vector_store_mem_create` is in-memory and
may not be thread-safe. The agent's recall path is called from the
turn handler on a single thread today, but if the daemon adds a
parallel embedder-write path (US-3.3 / US-3.4 backfill) racing with
recall, results would corrupt.

**Probability.** LOW for US-3.1 (no concurrency added by this story).
**Impact.** LARGE if it triggered (crash or wrong recall).
**Mitigation.** Out of scope for US-3.1; flag for US-3.3 to document
the daemon's read/write discipline on the in-memory store. Add a
TODO comment to the new `recall_hybrid` helper if it is the natural
home for the warning.

### R6 — Observability

**What could go wrong.** Silent fallback. If `embedder` create fails
in US-3.3 and the daemon passes `NULL, NULL`, the user just sees "BM25
recall" with no signal that hybrid was disabled.

**Probability.** MEDIUM (config errors are not rare).
**Impact.** MEDIUM (debuggability).
**Mitigation.** Not US-3.1's responsibility — US-3.3 owns the
init-time logging. But add a one-line `HU_LOG_DEBUG` (or whatever the
project's logging macro is — check `src/core/log.h` during
implementation) at the top of `recall_hybrid` so a future operator can
trace which path ran.

---

## Test strategy

- **Unit tests (new):** `tests/test_contact_memory_hybrid.c`
  - `recall_falls_back_to_bm25_when_embedder_null` — AC-3.1.2
  - `recall_falls_back_to_bm25_when_store_null` — AC-3.1.2
  - `recall_propagates_error_from_embedder_failure` — AC-3.1.5

- **Unit test (new, in `tests/test_retrieval_hybrid.c` — create file if
  absent):**
  - `hybrid_round_trip_preserves_original_key` — R-HIGH mitigation,
    validates the `hybrid.c` key-propagation fix in isolation.

- **Existing tests (update only, no behaviour change):**
  - `tests/test_memory_features.c:116, 144, 155` — pass `NULL, NULL`.
  - `tests/test_e2e_agent_loop.c:158, 193, 204` — pass `NULL, NULL`.
  All should pass unchanged after recompile.

- **Integration test (owned by US-3.2):**
  AC-3.1.3 routes verification through AC-3.2.5 (the semantic-recall
  assertion in the new integration test). This story does not block on
  US-3.2 — but its end-to-end proof is the integration test's
  `outdoor plans` → `Utah` assertion.

- **ASan + full suite:**
  ```bash
  cmake --build --preset dev
  ./build/human_tests
  ```
  Expected: 9800+ tests pass, 0 failures, 0 ASan errors.

---

## Acceptance criteria mapping

| AC | Evidence anchor |
| -- | --------------- |
| AC-3.1.1: hybrid_retrieve called when embedder + store non-NULL | New `recall_hybrid` static in `contact_memory.c`; `grep -n "hu_hybrid_retrieve" src/memory/contact_memory.c` returns ≥ 1 match. |
| AC-3.1.2: backward-compat fallback when either is NULL | Test `recall_falls_back_to_bm25_when_embedder_null` + `recall_falls_back_to_bm25_when_store_null`. Full existing test suite passes. |
| AC-3.1.3: RRF wiring + contact-prefix filter on hybrid output | Routing block (verbatim in §Approach). AC-3.2.5 integration test catches semantic Utah hit. |
| AC-3.1.4: graph=NULL passed, only one `graph` ref in contact_memory.c | `grep -n "graph" src/memory/contact_memory.c` returns exactly 1 line — the literal `NULL` in the call. |
| AC-3.1.5: error propagation without leaks | Test `recall_propagates_error_from_embedder_failure`; ASan clean on full suite. |
| AC-3.1.6: all callers updated | Step 6 (test files) + Step 5 (prod call site); `cmake --build --preset dev` exits 0 proves the compiler accepts every call site. |
| AC-3.1.7: header updated with /* nullable */ annotation | Step 2; `grep -A6 "hu_memory_recall_for_contact" include/human/memory.h` shows both new params + nullable annotation. |

All seven ACs trace to a concrete artifact. No AC is satisfied by
hand-wave.

---

## Out of scope (reaffirmed)

- Daemon-side embedder/store initialisation — US-3.3.
- Backfill of existing memories — US-3.4.
- Per-contact vector store partitioning — deferred (R5 follow-up).
- Adding similarity scores to the public `hu_memory_entry_t**` return
  shape — would expand AC-3.1.1's mandated signature change. Defer to a
  separate story if Sprint 4 needs it.
- Touching `hu_search_result_t`'s shape (carrying key as a first-class
  field) — see R-HIGH "alternative mitigation rejected".
- Logging strategy for the fallback signal (R6) — US-3.3.

---

`RESULT_tech-lead=READY`
