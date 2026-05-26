# M3 Dispatch Unification — Requirements

## Why this spec exists

The Jordan incident (2026-05-26: daemon sent "tbh morning. you awake yet?"
to a real contact) exposed that h-uman has **two parallel proactive
dispatch paths** that don't share the same composer:

1. **`init_proposer.tick_with_provider`** ([src/agent/init_proposer.c:421](../../src/agent/init_proposer.c)) —
   the M3 deliverable. Asks "given everything I know about Seth's life
   right now, should I bring something up?" Runs governor gates, calls
   an analytical-tier LLM with a propose-or-skip prompt, parses a JSON
   decision, returns `HU_INIT_RESULT_FIRED` with a draft.

2. **`daemon_proactive` composition + send** ([src/daemon.c:~2086](../../src/daemon.c)) —
   the older path. For each contact with a proactive schedule, builds
   a rich prompt via `hu_daemon_proactive_prompt_for_contact` (memory
   callbacks, weather, calendar, feeds), calls `hu_agent_turn`,
   validator-chains the response, sends.

Sprint 41 (2026-05-26) **unified the gate stack** via
`hu_init_proposer_governor_check_only` — both paths now honor the same
quiet-hours, daily-budget, and per-contact-recency gates. But the
**composition** remains bifurcated: init_proposer has its own
propose-or-skip prompt; daemon_proactive has its own rich-context
prompt. They cannot share confidence thresholds, draft-quality gates,
or DPO pair capture without per-path duplication.

This spec is the architectural follow-up: make init_proposer the
**single** proactive composer.

## Goals

- **G1.** A single source-code path composes every proactive outbound
  send. `hu_init_proposer_tick_with_provider` becomes that path.
- **G2.** Init_proposer's prompt builder accepts the rich context
  daemon_proactive currently constructs (memory recall, weather,
  calendar, feeds, persona overlay, autoresponder context) so quality
  doesn't regress.
- **G3.** Init_proposer's confidence threshold + draft-validation
  pipeline (G1–G9 detectors + persona_voice + identity-anchor retry)
  governs every proactive send. No outbound bypasses these.
- **G4.** Daemon_proactive becomes a thin **scheduler** that selects
  candidate contacts and schedules ticks. It no longer composes or
  sends directly.
- **G5.** DPO negative-pair capture (Sprint 41 follow-up #3) covers
  100% of proactive rejections. No path bypasses the logger.

## Non-goals

- Reactive (inbound-triggered) path stays as-is. This spec is about
  **proactive** unification only.
- Voice channel proactive sends — out of scope; voice uses its own
  duplex turn-taking FSM.
- Persona retraining or LoRA-pipeline changes. The unification gives
  the DPO logger a single funnel; what the trainer does with it is
  a separate sprint.

## Acceptance criteria (AC)

**AC-1.** Every proactive outbound send in production goes through
`hu_init_proposer_tick_with_provider`. Verified by:
- Grep: `vtable->send` calls reached via daemon proactive paths
  appear ONLY downstream of init_proposer.tick_with_provider.
- A new daemon-integration test
  (`tests/test_proactive_dispatch_unified.c`) wires a mock provider
  + mock channel and asserts every proactive send was preceded by an
  init_proposer FIRED result.

**AC-2.** Init_proposer's prompt builder accepts a "rich context
bundle" containing the same fields daemon_proactive currently injects:
- Memory callbacks (recall + degradation + protective filter)
- Weather (when configured)
- Calendar (when configured)
- Feeds (per-contact-scoped, post-Sprint 59 Phase C)
- Persona overlay for the channel
- Autoresponder hint when in quiet-window-adjacent

Verified by: a test that hands a populated `hu_init_context_bundle_t`
with all 6 sources to `hu_init_proposer_build_propose_prompt` and
confirms the rendered prompt contains substrings from each.

**AC-3.** The new path runs the SAME validator chain as the reactive
path:
`hu_validators_build_default_outbound_chain` is the single source of
truth for response shaping. Verified by:
- Source audit (the chain-builder is called exactly twice: reactive
  agent_turn, init_proposer's send wrap).
- A regression test that injects a G9-tripping draft through
  init_proposer's send path and asserts it is rejected + retried +
  captured as a DPO negative.

**AC-4.** Confidence threshold + draft text from init_proposer flow
to the channel send. Verified by:
- Tick test with cfg.confidence_threshold=0.85 and a parsed decision
  with confidence=0.80 → no send (LOW_CONFIDENCE).
- Same with confidence=0.90 → send fires via the daemon's
  channel-vtable adapter, with the draft as the message text.

**AC-5.** Daemon_proactive's per-contact scheduler still works. It:
- Iterates contacts per their `proactive_channel` schedule.
- Resolves the channel + target route via
  `hu_daemon_proactive_apply_route`.
- For each candidate, invokes `hu_init_proposer_tick_with_provider`
  with the rich context bundle for that contact.
- On FIRED, sends the draft via the channel vtable + records the
  send via `hu_proactive_throttle_record_send` + `hu_governor_record_sent`.
- On any other result, logs the gate reason and moves on.

Verified by: integration test that walks 3 contacts (one with budget,
one in quiet hours, one with recent inbound) through one tick cycle
and asserts only the eligible one actually sends.

**AC-6.** Backwards compatibility for operators: existing config keys
in the `initiative.*` and `proactive.*` namespaces continue to work.
No config migration required for the unification to land.

**AC-7.** Zero regression in the 12,482-test suite at sprint-start.

## Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Init_proposer's confidence-threshold defaults reject legitimate proactive sends post-unification | High | A/B period: ship the unified path behind `cfg.proactive.use_unified_dispatch` (default false) for one week; observe FIRED rate vs reactive baseline; only flip default once rates match. |
| Rich context bundle exceeds the propose-or-skip prompt's token budget | Medium | Trim/summarize per-source bytes in `hu_init_proposer_assemble_context` using prompt_budget machinery. |
| Memory-callback safety predicate (`hu_daemon_callback_content_is_safe`) doesn't get applied | High | Move the predicate INTO `hu_init_proposer_assemble_context` so it fires regardless of caller. Pin with a test that constructs an unsafe memory entry + asserts it's filtered. |
| DPO logger writes duplicate rows when both reactive and proactive paths hit it for the same agent turn | Low | Add `_path:"proactive"` vs `_path:"reactive"` field to the JSONL so the trainer can dedupe at ingest. |
| The "scheduler-only" daemon_proactive breaks downstream callers that expected the old behavior | Medium | Audit `hu_daemon_proactive_*` exports BEFORE refactoring; any caller outside the daemon's own scheduler block is a refactor blocker until that caller migrates. |

## Out of scope (explicitly)

- The follow-up watcher's outbound path. It uses init_proposer-style
  gating already (Sprint 41 budget+quiet parity) but composes via a
  dedicated autoresponder prompt. Separate concern.
- iMessage tier-1/2/3 reply paths (Sprint 59 Phase C). Reactive-only.
- The CLI `human` invocation path (no proactive dispatch surface there).
