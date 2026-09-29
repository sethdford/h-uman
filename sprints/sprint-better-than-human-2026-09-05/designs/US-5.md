# Design for US-5: Register-conditioned semantic recall (protect the LIVE gate from EI drift)

STATUS: READY

## Key finding that simplifies the skeleton's assumed plumbing

The skeleton assumed the register signal might not be reachable at the `HU_GATE_LIVE`
call site and that an `hu_retrieval_options_t` field might be needed to carry it in.
**Verified false.** `hu_hybrid_retrieve(alloc, backend, embedder, vector_store, graph,
query, query_len, opts, out)` (`src/memory/retrieval/hybrid.c:698-701`) receives `query`/
`query_len` as its own parameters, and they are **never reassigned** between entry and
the `HU_GATE_LIVE` branch at line 861 (read the full body, lines 698-869: `query` is only
read, passed to `hu_keyword_retrieve`, `hu_graph_build_context`, `hu_semantic_retrieve` —
never mutated). Traced the call chain to confirm `query` at that point really is the
current turn's raw incoming text, not something already summarized/rewritten:

- `src/memory/retrieval/engine.c:51` — the `retrieve` vtable thunk forwards its own
  `query`/`query_len` straight into `hu_hybrid_retrieve` unchanged.
- `src/agent/memory_loader.c:169-170` — `hu_memory_loader_load`'s `query` param is what's
  forwarded to `loader->retrieval_engine->vtable->retrieve(...)`.
- `src/agent/agent_turn.c:2264-2265` (primary reactive/batch path) and
  `src/agent/agent_stream.c:494-495` (streaming path) both call
  `hu_memory_loader_load(&loader, msg, msg_len, ...)` — `msg`/`msg_len` is the agent
  turn's incoming message, unmodified.
