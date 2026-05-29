---
title: "Eval Registry: Persona Prompt Blocks (Sprint B + A/B/C-loops)"
created: 2026-05-24
status: reference
---

# Eval Registry: Persona Prompt Blocks (Sprint B + A/B/C-loops)

This document is the canonical registry of behavioral contracts pinned
by Sprint B's prompt-block features (`EMOTIONAL CONTEXT:`,
`UPCOMING:`, `WHAT WORKS:`, `IDENTITY:`, `STYLE HINT:`, `VOICE TONE:`).

Per the global CLAUDE.md rule: **"Don't change rules without re-
running /eval."** This document tells future engineers WHICH test
suites pin WHICH contract so a regression can be caught at the unit
level before it ever ships.

Run the full registry:

```bash
./build/human_tests --suite=emotional_context \
                    --suite=anticipatory \
                    --suite=causal_attribution \
                    --suite=identity_continuity \
                    --suite=style_adapter \
                    --suite=audio_emotion \
                    --suite=autoresponder \
                    --suite=lora_export
```

All must pass with **0 ASan errors and 0 failures**. Anything else
means a prompt-block contract regressed.

## Block 1 — `EMOTIONAL CONTEXT:`

**Module:** `src/memory/emotional_context.c` · **Suite:** `emotional_context`

| Contract | Test |
|---|---|
| Tender fact within 30d → surfaces | `test_matching_contact_lexicon_recent_renders` |
| Older than 30d → dropped | `test_older_than_lookback_window_dropped` |
| Effective confidence < 0.4 → dropped | `test_low_confidence_dropped` |
| "lovesick" must NOT match "sick" (word-boundary) | `test_sick_of_work_does_not_trigger` |
| "sickness" must NOT match "sick" | `test_sickness_word_boundary_does_not_match_sick` |
| Multiple matches → most recent wins | `test_multiple_matches_most_recent_wins` |
| Lexicon match across S/V/O fields | implicit in lexicon coverage |
| Custom lookback honored | `test_custom_lookback_honored` |
| `now=0` deterministic in test mode | `test_now_zero_returns_empty_in_test_mode` |

**Anti-regression key:** the word-boundary discipline. A change that
reverts to `strstr` will pass naive tests but break the
`lovesick/sickness` contracts and ship false-positive emotional
context to the LLM.

## Block 2 — `UPCOMING:`

**Module:** `src/memory/anticipatory.c` · **Suite:** `anticipatory`

| Contract | Test |
|---|---|
| Lexicon match + ≤14d → surfaces | `test_matching_contact_lexicon_recent_renders` |
| Older than 14d → dropped | `test_older_than_lookback_dropped` |
| "birthdays" must NOT match "birthday" | `test_birthdays_does_not_match_birthday` |
| Most recent wins | `test_multiple_matches_most_recent_wins` |
| 20-entry lexicon coverage | implicit in lexicon array |

**Anti-regression key:** the 14-day window vs B2's 30-day. Stale
upcoming-event mentions get awkward fast.

## Block 3 — `WHAT WORKS:`

**Module:** `src/memory/causal_attribution.c` · **Suite:** `causal_attribution`

| Contract | Test |
|---|---|
| Only `source_hint=="reaction_ingest"` facts count | `test_non_reaction_facts_ignored` |
| Positive verbs (loves/likes/appreciates/enjoys) → positive_count | `test_positive_verb_counted` |
| Negative verbs (hates/dislikes/resents) → negative_count | `test_negative_verb_counted` |
| Unknown predicate → neutral | `test_unknown_predicate_neutral` |
| Earliest + latest timestamps tracked | `test_earliest_latest_tracked` |
| Render omits "Nd ago" when `now=0` | `test_render_empty_writes_nothing_populated_renders` |

**Anti-regression key:** "dislikes" must NOT trigger "likes" (word-
boundary on predicate-verb match).

## Block 4 — `IDENTITY:`

**Module:** `src/memory/identity_continuity.c` · **Suite:** `identity_continuity`

| Contract | Test |
|---|---|
| Empty graph → no suggestion | `test_suggest_empty_graph_writes_nothing` |
| Handle already in graph → no spurious merge | `test_suggest_handle_already_in_graph_no_merge` |
| First-token name match → renders ONE candidate | `test_suggest_handle_name_token_match_renders` |
| No overlap → no suggestion | `test_suggest_handle_no_overlap_writes_nothing` |
| Multiple candidates → only FIRST surfaces (no flooding) | `test_suggest_emits_only_first_candidate` |

**Anti-regression key:** "no flooding" rule. If a future change loops
and emits per-candidate lines, the prompt fills with noise.

## Block 5 — `STYLE HINT:`

**Module:** `src/persona/style_adapter.c` · **Suite:** `style_adapter`

