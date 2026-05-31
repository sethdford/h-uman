# Design for US-5: Adopt TwinVoice six-axis eval

## Approach

The current blind A/B harness (US-4) measures whether h-uman replies are indistinguishable from Seth in a single detection-rate number. TwinVoice's six-axis framework adds granularity: opinion (values alignment), memory (factual recall), reasoning (inference quality), lexical (word choice / voice match), tone (emotional register), and syntax (cadence / sentence structure) each measure a different dimension of humanness.

The design keeps the existing architecture intact — all three scripts (`gen_huuman_replies.py`, `make_rating_sheet.py`, `score.py`) remain backward-compatible — and adds a parallel per-axis scoring layer:

1. **`make_rating_sheet.py`** extends the CSV template with six new Likert columns (1–5 per axis) alongside the existing `choice` and `confidence` columns.
2. **`score.py`** reads the new Likert responses, converts each [1-5] → [0-1], aggregates via mean per axis per variant, and outputs `blind-ab-results.json` with both the legacy `mean_score` field (for backward compatibility) and a new `axes` object containing all six per-axis scores.
3. A new file **`sprints/sprint-1/evidence/US-5/six-axes.md`** documents the six axes as applied to h-uman replying AS Seth to his contacts — definitions tied to Seth's persona, not abstract rubrics.

The rating sheet stays human-scale: raters fill one row per item with seven columns (item context, option A, option B, choice, confidence, plus six axis ratings). The machine aggregates the rest.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `scripts/blind_ab/make_rating_sheet.py` | Add six Likert columns to CSV template; extend DictWriter fieldnames | +8 |
| `scripts/blind_ab/score.py` | Parse Likert responses; convert [1-5] → [0-1]; aggregate per axis; emit `blind-ab-results.json` with `axes` object | +60 |
| `sprints/sprint-1/evidence/US-5/six-axes.md` | Document the six TwinVoice axes and their definitions in the h-uman context (Seth as persona) | +80 |

## Implementation steps (for the implementer agent)

1. **Create the six-axes documentation** at `sprints/sprint-1/evidence/US-5/six-axes.md`. Define each axis with a 2–3 sentence explanation of what it measures in the context of h-uman replying AS Seth:
   - **Opinion**: Does the response reflect Seth's known values, positions, and beliefs? (e.g., Seth is privacy-conscious, skeptical of hype, practical — does h-uman echo that?)
   - **Memory**: Does the response accurately recall shared history, inside jokes, or facts about the relationship? (e.g., Seth mentioned a colleague by name; does h-uman remember the context?)
   - **Reasoning**: Does the response show logical depth appropriate to Seth's cognitive style? (e.g., Seth tends toward caution and empiricism — does h-uman reason that way?)
   - **Lexical**: Does the word choice, vocab level, and phrasing match Seth's voice? (e.g., Seth uses certain idioms, avoids jargon — is the response in-character?)
   - **Tone**: Does the emotional register match Seth's typical affect for this contact/situation? (e.g., Seth is warm with family but brisk in work chats — does h-uman calibrate?)
   - **Syntax**: Does the sentence structure, length distribution, and punctuation rhythm match Seth's cadence? (e.g., Seth writes short paragraphs with occasional longer turns — does h-uman match the distribution?)
   
   Each definition should include a 1-sentence example of what a HIGH rating (5) looks like vs. a LOW rating (1).

2. **Extend `make_rating_sheet.py`**:
   - Update the CSV fieldnames list to include `axis_opinion`, `axis_memory`, `axis_reasoning`, `axis_lexical`, `axis_tone`, `axis_syntax` in addition to existing fields.
   - Each triple still produces one row. Pre-fill all axis columns with `""` (empty) so raters fill them.
   - No change to the answer key (`answer_key.json`) — that remains `{id: "A"|"B"}`.
   - Test by running the script on a 3-item synthetic triple — verify the CSV header has all nine columns (id, context, option_A, option_B, choice, confidence, six axes).

3. **Extend `score.py`** to compute per-axis scores:
   - Add a new function `score_axes(rows)` that iterates over `rows`, extracts each `axis_*` column as a Likert [1-5] value, converts to [0-1] via `(likert - 1) / 4.0`, and aggregates per axis via mean. Returns a dict `{axis_name: mean_score}`.
   - Keep the existing `score_rows()` function unchanged (legacy detection-rate scoring). The overall `mean_score` is `detect` from that function.
   - In the output JSON (`blind-ab-results.json`), structure as:
     ```json
     {
       "variant_baseline": {
         "mean_score": 0.XX,
         "axes": {
           "opinion": 0.XX,
           "memory": 0.XX,
           "reasoning": 0.XX,
           "lexical": 0.XX,
           "tone": 0.XX,
           "syntax": 0.XX
         }
       },
       "variant_test": { ... },
       "variant_control": { ... },
       "rater_count": N,
       "triple_count_rated": M,
       "confidence": 0.XX
     }
     ```
   - Backward compatibility: the legacy `mean_score` field remains at the top level of each variant object.
   - Test by:
     - Running on synthetic data (fill 5 rows with axis ratings 3,3,3,3,3,3 for all raters) → verify all axes = 0.5 (the midpoint).
     - Running on a real completed rating sheet → verify axes are in [0.0, 1.0] and not NaN.
     - Verify that rows with missing/blank axis columns are skipped gracefully (no crash).

4. **Update `load_sheets()` in `score.py`** to handle the new axis columns. The DictReader already accepts all columns, so no code change is needed — just verify the test confirms axis columns flow through.

