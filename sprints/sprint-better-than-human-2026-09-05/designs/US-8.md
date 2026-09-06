# Design for US-8: Difficulty-based routing SHADOW — log, measure, do not flip

**Status:** READY — verified against code (grep evidence inline; anything not
directly verified is marked UNVERIFIED).
**Date:** 2026-09-05
**Worktree:** `/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-better-than-human-2026-09-05`

## 0. What "today" actually does (verified, corrects the skeleton's framing)

The skeleton said CONVERSATIONAL turns "stay on-device." That's only true when the
Seth-voice `mlx_local` override is healthy, and even then the "cloud fallback" for
CONVERSATIONAL is not a different *model string* from ANALYTICAL — it's a different
*treatment*.

- `hu_model_router_default_config()` sets `conversational_model` **and**
  `analytical_model` to the same string, `"gemini-3.1-pro-preview"`
  (`src/agent/model_router.c:120-131`, pinned by
  `tests/test_model_router.c:default_models_are_not_deprecated`).
- `apply_tier_to_selection()` gives CONVERSATIONAL `thinking_budget=1024,
  temperature=0.7` and ANALYTICAL `thinking_budget=4096, temperature=0.7`
  (`model_router.c:254-286`).
- `maybe_override_to_mlx_local()` (`model_router.c:236-252`) overrides the **model**
  to the local LoRA server for REFLEXIVE and CONVERSATIONAL only, when
  `mlx_local_routing` is AUTO-and-healthy or FORCE — never for ANALYTICAL/DEEP
  (comment cites Dermot AC-7 / `lora-scale-default-or-die.md`: local 31B trades base
  reasoning for voice). ANALYTICAL/DEEP always call cloud.
- So "what cloud routing already does for that tier" (AC-8.5's parenthetical) means:
  **the full ANALYTICAL treatment** — cloud model + `thinking_budget=4096` — not a
  different model name. This SHADOW measures "should this substantive CONVERSATIONAL
  turn have gotten the ANALYTICAL treatment instead of today's mlx_local/CONVERSATIONAL
  treatment," not "on-device vs. cloud" in the literal sense.
- `hu_model_route_cloud_fallback(cfg, tier, ...)` (`model_router.c:132-155`) is the
  existing, already-used-elsewhere accessor for a tier's cloud model; the SHADOW
  decision reuses it for `HU_TIER_ANALYTICAL` rather than hand-picking a model.

## 1. AC-8.1 — persona head is shared (verified by reading; grep+test below make it a
   standing check)

