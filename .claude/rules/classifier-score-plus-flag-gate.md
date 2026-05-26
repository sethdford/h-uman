# Classifier Gating — Use Score AND Fail-Flags, Not Either Alone

When a classifier returns both a continuous score AND discrete fail
flags, those are TWO complementary signals. Gating on either one
alone leaves a known class of failures unblocked. The right pattern
is a hybrid OR:

    if (classifier.gate_fails || (classifier.flags & FLAGS_OF_INTEREST))

## The hazard

A classifier like `hu_shape_classify` (see `include/human/eval/shape.h`)
returns:

    typedef struct hu_shape_result {
        double   score;          /* [0.0, 1.0], 1.0 = best */
        bool     passed;         /* score >= 0.7 AND no fatal flag */
        uint32_t fail_flags;     /* bitmask of HU_SHAPE_FAIL_* */
    } hu_shape_result_t;

The scoring rule is asymmetric on purpose:

| Violation kind                            | Score deduction |
|-------------------------------------------|-----------------|
| Heavy (BULLET_LIST, HEADER, WAY_TOO_LONG) | -0.30           |
| Light (CERTAINLY, GREAT_QUESTION, etc.)   | -0.15           |

A single LIGHT violation lands the score at 0.85 — well above any
reasonable threshold-only gate. Yet "Certainly! I can help with that."
is unambiguously out-of-voice for an iMessage proactive to family.

The shape classifier penalizes individual light flags mildly because
it was designed for OFFLINE EVAL scoring where the LLM judge supplies
the dominant signal. When you wire it INLINE in a pipeline gate, you
need a different combiner — the score is too forgiving on its own.

### Real-world cost (2026-05-26 Sprint 60 persona wiring)

First implementation used a single-score threshold. Tests against the
RED-state code showed which thresholds miss what:

| Threshold | Misses                                          | False-positives |
|-----------|-------------------------------------------------|-----------------|
| 0.5       | "Certainly!" (0.85), "Here are\n-bullets" (0.55), 400-char body (0.7) | none observed |
| 0.7       | "Certainly!" (0.85), 400-char body (0.7)        | "Here are\n-bullets" (0.55) ✓ |
| 0.9       | none                                            | borderline TOO_LONG at 0.85 → false REGENERATE |

No score threshold is in the right place. The classifier's own
`.passed` predicate folds score + fatal-flag conditions and catches
gradient cases. The opener-flag mask catches single-flag light
violations. Together they cover both axes.

## Why the obvious fix is wrong

❌ **Pure score gate.** Score 0.5 misses single openers (0.85);
score 0.9 false-positives on borderline content. There's no
right number when the deduction-per-flag is asymmetric.

❌ **Pure flag gate (any fail_flag → REGENERATE).** Too aggressive
— TOO_LONG alone (-0.15) is borderline acceptable for some channels;
gating on every flag would block legitimate borderline content.

❌ **Lower the score threshold to 0.85.** Trades false negatives
for false positives. Single-heavy-flag content scores 0.7 and
would slip past; multi-light-flag content (e.g. CERTAINLY +
TOO_LONG = 0.7) would also slip past.

❌ **"Just use `.passed`".** Catches structural fails and gradient
score drops, but misses single-flag light violations whose
deduction doesn't drop the score below 0.7.

## The right shape

```c
if (classifier.passed_predicate_fails ||
    (classifier.flags & FLAGS_THAT_ARE_NEVER_OK)) {
    /* gate fires — REJECT or REGENERATE */
}
```

Two complementary signals, ORed. Where:

- `passed_predicate_fails` is the classifier's OWN definition of
  failure. Reuse what the classifier already says is "fail" — don't
  re-derive it from raw score. For `hu_shape_classify`, that's
  `!shape.passed` (which folds `score < 0.7` + `fatal_md` +
  `way_too_long` internally).
- `FLAGS_THAT_ARE_NEVER_OK` is the WHITELIST of specific flags you
  consider terminal even at high score. For shape on a persona
  surface, that's the AI-opener mask:

      #define HU_PERSONA_SHAPE_AI_OPENER_MASK \
          (HU_SHAPE_FAIL_DEPENDING_ON | HU_SHAPE_FAIL_HERE_ARE | \
           HU_SHAPE_FAIL_CERTAINLY    | HU_SHAPE_FAIL_ABSOLUTELY | \
           HU_SHAPE_FAIL_GREAT_QUESTION | HU_SHAPE_FAIL_I_UNDERSTAND)

Both halves of the OR are documented at the call site so a future
reader sees why the gate has two conditions.

## Threshold provenance — verify before adopting

Thresholds documented in design.md or spec files are TIED to specific
classifiers. The same number does NOT transfer between classifiers
with similar names.

Concrete near-miss from the same session: `design.md` Q-3 said
"fidelity < 0.5 → REGENERATE." That threshold came from the **persona
fidelity** classifier (adapter scoring, where v4-repair lifted Seth-
voice 0.586 → 0.856). Applying it to the **shape classifier** —
which uses different scoring math (asymmetric per-flag deductions
with a 0.7 "passed" baseline) — silently broke 4 of 10 tests.

Before adopting a numeric threshold from any design doc:

1. Identify the SPECIFIC classifier the threshold was measured against.
   Names like "fidelity," "shape," "score" are reused.
2. Check the classifier's own internal scoring rule (where do
   deductions come from, how are they combined).
3. Run the threshold against the corpus the threshold was derived
   from. If you don't have that corpus, treat the number as a starting
   guess, not a load-bearing constraint.

## When this rule applies

YES — apply when:
- Integrating a deterministic classifier (`hu_shape_classify`,
  future shape variants, persona fidelity scorers)
- Consuming any function whose output is `{score, fail_flags}` or
  `{score, fail_reasons}` (this shape is common in eval harnesses)
- Composing a classifier's output INLINE in a pipeline — the
  classifier was tuned for offline use and needs hybrid gating
  for production

NO — skip when:
- Pure-threshold predicates with no flag output (just gate on score)
- Single-flag signals where the flag IS the entire signal (boolean
  predicates like `is_safe`)
- Cases where false negatives are cheap and false positives
  expensive — then the gate should be conservative-score-only

## Audit pattern when adopting a new classifier

1. **Read the classifier's scoring rule.** What's the deduction per
   flag, what's the "passed" baseline?
2. **List the discrete flags.** Identify which ones you ALWAYS
   consider terminal regardless of overall score.
3. **Build the AI-opener-mask-equivalent** as a `#define` at the
   call site (not inline magic number) — names like
   `HU_PERSONA_SHAPE_AI_OPENER_MASK` document the rationale.
4. **Pin both branches with tests** — at least one test per:
   - structural fail (high-score, fail_flag in mask) → gate fires
   - gradient fail (low-score, no flags in mask) → gate fires
   - false-positive case (high-score, flags not in mask) → SEND

## Related

- `src/agent/outbound/persona.c:163-190` — first instance applying
  this rule (shape classifier + AI_OPENER_MASK)
- `src/agent/outbound/crosstalk.c:262-303` — sibling instance
  (Jaccard score + metadata-pattern flag, hybrid-ORed)
- `.claude/rules/security-predicate-extraction.md` — the "extract
  testable predicates" rule; this rule is the analogous "compose
  multiple signals into one predicate" pattern
- `~/.claude/rules/substring-classifier-pitfalls.md` — another case
  where naive single-signal gates fail (substring overlap); global
  rule, sibling to this one
- `~/.claude/rules/audit-verify-before-allege.md` — the audit-
  discipline rule that catches the threshold-provenance mistake
  (verify what the threshold was measured against before adopting)
