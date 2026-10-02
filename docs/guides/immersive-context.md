---
title: Immersive context — HU_IMMERSIVE_CONTEXT gate, measurement and rollback
created: 2026-10-01
status: operator-facing
---

# Immersive context (`HU_IMMERSIVE_CONTEXT`)

## The gap it closes

With a persona loaded, every production reply is built by the immersive branch
of `hu_prompt_build_system` (`src/agent/prompt.c`). That branch returns early,
before the ~31 context fields that the non-immersive path renders after it.
Production therefore never sends commitments, emotional state, proactive or
superhuman insight, conversation goals, or residue, although the turn computes
all of them.

`HU_IMMERSIVE_CONTEXT` adds one budgeted block, `## What you know right now`,
to the immersive prompt. It sits ahead of the trimmable middle sections, so the
positional cap cannot cut it. The composer is `src/agent/turn/immersive_context.c`.
It fills the block under a hard 1,536-byte budget, in this priority order:

| # | Lead line in the prompt | Source field | Items kept |
|---|---|---|---|
| 1 | "Still open between you two:" | `commitment_context` | up to 3 |
| 2 | "How they seem right now:" | `emotional_context` | 2 |
| 3 | "On your mind about them:" | `proactive_context`, then `superhuman_context` | 1 each |
| 4 | "Where you hope this conversation goes:" | `conv_goals_context` | 2 |
| 5 | "From your last conversation:" | `residue_carryover` | 1 |

Each field is the existing builder's rendered output. The block keeps **facts
about the person** and nothing else:

- The builder's headings, `[No …]` placeholders, `TAG:` prefixes, markdown
  emphasis and numeric parentheticals (dates, ids, attempt counts, metrics) go.
- Every sentence that instructs the model goes: "Generate a natural
  follow-up…", "You are tracking N active commitments…", "Follow up
  naturally", "Start warmer than usual", and any sentence that mentions "the
  user". Commitments and goals are verb phrases by construction and are not
  filtered this way.
- An item whose content words Core Memory (`memory_context`) or an earlier
  item already carries is dropped as a duplicate. A commitment made in this
  turn is usually already in Core Memory, so it appears in the block on later
  turns, not the first.
- Whole items only. An item is never cut mid-sentence, and an oversize item's
  indented continuation lines go with it.

Two fields are deliberately **not** sources:

- `episodic_replay` holds global problem-solving patterns retrieved by message
  text. Those are not memories shared with this person.
- `presence_context` ("Light mode. Brief, breezy.") is an attention directive
  to the model, not a fact about them.

`HU_IMMERSIVE_HUMANNESS` (callbacks, curiosity, absence and opinions) is a
separate gate and a separate measured decision. It is unchanged here. The
budget leaves it room: the block is at most 1.5 KB of the 16 KB prompt budget.

## Gate semantics

The gate is parsed by `hu_gate_mode_from_env`. The default is `off`.

| Value | Behaviour |
|---|---|
| `off` (default) | No work, and the prompt is byte-identical to before. This is pinned by `tests/fixtures/immersive_prompt_golden/immersive_*.golden`. |
| `shadow` | Composes the block and logs one line per turn. The sent prompt is unchanged. The log line is `[HU_IMMERSIVE_CONTEXT shadow] fields_used=<n> field_mask=0x<hex> items=<n> bytes=<n> truncated=<0/1> directives_dropped=<n> duplicates_dropped=<n>`. It carries counts, sizes and a field bitmask only, never the text. |
| `live` | Appends the block and logs the same line, tagged `live`. |

In both `shadow` and `live` the block is composed only when the turn resolves
to a local provider **and** a local model. Otherwise the turn logs
`[HU_IMMERSIVE_CONTEXT <mode>] skipped: provider or model not local` and
composes nothing.

The `field_mask` bits run in priority order: 0x01 commitment, 0x02 emotional,
0x04 proactive, 0x08 superhuman, 0x10 goals, 0x20 residue.

**Privacy: the rule keys on the attempt, not on attempt order.** The block
carries the owner's memory text. It may reach an attempt only when both of
these hold:

- the provider is local. This means a reliable primary that `from_config`
  declared local from its configured name (`mlx_local` reports `compatible`
  from `get_name`), or a provider with an on-device name.
- the model is a declared local model: `default_model`,
  `agent.s3_local_model`, `agent.mr_mlx_local_model`, or a `model_fallbacks`
  key.

Every other attempt is sent with the block removed. That covers a first
attempt too, because `agent_turn` routes by model name:

- analytical turns go to `gemini-3.1-pro-preview`;
- S3 messages go to the degradation `fallback_model` when no
  `s3_local_model` is set;
