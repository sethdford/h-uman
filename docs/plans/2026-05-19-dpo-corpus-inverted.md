# DPO Corpus is Inverted (2026-05-19)

> Discovered while applying the PersonaEval insight as a corpus-quality
> miner. Stops the Layer 1 ORPO plan from training against poisoned
> data. One commit's worth of evidence; rest of this doc is the
> remediation plan.

## Headline

**41.5% of the 369 rows in `dpo_pairs` are inverted** — the row
labeled "rejected" has a HIGHER P(Seth) score than the row labeled
"chosen". Training ORPO on this corpus today would teach the model
to prefer LESS Seth-shape responses.

The Layer 1 ORPO plan was scheduled to fire once `imessage_tapback`
threshold (≥500 real-signal rows) was hit. That schedule is now
gated on **fixing the existing corpus**, not just accumulating more.

## Evidence

Run: `python3 scripts/dpo_pair_persona_miner.py`

  Scoring 369 dpo_pairs rows

  === Pair quality buckets ===
    inverted  (P(chosen) < P(rejected)):   153 /  369 (41.5%)
    narrow    (|margin| < 0.1):             56 /  369 (15.2%)
    supporting (margin >= 0.1):            160 /  369 (43.4%)

    margin distribution: mean=+0.033, median=+0.053, min=-0.956, max=+1.000

  === Per-source quality ===
    generated_v2             n= 220  mean_margin=+0.073  inverted= 87  supporting= 99
    outbound_edit            n=  70  mean_margin=-0.014  inverted= 34  supporting= 24
    user_feedback            n=  69  mean_margin=+0.032  inverted= 23  supporting= 37
    reflection_retry         n=  10  mean_margin=-0.507  inverted=  9  supporting=  0

## Smoking gun

  id=31    margin=-0.956  source=user_feedback
    prompt:   "You are checking in with Annie Ford ..."
    chosen   P=0.000: ''         <- empty string in CHOSEN
    rejected P=0.956: 'SKIP'     <- "SKIP" in REJECTED

The `chosen` field is empty; the `rejected` field is "SKIP" or "GOOD".
That's not chosen-vs-rejected text — it's a label that the logger put
in the wrong column. ORPO would train the model to GENERATE EMPTY
STRINGS in this prompt's context, because the chosen column is what
gets reinforced.

## Per-source diagnosis

| source | n | mean_margin | inverted | supporting | diagnosis |
|---|---:|---:|---:|---:|---|
| `reflection_retry` | 10 | **−0.507** | 9 | 0 | Source completely backwards. Almost every row is inverted with large magnitude. Logger logic is wrong. |
| `outbound_edit` | 70 | −0.014 | 34 | 24 | ~50% inversion. The "edit" feedback may be conflating "kept my draft" vs "wrote a different one." Need a stricter chosen-rejected definition. |
| `user_feedback` | 69 | +0.032 | 23 | 37 | The empty-string + "SKIP" pattern dominates the inverted half. Logger writing skip-signals into the chosen column. |
| `generated_v2` | 220 | +0.073 | 87 | 99 | Synthetic pairs. Lowest signal. The margin is positive on average but 40% inverted; the synthesis isn't reliably picking the Seth-shape response as chosen. |

## Concrete remediations

### 1. Stop the bleeding — pre-train corpus filter

Before ANY ORPO run, exclude rows where `p_seth_chosen < p_seth_rejected`.
Concretely:

  # in human ml dpo-train, after corpus load:
  before = len(corpus)
  corpus = [r for r in corpus if r.p_seth_chosen >= r.p_seth_rejected]
  print(f"filtered {before - len(corpus)} inverted rows")

This is a one-day change. Doesn't require fixing the loggers.

### 2. Find and fix each logger

For each source, audit the code path that writes to `dpo_pairs`:

- `src/agent/reflection_retry.c` — writes 10/10 inverted rows. The
  swap is at the call site that converts retry-success / retry-failure
  into chosen / rejected.
- `src/channels/imessage.c` outbound-edit hook — writes 34/70 inverted.
  Likely conflating "user kept the draft" with "I drafted this and
  then sent something else."
- The `user_feedback` writer that emits empty-chosen + SKIP-rejected
  rows. These should be EXCLUDED from dpo_pairs entirely, not stored
  as chosen-vs-rejected — they're skip signals.

Each is a localized fix; total estimate: 1-2 days per source.

### 3. Add a corpus-quality gate to the watcher

`scripts/orpo_readiness_watcher.sh` currently fires ORPO training when
real-signal row count ≥ 500. Extend the readiness check:

  python3 scripts/dpo_pair_persona_miner.py
  # exits 1 if inverted rate > 5%