| Contract | Test |
|---|---|
| Fewer than 3 reactions → UNKNOWN (silent) | `test_fewer_than_min_reactions_unknown` |
| 3+ positive, <5 total → POSITIVE | `test_min_reactions_all_positive_positive` |
| ≥5 positive AND >80% → VERY_POSITIVE | `test_very_positive_threshold` |
| 60% positive → POSITIVE (not VERY) | `test_positive_but_below_very_positive` |
| >50% negative → NEGATIVE | `test_negative_majority_negative` |
| Mixed → NEUTRAL | `test_mixed_neutral_majority_neutral` |
| Integer percent math (no float-equality flakes) | implicit in determinism |

**Anti-regression key:** MIN_REACTIONS=3 floor. Below it the signal
is too noisy; surfacing UNKNOWN-driven hints would degrade prompts.

## Block 6 — `VOICE TONE:`

**Module:** `src/memory/audio_emotion.c` · **Suite:** `audio_emotion`

| Contract | Test |
|---|---|
| <100 wpm → DELIBERATE | `test_slow_pace_deliberate` |
| 100-180 wpm → NEUTRAL | `test_normal_pace_neutral` |
| >180 wpm → ENERGETIC | `test_fast_pace_energetic` |
| Long + <50 wpm → HESITANT (overrides DELIBERATE) | `test_long_low_rate_hesitant_overrides_deliberate` |
| UNKNOWN tone → render returns 0 | `test_render_unknown_writes_nothing_populated_renders` |
| Boundary at exactly 100 wpm | `test_boundary_around_slow_threshold` |
| `format_fact` omits `VOICE TONE:` prefix (fact, not prompt block) | `test_format_fact_populated_omits_prompt_prefix` |

**Anti-regression key:** HESITANT must override DELIBERATE for long
recordings with few words — the signal there is silence, not
deliberate speech.

## Autoresponder — channel-agnostic contract

**Module:** `src/agent/autoresponder.c` · **Suite:** `autoresponder`

The A-loop daemon wire is channel-agnostic — same gate fires for
iMessage, Slack, Discord, Telegram. Pinned by:

| Contract | Test |
|---|---|
| `should_respond` does NOT read channel | `test_should_respond_is_channel_agnostic` |
| `build_prompt` carries channel name through to LLM context | `test_build_prompt_includes_channel_name_when_provided` |
| Allowlist exact-match (NOT prefix) | `test_allowlist_prefix_does_not_match` |
| DND wrapped window (22:00→07:00) | `test_dnd_wrapped_window_both_halves` |
| Sanitize replaces "I am <name>" without "'s assistant" | `test_sanitize_false_user_claim_replaced` |
| Digest aggregator skips malformed log lines | `test_digest_malformed_lines_skipped` |

**Anti-regression key:** the channel-agnostic invariant. If a future
change makes `should_respond` consult channel, an iMessage-only
autoresponder will silently miss Slack/Discord/Telegram traffic.

## LoRA exporter — round-trip contract

**Module:** `src/ml/lora_export.c` · **Suite:** `lora_export`

| Contract | Test |
|---|---|
| JSON escape: quotes/backslashes/newlines/tabs/controls | `test_json_escape_*` (5 tests) |
| DPO shape (with rejected) | `test_render_jsonl_with_rejected_dpo_shape` |
| SFT shape (without rejected) | `test_render_jsonl_without_rejected_sft_shape` |
| Drop rows missing prompt or chosen | `test_render_jsonl_drops_unusable` |
| Build-gate: NOT_SUPPORTED on test/non-SQLite builds | `test_export_returns_not_supported_in_test_build` |

**Anti-regression key:** the "drop unusable" gate. mlx-lm-lora
rejects rows with empty prompt or chosen; the exporter must NOT
write them.

## How to add a new prompt block (the discipline going forward)

When you add a new `XYZ:` prefix to the persona prompt builder:

1. Pick a 1-word UPPER-CASE prefix unique in the codebase
   (`grep -r "PREFIX:" src/memory src/persona`)
2. Implement as a pure function in its own module + header
3. Add ≥6 contract tests including word-boundary safety + NULL/empty
4. Wire into `hu_personal_model_build_prompt_with_overlay`'s
   contact-walk loop
5. **Add a section to this document** with the contract → test
   table
6. Re-run the full registry above to confirm no cross-block
   regression

This discipline is what keeps the 6 prompt blocks composable: each
has a sharp prefix, a sharp test suite, and a documented contract.
Adding the 7th block doesn't risk breaking the 1st.

## Related

- `~/.claude/CLAUDE.md` "Measure before tuning" — global rule that
  inspired this doc
- `.claude/rules/substring-classifier-pitfalls.md` — the word-
  boundary discipline these tests pin
- `docs/guides/m3-bridge-runbook.md` — what the WHAT WORKS / STYLE
  HINT signals eventually become (LoRA training data)