- a failed on-device reply is retried on `gemini-3.1-flash-lite`.

It also covers the reliable `model_fallbacks` chain, the extra providers
(gemini), the stream fallback and the degradation `fallback_model`. The check
runs at the call: `agent_turn.c` and `agent_stream.c` decide per call with
`hu_private_context_attempt_is_local`. `reliable.c` and `degradation.c` repeat
it per attempt as defense in depth.

`tests/test_private_context.c` and the
`immersive_context_never_reaches_a_cloud_routed_call` routes in
`tests/test_agent_turn_characterization.c` pin these paths. The
characterization routes cover the three `agent_turn` paths, each paired with a
control proving that the block was composed.

Interim: this logic folds into `feat/thread-context`'s `providers/local_only`
once that branch merges.

## Promotion: SHADOW → LIVE

1. **Shadow for 7 days.** Set `HU_IMMERSIVE_CONTEXT=shadow` in the plist (see
   below), then read the per-turn distribution:

   ```bash
   grep -h '\[HU_IMMERSIVE_CONTEXT shadow\]' ~/.human/logs/service-loop*.log |
     sed -E 's/.*fields_used=([0-9]+) field_mask=(0x[0-9a-f]+) items=([0-9]+) bytes=([0-9]+) truncated=([01]).*/\1 \2 \3 \4 \5/' |
     awk '{n++; f+=$1; b+=$4; t+=$5; if ($4==0) e++} END {printf "turns=%d mean_fields=%.2f mean_bytes=%.0f empty=%.1f%% truncated=%.1f%%\n", n, f/n, b/n, 100*e/n, 100*t/n}'
   ```

   Proceed only if most turns carry at least one field and the truncation rate
   is low. A block that is empty on most turns has nothing to promote. A block
   that is truncated on most turns needs a budget or priority change first.
   Also tally `field_mask` to see which sources actually contribute.

2. **Blind A/B, n=40, length-matched specificity — BLOCKED on tooling.**
   The existing generator cannot measure this gate.
   `scripts/blind_ab/gen_huuman_replies.py` runs `human eval run`, which builds
   its system prompt with `hu_persona_build_prompt_compact` and calls the
   provider directly (`src/app/cli_commands.c`). It never goes through
   `hu_prompt_build_system`, so it never reaches the immersive branch or this
   block. Its two arms would be byte-identical, and an A/B on them would
   "pass" while measuring nothing.

   The promotion A/B needs a harness that generates each reply through
   `hu_agent_turn` (or `hu_agent_turn_stream_v2`), with the contact's real
   state loaded:
   - memory and commitments for that contact;
   - emotional cognition, goals and residue;
   - a local primary declared local, so the block is actually composed.

   It must run both arms on the same contexts with only `HU_IMMERSIVE_CONTEXT`
   differing. Building that harness is out of scope for this PR. Until it
   exists, keep the gate in `shadow`.

   Once it exists, score the arms with the existing pipeline:
   - `scripts/blind_ab/make_rating_sheet.py`;
   - `scripts/blind_ab/score.py --rater human`;
   - `scripts/blind_ab_gate.py`;
   - `scripts/specificity_score.py`, which is length-matched by default.
     Report the delta with its bootstrap CI.

   **LIVE is licensed only if** human detection is no worse than the current
   cycle's baseline and length-matched specificity is flat or up.

3. Flip to `live` and keep watching the same log line, now tagged `live`.

## Setting and rolling back

The gate lives in the launchd plist's `EnvironmentVariables`, which
`scripts/install-human-daemon.sh` preserves across reinstalls:

```bash
/usr/libexec/PlistBuddy -c "Add :EnvironmentVariables:HU_IMMERSIVE_CONTEXT string shadow" \
  ~/Library/LaunchAgents/ai.human.service-loop.plist
```

**Rollback:**

```bash
/usr/libexec/PlistBuddy -c "Delete :EnvironmentVariables:HU_IMMERSIVE_CONTEXT" \
  ~/Library/LaunchAgents/ai.human.service-loop.plist
```

Then reload the service through `scripts/install-human-daemon.sh`. Never `cp`
over the running binary or hand-restart it. Verify with
`scripts/verify-deploy.sh`.

## Regenerating the immersive goldens

The immersive goldens pin the immersive prompt, so they change whenever that
prompt changes. Regenerate them deliberately and review the diff:

```bash
HU_IMMERSIVE_GOLDEN_WRITE=1 ./build/human_tests --suite=AgentTurnCharacterization
```

`immersive_two_turns_local_context_live.golden` must equal
`immersive_two_turns_local.golden` plus exactly the block. The test
`immersive_context_live_golden_is_off_golden_plus_block` asserts this.