- Two secondary call sites exist inside the tool-use iteration loop,
  `src/agent/agent_turn.c:10394` (`last_result`, a tool result's text) and `:10423`
  (`msg` again, the "goal-relevant memory" re-query). See **Scope note** below.

**Conclusion: the predicate can read `query`/`query_len` directly inside
`hu_hybrid_retrieve`, at the call site itself. No new `hu_retrieval_options_t` field, no
`agent_turn.c`/`agent_stream.c`/`daemon.c` changes are needed.** This removes an entire
axis of risk the skeleton was hedging against (touching `agent/` or `daemon.c`, which
would trip `agent-core-boundary.md` / the file-size ceiling).

Second verified finding, also load-bearing for the eval-script design below:
`scripts/eval_semantic_live_gate.py`'s LIVE arm does **not** call `hu_hybrid_retrieve` at
all — it calls `human memory search --semantic <ctx>` (semantic-only: `hu_semantic_retrieve`
directly, `src/app/cli_commands.c:664-681`) and reproduces the `HU_GATE_LIVE` branch's
filter+clamp behavior in Python (`hit_is_excluded`, `build_memories_block`, already
documented as a deliberate mirror in `semantic_recall.h`'s block comment: *"the daemon's
own in-process recall is not addressable from Python... mirrors hit_is_excluded in Python
because its LIVE arm calls memory search --semantic, not the hybrid path"*). A `--hybrid`
CLI mode does exist (`src/app/cli_commands.c:684-712`) and does call `hu_hybrid_retrieve`,
but it unconditionally sets `opts.reconstructive = true` (Contract C2 scene-select path) —
confirmed **not** what production uses: `src/agent/memory_loader.c:160-165`'s designated
initializer never sets `.reconstructive`, so it defaults `false` and production always
takes the plain RRF+cross-encoder path. Switching the eval script's LIVE arm to `--hybrid`
would therefore measure a *different* retrieval algorithm than production runs — worse
fidelity, not better. **The register gate's Python-side measurement must follow the
existing mirror pattern (add a Python twin of the predicate), not switch to `--hybrid`.**
This is not a new anti-pattern introduced by this story; it is the same constraint that
already forced `hit_is_excluded` to be duplicated, called out again here so the next
reader doesn't propose "just use `--hybrid`" as a simplification.

## Approach

### C side — `src/memory/semantic_recall.c` / `.h`

Add a pure predicate (`.claude/rules/security-predicate-extraction.md` shape: `const
char*, size_t -> bool`, no allocator, no I/O):

```c
/* Register boundary shared with scripts/blind_ab/authorship_gap.py:87
 * (reg = "casual" if len(r.split()) <= 12 else "substantive"). Word count
 * mirrors Python's str.split(): runs of whitespace (isspace) delimit words;
 * leading/trailing whitespace does not add a word. */
#define HU_SEMANTIC_RECALL_REGISTER_MAX_CASUAL_WORDS 12u

/* True when the turn's register admits semantic recall (word count strictly
 * greater than the casual boundary, i.e. "substantive"). NULL / empty input
 * is treated as casual (fails closed to suppression — recall is the side
 * being gated for safety, not admission, per feature-gate-requires-
 * measurement.md; an ambiguous turn should not get the untested-for-casual
 * behavior). Pure: no I/O, no allocation. */
bool hu_semantic_recall_register_admits(const char *query, size_t query_len);

/* $HU_SEMANTIC_RECALL_REGISTER_GATE=off|shadow|live, default OFF. Layered on
 * top of hu_semantic_recall_mode(): only consulted inside the HU_GATE_LIVE
 * branch, so this being non-OFF has zero effect unless HU_SEMANTIC_RECALL is
 * already live. See feature-gate-requires-measurement.md — promotion to LIVE
 * requires scripts/eval_semantic_live_gate.py's register_breakdown.substantive
 * verdict to stay PROMOTE-eligible (composite/EI/reality within the existing
 * tolerances), not merely a green build. */
hu_gate_mode_t hu_semantic_recall_register_gate_mode(void);
```

`.c` implementation — word count by hand (no existing helper matches Python's
`str.split()` semantics; grepped `include/human/core/string.h` and found none):

```c
bool hu_semantic_recall_register_admits(const char *query, size_t query_len) {
    if (!query || query_len == 0)
        return false; /* fail closed: ambiguous input is treated as casual */
    size_t words = 0;
    bool in_word = false;
    for (size_t i = 0; i < query_len; i++) {
        bool ws = isspace((unsigned char)query[i]) != 0;
        if (!ws && !in_word)
            words++;
        in_word = !ws;
    }
    return words > HU_SEMANTIC_RECALL_REGISTER_MAX_CASUAL_WORDS;
}

hu_gate_mode_t hu_semantic_recall_register_gate_mode(void) {
    return hu_gate_mode_from_env("HU_SEMANTIC_RECALL_REGISTER_GATE", HU_GATE_OFF);
}
```

`#include <ctype.h>` is already present in `semantic_recall.c` (used by
`content_is_bare_identity_question`) — no new include needed.

### Call site — `src/memory/retrieval/hybrid.c:861` (the ONLY line touched in this file)

```c
} else if (hu_semantic_recall_mode() == HU_GATE_LIVE && semantic_result.count > 0) {
    hu_gate_mode_t reg_gate = hu_semantic_recall_register_gate_mode();
    bool admits = hu_semantic_recall_register_admits(query, query_len);
    if (reg_gate != HU_GATE_OFF && !admits) {
        if (reg_gate == HU_GATE_LIVE) {
            hu_log_info("semantic_recall_register", NULL,
                        "live: suppressed (casual, words<=%u) sem=%zu -> 0",
                        HU_SEMANTIC_RECALL_REGISTER_MAX_CASUAL_WORDS, semantic_result.count);
            hu_retrieval_result_free(alloc, &semantic_result);
            semantic_result.entries = NULL;
            semantic_result.count = 0;
            semantic_result.scores = NULL;
        } else { /* SHADOW: log the would-be suppression, change nothing */
            hu_log_info("semantic_recall_register", NULL,
                        "shadow: would suppress (casual, words<=%u) sem=%zu (kept)",
                        HU_SEMANTIC_RECALL_REGISTER_MAX_CASUAL_WORDS, semantic_result.count);
        }
    }
    if (reg_gate != HU_GATE_LIVE || admits) {
        /* existing body, byte-for-byte unchanged: */
        size_t before = semantic_result.count;
        size_t filtered = hu_semantic_recall_filter_result(alloc, &semantic_result);
        size_t kept = hu_semantic_recall_clamp_result(alloc, &semantic_result,
                                                       hu_semantic_recall_max_bytes(),
                                                       HU_SEMANTIC_RECALL_HIT_MAX_BYTES);
        hu_log_info("semantic_recall", NULL,
                    "live: sem=%zu filtered=%zu kept=%zu bytes=%zu budget=%zu", before, filtered,
                    semantic_result.count, kept, hu_semantic_recall_max_bytes());
    }
}
```

Default-OFF verified: `hu_gate_mode_from_env` returns `HU_GATE_OFF` for an unset env var
(`gate_mode.h`'s documented contract), so `reg_gate == HU_GATE_OFF` short-circuits both new
branches and the function is byte-for-byte identical to today (AC-5.5). `:8741` / the
service-loop are not touched or restarted by this story — the env var only changes what a
freshly-started process reads.

### Scope note — the two secondary call sites (agent_turn.c:10394/10423)

Because the predicate reads whatever `query` the caller passes, it also applies (correctly,
by construction — it is genuinely pure) to the tool-iteration-loop mid-conversation
retrieval at `agent_turn.c:10394` (`last_result`, a tool result's text, already gated
`last_result_len > 20` chars before the loader is even called) and `:10423` (`msg` again,
same text as the primary call). This is very unlikely to change behavior in practice (tool
output and the original user message both tend to exceed 12 words once past the existing
20-char gate) but it is a real, honest side effect of NOT plumbing a narrower "is this the
user's primary turn" flag through `opts`. Documented here rather than silently accepted:
if a future measurement shows this secondary-path suppression matters, it is a one-line
fix (skip the gate when `opts` carries some future "this is not the primary turn" marker) —
not a blocker for this story, whose story and measurement are both about the primary
per-turn register (the RAG A/B finding cited in AC-5.3's motivation was about the
user's own message, not tool output).

### Python side — `scripts/eval_semantic_live_gate.py`

1. **Tag every selected context by register** where it's selected (`select_contexts`
   already returns a flat `list[str]`; add a same-length parallel classification, computed
   once, right after selection in `main()`):

   ```python
   REGISTER_MAX_CASUAL_WORDS = 12  # mirrors semantic_recall.h; keep the two numbers equal

   def classify_register(text):
       """Mirrors src/memory/semantic_recall.c's hu_semantic_recall_register_admits —
       same word-count rule (whitespace-run split, i.e. Python str.split() semantics,
       which the C loop replicates character-by-character). Keep in lock-step, same
       discipline as hit_is_excluded's existing C<->Python mirror (semantic_recall.h)."""
       return "casual" if len(text.split()) <= REGISTER_MAX_CASUAL_WORDS else "substantive"

   registers = {i: classify_register(ctx) for i, ctx in enumerate(contexts)}
   ```

2. **New flag** `--register-gate {off,shadow,live}` (default `off`), mirroring the C
   three-state gate so the harness can simulate all three states without ever touching the
   live daemon's env:
   - `off`: `run_arm` unchanged; per-register **reporting** still happens (see step 4) as a
     diagnostic split of the existing unconditional-LIVE numbers — this reproduces the
     "aggregate hides the split" evidence the story cites (0.919/4.275 aggregate) without
     requiring a second live run.
   - `shadow`: `run_arm`'s `live` branch behaves exactly as `off` (memories block still
     built and injected for casual contexts — SHADOW never changes output) but records
     `"register_gate_would_suppress": registers[i] == "casual"` per context, giving the
     harness the same OFF/SHADOW/LIVE parity the C gate has.
   - `live`: in `run_arm`, when `arm_name == "live"` and `registers[i] == "casual"`, **skip
     the `semantic_search` call entirely** (cheaper than calling it and discarding, and
     behaviorally identical — recall_bytes ends at 0 either way) — this is the mirror
     enactment of the C predicate's suppression, analogous to how `hit_is_excluded` is
     already mirrored for content filtering.

3. **`build_context_rows`**: add `"register": registers[i]` to each row (a bucket label,
   not content — safe under the existing "no incoming-message text, no reply text" privacy
   contract; confirmed by reading a live sample row from
   `docs/plans/2026-08-02-semantic-retrieval/semantic-live-gate-2026-09-03-content-filter.json`:
   `{"id": 0, "recall_bytes": 618, "shadow": {...}, "live": {...}}` — no text fields exist
   today, so adding a coarse bucket label changes nothing about that guarantee).

4. **Per-register breakdown** (new `register_breakdown` key in the output doc, additive —
   the existing top-level `shadow`/`live`/`verdict` keys are unchanged so old consumers of
   the JSON schema keep working):

   ```python
   def register_ids(ids, registers, want):
       return [i for i in ids if registers[i] == want]

   casual_ids = register_ids(ids, registers, "casual")
   substantive_ids = register_ids(ids, registers, "substantive")

   def summarize_register(name, reg_ids, min_n_register):
       if len(reg_ids) < min_n_register:
           return {"n": len(reg_ids), "verdict": "INCONCLUSIVE",
                   "reasons": [f"{name}: n={len(reg_ids)} < --min-n-register {min_n_register}"]}
       s = summarize_paired_arm(shadow_results, reg_ids)
       l = summarize_paired_arm(live_results, reg_ids)
       if name == "casual" and args.register_gate == "live":
           # AC-5.4: hard, non-vacuous assertion — not an eyeballed number.
           # A casual id with recall_bytes > 0 here means the mirror (or the
           # real predicate it mirrors) is broken; REFUSE rather than report
           # a wrong PROMOTE (reports-success-does-nothing.md).
           leaking = [i for i in reg_ids if live_results[i]["recall_bytes"] > 0]
           if leaking:
               sys.exit(f"REFUSE: register gate LIVE but casual ids {leaking} have "
                        f"recall_bytes > 0 in the LIVE arm — suppression did not happen")
           return {"n": len(reg_ids), "shadow": s, "live": l,
                   "all_recall_bytes_zero": True, "verdict": "GATE_CONFIRMED"}
       coverage = recall_coverage_of(live_results, reg_ids)
       verdict, reasons = decide_verdict(s, l, coverage, args.composite_tolerance,
                                         args.ei_tolerance, args.reality_tolerance,
                                         args.min_recall_coverage)
       return {"n": len(reg_ids), "shadow": s, "live": l,
               "recall_coverage": coverage, "verdict": verdict, "reasons": reasons}

   doc["register_breakdown"] = {
       "boundary_words": REGISTER_MAX_CASUAL_WORDS,
       "register_gate_mode": args.register_gate,
       "casual": summarize_register("casual", casual_ids, args.min_n_register),
       "substantive": summarize_register("substantive", substantive_ids, args.min_n_register),
   }
   ```

   New arg: `ap.add_argument("--min-n-register", type=int, default=10, help="per-register "
   "floor for register_breakdown; below this the subset's verdict is INCONCLUSIVE, not "
   "refused — the aggregate verdict is unaffected")`. Default 10 chosen conservatively
   below the aggregate's own `--min-n 30`; the actual casual/substantive split ratio in the
   corpus is UNVERIFIED (would need a live run against `~/.human/logs/eval-archive/...` to
   know), so this is a starting value, not a measured one — flag it as such if the first
   real run comes back skewed enough that one bucket is chronically INCONCLUSIVE.

5. **Overall/aggregate verdict is unchanged** — still computed over the full paired `ids`
   exactly as today. `register_breakdown` is purely additive. This keeps the existing
   `2026-09-03` PROMOTE record comparable to future runs (AC-5.3 wants the split
   *in addition to* the aggregate, not instead of it).

6. **AC-5.3's exact numeric check** ("substantive-only paired EI must not drop beyond the
   script's existing 0.15 tolerance and composite not beyond 0.02") is enforced by
   `summarize_register`'s call into the SAME `decide_verdict` with the SAME
   `args.composite_tolerance`/`args.ei_tolerance` defaults (0.02 / 0.15) — no new tolerance
   constants invented, per AC-5.2's spirit of reusing named constants rather than
   re-deriving them.

7. **Docstring update**: extend the module docstring's step 6 ("Composes a per-arm
   composite...") with one sentence noting the register split, and extend the "REFUSES"
   list with the new casual-leak refusal from step 4.

### One-request-in-flight / refuse-on-too-few-successes — already satisfied, verified

`run_arm` is a plain `for` loop (`scripts/eval_semantic_live_gate.py:568`), called
sequentially — `shadow_results, shadow_fail = run_arm("shadow", ...)` fully completes
before `live_results, live_fail = run_arm("live", ...)` starts (`main()`, no threading, no
`asyncio`, no `concurrent.futures` anywhere in the file — grepped, zero hits). Every HTTP
call to `:8741` already carries `X-HU-Priority: batch` (`PRIORITY_HEADER`, line ~121) and
happens one at a time. **This story adds no concurrency** — the register-gate additions run
inside the same sequential loop. The existing `refuse()` contract (exit 2, write nothing)
already covers "too few successes" for the aggregate; step 4 above extends the same
discipline to the per-register subsets without weakening the aggregate's contract.

## Files touched

- `include/human/memory/semantic_recall.h` — predicate + gate-mode function prototypes +
  `HU_SEMANTIC_RECALL_REGISTER_MAX_CASUAL_WORDS` constant + doc comments.
- `src/memory/semantic_recall.c` — predicate + gate-mode implementations (~20 LOC).
- `src/memory/retrieval/hybrid.c` — call site only, inside the existing `HU_GATE_LIVE`
  branch at line 861 (~25 LOC delta, no other line in the file touched).
- `scripts/eval_semantic_live_gate.py` — register tagging, `--register-gate` +
  `--min-n-register` flags, `register_breakdown` doc key, docstring update (~120-160 LOC).
- `tests/test_semantic_recall_register.c` — new file (see Tests below).
- `CMakeLists.txt` — register the new test file next to `tests/test_semantic_recall.c`
  (line ~3666), same unconditional (no `HU_ENABLE_*` guard) list — `semantic_recall.c`
  itself is registered unconditionally (`CMakeLists.txt:1022`), so no gate-symmetry
  concern (`.claude/rules/test-source-gate-symmetry.md` doesn't apply — there is no gate to
  match). One `#ifdef HU_ENABLE_SQLITE` block *inside* the new test file, matching
  `tests/test_hybrid_reconstructive.c`'s stub-runner pattern, for the one integration test
  that needs the sqlite-vec store fixture.
- `tests/test_main.c` — forward-declare + call `run_semantic_recall_register_tests()`,
  next to the existing `run_semantic_recall_tests()` registration (line 417/1484).

Not touched: `src/daemon.c`, `src/agent/*`, `src/persona/*` — confirmed by construction
(the predicate lives in `src/memory/`, the call site is the one line in `hybrid.c`).

## Risk analysis

1. **Never restart `:8741`/service-loop.** This story's C change only affects freshly
   started processes reading a new env var; the Python measurement only reads from `:8741`
   (generation calls), never restarts it — same as the existing script does today.
2. **Never a second model loader.** The eval script's embed/generate calls go through the
   existing single `:8741` server exactly as the unmodified script already does; no new
   process, no new port. `scripts/check-no-resident-model.sh` is unaffected (this story
   touches no training/serving script).
3. **No private text in committed artifacts.** The new `register` field is a bucket label
   (casual/substantive), not content — verified against a live sample row (see Python
   section, point 3). No incoming/reply text is added anywhere.
4. **Ratchets stay green, verified by construction, not by running the scripts (no build
   available to me in this read-only session — see UNVERIFIED note):**
   - `check-file-size-ceiling.sh`: `hybrid.c` is 988 LOC, `semantic_recall.c` is 292 LOC
     today; the ceiling (`MAX_BASELINE=12313`, `src/daemon.c`) is untouched by this story
     and neither file is remotely close to it after the ~20-25 LOC additions.
   - `check-agent-core-boundary.sh` / `check-modeled-person-layering.sh`: grepped both
     scripts — neither references `src/memory/` at all (they scan `src/agent/` and
     `src/persona|cognition|behavior/` respectively). This story touches none of those
     directories, so AC-5.7's two boundary checks are unaffected **by construction**, not
     coincidentally.
   - `check-sqlite-includer-ratchet.sh`: the new test file's SQLite-gated integration test
     includes `<sqlite3.h>`-adjacent headers only via the existing
     `store_sqlite_vec.h`/`hu_sqlite_memory_get_db` pattern already used by
     `test_hybrid_reconstructive.c` (a test file, exempt like all of `tests/`).
   - `check-clone-ratchet.sh` / `check-no-new-root-files.sh` / `check-edge-context-isolation.sh`:
     not applicable — no new root `.c` file, no `src/channels/` file touched.
   - UNVERIFIED: I did not run any of these scripts (read-only session outside
     `sprints/.../designs/`); the implementer must run them post-change and paste output,
     per `.claude/rules/verify-before-you-claim.md` — grep-based "by construction" claims
     above are a strong prior, not a substitute for running the actual gate.
5. **Default-OFF so prod is unchanged.** Verified: `hu_gate_mode_from_env` returns the
   `unset_default` argument (`HU_GATE_OFF`, passed explicitly) when the env var is unset —
   read from `gate_mode.h`'s documented contract, this is the same helper every other
   OFF/SHADOW/LIVE gate in the codebase already uses, so there is no new parsing surface to
   get wrong.

## Test seam

The predicate is pure and hermetic — no DB, no network, no model, no env var needed for
the predicate itself (the *gate mode* function does read `getenv`, tested the same way
`test_semantic_recall.c` already tests `hu_semantic_recall_mode()` — `setenv`/`unsetenv`
around each case).

## Tests — `tests/test_semantic_recall_register.c` (non-vacuous, per AC-5.6)

**Pure predicate (no `#ifdef`, always compiled):**

- `test_register_admits_boundary_12_words_is_casual` — exactly 12 words → `false`.
- `test_register_admits_boundary_13_words_is_substantive` — exactly 13 words → `true`.
- `test_register_admits_short_casual_input_suppressed` — e.g. `"yo what's up"` (3 words) →
  `false`.
- `test_register_admits_long_substantive_input_admitted` — a realistic >12-word sentence →
  `true`.
- `test_register_admits_null_fails_closed` — `NULL, 0` → `false`.
- `test_register_admits_whitespace_only_fails_closed` — `"   \n\t  "` → `false` (0 words).
- `test_register_admits_extra_whitespace_does_not_inflate_count` — `"  hi   there  "` (2
  words, irregular spacing) → `false`, pinning that the count matches Python's
  `str.split()` (which collapses runs and ignores leading/trailing whitespace) and not a
  naive "count spaces + 1".
- `test_register_gate_mode_default_off` — unset env → `HU_GATE_OFF` (`setenv`/`unsetenv`
  around the call, same pattern as `test_semantic_recall.c` lines 11-17).
- `test_register_gate_mode_parses_shadow_and_live` — `setenv("HU_SEMANTIC_RECALL_REGISTER_GATE", "shadow"/"live", 1)`.

**Integration, `#ifdef HU_ENABLE_SQLITE` (mirrors `test_hybrid_reconstructive.c`'s stub
embedder + sqlite-vec store fixture; `#else` provides an empty `run_...` stub so the symbol
resolves in non-SQLite builds — no `test-source-gate-symmetry.md` violation since
`semantic_recall.c`/`hybrid.c` compile unconditionally and only the vector-store fixture
needs SQLite):**

- `test_hybrid_retrieve_register_gate_live_suppresses_casual_turn` — seed one memory whose
  content the stub embedder maps near the query vector; set
  `HU_SEMANTIC_RECALL=live`, `HU_SEMANTIC_RECALL_REGISTER_GATE=live`; call
  `hu_hybrid_retrieve` with a <=12-word query; assert the returned `hu_retrieval_result_t`
  contains **zero** entries sourced from the semantic leg (only keyword-sourced entries, or
  zero total if no keyword match) — i.e. "a casual turn produces NO recall block under the
  gate," proven by actually calling the production symbol, not by re-asserting the
  predicate in isolation (`.claude/rules/verify-before-you-claim.md` /
  `integration-done-contract.md` — this is the caller-outside-definition-file proof: grep
  after landing must show `hu_semantic_recall_register_admits` called from
  `hybrid.c`, not just defined and unit-tested in `semantic_recall.c`).
- `test_hybrid_retrieve_register_gate_live_admits_substantive_turn` — same fixture, same
  env, a >12-word query matching the seeded memory; assert the semantic entry **is**
  present in the result — "a substantive one does."
- `test_hybrid_retrieve_register_gate_shadow_never_changes_output` — same fixture,
  `HU_SEMANTIC_RECALL_REGISTER_GATE=shadow`, casual query; assert the result is IDENTICAL
  (count and content) to the same call with the register gate unset — pins AC-5.5's
  "SHADOW never changes behavior" half of the three-state contract, which nothing else in
  this test file would otherwise catch (the pure-predicate tests can't see the call site's
  wiring).
- `test_hybrid_retrieve_register_gate_off_default_unchanged` — same fixture, gate env
  unset, casual query; assert result identical to a baseline captured with
  `HU_SEMANTIC_RECALL_REGISTER_GATE` never set at all — pins AC-5.5's literal "current LIVE
  production behavior is unchanged unless the new var is explicitly set."

Each assertion above is pre/post-shaped (per `integration-done-contract.md`): construct the
fixture so the semantic leg WOULD have non-trivially contributed absent the gate (seed data
that the stub embedder ranks highly), then assert the gate's effect on that specific,
falsifiable outcome — not `count >= 0` or any tautology `tests-that-pin-bugs.md` warns
against.

## Measurement

Paired SHADOW-vs-LIVE via `scripts/eval_semantic_live_gate.py --register-gate live`,
against shared production `:8741`, one request in flight (verified sequential, see above),
refusing (exit 2, no JSON written) on too few PAIRED successes overall exactly as today,
and reporting (not refusing) `INCONCLUSIVE` per-register when a bucket is under
`--min-n-register`. AC-5.4 is enforced as a hard `sys.exit` refusal (not a soft
warning) if any casual id leaks `recall_bytes > 0` under `--register-gate live` — this
would mean the Python mirror (or, if the mirror is later replaced, the real gate itself)
is broken, and a PROMOTE verdict computed on top of that would be exactly the
"reports-success-does-nothing.md" failure shape this whole rule file exists to catch.

Sequence:
1. Run `scripts/eval_semantic_live_gate.py --register-gate off` once (or reuse the existing
   `semantic-live-gate-2026-09-03-content-filter.json` run's raw per-context data if it's
   still available — UNVERIFIED whether the script currently persists enough to
   recompute `register_breakdown` from an old JSON without re-generating; the `context_rows`
   in that file lack incoming-message text by design, so `registers[i]` cannot be recovered
   from it after the fact — a **fresh run is required**, this is not free) to get the
   diagnostic per-register split of today's *unconditional* LIVE behavior (expected,
   per the story's cited RAG A/B finding: substantive holds up, casual likely shows the EI
   drop).
2. Run with `--register-gate live` to get the `GATE_CONFIRMED` (casual, recall_bytes all
   zero) + `PROMOTE`/`HOLD` (substantive, real tolerance check) verdicts required by
   AC-5.3/AC-5.4.
3. Commit both `docs/plans/2026-08-02-semantic-retrieval/semantic-live-gate-<date>-register-off.json`
   and `...-register-live.json` (or fold into one run with both flags recorded — the doc
   already carries `register_gate_mode`, so one script invocation per mode, two files, is
   simplest and matches the existing one-file-per-run convention in that directory).

## Refusal conditions (superset of the existing script's)

- Existing: embedder/judge preflight failure, memory.db copy failure, <`--min-n` usable
  contexts, <`--min-n` PAIRED successes, <`--min-n` anti_ai or EI scores in the paired set.
- New, hard refuse (exit 2, no JSON): any casual id has `recall_bytes > 0` under
  `--register-gate live` (mirror/gate contradiction — AC-5.4's proof failed).
- New, soft/non-blocking (JSON still written, subset marked `INCONCLUSIVE`): a register
  bucket has fewer than `--min-n-register` (default 10) paired contexts.

## Privacy

Unchanged from the existing script's contract: no incoming-message or reply text in the
output JSON. The new `register` field is a two-valued bucket label derived from a word
count, carrying strictly less information than the existing `recall_bytes` integer already
in every row — not a new privacy surface. Committed test fixtures in
`tests/test_semantic_recall_register.c` use synthetic sentences (e.g. "the weather has been
really nice this week and I've been meaning to go for a walk"), never real corpus text.

## Conflicts — verified against this sprint's own `stories.md`

`grep -n "hybrid\.c\|semantic_recall" sprints/sprint-better-than-human-2026-09-05/stories.md`
returns only US-5's own lines (163, 184, 186-189). **No other story in this sprint touches
`src/memory/retrieval/hybrid.c` or `src/memory/semantic_recall.c`.** Clean.

Soft conflict, flagged not blocking: US-8 (AC-8.3) says it will extend
`scripts/eval_semantic_live_gate.py`'s "paired-arms machinery, **or** a new script
following the same shape" — an explicit either/or. If US-8's implementer picks "extend the
same file," it will collide with this story's `--register-gate`/`register_breakdown`
additions (both stories add CLI flags and doc keys to the same ~900-line file in the same
sprint). Recommend to the scrum-master: sequence US-5 before US-8 (US-5 is P1, US-8 is P3,
so priority order already implies this), and have whoever picks up US-8 read the merged
`register_breakdown` shape here before choosing "extend" vs "new script" — extending after
US-5 lands is fine; extending in parallel is not. No code change needed now, just
sequencing awareness.

Outside this sprint: `hybrid.c` has a real churn history (`git log --oneline -8` shows 8
commits touching it, most recently `3fa10ac40` "carry memories.key through hybrid RRF
merge," merged) and this repo runs many concurrent worktrees
(`.claude/rules/session-worktree-isolation.md`). This design is based on the current
on-disk state of this worktree; the implementer should re-`git log`/re-read the exact lines
before editing, in case another session lands a change to the same ~15-line region first.

## Out of scope

Flipping the register gate to default-ON or default-SHADOW; any change to
`HU_SEMANTIC_RECALL`'s own default; `src/daemon.c`; the batch-reply carve-out; switching
`eval_semantic_live_gate.py`'s LIVE arm from `--semantic`-based mirroring to `--hybrid`
(confirmed wrong fidelity above); narrowing the predicate's reach to exclude the two
secondary tool-loop call sites (documented as a scope note, not fixed here).

## Estimate

**L**, consistent with the story's own estimate. Breakdown: ~20 LOC C (predicate + gate-mode
fn, isolated bounded context, no plumbing needed thanks to the verified finding above) +
~25 LOC call-site change in `hybrid.c` + ~140 LOC Python (new flag, per-register summarize,
hard-refusal assertion, docstring) + ~180 LOC new hermetic test file (9 pure-predicate
tests + 4 SQLite-gated integration tests, following an existing fixture pattern so no new
test infrastructure is invented) + 2 lines of CMake/test_main.c registration + one real
measurement run against shared `:8741` (sequential, ~40 contexts x 2 arms x generation+judge
latency — the existing script's own run time, unchanged by this story). The complexity is
in getting the eval-script per-register logic and refusal conditions right, not in the C
change, which is small and isolated.

## RESULT_tech-lead=READY

Design is grounded in the current on-disk state of `src/memory/retrieval/hybrid.c`,
`src/memory/semantic_recall.{c,h}`, `scripts/eval_semantic_live_gate.py`, and
`scripts/blind_ab/authorship_gap.py`; the call chain from `agent_turn.c`/`agent_stream.c`
down to the `HU_GATE_LIVE` branch was traced and confirmed (register is computable in
place, no options-field plumbing needed); the sprint's own `stories.md` was grepped and
shows no other story touching the two C files. Two items are explicitly UNVERIFIED and
called out for the implementer to close: (1) the four file-size/boundary/ratchet scripts
were reasoned about by construction/grep, not executed — run them post-change; (2) whether
a fresh `--register-gate off` baseline run is actually required vs. recoverable from the
existing `2026-09-03` JSON — it is required, since that JSON's `context_rows` carry no text
to reclassify by register after the fact.
