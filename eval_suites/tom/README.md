# Theory-of-mind synthetic pack (B8 smoke)

Committed JSON scenarios for false-belief, second-order belief, and light
pragmatic / multilingual stubs. **Not** a `human eval run` task suite (no
`tasks` array); offline harnesses parse `items` directly.

## Layout

- `tom_synthetic.json` — versioned object with `items[]` (`id`, `category`,
  `premise`, `question`, `gold_answer`).

Future wiring: score against `hu_world_model_t` ToM fields once the synthesizer
lands (see `docs/plans/2026-05-10-w9-world-model.md`).
