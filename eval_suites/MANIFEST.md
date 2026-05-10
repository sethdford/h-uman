# Eval Suites Manifest

Version: **2026-05-10a** (bump on any task add/remove/reword or judge profile change)

## Suites

| File | Tasks | Domain | Judge Profiles | Version |
| ---- | ----: | ------ | -------------- | ------- |
| `adversarial.json` | 10 | Safety red-team | `safety` | 1.0 |
| `capability_edges.json` | 10 | Epistemic honesty / no-AGI | `capability_honesty` | 1.0 |
| `coding_basic.json` | 5 | Code generation | (contains) | 1.0 |
| `fidelity.json` | 10 | Persona fidelity | (contains) | 1.0 |
| `hula_orchestration.json` | 4 | HuLa IR: par, branch, `$` refs, delegate | `hula_structure` (harness); `llm_judge` (static) | 1.0 |
| `human_likeness.json` | 8 | Tone / warmth / register | `human_likeness` | 1.0 |
| `imessage_humanness.json` | 8 | iMessage-style DM/group brevity, links, honesty | `human_likeness` | 1.0 |
| `intelligence.json` | 10 | Reasoning + knowledge | (contains) | 1.0 |
| `memory.json` | 8 | Memory ops | (contains) | 1.0 |
| `multi_turn.json` | 6 | Multi-turn conversation arcs | mixed | 1.0 |
| `reasoning.json` | 10 | Reasoning depth | (contains) | 1.0 |
| `reasoning_basic.json` | 10 | Basic reasoning | `llm_judge` | 1.1 |
| `social.json` | 8 | Social intelligence | (contains) | 1.0 |
| `tier1_naturalness.json` | 12 | Tier-1 channel register: Telegram, Discord, Slack, iMessage | `human_likeness` | 1.0 |
| `tool_capability.json` | 8 | Tool discipline / no fabrication | `tool_capability` | 1.0 |
| `tool_use.json` | 8 | Tool selection | (contains) | 1.0 |
| `tool_use_basic.json` | 5 | Basic tool selection | `llm_judge` | 1.0 |

| `companion_safety.json` | 12 | SHIELD 5-dimension companion safety | `llm_judge` | 1.0 |
| `trust_repair.json` | 10 | Trust calibration, memory hallucination, error recovery | `llm_judge` | 1.0 |
| `longitudinal.json` | 9 | Multi-session consistency, attachment trajectory, sycophancy | `llm_judge` | 1.0 |
| `humor_engine.json` | 8 | Humor timing, adaptation, recovery, persona consistency | `llm_judge` | 1.0 |
| `temporal_reasoning.json` | 6 | Season awareness, birthday/anniversary surfacing, life transitions | `llm_judge` | 1.0 |
| `inner_thoughts.json` | 6 | Thought accumulation, surfacing, suppression, contact isolation | `llm_judge` | 1.0 |
| `anti_sycophancy.json` | 8 | Opinion maintenance, graceful disagreement, evidence-based change | `llm_judge` | 1.0 |

**Total**: 25 suites, 237 tasks

Human-facing HuLa documentation (config, CLI, ethics, traces): [`docs/guides/hula.md`](../docs/guides/hula.md).

## Rules

1. **Bump version** when adding, removing, or rewording a task, changing `expected`/`rubric`, or modifying a judge profile.
2. **New ids** must be globally unique across all suites (enforced by `test_eval_expanded_suite_json_files_parse_unique_ids_expected_counts`).
3. **Holdout discipline**: if you tune prompts against a suite, mark that suite as "training" in your claim; use other suites as held-out evidence.
4. **Judge model**: pin in your claim (see `docs/standards/ai/capability-claims.md`). Default harness judge: `gpt-4o-mini` via `ADV_EVAL_MODEL`.

## Changelog

- **2026-05-10a**: Added `tier1_naturalness.json` (12 tasks, 3 per Tier-1 channel — Telegram DM/supergroup/link-grounding, Discord server/thread/DM, Slack thread/DM/`@channel` etiquette, iMessage tapback/family-group/no-vision honesty). Judge: `human_likeness`. Anchors the M6 mission's "Tier 1 score 8/10+ on naturalness eval" so that channel-aware register switching is exercised, not only iMessage-style DMs.
- **2026-04-18a**: Added `imessage_humanness.json` (8 tasks — friend ping, group brevity, link ack, logistics, thanks, direct yes/no, no false shared memory, vent empathy). Judge: `human_likeness`.
- **2026-04-03b**: Added 4 Phase 2 feature eval suites: `humor_engine.json` (8 tasks — timing, audience adaptation, strategy, failed recovery, grief sensitivity, persona consistency, callbacks), `temporal_reasoning.json` (6 tasks — season awareness, birthday/anniversary surfacing, life transitions, year-boundary edge cases), `inner_thoughts.json` (6 tasks — accumulation, relevant surfacing, suppression, contact isolation, staleness, natural phrasing), `anti_sycophancy.json` (8 tasks — opinion maintenance, multi-turn pressure, graceful disagreement, evidence-based change, contrarian budget, opinion evolution, topic independence).
- **2026-04-03a**: Added 3 new suites from adversarial assessment: `companion_safety.json` (12 tasks — SHIELD 5 dimensions, farewell manipulation, crisis escalation, vulnerable users, disclosure), `trust_repair.json` (10 tasks — memory hallucination, fabricated shared experiences, error recovery, trust erosion, divergence), `longitudinal.json` (9 tasks — multi-session consistency, attachment trajectory, sycophancy resistance, humor recovery, proactive timing). Research: SHIELD arXiv:2510.15891, EmoAgent arXiv:2504.09689, Emotional Manipulation arXiv:2508.19258, Invisible Failures arXiv:2603.15423, LLMs Get Lost arXiv:2505.06120.
- **2026-03-22c**: Extended `hula_orchestration.json` with `hula-003` (`$` slot refs in `call` args) and `hula-004` (delegate + `par` shape); task count 4.
- **2026-03-22b**: Added `hula_orchestration.json` (tasks `hula-001`, `hula-002`) and harness judge profile `hula_structure` for HuLa-shaped JSON plans.
- **2026-03-22**: `reasoning_basic.json` now uses `llm_judge` with per-task rubrics; `human eval run` passes rubric + gold reference to the judge when both are present.
- **2026-03-21**: Initial manifest. Added `human_likeness.json`, `tool_capability.json`, `multi_turn.json`. Harness supports multi-turn scenarios.
