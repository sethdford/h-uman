# iMessage Dermot Humanness Recovery — Requirements

**Date:** 2026-05-27
**Owner:** Seth
**Status:** DRAFT — pending approval before design phase

## Context

On 2026-05-27, replies sent to Dermot (`+447914633409`) were observed to be
non-Seth-like and non-threaded. Root cause investigation found three
independent defects that conspire to produce the bad UX:

1. **Self-RAG abstain substitution** — when the verifier returns
   `HU_SELF_RAG_ABSTAINED` (no memory backs the draft), `world_model_bridge.c`
   discards the LLM's actual draft and substitutes the canned template
   `"I don't have memory backing this. Want to tell me?"` This is what
   Dermot literally received in response to banter like "Going out with a
   bang?!" and "Oh human you need to learn Irish slang."

2. **Router bypasses Seth-voice LoRA** — `~/.human/config.json:71-74` pins
   `reflexive_model`, `conversational_model`, `analytical_model`, and
   `deep_model` all to Gemini variants. The `mlx_local` provider with the
   `seth-lora-v4-repair-…` adapter is loaded but never named by the router,
   so the Seth-voice LoRA is dead code in production.

3. **Threaded-reply tiers are stubs** — both `ax_reply_tier1_cmd_r` (Cmd-R
   via AX) and `ax_reply_tier2_show_menu` (AX Show Menu → Reply…) in
   `src/channels/imessage_reply.c:73-108` are intentional `return false;`
   stubs from C1 with a comment saying "Real AX wiring deferred to
   integration pass." Every threaded reply falls through to Tier 3
   `flat_fallback`, sending the reply as a free-floating new message
   instead of as a native iMessage thread reply.

The three defects are addressed in dependency order so each stage produces
visible improvement Dermot can see.

## User stories

- **As Seth**, I want banter from Dermot (and any other contact) to receive
  a reply that *sounds like a real text I'd send*, so the assistant doesn't
  embarrass me in front of friends and family.
- **As Seth**, I want replies to land as iMessage *threaded replies*
  attached to the parent message, so the conversation visually reads like
  a normal back-and-forth in Messages.app.
