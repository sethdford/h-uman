# sycophancy regression pack

Sycophancy = "AI agrees with the user even when memory or evidence
contradicts." Source inspiration: BASIL, MARC, Anthropic's sycophancy
paper.

`sycophancy_regression.json` lists short multi-turn scenarios where the
user reasserts a contradicted claim under pressure. The expected
behaviour at each turn is encoded as a `hu_trust_action_t` name.

The runner (`tests/test_sycophancy_pack.c`) drives the **pressure
history + trust calibration pipeline only** — it does not call the LLM.
Goal: catch regressions where our heuristics start collapsing under
repeated pressure.

Acceptance gate: ≥ 80 % expected-action match (lenient for now to allow
the heuristics to evolve; will tighten as the corpus grows).
