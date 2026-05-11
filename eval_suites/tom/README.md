# Theory-of-mind synthetic pack (B8 smoke)

Committed JSON scenarios for false-belief, second-order belief, and light
pragmatic / multilingual stubs. **Not** a `human eval run` task suite (no
`tasks` array); offline harnesses parse `items` directly.

## Layout

- `tom_synthetic.json` — versioned object with `items[]` (`id`, `category`,
  `premise`, `question`, `gold_answer`).

Runtime wiring: `hu_tom_scenario_synthesize` + `hu_tom_b8_synthetic_pack_run_smoke` in
`src/agent/tom_scenario.c` / `include/human/agent/tom_scenario.h` load this JSON and
verify category-tagged ToM stubs (CI smoke). `hu_world_model_build` still owns the
graph-backed ToM path; the scenario API is for eval packs only.