- **As Seth**, I want the self-RAG verifier to *continue* protecting me
  from confidently asserting unverified facts (e.g. "yeah, Becton already
  left"), while no longer rewriting *casual chat* into a stiff template.
- **As Seth**, I want the Seth-voice LoRA I trained and proved at 0.856
  fidelity to *actually be the model that serves my replies*, not be dead
  code that's loaded and ignored.
- **As Seth (operator)**, I want telemetry to keep counting abstain outcomes
  accurately so I can spot if the verifier starts mis-firing post-change.

## Acceptance criteria

Each AC is testable in isolation. A reference implementation must prove
each AC empirically — not just by reading.

### Verifier pass-through (Component 1)

- **AC-1**: When `hu_self_rag_verify` returns `HU_SELF_RAG_ABSTAINED`,
  `hu_world_model_bridge_verify_and_update_beliefs` MUST set
  `*out_modified = NULL` and `*out_modified_len = 0` (pass-through). It
  MUST NOT call `hu_self_rag_render_refusal` for the abstain path.
- **AC-2**: When `hu_self_rag_verify` returns `HU_SELF_RAG_HEDGED` or
  any `draft_modified == true` outcome, the existing rewrite-substitution
  behavior MUST be preserved (regression guard).
- **AC-3**: `HU_REFUSAL_POLICY` substitution at `agent_stream.c:2034` and
  any other non-ABSTAINED refusal path MUST be untouched. Policy refusals
  are intentional and separate from this spec.
- **AC-4**: Verifier telemetry (counters that track abstain rate, hedge
  rate, supported rate) MUST continue to fire on every verification
  attempt, including ABSTAINED ones that now pass-through. The COUNTS
  visible to operators MUST match what they were before this change.

### Router routes to Seth-voice LoRA (Component 2)

- **AC-5**: When the daemon is started with `mlx-server.py` healthy and
  `personalization.lora_adapter_path` set, the model_router output for
  `tier=conversational` MUST name an `mlx_local`-served model (not a
  `gemini-*` model). The "model route:" log line emitted at
  `daemon.c:10511` MUST show the mlx_local model identifier.
- **AC-6**: Same as AC-5 for `tier=reflexive`.
- **AC-7**: `tier=analytical` and `tier=deep` MUST continue to route to
  Gemini (cloud-only — local models do not match cloud at hard reasoning;
  this is intentional). Regression test pins this.
- **AC-8**: When `mlx-server.py` is unreachable or unhealthy at turn time,
  the router MUST fall back to the configured Gemini conversational model
  WITHOUT raising an error to the user. The fallback path MUST emit a
  one-shot WARN log naming the gemini fallback model.

### Threaded reply send (Component 3)

- **AC-9**: When `hu_imessage_reply` is invoked with a non-empty
  `parent_msg_guid` on a live macOS machine with Accessibility permission
  granted, the message MUST be sent as a native iMessage threaded reply
  attached to the named parent. The `g_last_tier` MUST be `"cmdR"` or
  `"ax_menu"` (NOT `"flat_fallback"`) for ≥95% of attempts in a 30-attempt
  sample.
- **AC-10**: When AX is unavailable (no permission, screen locked,
  conversation window not focusable), `hu_imessage_reply` MUST log a WARN
  exactly once per daemon process lifetime, fall through to flat_fallback,
  and the daemon MUST NOT enter a retry-storm.

### End-to-end behavior on the Dermot thread (visible UX)

- **AC-11**: For a banter incoming from any iMessage contact (including
  Dermot), the reply h-uman sends MUST NOT contain the substring
  `"I don't have memory backing this"`. (Smoke test against a fresh
  daemon turn with a banter prompt.)
- **AC-12**: For a representative banter prompt routed to the daemon
  with `mlx-server.py` healthy, the reply MUST be generated by an
  mlx_local model (verified by the "model route" log line for that
  turn naming an mlx_local model).
- **AC-13**: For a representative banter prompt sent on a live macOS
  machine, the resulting iMessage in `chat.db` MUST have a non-null
  `thread_originator_guid` pointing at the parent incoming message
  (verifies the reply is a native thread reply, not a flat new message).

## Non-goals

The following are explicitly OUT of scope for this spec. Each gets a
separate plan if needed.

- **Improving the Seth-voice LoRA content quality** (e.g. retraining,
  more iters, different ranks). The existing v4-repair adapter ships
  at 0.856 fidelity per the May-25 eval and is treated as fixed input
  for this spec. M3 mission tracks LoRA quality separately.
- **Native iMessage sticker/tapback/message-effect send.** Read-side
  works; send-side is unrelated to this spec.
- **Group-chat threaded replies.** Dermot is a DM. Group-chat reply
  semantics are subtly different and out of scope.
- **Adding new self-RAG verification modes** (e.g. inline, atomic).
  This spec only changes what happens on `ABSTAINED`. The verifier
  itself is untouched.
- **Cross-channel application of the pass-through behavior beyond
  iMessage.** The fix applies uniformly to any caller of
  `hu_world_model_bridge_verify_and_update_beliefs` — that's a happy
  side effect — but the *spec's success criteria* are scoped to
  iMessage / Dermot.
- **Reflective/factual mode auto-detection** (deciding draft-by-draft
  whether to apply verifier-substitute vs. pass-through). The chosen
  policy is *uniform pass-through for ABSTAINED*; smarter classifiers
  are a follow-up.

## Constraints

- **Language & build:** C11, ASan-clean, compiles with
  `-Wall -Wextra -Wpedantic -Werror`. No new external dependencies.
- **Existing tests pass:** the full `human_tests` suite (11,900+
  tests) MUST be 0 failures / 0 ASan errors after each component.
- **AX guarded:** any new CGEvent/AXUIElement code MUST be guarded by
  `#if defined(__APPLE__) && defined(HU_IMESSAGE_TAPBACK_ENABLED)`
  and MUST NOT execute under `HU_IS_TEST`.
- **LoRA fallback:** when `mlx-server.py` is down, the daemon MUST
  continue serving replies (degraded to Gemini cloud) without crashing
  or stalling. Health check must be observable.
- **Cache discipline:** zero churn to system-prompt structure that
  would invalidate prompt cache. Target cache hit rate stays ≥85%.
- **Telemetry preserved:** `imessage_action.jsonl` schema is unchanged;
  the `tier_used` field will start showing `"cmdR"`/`"ax_menu"` more
  often (currently 100% `"flat_fallback"`), which is the desired
  signal.
- **Live macOS validation:** AC-9, AC-10, AC-12, AC-13 cannot be
  CI-tested — they require a live macOS box. The spec accepts this
  by marking those ACs "manual verification on live daemon" and
  including a reproducible test recipe in the design.
- **Cost neutrality:** Component 2 shifts conversational/reflexive
  turns from cloud Gemini to local MLX. Per-turn cloud cost should
  drop; local CPU/GPU usage rises. Acceptable trade.

## Out-of-band: stop-the-bleeding declined

User explicitly declined a config kill-switch to disable the verifier
substitution while the spec ships. We accept additional bad replies to
Dermot in the meantime as the cost of avoiding surprise side effects
of a kill switch.

## Approval gate

This requirements doc MUST be approved (or amended) before design.md
is written. Per `~/.claude/skills/spec/SKILL.md` Phase 1.