`hu_agent_build_persona_head()` (`src/agent/agent_turn.c:1049-1075`) is called exactly
**once per turn**, unconditionally, from inside `hu_agent_turn()` at
`agent_turn.c:3149` — *before* the model-dispatch code later in the same function
(`turn_model` selection at `agent_turn.c:5779-5810`, dispatch via
`agent->provider.vtable->chat*(..., turn_model, ...)` at `agent_turn.c:6023/6032`).
There is no branch between the persona-head build and the dispatch that swaps which
head-building function runs based on which model was chosen — the same
`hu_agent_build_persona_head()` call produces the head fed to `agent->provider`
regardless of whether `turn_model` resolves to the mlx_local server or to Gemini. The
streaming path (`agent_stream.c:700`) calls the identical function; a comment at
`agent_turn.c:3145` says so explicitly ("shared helper, same as
`hu_agent_turn_stream_v2`").

**AC-8.1 check (grep + test), not just this reading:**
```bash
# 1. Exactly one persona-head builder function exists (no per-provider fork):
grep -rn "hu_error_t hu_agent_build_persona_head" src/agent/*.c
# expect: exactly one definition (agent_turn.c)

# 2. It is called from both turn-shape entry points, nowhere gated on model/tier:
grep -n "hu_agent_build_persona_head(" src/agent/agent_turn.c src/agent/agent_stream.c
# expect: call sites present, NONE inside an `if (turn_model == ...)` or
# `if (sel.tier == ...)` block — confirm by reading 10 lines above each hit.
```
New test `tests/test_agent_turn_persona_head_shared.c` (or an addition to an existing
persona-head test file if one already covers this shape — check
`tests/test_persona_head_gate.c` first): construct an agent, call
`hu_agent_build_persona_head()` twice with the SAME `agent`/`topic`/`channel` inputs,
once with `agent->turn_model` set to a local model string and once to a cloud model
string, and assert the two outputs are **byte-identical**. This proves in-process what
the grep proves structurally: the function's output does not depend on which model
string is set on the agent.

## 2. Approach — the SHADOW call site

Add a pure predicate plus one new call site in `src/agent/model_router.c`, using the
canonical three-state gate (`include/human/core/gate_mode.h`, already used by
`HU_PERSONA_HEAD`, `HU_GRAPH_GROUNDING`, etc. — verified: `hu_gate_mode_t` has
`HU_GATE_OFF=0, HU_GATE_SHADOW, HU_GATE_LIVE` and `hu_gate_mode_from_env(name,
default)` parses `off|shadow|on/live/1`, case-insensitive, fails closed to OFF on
garbage).

```c
/* include/human/agent/model_router.h — new constant, predicate, enum value */
#define HU_DIFFICULTY_ROUTE_SUBSTANTIVE_WORDS 12  /* matches US-5's casual/substantive
                                                    * boundary (scripts/blind_ab/
                                                    * authorship_gap.py); kept as an
                                                    * independent constant here — see
                                                    * "Conflicts" below for why these
                                                    * are not yet unified. */

/* US-8 SHADOW-only: would a difficulty classifier promote this CONVERSATIONAL turn
 * to the ANALYTICAL treatment? Pure predicate, no side effects. Reuses this file's
 * own word_count/needs_reasoning/emotional_weight helpers on an INDEPENDENT boundary
 * from compute_heuristic_score's own thresholds (words<=3/<=8/>30/>80), so this is
 * not tautological with the tier heuristic that already placed the turn in
 * CONVERSATIONAL. */
bool hu_model_route_shadow_would_promote_conversational(const char *msg, size_t msg_len);
```
```c
/* model_router.h — hu_route_source_t gains one value, appended (does not renumber
 * the existing three, so any serialized/logged old value is unaffected) */
typedef enum hu_route_source {
    HU_ROUTE_HEURISTIC = 0,
    HU_ROUTE_JUDGE,
    HU_ROUTE_JUDGE_CACHED,
    HU_ROUTE_JUDGE_FALLBACK,
    HU_ROUTE_SHADOW_DIFFICULTY, /* US-8: hypothetical decision, never applied */
} hu_route_source_t;
```

In `model_router.c`, `hu_model_route_shadow_would_promote_conversational()` is
implemented next to `needs_reasoning()` (both are file-private helpers on the message
text) and exported:
```c
bool hu_model_route_shadow_would_promote_conversational(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return false;
    if (word_count(msg, msg_len) <= HU_DIFFICULTY_ROUTE_SUBSTANTIVE_WORDS)
        return false; /* casual — never shadow-promote */
    return needs_reasoning(msg, msg_len) || emotional_weight(msg, msg_len) > 0;
}
```

The call site is added inside `hu_model_route()`, **after** the existing
`hu_route_log_record(hu_route_global_log(), &sel, score, (int64_t)time(NULL));` at
`model_router.c:365` and before `return sel;` (`model_router.c:367`). `sel` — the
value actually returned and applied by every caller — is never touched:

```c
    hu_route_log_record(hu_route_global_log(), &sel, score, (int64_t)time(NULL));

    /* US-8: SHADOW-only difficulty-routing measurement. Does not alter sel. */
    if (sel.tier == HU_TIER_CONVERSATIONAL) {
        hu_gate_mode_t diff_mode = hu_gate_mode_from_env("HU_DIFFICULTY_ROUTE", HU_GATE_OFF);
        if (diff_mode == HU_GATE_SHADOW &&
            hu_model_route_shadow_would_promote_conversational(msg, msg_len)) {
            hu_model_selection_t shadow_sel;
            memset(&shadow_sel, 0, sizeof(shadow_sel));
            shadow_sel.tier = HU_TIER_ANALYTICAL;
            shadow_sel.source = HU_ROUTE_SHADOW_DIFFICULTY;
            size_t cloud_len = 0;
            const char *cloud_model =
                hu_model_route_cloud_fallback(cfg, HU_TIER_ANALYTICAL, &cloud_len);
            shadow_sel.model = cloud_model;
            shadow_sel.model_len = cloud_len;
            shadow_sel.thinking_budget = 4096; /* matches apply_tier_to_selection's ANALYTICAL branch */
            shadow_sel.temperature = 0.7;
            hu_route_log_record(hu_route_global_log(), &shadow_sel, score, (int64_t)time(NULL));
        } else if (diff_mode == HU_GATE_LIVE) {
            /* LIVE is not implemented by this story (AC-8.5) — fail closed to a
             * visible one-shot warning rather than silently behaving as OFF.
             * See ~/.claude/rules/silent-config-gated-subsystems.md. */
            static atomic_bool warned_live_unimplemented = false;
            hu_log_warn_once(&warned_live_unimplemented, "model_router", NULL,
                             "HU_DIFFICULTY_ROUTE=live requested but LIVE is not implemented "
                             "(US-8 ships SHADOW-only); behaving as OFF");
        }
    }

    return sel;
```

Scope note: this call site is added **only** to `hu_model_route()`, not to
`hu_model_route_with_judge()` (which has four early-return branches and would need
the same ~15 lines duplicated at each, pushing this past its `+30 LOC` budget and
adding clone-ratchet risk for a P3 SHADOW story). Documented explicitly under Out of
Scope, not silently dropped.

### Why a second ring-buffer entry, not a new field on `hu_route_decision_t`

Two shapes were considered:
1. Add `bool shadow_would_promote` to `hu_route_decision_t` and set it on the SAME
   entry as the real decision.
2. Log a SECOND, distinctly-`source`-tagged entry into the same
   `hu_route_decision_log_t` ring buffer (chosen).

(1) is a smaller diff but means every consumer of `hu_route_decision_t` (today: only
`cp_admin_models_decisions`, `hu_route_log_tier_counts`) has to remember to check a
new field it doesn't otherwise care about, and the ABI-sized struct grows even for
turns where no shadow decision was made. (2) reuses the exact struct and functions
unchanged (`hu_route_decision_t`, `hu_route_log_record`, `hu_route_log_get`) — true to
AC-8.2's "no new logger invented" — and keeps a shadow decision indistinguishable in
*shape* from a real one, which is exactly what the offline eval and any future
dashboard view need (it can be displayed as "what would have happened" ).

### The consumer bug this catches (verified, must be fixed in the same commit)

`hu_route_log_tier_counts()` (`model_router.c:735-751`) loops every entry and
increments `counts[d->tier]` with no source filter. `cp_admin_models_decisions()`
(`src/gateway/cp_admin.c:594-639`) exposes `tier_distribution` over that same log to
the control-plane dashboard. **Without a fix, a SHADOW entry tagged `tier=ANALYTICAL`
would silently inflate `tier_distribution.analytical` in an existing, already-shipped
consumer** — exactly the "reports success/does something while doing nothing"-adjacent
shape this repo has been bitten by five times in two weeks
(`.claude/rules/reports-success-does-nothing.md`): a dashboard number that moves for a
reason nobody asked it to move for. Fix, in the same commit:

```c
void hu_route_log_tier_counts(const hu_route_decision_log_t *log, size_t counts[4]) {
    ...
    for (size_t i = 0; i < log->count; i++) {
        const hu_route_decision_t *d = &log->entries[(start + i) % HU_ROUTE_LOG_SIZE];
        if (d->source == HU_ROUTE_SHADOW_DIFFICULTY)
            continue; /* hypothetical, never applied — US-8 */
        if (d->tier <= HU_TIER_DEEP)
            counts[d->tier]++;
    }
    ...
}
```
`cp_admin_models_decisions()` itself needs no change — it already echoes
`source: hu_route_source_str(d->source)` per-entry, so a shadow row is visible in the
raw `decisions` array (useful for anyone inspecting it) while excluded from the
aggregate `tier_distribution` once the fix above lands. `hu_route_source_str()`
(`model_router.c:770-783`) needs one added `case HU_ROUTE_SHADOW_DIFFICULTY: return
"shadow_difficulty";` — verified this is the ONLY `switch` over `hu_route_source_t` in
the tree (`grep -rn "switch (source)" src/agent/model_router.c` — one hit, at line
785; no other file switches on this enum).

### Minor, disclosed side effect

Every substantive CONVERSATIONAL turn under `HU_DIFFICULTY_ROUTE=shadow` now writes
**two** entries into the 100-slot ring buffer instead of one, so the buffer covers a
shorter wall-clock window while the gate is SHADOW. Not a correctness bug (the buffer
already wraps under load), but worth naming: an operator diffing `tier_distribution`
before/after enabling the gate should expect the *history depth*, not just the
counts, to change. No action needed; OFF (default) is unaffected.

## 3. AC-8.3/8.4 — paired offline measurement

New script `scripts/eval_difficulty_route_shadow.py`, same shape as
`scripts/eval_semantic_live_gate.py` (verified structure: `select_contexts()`,
`run_arm()`, `paired_ids()`, `summarize_paired_arm()`, `DEFAULT_COMPOSITE_TOLERANCE =
0.02`, refuse-on-too-few-paired at `eval_semantic_live_gate.py:56-121` docstring).

- **Contexts**: reuse `select_contexts(path, n, min_len, max_len)`
  (`eval_semantic_live_gate.py:321-336`), filtered post-hoc to
  `len(text.split()) > HU_DIFFICULTY_ROUTE_SUBSTANTIVE_WORDS` (12) so the sampled set
  matches the C predicate's boundary; `n>=20` per AC-8.3.
- **Arm "on_device"**: `generate()` against `DEFAULT_SERVER =
  http://127.0.0.1:8741` (`eval_semantic_live_gate.py:110/303-319`) — the SAME shared
  server every other eval script in this sprint uses; no second model loaded, HTTP
  client only.
- **Arm "cloud_shadow"**: call Gemini `gemini-3.1-pro-preview` via Vertex, reusing
  `eval_blinded_ab`'s ADC helpers (`_get_adc_token()`, `_gemini_url()`,
  `hu_gemini_base_is_vertex`-equivalent check already embodied in
  `eval_semantic_live_gate.py`'s own `call_gemini()` at line 233 — **UNVERIFIED**:
  confirm `call_gemini()` builds the Vertex URL and never appends `?key=`; if it
  currently targets the public Generative Language API instead of Vertex, this script
  must switch it to Vertex ADC to match `src/providers/gemini.c`'s
  `hu_gemini_base_is_vertex` contract (`gemini.c:41`, `:862-885`) and the project rule
  against `?key=` on Vertex hosts) with `thinkingConfig.thinkingBudget=4096`
  (matches the shadow selection's `thinking_budget`, and the CLAUDE.md thinking-budget
  gotcha: never omit it).
