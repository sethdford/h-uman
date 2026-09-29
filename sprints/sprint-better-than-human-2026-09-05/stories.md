# Sprint sprint-better-than-human-2026-09-05 Backlog

## Goal

Turn the six "better than human" levers named in the 2026-09-05 SOTA closing report
(`docs/research/2026-09-05-sota-fleet-closing-report.md`) and the 2026-09-02 gap analysis
(`docs/research/2026-09-02-august-2026-sota-gap-analysis.md`) into measured gates — fixing
the one known live regression (86% lowercase-start from the preference-corpus casing
confound) and protecting the one feature already flipped LIVE (semantic recall, EI drifting
~0.1/run toward its 0.15 revert threshold) — without restarting `:8741`, without a second
resident model outside the nightly window, and without growing `src/daemon.c` past its
12,313-LOC ratchet.

## User Stories (in priority order)

### US-1 (P0): Rebalance and re-provenance the authorship preference corpus
**As a** persona-fidelity engineer, **I want** the preference-training corpus expanded with
additional Seth-authored (not daemon-authored) text and rebalanced on both sides via
`--match-sides`, **so that** the next training cycle fixes the 86%-lowercase-start
regression (caused by a 77.5%-lowercase CHOSEN side, per the 2026-09-04 casing audit)
without reintroducing the confound the 2026-09-04 rebalance already diagnosed.