If miner exits 1, the watcher logs "corpus quality poor, blocking
training" and does not fire ORPO. Operator must fix the logger or
backfill clean data first.

### 4. Reframe Layer 1 in the master plan

The 6-month plan's Layer 1 was: "accumulate ≥500 real-signal rows,
then fire ORPO." After this finding, Layer 1 becomes:

  1.1  Fix each inverted-source logger
  1.2  Add corpus-quality gate to watcher
  1.3  Accumulate ≥500 real-signal rows (passive, unchanged)
  1.4  Fire ORPO with corpus filter active
  1.5  Score adapter with shape + P(Seth); ship if ΔP(Seth) ≥ +0.05

This moves the L1 critical path from "wait for corpus" to "fix
loggers in parallel with corpus accrual."

## Why this caught a real bug

Without the P(Seth) classifier, you'd never know the corpus was
inverted. The shape classifier is too coarse — both "SKIP" and an
empty string fail shape, so neither has a shape-score advantage.

P(Seth) on "SKIP" is 0.956 not because SKIP is Seth-shape but because
the classifier saw very few one-token responses in training and
defaults to high-confidence on short outputs. That's actually a
classifier weakness (need more "non-Seth-but-short" negatives), but
EVEN THIS WEAKNESS surfaces the inversion — the empty string scores
0.0 against ANYTHING, so any swapped-column row jumps out.

The lesson is broader: **a deterministic, cheap quality scorer over
training corpora is more valuable for catching dataset bugs than for
ranking responses.** This is now wired in as `dpo_pair_persona_miner.py`
and intended to run before every ORPO training round.

## Update — 2026-05-19 second pass

After the initial fix landed (refuse trivially-short writes in
`hu_dpo_record_from_feedback` + iterator filter on read), a deeper trace
of the `reflection_retry` rows surfaced a separate logger bug:

The 9/10 inverted reflection_retry rows have:
  chosen   = "NEEDS_RETRY. The response 'GOOD' is irrelevant..."  (critique)
  rejected = "GOOD"                                                (bad original)

The chosen column holds the CRITIQUE TEXT, not the corrected response.
This happens when the LLM, having seen the reflection critique in its
history, echoes the critique structure back as its retry attempt.
`agent_turn.c:5402` correctly passes (rejected=bad, chosen=resp.content),
but `resp.content` IS the echoed-critique text on those failure modes.

**Second-layer fix in `hu_dpo_record_from_retry`:**

  if (chosen_len < 4 || rejected_len < 4) return INVALID_ARGUMENT;
  if (chosen == rejected) return INVALID_ARGUMENT;
  if (chosen starts with "NEEDS_RETRY" or "needs_retry") return INVALID;

The third clause catches the echoed-critique pattern. Tests:
- `dpo_retry_with_identical_chosen_rejected` flipped from bug-pinning
  (HU_OK) to corrected (HU_ERR_INVALID_ARGUMENT)
- `dpo_retry_refuses_critique_as_chosen` is a NEW positive contract:
  exact-prefix critiques refused, embedded mentions allowed.

**On the universal write-side invariant:** I attempted to refuse
single-sided writes in `hu_dpo_record_pair` itself (covering the
`reaction_handler.c` path that writes positive-only / negative-only
tapback signal as poison). That broke 12 pre-existing tests whose
contract WAS "single-sided writes accumulate and are counted." The
trade-off — symmetric defense at write site vs. read-side filter only —
favors the read-side filter because:
  (a) `hu_dpo_iterate_pairs` already skips bad rows before training
  (b) the reaction signal IS valuable; the right home is a separate
      `negative_signals` / `positive_signals` table, not "block the
      write entirely"

That separate-table refactor is the follow-up; until then dpo_pairs
collects the rows (for completeness/audit) and the iterator skips them.

**Final defense graph (post-second-pass):**

  Write site (rejects with HU_ERR_INVALID_ARGUMENT):
    hu_dpo_record_from_feedback — response_len ≥ 4
    hu_dpo_record_from_retry    — both sides ≥ 4
                                — chosen != rejected
                                — chosen NOT starting with NEEDS_RETRY
    hu_dpo_record_pair          — permissive (legacy reaction_handler)

  Read site (silently skips):
    hu_dpo_iterate_pairs        — both sides ≥ 4 bytes

Full suite: 11110/11110 pass.

## Artifacts

  /tmp/dpo_persona_audit.json   full per-row audit
  scripts/dpo_pair_persona_miner.py    the miner
