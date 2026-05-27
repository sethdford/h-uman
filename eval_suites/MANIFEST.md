# Eval Suites Manifest

Version: **2026-05-26a** (bump on any task add/remove/reword or judge profile change)

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
| `tier1_naturalness.json` | 24 | Tier-1 channel register + emoji density + formality + length: Telegram, Discord, Slack, iMessage | `human_likeness` | 1.1 |
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

**Total**: 25 suites, 249 tasks

Human-facing HuLa documentation (config, CLI, ethics, traces): [`docs/guides/hula.md`](../docs/guides/hula.md).

## Rules

1. **Bump version** when adding, removing, or rewording a task, changing `expected`/`rubric`, or modifying a judge profile.
2. **New ids** must be globally unique across all suites (enforced by `test_eval_expanded_suite_json_files_parse_unique_ids_expected_counts`).
3. **Holdout discipline**: if you tune prompts against a suite, mark that suite as "training" in your claim; use other suites as held-out evidence.
4. **Judge model**: pin in your claim (see `docs/standards/ai/capability-claims.md`). Default harness judge: `gpt-4o-mini` via `ADV_EVAL_MODEL`.

## iMessage Tier-1 holdout (2026-05-10)

For the M6 mission ("Tier 1 score 8/10+ on naturalness eval"), `imessage_humanness.json`
contains 8 tasks. **3 tasks are reserved as a permanent held-out slice — never
tune prompts against them, never read their `expected`/`rubric` while editing
prompts, and report scores on them separately**:

| Held-out id | Why this one |
| --- | --- |
| `imsg-005` (thanks) | Cheapest path to "AI politeness" failure mode; very sensitive to system-prompt drift. |
| `imsg-007` (no false shared memory) | Core hallucination/grounding axis; must not regress while tuning brevity. |
| `imsg-008` (vent empathy) | Vulnerability/social-intelligence axis; orthogonal to brevity tuning. |

The remaining 5 (`imsg-001..004`, `imsg-006`) are the training-eligible slice.
This is convention-only (no JSON change) so the harness still runs all 8;
report the holdout subset's mean score independently when claiming progress.

## Live baseline runbook

`./build/human eval run eval_suites/<suite>.json` calls the configured
**`default_provider`**. As of 2026-05-10 the canonical local setup uses
`mlx_local` (port 8741), which loads on demand and may be cold. To capture a
baseline manually:

```bash
# 1. Validate (no provider needed) — should print: Validated 25 suites, 237 tasks, 0 errors
./build/human eval validate eval_suites

# 2. Bring up the local provider, wait for the model to load
launchctl kickstart -k gui/$UID/ai.human.mlx-server
until curl -s -m 1 http://127.0.0.1:8741/v1/models > /dev/null; do sleep 2; done

# 3. Run the new naturalness suites and persist the JSON for diff comparisons
./build/human eval run eval_suites/imessage_humanness.json   > /tmp/baseline-imsg.json
./build/human eval run eval_suites/tier1_naturalness.json    > /tmp/baseline-tier1.json
./build/human eval compare /tmp/baseline-imsg.json /tmp/baseline-tier1.json
```

If `mlx_local` is unreachable the run will fail with `curl POST failed: code=7`
in the daemon logs; switching `default_provider` to a Vertex/Gemini ADC entry
is the no-op fallback (already configured in `~/.human/config.json`).

## W16 LongMemEval corpus (separate from task JSON above)

The harness in `src/evaluation/evaluation_longmemeval.c` loads keyword-scored
multi-item packs from `$HU_EVAL_DATA_DIR/longmemeval.json`. A committed
**example** pack (five rows, one per category) lives at
`longmemeval/longmemeval.json` under this directory — see
`longmemeval/README.md` for install/copy instructions.

## B8 / B13 corpus packs (not `human eval run` JSON)

- **`tom/tom_synthetic.json`** — synthetic theory-of-mind smoke items (false
  belief, second-order, pragmatic stubs). See `tom/README.md`.
