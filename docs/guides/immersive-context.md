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
Production therefore never sends commitments, episodic replay, emotional state,
presence, proactive or superhuman insight, conversation goals, or residue,
although the turn computes all of them.

`HU_IMMERSIVE_CONTEXT` adds one budgeted block, `## What you know right now`,
to the immersive prompt. It sits ahead of the trimmable middle sections, so the
positional cap cannot cut it. The composer is `src/agent/turn/immersive_context.c`.
It fills the block under a hard 1,536-byte budget, in this priority order:

| # | Lead line in the prompt | Source field | Items kept |
|---|---|---|---|
| 1 | "Still open between you two:" | `commitment_context` | up to 3 |
| 2 | "Something that has worked with them before:" | `episodic_replay` | 1 |
| 3 | "How they seem right now:" | `emotional_context`, then `presence_context` | 2, then 1 |
| 4 | "On your mind about them:" | `proactive_context`, then `superhuman_context` | 1 each |
| 5 | "Where you hope this conversation goes:" | `conv_goals_context`, then `residue_carryover` | 2, then 1 |

Each field is the existing builder's rendered output. The composer drops the
builder's own headings, `[No …]` placeholders and markdown emphasis. It keeps
or drops whole items, so an item is never cut mid-sentence. Empty fields add
nothing, and no items at all means no block.

`HU_IMMERSIVE_HUMANNESS` (callbacks, curiosity, absence and opinions) is a
separate gate and a separate measured decision. It is unchanged here. The
budget leaves it room: the block is at most 1.5 KB of the 16 KB prompt budget.

## Gate semantics

The gate is parsed by `hu_gate_mode_from_env`. The default is `off`.

| Value | Behaviour |
|---|---|
| `off` (default) | No work, and the prompt is byte-identical to before. This is pinned by `tests/fixtures/immersive_prompt_golden/immersive_*.golden`. |
| `shadow` | Composes the block and logs one line per turn. The sent prompt is unchanged. The log line is `[HU_IMMERSIVE_CONTEXT shadow] fields_used=<n> field_mask=0x<hex> items=<n> bytes=<n> truncated=<0/1>`. It carries counts, sizes and a field bitmask only, never the text. |
| `live` | Appends the block and logs the same line, tagged `live`. |

The `field_mask` bits run in priority order: 0x01 commitment, 0x02 episodic,
0x04 emotional, 0x08 presence, 0x10 proactive, 0x20 superhuman, 0x40 goals,
0x80 residue.

**Privacy.** The block carries the owner's memory text. It reaches only the
primary attempt: the reliable wrapper's inner provider with the caller's model,
which in production is local mlx. Every other attempt gets a copy with the
block removed. That covers the `model_fallbacks` chain, the extra providers
such as gemini, the streaming fallback, and the degradation `fallback_model`.
The helper is `include/human/providers/private_context.h`, and
`tests/test_private_context.c` pins all of these paths. If the configured
primary provider is itself a cloud provider, the block goes to it. Keep the
gate `off` on such a configuration.

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

2. **Blind A/B, n=40, length-matched specificity.** Generate the candidate's
   replies with `HU_IMMERSIVE_CONTEXT=live` on a candidate server, never on
   :8741 or :8743 while those are owned. Then run the existing pipeline:
   - `scripts/blind_ab/export_seth_triples.py`, then
     `scripts/blind_ab/gen_huuman_replies.py` (contexts and both replies);
   - `scripts/blind_ab/make_rating_sheet.py` and
     `scripts/blind_ab/score.py --rater human`;
   - `scripts/blind_ab_gate.py`, the promotion gate;
   - `scripts/specificity_score.py`, length-matched, which is the default.
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

`immersive_commitment_context_live.golden` must equal
`immersive_commitment.golden` plus exactly the block. The test
`immersive_context_live_golden_is_off_golden_plus_block` asserts this.
