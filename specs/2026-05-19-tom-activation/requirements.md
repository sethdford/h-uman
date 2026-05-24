# Theory-of-Mind Activation — Requirements

## Goal

Activate the existing theory-of-mind (TOM) infrastructure — specifically the dead-code path for detecting user expectations — and extend the belief representation with conversation-local temporality and agent-self-change events, so that the agent can detect and surface mismatches between what the user assumes the agent knows/remembers and what the agent has actually recorded.

Today, the codebase has a working TOM layer for AI-about-user beliefs: `hu_tom_belief_state_t`, post-turn belief recording, context injection into the prompt, and gap detection. BUT:

- `hu_tom_detect_user_expectation()` is defined and has a sophisticated pattern table — and has **zero callers in production code**. The user-expectation extraction loop never fires.
- Beliefs are contact-global; there's no "told in this conversation" vs. "told last month" distinction.
- The agent's own changes (persona delta, LoRA adapter swap, emotional register shift) are not recorded as events that may have invalidated the contact's beliefs.

This spec activates the dead code, adds conversation-local temporality, and wires self-change events into the belief state. **It is NOT a TOM redesign.**

## User stories

- As a user messaging h-uman with phrases like "as you know I prefer X" or "remember when I told you Y", I want the agent to recognize those as expectations it should track, so that when it doesn't actually know X or Y, it surfaces the gap instead of confidently confabulating.
- As a user, I want the agent to distinguish "you told me this five minutes ago in this chat" from "you told me this two months ago," so that recency-sensitive expectations (like "as I just mentioned") are handled correctly.
- As an agent, I want my own behavioral changes (persona delta applied, LoRA adapter rotated, emotional register shifted) to register against the affected contact's belief state, so that later turns can decide whether to acknowledge the shift.
- As a developer, I want activation of existing infrastructure, not a parallel TOM subsystem, so that we keep one belief model — not two diverging ones.

## Acceptance criteria

- [ ] **AC-TOM-1: User-expectation detection fires on every inbound message.** `hu_tom_detect_user_expectation()` is called from the pre-turn context-assembly hook (around `daemon.c:8880`, before the t1b TOM block runs) for every inbound user message. Pinned by a test that injects an inbound message containing "as you know I prefer concise replies" and asserts the resulting expectation row records `topic="concise replies"` with `expected_knowledge_type=HU_TOM_EXPECT_REMEMBERS`.
- [ ] **AC-TOM-2: Recording API + persistence.** A `hu_tom_record_user_expectation()` function exists (or `hu_tom_record_belief()` is extended with an expectation variant) that writes to either a new `tom_user_expectations` SQLite table or to the existing `hu_tom_belief_state_t` with a discriminator field. The choice is a design-phase decision; the requirement is: expectations are durable across daemon restarts and queryable by `(contact_id, topic)`.
- [ ] **AC-TOM-3: Pre-generation prompt includes unmet expectations.** The pre-generation prompt-assembly path adds a "User-Expectations-Unmet" directive section listing each currently-active expectation for the active contact where no corresponding agent belief is recorded. Format and exact placement is a design decision; the contract is: a test that records expectation E on topic T with no matching belief asserts that the assembled prompt contains a substring identifying T and flagging the gap.
- [ ] **AC-TOM-4: Conversation-local belief temporality.** `hu_tom_belief_t` gains `(conversation_id, turn_number)` (or equivalent — a typed "told-context" handle is acceptable) so that beliefs distinguish "recorded in current conversation" from "recorded in prior conversation." Existing global-contact belief queries continue to work unchanged (the new fields default to a sentinel meaning "not conversation-scoped"). Pinned by a test that records the same belief twice in two different conversation contexts and asserts the two rows are separable by the temporal handle.
- [ ] **AC-TOM-5: Agent-self-change events recorded against contact belief states.** When any of the following events occur within a conversation:
  - persona delta applied (`hu_persona_delta_t`)
  - LoRA adapter swapped (call to `hu_mlx_admin_swap_adapter()` for this contact's route)
  - emotional register shifted past calibrated threshold
  
  a new `hu_tom_self_change_event_t` row is appended to a new `tom_self_change_events` table with `(contact_id, event_kind, conversation_id, turn_number, timestamp_utc_ms, magnitude)`. Pinned by three tests, one per event kind.
- [ ] **AC-TOM-6: Gap-detection surfaces self-change-driven invalidation.** The existing `hu_tom_detect_gaps()` is extended to flag beliefs that pre-date a recent self-change event of relevant kind for the same contact, so that the prompt can include a "your earlier inference about this contact may be stale because you changed X" directive. Pinned by a test that records a belief at T1, applies a persona delta at T2, queries gaps at T3, and asserts the staleness gap is reported.

## Non-goals

- **Probabilistic forecasting of future user beliefs.** This spec records observable expectations only.
- **Cross-contact belief sharing.** Each contact's belief state is private.
- **Acting on belief mismatches automatically** (spontaneously clarifying, asking probing questions). This spec records and surfaces in the prompt; downstream prompt-driven generation decides what to do.
- **Redesigning the belief representation.** Activate + extend existing structures; do not introduce a parallel TOM API.
- **NLP improvements to `hu_tom_detect_user_expectation()`.** Use the existing pattern table as-is; tuning the patterns is a separate (eval-driven) task.
- **`HU_TOM_USER_ABOUT_AI` direction.** The header reserves this enum; this spec stays in `HU_TOM_AI_ABOUT_USER`.
- **TOM for non-conversational interfaces** (CLI, dashboard). Conversational channels only.

## Constraints

- C11 `-Wall -Wextra -Wpedantic -Werror`, ASan-clean.
- Activate dead code; do not add a parallel TOM subsystem.
- Backward compatible: existing belief rows continue to work; new temporal fields default to sentinels meaning "no conversation context."
- Test-deterministic; existing daemon-test harness reused.
- Privacy: stored topic strings come from the existing belief-recording flow, which already sanitizes content. No new content-capture introduced.
- Performance: pre-turn expectation detection runs on the inbound message hot path; must be O(message_length × pattern_count) where pattern_count is the existing fixed table (~14 entries).
- Build flag: respect the existing TOM gating (if any); if TOM is unconditional today, the new code is unconditional too.

## Glossary

- **TOM (theory of mind)**: the agent's representation of what the user believes, knows, expects, or doesn't know.
- **Belief**: AI's observation about what the contact knows (e.g., "Seth uses iTerm" with confidence 0.8). Existing.
- **Expectation**: contact's apparent assumption about what the AI knows or remembers (e.g., "as I told you, my preference is X"). Dead code today; activated here.
- **Self-change event**: any agent-side change (persona delta, adapter swap, register shift) that may invalidate prior beliefs for that contact.
- **Staleness gap**: a gap-detection output flagging a belief that pre-dates a relevant self-change event.