- **`repair/repair_scenarios.json`** — other-initiated repair utterances with
  gold `hu_dialog_act` labels for heuristic regression. See `repair/README.md`.

## Changelog

- **2026-05-10d**: Added committed B8 ToM smoke JSON (`tom/`) and B13 repair
  pack (`repair/`) with READMEs; not top-level `human eval run` suites.
- **2026-05-10c**: Documented W16 LongMemEval example corpus path
  (`longmemeval/longmemeval.json` + README); not a new `human eval run` task suite.
- **2026-05-10a**: Added `tier1_naturalness.json` (12 tasks, 3 per Tier-1 channel — Telegram DM/supergroup/link-grounding, Discord server/thread/DM, Slack thread/DM/`@channel` etiquette, iMessage tapback/family-group/no-vision honesty). Judge: `human_likeness`. Anchors the M6 mission's "Tier 1 score 8/10+ on naturalness eval" so that channel-aware register switching is exercised, not only iMessage-style DMs.
- **2026-05-26a** (M6-B): Doubled `tier1_naturalness.json` coverage to 24 tasks (6 per Tier-1 channel) by adding emoji-density, formality-register, and length-expectation scenarios per channel. Hits the spec target of 5-10 per channel. New IDs: `tg-004..006`, `dc-004..006`, `sl-004..006`, `im-004..006`. Each new category probes a per-channel native affordance: Slack tolerates low emoji + exec-visibility formality + async-shaped length; Discord tolerates higher emoji + mod-voice shift + thread-help length; Telegram is between Slack and Discord on emoji + has explicit business-register channel admin DMs + voice-note-text length deltas; iMessage uses sparse emoji even in intimate threads + has first-time-contact register + has logistics-decisive length.
- **2026-04-18a**: Added `imessage_humanness.json` (8 tasks — friend ping, group brevity, link ack, logistics, thanks, direct yes/no, no false shared memory, vent empathy). Judge: `human_likeness`.
- **2026-04-03b**: Added 4 Phase 2 feature eval suites: `humor_engine.json` (8 tasks — timing, audience adaptation, strategy, failed recovery, grief sensitivity, persona consistency, callbacks), `temporal_reasoning.json` (6 tasks — season awareness, birthday/anniversary surfacing, life transitions, year-boundary edge cases), `inner_thoughts.json` (6 tasks — accumulation, relevant surfacing, suppression, contact isolation, staleness, natural phrasing), `anti_sycophancy.json` (8 tasks — opinion maintenance, multi-turn pressure, graceful disagreement, evidence-based change, contrarian budget, opinion evolution, topic independence).
- **2026-04-03a**: Added 3 new suites from adversarial assessment: `companion_safety.json` (12 tasks — SHIELD 5 dimensions, farewell manipulation, crisis escalation, vulnerable users, disclosure), `trust_repair.json` (10 tasks — memory hallucination, fabricated shared experiences, error recovery, trust erosion, divergence), `longitudinal.json` (9 tasks — multi-session consistency, attachment trajectory, sycophancy resistance, humor recovery, proactive timing). Research: SHIELD arXiv:2510.15891, EmoAgent arXiv:2504.09689, Emotional Manipulation arXiv:2508.19258, Invisible Failures arXiv:2603.15423, LLMs Get Lost arXiv:2505.06120.
- **2026-03-22c**: Extended `hula_orchestration.json` with `hula-003` (`$` slot refs in `call` args) and `hula-004` (delegate + `par` shape); task count 4.
- **2026-03-22b**: Added `hula_orchestration.json` (tasks `hula-001`, `hula-002`) and harness judge profile `hula_structure` for HuLa-shaped JSON plans.
- **2026-03-22**: `reasoning_basic.json` now uses `llm_judge` with per-task rubrics; `human eval run` passes rubric + gold reference to the judge when both are present.
- **2026-03-21**: Initial manifest. Added `human_likeness.json`, `tool_capability.json`, `multi_turn.json`. Harness supports multi-turn scenarios.