**Acceptance criteria:**
- AC-1.1: Corpus merge reuses the exact de-dup key from the persona-evolution spec —
  `(timestamp-to-the-second, sha256(stripped text))` — over the two provenance-verified
  Seth-authored stores identified in `docs/plans/2026-09-02-persona-evolution/spec.md`
  §3b ("used"/"usable" rows only): `data/imessage/training_pairs.jsonl` (1,303 rows) and
  `~/.human/logs/eval-archive/ground_truth-backup-20260725-113527.jsonl` (690 rows,
  59 not already in the repo copy). Merge script prints its own actual merged-row count
  (do not assume it reproduces the spec's 2,283 — that number was for a different task).
- AC-1.2: Every row's CHOSEN/Seth side traces to a store marked "used" or "usable" in that
  spec's provenance table; rows from stores marked "rejected — daemon output" (`memory.db
  messages`, `dpo_pairs` auto_correction/implicit_feedback/arena, `production_outcomes`,
  `m3-corpus.jsonl` `channel=memory_db` rows) are excluded — verified by a script assertion,
  not manual review.
- AC-1.3: `scripts/rebalance_preference_corpus.py --match-sides` is run on the merged
  corpus; before/after lowercase-start and terminal-punct margins are reported and
  committed, in the same shape as the 2026-09-04 numbers (margins 0.715→0.005 lowercase,
  0.338→0.000 punctuation) — this run's own numbers, not a repeat of that citation.
- AC-1.4: Script refuses (exit non-zero, writes nothing) if, per its own documented
  contract, no readable style card / target is available, there are zero rebalanceable
  rows, or (with `--match-sides`) the post-rebalance margin on either axis still exceeds
  `--max-margin` (default 0.10).
- AC-1.5: No raw message text, phone numbers, or contact names are committed to the repo;
  the merged/rebalanced corpus lives under `~/.human/training-data/` (gitignored); only
  aggregate row counts and margins are committed, under
  `sprints/sprint-better-than-human-2026-09-05/evidence/`.
- AC-1.6: This story does not trigger a training run. `scripts/check-no-resident-model.sh`
  passes throughout (no model loaded outside the nightly `HU_RETRAIN_MLXTUNE` window).

**Files likely touched:** `scripts/build_v6_preference_corpus.py` (or a new
`scripts/merge_seth_preference_sources.py`), no C changes.
**Dependencies:** none (feeds US-2).
**Estimate:** M

### US-2 (P0): Per-cycle LUAR promotion gate that blocks a regressed adapter
**As a** ML pipeline engineer, **I want** a promotion gate that runs
`scripts/blind_ab/authorship_gap.py` after every training cycle and blocks registry
promotion unless the twin similarity (today 0.625–0.633, ceiling 0.70–0.71, floor
0.62–0.63) moves toward the ceiling relative to the previous promoted cycle, **so that** an
adapter regressed on authorship (like the casing-confounded v6) cannot promote silently the
way the reconstructive-retrieval and delay-model regressions in this same fleet nearly did.

**Acceptance criteria:**
- AC-2.1: Gate reads twin/ceiling/floor from `authorship_gap.py`'s own committed JSON
  output (e.g. `~/.human/logs/authorship-gap-*.json`) — never hardcodes the 0.625/0.70/0.62
  numbers cited above; those are context, not constants to bake in.
- AC-2.2: Gate BLOCKS (non-zero exit, no registry write) when new-cycle twin ≤
  previous-cycle twin, OR new-cycle twin < the measured floor (0.62) — reusing the
  precedent gate shape from `scripts/register_v6_adapter.py` / `adapter_is_real.py`
  (lora_b non-zero, size, scale ≤4).
- AC-2.3: Gate reports INCONCLUSIVE (not silent PASS) when `authorship_gap.py` itself
  refuses (fewer than 20 other senders with 5+ texts, or model load failure) — matching
  the DPO regression gate's `val_loss=None` → INCONCLUSIVE precedent, per
  `.claude/rules/no-number-without-a-measurement.md`.
- AC-2.4: A test fixture reconstructs the known 2026-09 regression shape (prior twin 0.70,
  new twin 0.625) and asserts the gate BLOCKS for the right reason — proving the guard
  discriminates before it is trusted, per `.claude/rules/reports-success-does-nothing.md`.
- AC-2.5: Gate is wired to run inside the existing nightly sequence (Appendix H:
  `training_loop.py` → `adapter_is_real.py` → registry write) between those last two
  steps, and only inside that serving-down window.
- AC-2.6: One real gate run's JSON output (PASS, BLOCK, or INCONCLUSIVE, whichever the
  next nightly window actually produces) is committed as evidence with its exact numbers.

**Files likely touched:** `scripts/blind_ab/authorship_gap.py` (read; possibly a
`--prior-twin` comparison flag), `scripts/register_v6_adapter.py`, new
`scripts/test_authorship_promotion_gate.py`.
**Dependencies:** US-1 (the gate is most useful scored against the rebalanced corpus's
resulting adapter, but the gate itself can be built and fixture-tested independently).
**Estimate:** M

### US-3 (P1): Seth's own initiation-response baseline from chat.db
**As a** when-to-speak engineer, **I want** a read-only measurement of how often Seth's own
conversation-opening sends go unanswered within the same window `eval_when_to_speak.py`
uses for FIR, **so that** the daemon's FIR (currently 0.670, on an admittedly inflated
fallback source) has a real human reference point instead of an arbitrary threshold.

**Acceptance criteria:**
- AC-3.1: New script (`scripts/eval_seth_initiation_baseline.py` or a `--seth-baseline`
  mode on `eval_when_to_speak.py`) imports `FIR_WINDOW_HOURS` from
  `scripts/eval_when_to_speak.py` rather than re-deriving it, so the two rates are
  comparable on the same window definition.
- AC-3.2: Reads `~/Library/Messages/chat.db` with `mode=ro&immutable=1`; zero writes;
  code review / test asserts no `INSERT`/`UPDATE` statement exists in the module.
- AC-3.3: Refuses (exit non-zero, writes nothing) if fewer than 30 qualifying
  Seth-initiated sends exist in the available window — the 2026-08-03 retention floor
  (per `docs/plans/2026-09-02-persona-evolution/spec.md` §3) means this may genuinely
  refuse; that is an acceptable, honestly-recorded outcome.
- AC-3.4: On success, output includes n, rate, 95% Wilson CI (reuse
  `scripts/blind_ab/score.py`'s `wilson()` — do not reimplement), and the exact date range
  covered.
- AC-3.5: No message text, phone numbers, or contact names appear in the output or the
  committed artifact — counts and rates only.
- AC-3.6: Hermetic tests in `scripts/test_eval_seth_initiation_baseline.py` cover: the
  refusal path (n<30), a synthetic case with a known rate, and window-definition parity
  with `eval_when_to_speak.py`'s FIR window (same constant, not a copy).

**Files likely touched:** new `scripts/eval_seth_initiation_baseline.py`, new
`scripts/test_eval_seth_initiation_baseline.py`.
**Dependencies:** none (feeds US-4).
**Estimate:** S

### US-4 (P1): Re-run when-to-speak MIR/FIR against the real `proactive_decisions` log
**As a** when-to-speak engineer, **I want** `scripts/eval_when_to_speak.py` re-run now that
`proactive_decisions` (contract C5 Part A) has real rows since 2026-09-03, **so that** we
replace the known-inflated fallback-sourced numbers (MIR 0.613 / FIR 0.670, Appendix E)
with a real measurement and compare FIR against Seth's own baseline (US-3).

**Acceptance criteria:**
- AC-4.1: Script's printed source line reads `proactive_decisions`, not `fallback`
  — proof the real log, not the structurally-biased fallback, was used.
- AC-4.2: If `proactive_decisions` has fewer rows than the script's `--min-n` default,
  the script's existing refusal fires (exit non-zero, "REFUSE: insufficient n ...") and
  this story records that exact row count as its (negative) result — it does not lower
  `--min-n` to force a number.
- AC-4.3: On success, new MIR and FIR values are committed to
  `sprints/sprint-better-than-human-2026-09-05/evidence/` as JSON, distinct from and
  compared against the Appendix E fallback-sourced numbers.
- AC-4.4: FIR is compared against the Seth-initiation baseline from US-3; the comparison
  (FIR ≤ baseline, FIR > baseline, or "baseline unavailable — US-3 refused") is stated
  explicitly, not implied.
- AC-4.5: No change to `daemon.c`'s decision logic or to the reply-delay model's `off`
  state — this story is measurement only. Policy stays `SHADOW`/logged; nothing flips to
  LIVE.
- AC-4.6: `:8741` and the service-loop are not restarted or repointed as part of this
  story.

**Files likely touched:** `scripts/eval_when_to_speak.py` (read; only touched if the
`proactive_decisions` source-detection branch needs a fix), no C changes expected.
**Dependencies:** US-3 (for the baseline comparison in AC-4.4); can start in parallel and
block only on that one AC.
**Estimate:** S

### US-5 (P1): Register-conditioned semantic recall (protect the LIVE gate from EI drift)
**As a** memory engineer, **I want** semantic recall's LIVE inclusion (flipped LIVE at
daemon `a28d7c9b0`, 2026-09-03) conditioned on message register, **so that** recall is
applied where the RAG finding shows a benefit (substantive +0.110) and skipped where it
costs EI (casual −0.078), protecting the composite/EI numbers that are already drifting
~0.1 per run toward the 0.15 revert threshold noted in the closing report.

**Acceptance criteria:**
- AC-5.1: A new pure predicate (per `.claude/rules/security-predicate-extraction.md`
  pattern) gates the `HU_GATE_LIVE` branch at `src/memory/retrieval/hybrid.c:861` in
  addition to the existing mode check — casual/short turns skip semantic recall even when
  `HU_SEMANTIC_RECALL=live`.
- AC-5.2: Register classification reuses the casual/substantive boundary already defined
  in `scripts/blind_ab/authorship_gap.py` (reply ≤12 words = casual) so numbers stay
  comparable across tools; the boundary is a named constant, not a re-derived one.
- AC-5.3: `scripts/eval_semantic_live_gate.py` is re-run with the register gate active and
  reports EI/composite **per register**, not only the current aggregate (0.919→0.908
  composite, 4.275→4.175 EI) — substantive-only paired EI must not drop beyond the
  script's existing 0.15 tolerance and composite not beyond 0.02.
- AC-5.4: Casual-register paired contexts show `recall_bytes=0` in the LIVE arm (per the
  script's existing `recall_bytes` field) — proof the gate actually suppressed recall
  there, not just that the aggregate improved.
- AC-5.5: Ships behind a new, default-OFF env var (e.g. `HU_SEMANTIC_RECALL_REGISTER_GATE`)
  layered on top of the existing `HU_SEMANTIC_RECALL` gate; current LIVE production
  behavior is unchanged unless the new var is explicitly set — `:8741` is not restarted as
  part of landing this story.
- AC-5.6: Unit tests for the predicate: short/casual input → recall suppressed; long
  substantive input → recall admitted; the 12-word boundary tested on both sides.
- AC-5.7: `scripts/check-file-size-ceiling.sh`, `scripts/check-agent-core-boundary.sh`, and
  `scripts/check-modeled-person-layering.sh` stay green — the predicate lives in
  `src/memory/semantic_recall.c`, not in `src/agent/` or `src/daemon.c`.

**Files likely touched:** `src/memory/semantic_recall.c`,
`include/human/memory/semantic_recall.h`, `src/memory/retrieval/hybrid.c` (call site
only), `scripts/eval_semantic_live_gate.py` (add per-register breakdown), new
`tests/test_semantic_recall_register.c`.
**Dependencies:** none technical; builds on the already-LIVE C1 gate.
**Estimate:** L

### US-6 (P2): Preference-based human blind A/B (win rate, not detection)
**As a** product owner measuring "better than human" rather than "indistinguishable from
human," **I want** the existing blind-A/B machinery (`scripts/blind_ab/`, currently a
detection task — "which is real Seth?", target detection ≤0.60) extended to a preference
task — "which reply do you prefer?" — rated by people who know Seth, **so that** we have a
win-rate-with-CI number that is a genuinely different (and harder) bar than detection.

**Acceptance criteria:**
- AC-6.1: A new rating-sheet mode (`scripts/blind_ab/make_rating_sheet.py --mode
  preference` or a sibling script) frames the task as preference, not detection — new
  instructions text, clearly separated from `PROTOCOL.md`'s existing detection framing so
  the two are never conflated in one sheet.
- AC-6.2: Scoring reuses `scripts/blind_ab/score.py`'s `wilson()` function unmodified to
  compute win rate (fraction of pairs where the model reply was preferred) + 95% CI — no
  reimplementation of the interval math.
- AC-6.3: No LLM judge is in the loop for the verdict; the script refuses to write a
  promotion-relevant result for any sheet whose rater tag is not `human`, reusing the
  existing `--rater human` vs `synthetic` split in `score.py`.
- AC-6.4: The rating-sheet generator never includes phone numbers or contact names; a test
  asserts no phone-number-shaped or contact-name-shaped string appears in a generated
  sheet (extend existing redaction if `make_rating_sheet.py` doesn't already have it, add
  it if it doesn't).
- AC-6.5: At least one real run (n≥20 pairs) is committed as an aggregate JSON (win rate +
  Wilson CI + n) under `sprints/sprint-better-than-human-2026-09-05/evidence/`; the raw
  sheet text itself is never committed to the repo, matching the existing
  `~/.human/blind_ab_human/` pattern.
- AC-6.6: A win rate below 0.5 is an acceptable, honestly-recorded outcome — this story
  does not retry sampling or reframe questions until the number looks better.

**Files likely touched:** `scripts/blind_ab/make_rating_sheet.py`,
`scripts/blind_ab/score.py` (or new `score_preference.py`),
`scripts/blind_ab/PROTOCOL.md` (new section), `scripts/blind_ab/test_score.py`.
**Dependencies:** none blocking.
**Estimate:** M

### US-7 (P2): Windowed, re-derivable persona style card with honest event-shift reporting
**As a** persona-fidelity engineer, **I want** a recurring windowed re-derivation of the
style card (reusing `scripts/eval_persona_evolution.py`'s own axis functions) plus an
honest write-up of what the 2026-09-03 re-measurement (`results-2026-09-03.json`) actually
found, **so that** "persona ages" has a real, re-derivable measurement instead of a
one-off appendix, and the next life event gets clean pre/post data instead of another
`chat.db`-retention surprise.

**Acceptance criteria:**
- AC-7.1: This story does NOT redo the single-source style-card reconciliation — that
  landed at `8fc8a022d` per the closing report. It builds on top of it.
- AC-7.2: A `--window-days N` mode is added to `scripts/eval_persona_evolution.py` (or a
  thin wrapper) that re-derives all 9 axes over the trailing N days, reusing the existing
  per-axis functions and the existing `bootstrap_ci`/`min_n`/refusal-contract code
  verbatim — not reimplemented.
- AC-7.3: Because `chat.db`'s outbound retention floor (2026-08-03) and today's date
  (2026-09-05) bound the available history to ~33 days, a run with `--window-days 60`
  MUST report the true covered days (not 60) in its `coverage` field, exactly like
  `results-2026-09-03.json`'s existing `coverage.covered_days` shape — this story explicitly
  does not claim 60 days of real coverage it doesn't have.
- AC-7.4: Refuses (exit non-zero, writes nothing) if the window has n < 100, reusing the
  existing `min_n` constant/contract — not a new magic number.
- AC-7.5: The story's write-up (a short section added to
  `docs/plans/2026-09-02-persona-evolution/spec.md` or a new dated results file) states
  plainly which axes moved beyond their own CI in the existing 2026-09-03 re-measurement
  (e.g. `lowercase_start_rate` +0.158 after the move, −0.088 after the job change — opposite
  directions) and flags the move-event's pre-window coverage (5.1 of an intended 30 days)
  as low-confidence, rather than presenting both events' results with equal confidence.
- AC-7.6: Recovering pre-August history beyond what `spec.md` §3b already inventoried is
  explicitly OUT of scope for this story (filed separately per the closing report's open
  chip list) — this story does not attempt new data recovery.
- AC-7.7: No message text, phone numbers, or contact names are committed; only aggregate
  per-axis stats.

**Files likely touched:** `scripts/eval_persona_evolution.py`,
`scripts/test_eval_persona_evolution.py`, `docs/plans/2026-09-02-persona-evolution/spec.md`.
**Dependencies:** none.
**Estimate:** M

### US-8 (P3): Difficulty-based routing SHADOW — log substantive-turn cloud routing, measure, do not flip
**As a** model-routing engineer, **I want** a SHADOW-only log of what cloud routing
(`HU_TIER_ANALYTICAL`, already the live fallback for that tier per `model_router.c`) would
decide for substantive `HU_TIER_CONVERSATIONAL` turns that today stay on-device, plus a
paired offline measurement of humanness composite with the persona head verified identical
on both paths, **so that** a future routing change has a real number instead of an
assumption — and does not flip live routing behavior in this sprint.

**Acceptance criteria:**
- AC-8.1: Before any measurement, a check (grep + a test) confirms the SAME
  persona/system-prompt-building function is invoked on both the on-device and
  `hu_model_route_cloud_fallback` cloud paths for the sampled turns — "persona head intact"
  is verified, not assumed.
- AC-8.2: A SHADOW log call site is added at the `HU_TIER_CONVERSATIONAL` decision point in
  `src/agent/model_router.c`, reusing the existing `hu_route_decision_log_t` /
  `s_global_log` machinery already in that file — no new logger invented, and the decision
  actually applied is unchanged.
- AC-8.3: A paired offline eval (extending `scripts/eval_semantic_live_gate.py`'s
  paired-arms machinery, or a new script following the same shape) compares humanness
  composite and one fidelity axis (reusing `authorship_gap.py`'s twin score) for on-device
  vs cloud-fallback replies to the same n≥20 substantive `CONVERSATIONAL`-tier turns.
- AC-8.4: Gate: PROMOTE-worthy only if composite does not drop and the fidelity axis does
  not drop on the paired sample; otherwise the negative/neutral result is recorded with its
  exact numbers, matching the C5 reply-delay-model precedent (a model that loses to the
  baseline stays `off` and is documented, not hidden).
- AC-8.5: Regardless of the measurement's outcome, this story does NOT change which tier
  ships LIVE for any turn — `HU_TIER_ANALYTICAL`/`HU_TIER_DEEP` already route to cloud by
  design; flipping `CONVERSATIONAL`'s default routing is explicitly Seth's call and out of
  scope here.
- AC-8.6: `:8741` and the service-loop are never restarted/repointed by this story; no
  second resident model is loaded outside the nightly window
  (`scripts/check-no-resident-model.sh` stays green); `src/daemon.c` is not touched (stays
  at or under 12,313 LOC).
- AC-8.7: New hermetic unit test asserts the shadow-log call site records a decision without
  altering `sel->tier` or `sel->model` for the turn actually served.

**Files likely touched:** `src/agent/model_router.c` (shadow log call site only), a new or
extended offline eval script, new test in the model-router test file.
**Dependencies:** soft dependency on US-2 (reuses `authorship_gap.py`'s twin-scoring
function as the fidelity axis).
**Estimate:** L

## Non-goals

- We will NOT flip any SHADOW-only policy to LIVE this sprint (when-to-speak, the
  register recall gate, or difficulty-based routing) — every lever that touches the send
  path ships OFF→SHADOW at most, per `.claude/rules/feature-gate-requires-measurement.md`.
- We will NOT run a real mlx-tune/nightly training cycle outside its scheduled
  `HU_RETRAIN_MLXTUNE` window, and never load a second Python LLM instance alongside `:8741`.
- We will NOT attempt to recover pre-August chat.db history beyond the stores already
  inventoried in `docs/plans/2026-09-02-persona-evolution/spec.md` §3b — that is a
  separately-filed chip.
- We will NOT touch the `daemon.c` batch-reply carve-out (contract C7) — separate,
  already-filed two-slice plan.
- We will NOT change the existing detection-framed blind-A/B protocol or its 0.60
  threshold — the preference framing in US-6 is additive, not a replacement.
- We will NOT open the allowlist — that remains a product decision gated on #1–#3 of the
  gap analysis, unaffected by this sprint's scope.

## Open questions for stakeholder

- US-7: "windowed style card" was specified as 60 days, but `chat.db`'s outbound floor
  (2026-08-03) plus today (2026-09-05) bounds real coverage to ~33 days. AC-7.3 handles
  this by reporting true coverage rather than claiming 60 — confirm that's the desired
  behavior rather than waiting until 60 real days accrue before shipping the windowed mode.
- US-6: should the preference-based human gate reuse the SAME rated trial pairs as the
  existing detection cycle, or does the rater pool have separate time budget for a fresh
  preference-framed batch? Assumed fresh batch (AC-6.5); confirm rater availability.
- US-8 is scoped as measurement-only per the sprint's explicit routing lever, but its
  leverage depends entirely on a future LIVE decision that is out of scope here — confirm
  this is worth L-sized effort this sprint vs. deferring to a future sprint once US-2/US-5
  land and free up review bandwidth.

RESULT_product-owner=READY — six MEASURED levers were broken into 8 stories with numeric,
file-cited acceptance criteria drawn directly from the fleet's own closing report and gap
analysis; the three genuine ambiguities (60-day window vs ~33 real days, rater-pool sizing
for a new preference batch, and US-8's speculative payoff) are flagged as open questions
without blocking the other seven stories, none of which require stakeholder input to start.