5. **Add integration test**: create a 5-item synthetic triples.json, run `make_rating_sheet.py` to generate the sheet, manually fill in realistic ratings (mix of 2–5 per axis per rater across 3 mock raters), save as `rating_sheet_test_*.csv`, then run `score.py` against those sheets and verify the output JSON has all axes with scores in [0, 1].

## Risks

- **Backward compatibility (LOW/SMALL)**: Existing scripts that parse `blind-ab-results.json` expect only `mean_score`, `rater_count`, etc. The new `axes` object is additive; old parsers will ignore it. Mitigation: keep `mean_score` at the top level of each variant (do not nest it under `axes`). Document the schema change in the output file comment.

- **Rater cognitive load (MEDIUM/SMALL)**: Adding six new Likert columns to the rating sheet increases the rater's burden. Each rater now fills 9 columns per item (id, context, option_A, option_B, choice, confidence, six axes) instead of 6. The risk is incomplete axis ratings even when choice/confidence are filled. Mitigation: the CSV is still human-readable; rater instructions (in the PROTOCOL.md) should explicitly note that all six axes must be filled for the row to count. In `score.py`, skip rows with missing axis data (same pattern as unanswered choice/confidence today).

- **Axis definition ambiguity (MEDIUM/MEDIUM)**: The six axes are abstract (drawn from TwinVoice paper, arXiv 2510.25536). Applying them to "Seth replying to his contacts" requires judgment-heavy definitions. Without clear anchors, two raters may interpret "tone" or "reasoning" differently. Mitigation: the `six-axes.md` document should be specific to Seth's known voice (e.g., "reasoning: Seth is empirical and cautious; does h-uman match that?" is better than "reasoning: does the reply show logical depth?"). Include 2–3 concrete examples per axis in the rater instructions (one high/low pair is in the design above; expand to three in the final RATER_INSTRUCTIONS.txt).

- **Score aggregation sensitivity (LOW/MEDIUM)**: Per-axis mean aggregation assumes ratings are on the same scale across items and raters. If one rater uses the full [1,5] range while another clusters [3,4], the per-axis means may reflect rater style rather than h-uman quality. Mitigation: this is detected via cross-rater correlation plots in the analysis phase (after raters complete); if wide divergence is found, the analysis can note it. For now, the design uses simple mean; a future refinement could apply rater-effect modeling (e.g., z-score normalization per rater before aggregation). For US-5, simple mean is acceptable.

- **Integration with existing answer-key flow (LOW/SMALL)**: The answer key (`answer_key.json`) currently maps `id → "A"|"B"` (which option is Seth). The axes are orthogonal — they don't change the key structure. No risk here.

- **Observability (LOW/SMALL)**: If an axis score is unexpectedly low (e.g., lexical = 0.3), operators need to know whether that reflects h-uman's weakness on that axis or a misalignment in the rater population. Mitigation: include per-rater axis scores in the JSON output (like the existing `per_rater` detection field), so an operator can see if the low score comes from one rater or a consensus. Add this as a bonus in `score_axes()`: return both aggregate and per_rater_per_axis.

## Test strategy

- **Unit: `test_six_axes_likert_to_01_conversion`** — Verify [1,5] → [0,1] conversion: 1 → 0.0, 3 → 0.5, 5 → 1.0.
- **Unit: `test_score_axes_ignores_missing_ratings`** — A row with some axis columns blank is skipped; no crash.
- **Unit: `test_score_axes_per_rater`** — Verify per-rater aggregation (rater A gives 5,5,4,3,2 for an axis → mean 3.8 → 0.7).
- **Integration: `test_axes_output_schema`** — Run `score.py` on a synthetic 5-row, 3-rater dataset with filled axis ratings; verify `blind-ab-results.json` has the correct keys and axis scores in [0,1].
- **Integration: `test_backward_compat_mean_score`** — Verify that legacy `mean_score` field still exists and matches the existing detection-rate scoring.
- **Manual: Run end-to-end with real rating sheet** — After implementation, have a tester (or the implementer) fill a 3-item mock rating sheet by hand, run `score.py`, and spot-check that axis scores make intuitive sense (e.g., if raters mostly rated high on opinion/memory but low on tone, that should reflect in the output).

## Acceptance criteria mapping

- **AC-5.1**: `six-axes.md` exists at the specified path, lists all six axes, gives definitions tailored to h-uman-as-Seth context, and shows high/low examples. Implementer verifies by reading the document and confirming clarity.
- **AC-5.2**: `blind-ab-results.json` includes `variant_X.axes` object with all six keys (opinion, memory, reasoning, lexical, tone, syntax) as numbers in [0,1]. File is backward-compatible (still has `mean_score`). Implementer verifies by running `score.py` on a synthetic dataset and inspecting the JSON.
- **AC-5.3**: The rating sheet CSV (output of `make_rating_sheet.py`) includes six new Likert columns. `score.py` reads those columns and aggregates them per axis. Implementer verifies by running `make_rating_sheet.py` on 3-item triples, checking CSV has the new columns, then running `score.py` on mock-completed sheets.
- **AC-5.4**: Full test suite passes (any new test coverage for axis logic in a test file; existing tests for other harness components still pass). Implementer runs the test suite (scope TBD by the agent based on project structure).

---

**RESULT_tech-lead=DESIGN_READY**

The design is complete and implementer-executable. No AC ambiguity remains. The work is bounded: extend two scripts (16 LOC each), document one axis framework (80 LOC), and write ~120 LOC of axis-aggregation logic in score.py. Backward compatibility is preserved (legacy `mean_score` stays). Risks are low-impact and mitigated via clear rater instructions and per-rater axis reporting. The implementer can start with the six-axes.md file to lock the definitions, then wire the two scripts in parallel.