- **AC-8.1 runtime check**: build `production_system_prompt()`
  (`eval_blinded_ab.py:211`) once per context and pass the IDENTICAL string to both
  arms; hash both (`sha256`) before sending and refuse (exit non-zero, write nothing)
  if they ever differ for a paired context — this is the runtime half of AC-8.1 (the
  grep+test above is the static half).
- **Composite**: `humanness_compose.compute_composite(axes)` over the PAIRED set only
  (both arms succeeded), same pairing discipline as
  `eval_semantic_live_gate.py:611-620` ("comparing arm-wide means computed over
  DIFFERENT context sets is not a measurement of anything").
- **Fidelity axis**: the LUAR-MUD twin score from `authorship_gap.py`. **Design
  decision** (flagged, not silently done): `authorship_gap.py` does not currently
  expose a standalone `twin_score()` function — the cosine computation is inline in
  `main()` (`authorship_gap.py:87-101`). Extract a small
  `twin_score(profile_fn, seth_profile_texts, candidate_texts) -> float` helper into
  `authorship_gap.py` (≈15 LOC, pure refactor, no behavior change — `main()` calls it
  too) so both scripts import the same LUAR-MUD cosine logic instead of duplicating
  it. This is Python, so it does not touch the C clone-detection ratchet
  (`scripts/check-clone-ratchet.sh` scans `src/**/*.c` only), but the same "don't
  duplicate the classifier" discipline applies.
- **Gate** (AC-8.4): PROMOTE-worthy only if, on the paired n≥20 sample,
  `composite_cloud_shadow >= composite_on_device - DEFAULT_COMPOSITE_TOLERANCE (0.02)`
  **and** `twin_cloud_shadow >= twin_on_device` (no tolerance subtracted for the
  fidelity axis — US-2's promotion gate treats `new_twin <= prev_twin` as BLOCK with
  no slack, and this reuses that same strictness rather than inventing a third
  number). Otherwise: **HOLD**, and the exact numbers (composite deltas, twin means +
  95% CI, n) are written to the evidence file and this design doc's Estimate section
  is updated with the real result — matching the C5 reply-delay-model precedent
  (`stories.md:290`: "a model that loses to the baseline stays `off` and is
  documented, not hidden").
- **Refusal conditions** (`no-number-without-a-measurement.md`): exit non-zero, write
  nothing, if any of — fewer than 20 paired successes; a persona-head hash mismatch
  (AC-8.1 runtime check fails); LUAR unavailable; ADC token fetch fails (must not
  silently fall back to an API key); either arm returns the literal `"[timeout]"`
  sentinel for ≥1 paired context (per the `authorship_gap.py:64-69` `usable()`
  precedent — a timeout string must not be scored as content); `:8741` unreachable.
- **`:8741` sharing**: both arms are pure HTTP clients (on-device via the shared
  `:8741`, cloud via Vertex) — **no in-process model load occurs**, so
  `scripts/check-no-resident-model.sh` is not a precondition for this script (verified
  by reading `eval_semantic_live_gate.py`'s `generate()`/`call_gemini()` — HTTP only,
  no `mlx_lm`/`torch` model construction outside `authorship_gap.py`'s `load_luar()`,
  which loads a small LUAR-MUD scorer, not the persona base model, and is CPU/short-
  lived like it already is for the existing nightly `authorship_gap.py` runs). Per
  `.claude/rules/session-worktree-isolation.md`, running this script still requires
  claiming `:8741` as a resource (announce in the session bus / coordinate with
  whichever session owns it) before generating — it is a live shared server even
  though this story never restarts or repoints it.

## 4. AC-8.6 — no daemon.c, no resident model, no restart

- `src/daemon.c` is not in the files-touched list; current size 12,313 LOC, exactly at
  `scripts/check-file-size-ceiling.sh`'s `MAX_BASELINE=12313`
  (`check-file-size-ceiling.sh:7`) — untouched means the ceiling check passes
  trivially. Verified: `wc -l src/daemon.c` → 12313.
- `scripts/check-no-resident-model.sh` stays green: this story adds zero in-process
  model loads (see §3's `:8741` sharing note); the script's own precondition (no
  server-health-URL change, no new trainer pattern) is unaffected by a pure-C
  predicate + pure-HTTP eval script.
- `:8741` and the service-loop are never restarted or repointed — the eval script only
  ever POSTs to the already-running server, exactly like `eval_semantic_live_gate.py`
  does today.

## 5. Privacy

- The C-side shadow log entry (`hu_route_decision_t`) carries `tier`, `source`,
  `timestamp`, `heuristic_score`, and a 64-byte model name — **no message text, no
  contact identifier** (verified: the struct has no text field,
  `include/human/agent/model_router.h:143-149`). This matches the existing real-decision
  entries; the shadow entry adds nothing new to the privacy surface of an
  already-shipped, already-audited log.
- The offline eval script's evidence file
  (`sprints/sprint-better-than-human-2026-09-05/evidence/US-8-*.json`) carries only
  aggregate numbers (composite means, twin means + CI, n, paired context **indices**,
  not text) — same discipline as US-2's design (`designs/US-2.md`: "Gate reads only
  aggregate JSON ... No message text, no handles, no contact names committed").
- The corpus file used for `select_contexts()` (wherever `eval_semantic_live_gate.py`
  already sources it from) is read locally and never re-committed; this story adds no
  new corpus export.

## 6. Files touched

| File | Change | LOC (est.) |
|---|---|---|
| `include/human/agent/model_router.h` | `HU_DIFFICULTY_ROUTE_SUBSTANTIVE_WORDS` constant, `hu_model_route_shadow_would_promote_conversational` prototype, `HU_ROUTE_SHADOW_DIFFICULTY` enum value | +12 |
| `src/agent/model_router.c` | predicate impl, SHADOW call site in `hu_model_route()`, `hu_route_log_tier_counts()` filter fix, `hu_route_source_str()` case | +38 |
| `tests/test_model_router.c` | predicate boundary tests (12/13 words), end-to-end shadow-log test, tier_counts-excludes-shadow regression test | +70 |
| `tests/test_agent_turn_persona_head_shared.c` (new, or added to an existing persona-head test file — check `test_persona_head_gate.c` first) | AC-8.1 byte-identical persona-head test | +35 |
| `scripts/blind_ab/authorship_gap.py` | extract `twin_score()` helper (pure refactor) | +15 / -8 |
| `scripts/eval_difficulty_route_shadow.py` (new) | paired on-device vs. cloud-shadow measurement | +230 |
| `scripts/test_eval_difficulty_route_shadow.py` (new) | hermetic fixture tests (pairing, refusal conditions, gate arithmetic) — no network | +90 |

Total ≈ +490 LOC (skeleton estimated +290; the difference is the AC-8.1 runtime
check, the tier_counts consumer fix, and the Python hermetic test file the skeleton
didn't itemize).

## 7. Hermetic tests

**C (`tests/test_model_router.c`)** — pure, no network, no `:8741`, no ASan surprises:
1. `shadow_predicate_boundary`: 12-word message → `false`; the same message plus one
   more word (13) with a reasoning marker ("should i") → `true`; a 13-word message with
   no reasoning/emotion marker → `false` (word count alone is not sufficient — matches
   the predicate's `&&`/`||` structure, not just the boundary).
2. `shadow_log_records_without_altering_sel`: reset `hu_route_global_log()`
   (`hu_route_log_init`, matching the `Sprint 38` precedent at
   `test_gateway_extended.c:1305-1308`), `setenv("HU_DIFFICULTY_ROUTE", "shadow", 1)`,
   call `hu_model_route()` with a substantive CONVERSATIONAL-scoring message, assert:
   - the **returned** `sel.tier == HU_TIER_CONVERSATIONAL` and `sel.model` equals
     whatever the non-shadow path would have produced (call the same route once with
     the gate `unsetenv`'d first, in the SAME test, and compare) — this is the literal
     AC-8.7 assertion.
   - `hu_route_log_count(hu_route_global_log()) == 2` after the shadowed call (one
     real + one shadow entry).
   - the second entry (`hu_route_log_get(log, 1)`) has `source ==
     HU_ROUTE_SHADOW_DIFFICULTY`, `tier == HU_TIER_ANALYTICAL`, `model ==
     cfg.analytical_model`.
   - `unsetenv("HU_DIFFICULTY_ROUTE")` at the end (test hygiene — matches
     `test_graph_grounding.c`/`test_persona_head_gate.c` pattern).
3. `shadow_off_by_default`: no env var set, same substantive message → exactly ONE log
   entry (the real one) — proves the default-OFF contract.
4. `shadow_live_not_implemented_warns_once`: `setenv("HU_DIFFICULTY_ROUTE", "live",
   1)`, call twice, assert still exactly one real entry per call (no promotion
   happens) — proves LIVE is inert, not silently equivalent to SHADOW.
5. `tier_counts_excludes_shadow_entries`: with a shadow entry present (source =
   `HU_ROUTE_SHADOW_DIFFICULTY`, tier = ANALYTICAL) and one real CONVERSATIONAL entry,
   `hu_route_log_tier_counts()` reports `counts[ANALYTICAL] == 0` and
   `counts[CONVERSATIONAL] == 1` — the regression test for the dashboard-corruption
   bug found in §2.
6. `casual_message_never_shadow_logged`: a ≤12-word CONVERSATIONAL message with
   `HU_DIFFICULTY_ROUTE=shadow` set → exactly one log entry (no shadow row) — the
   predicate's casual short-circuit, exercised through the real call site.

**AC-8.1 (new or extended test)**: `hu_agent_build_persona_head()` called twice with
identical persona/channel/topic inputs and differing `agent->turn_model`, asserting
byte-identical output (see §1).

**Python (`scripts/test_eval_difficulty_route_shadow.py`)**, fixture-driven, no
network, no `:8741`, mirroring `eval_semantic_live_gate.py`'s own test file if one
exists (**UNVERIFIED**: `grep -l eval_semantic_live_gate scripts/test_*.py` not yet
run — check for `scripts/test_eval_semantic_live_gate.py` before writing a fresh
pattern, prefer matching its existing fixture shape):
1. Pairing: contexts where only one arm succeeds are excluded from composite/twin
   comparison but appear in `shadow_only`/`live_only`-equivalent counts.
2. Refusal: fewer than 20 paired successes → non-zero exit, no file written.
3. Refusal: a `"[timeout]"` sentinel in either arm for a paired context → excluded
   from scoring (or refused if it drops the paired count below 20).
4. Refusal: persona-head hash mismatch between arms → non-zero exit, no file written.
5. Gate arithmetic: synthetic composite/twin pairs feeding `decide()`-shaped pure
   function, asserting PROMOTE/HOLD boundaries exactly (mirrors US-2's
   `test_authorship_promotion_gate.py` pattern from `designs/US-2.md`).

## 8. Risks

1. **Never restart `:8741`/service-loop.** No code path in this story issues a
   restart; the eval script is a pure HTTP client of the already-running server (§3,
   §4).
2. **No second resident model outside the nightly window.** Verified: zero in-process
   model construction added; `authorship_gap.py`'s `load_luar()` (a small scorer, not
   the persona base model) is unchanged by this story — it already runs today.
3. **Gemini only via Vertex ADC, `thinkingBudget` explicit.** The shadow selection
   sets `thinking_budget=4096` matching ANALYTICAL's real treatment (§2); the eval
   script's cloud arm must set `thinkingConfig.thinkingBudget=4096` explicitly — **the
   ADC-vs-`?key=` correctness of `eval_semantic_live_gate.py`'s existing
   `call_gemini()` is UNVERIFIED** (flagged in §3) and must be confirmed (or fixed)
   before this story's cloud arm is trusted, since a `?key=`-only path would silently
   401 against a Vertex host or silently succeed against the wrong (non-Vertex) API,
   producing a "difficulty routing" number that isn't measuring what the tier's real
   ANALYTICAL cloud call actually does.
4. **No private text.** C-side log carries no text (§5); Python evidence carries only
   aggregates (§5).
5. **Ratchets.** `daemon.c` untouched (§4). File-size ceiling: `model_router.c` grows
   by ~38 LOC from 819 → ~857, nowhere near the 12,313 ceiling. Clone ratchet: the new
   predicate calls existing `word_count`/`needs_reasoning`/`emotional_weight` rather
   than reimplementing them, so it should not add new 6-line duplicate windows;
   **UNVERIFIED** — run `scripts/check-clone-ratchet.sh` after implementation, before
   commit. `agent-core-boundary.md`: no provider-factory include, no channel-name
   `memcmp` added — not applicable to this change. `sqlite-includer-ratchet.md`: not
   applicable (no SQLite touched). `edge-context-isolation.md`: not applicable (no
   `src/channels/` touched). `modeled-person-layering.md`: not applicable
   (`src/agent/` is Conversation-context, not persona/cognition/behavior).
6. **Ring-buffer history depth halves under SHADOW** for substantive CONVERSATIONAL
   turns (§2, "Minor, disclosed side effect") — cosmetic, not a correctness risk,
   disclosed rather than silently accepted.
7. **`hu_route_source_t` enum growth** touches exactly one `switch` statement
   (`hu_route_source_str`, verified the only one in the tree) — low risk, but any
   future `switch (source)` added elsewhere must remember the new case; no enforcement
   exists for this today (a `-Wswitch-enum` build flag would catch it structurally —
   **not currently enabled**, UNVERIFIED whether enabling it project-wide is
   in-scope-worthy; out of scope for this story either way).

## 9. Conflicts / coordination

- **US-5 boundary duplication (by design, not oversight).** Both US-5
  (`src/memory/semantic_recall.c`) and this story define their own 12-word
  casual/substantive boundary constant, each citing `authorship_gap.py`'s value
  rather than importing a single shared C header constant. Neither design commits to
  extracting a shared `include/human/core/register.h` constant, because doing so would
  create a hard build-order dependency between two otherwise-independent P1/P3
  stories in the same sprint. **Recommendation, not a blocker**: if both land in this
  sprint, a fast-follow chip should extract `HU_REGISTER_CASUAL_WORD_BOUNDARY` into a
  shared header once both call sites exist, so a future change to the boundary can't
  silently drift between the two subsystems. Until then, a comment in each constant's
  definition cross-references the other (already included in §2's constant comment).
- **US-2 soft dependency (per stories.md).** This story's fidelity axis reuses
  `authorship_gap.py`'s LUAR-MUD twin scoring; the `twin_score()` extraction in §3 is
  a prerequisite for a CLEAN reuse but is not blocked by US-2's own promotion-gate
  work landing first — the extraction is additive to `authorship_gap.py` and does not
  touch US-2's `authorship_promotion_gate.py` (new file, per `designs/US-2.md`). If
  US-2 lands first, `twin_score()` should be added once and both US-2's gate script
  (if it also calls LUAR — **UNVERIFIED**, `designs/US-2.md` describes it as reading
  JSON output, not calling LUAR directly, so likely no overlap) and this story's eval
  script both benefit.
- **CP-admin dashboard consumers** (`cp_admin_models_decisions`, and any UI that reads
  `models.decisions`/`tier_distribution` — **UNVERIFIED**, did not grep `ui/` for a
  consumer of this endpoint) should be told a `source: "shadow_difficulty"` row can
  appear in the raw `decisions` array once this ships with the gate set to `shadow`;
  the aggregate `tier_distribution` is protected by the fix in §2, but a
  human/dashboard glancing at individual rows should not mistake a shadow row for an
  applied decision. No UI change is proposed here (out of scope) — flagging for
  whoever owns the dashboard.

## 10. Out of scope

- Flipping CONVERSATIONAL's live routing to cloud/ANALYTICAL treatment for any turn —
  Seth's call (AC-8.5), and no LIVE code path is implemented at all (only OFF/SHADOW;
  LIVE parses but warns-and-no-ops, §2).
- Adding the SHADOW call site to `hu_model_route_with_judge()` (§2 scope note).
- `src/daemon.c` changes of any kind.
- Any change to `mlx_local_routing` policy, the circuit breaker in
  `src/providers/reliable.c`, or provider registration.
- Unifying the US-5/US-8 word-boundary constant into a shared header (§9).
- Building a dashboard UI affordance for shadow rows (§9).

## 11. Estimate

**L** (matches stories.md). Breakdown: ~0.5 day for the C predicate + call site + the
tier_counts fix + C hermetic tests (§2, §7); ~0.5 day for the AC-8.1 static+runtime
verification (§1); ~1.5 days for the paired offline eval script including the
`authorship_gap.py` extraction, ADC/Vertex verification (§3, risk #3), and its own
hermetic tests (§3, §7); ~0.5 day for running the actual n≥20 measurement, recording
the PROMOTE/HOLD verdict with real numbers, and updating this doc. No single piece is
XL-sized, but the AC-8.1 verification + the reusable-fidelity-axis extraction + the
Vertex-vs-`?key=` confirmation are three separate small investigations stacked on top
of a otherwise-M-sized code change, which is what pushes the total to L.

## 12. Acceptance criteria mapping

- **AC-8.1** → §1 (grep + new/extended test; runtime hash check in the eval script).
- **AC-8.2** → §2 (call site, reuses `hu_route_decision_log_t`/`s_global_log`/
  `hu_route_log_record` verbatim; `sel` unchanged; new enum value, no new logger).
- **AC-8.3** → §3 (paired arms extending `eval_semantic_live_gate.py`'s shape; n≥20;
  `authorship_gap.py` twin reuse via extracted `twin_score()`).
- **AC-8.4** → §3 gate + refusal conditions; C5 precedent for a documented HOLD.
- **AC-8.5** → §2 (no LIVE implementation at all; warns if requested) + §10.
- **AC-8.6** → §4 (daemon.c untouched at ceiling; no resident model; `:8741` never
  restarted).
- **AC-8.7** → §7, test 2 (`shadow_log_records_without_altering_sel`).

---

**UNVERIFIED items requiring confirmation before/during implementation** (called out
inline above, collected here for the implementer):
1. Whether `eval_semantic_live_gate.py`'s `call_gemini()` already targets Vertex with
   ADC (not `?key=` against the public API) — §3, §8 risk 3.
2. Whether `scripts/test_eval_semantic_live_gate.py` (or similarly named) already
   exists to pattern-match for the new test file's shape — §7.
3. Whether any `ui/` code consumes `cp_admin_models_decisions`'s `decisions` array in
   a way that would display a raw shadow row to a human — §9.
4. Clone-ratchet delta after implementation (`scripts/check-clone-ratchet.sh`) — §8
   risk 5.
5. US-2's `authorship_promotion_gate.py` calls LUAR directly vs. only reads JSON — §9
   (affects whether `twin_score()` extraction benefits it too).

RESULT_tech-lead=READY
