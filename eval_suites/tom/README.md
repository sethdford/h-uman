# Theory-of-mind synthetic pack (B8 smoke)

Committed JSON scenarios for false-belief, second-order belief, and light
pragmatic / multilingual stubs. **Not** a `human eval run` task suite (no
`tasks` array); offline harnesses parse `items` directly.

## Layout

- `tom_synthetic.json` — versioned object with `items[]` (`id`, `category`,
  `premise`, `question`, `gold_answer`).

Runtime wiring (`src/agent/tom_scenario.c` / `include/human/agent/tom_scenario.h`):

- `hu_tom_scenario_synthesize` — premise + question + category → `hu_theory_of_mind_t` with a `[ToM:<tag>]` planner hint.
- `hu_world_model_merge_tom_scenario` — merges that ToM into an existing `hu_world_model_t` so the bridge renders a single block.
- `hu_w7_render_world_model` accepts optional trailing `tom_premise` / `tom_question` / `tom_category` parameters; when all three are non-empty the bridge merges before formatting. `hu_agent_set_tom_scenario` copies (or clears) per-agent buffers that `agent_turn.c` / `agent_stream.c` thread through. Production turns leave the fields empty; eval / benchmark drivers set them before `hu_agent_turn`.
- `hu_tom_b8_synthetic_pack_run_smoke` — category-tag self-test against the JSON pack (CI smoke).
- `hu_tom_b8_synthetic_pack_score_gold` — same JSON, scored against premise + question + synthesized ToM (drift sentinel for the rubric).
- `hu_tom_b8_synthetic_pack_score_responses` — CLI / model-eval hook: takes an `hu_tom_b8_response_t[]` (id + free-form response) and scores via the underscore-tokenised gold matcher. The `count_unanswered_as_failed` flag lets a benchmark driver decide whether missing answers degrade the score or are skipped.

`hu_world_model_build` still owns the graph-backed ToM path; the scenario API is for eval packs and benchmarks only.
