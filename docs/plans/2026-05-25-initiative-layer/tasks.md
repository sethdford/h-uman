# Initiative Layer — Tasks

**Status:** Draft (2026-05-25)
**Reads:** `requirements.md`, `design.md`
**Blocked on:** `2026-05-24-reactive-imessage-recovery/` reaching AC-1 (reactive replies work end-to-end). Don't ship initiative against a system that can't yet reply.
**Companion:** `2026-05-25-proactive-ext-completion.md` (F30/F31/F129 affordances). Initiative can ship without all three, but at least one should be live for the proposer to have meaningful input.

## Sequencing

```
[BLOCKED until reactive works]
        ↓
T0 — Resolve open design decisions with Seth
        ↓
T1 — Author hu_init_proposer skeleton (no LLM, just governor + SKIP)
        ↓
T2 — Wire context bundle assembly
        ↓
T3 — Add LLM call + decision gate
        ↓
T4 — Wire to cron scheduler + iMessage channel
        ↓
T5 — Ship behind config flag (disabled by default), gather one week of dry-run logs
        ↓
T6 — Tune confidence threshold + cadence based on dry-run data
        ↓
T7 — Enable for Seth, monitor for one week
        ↓
T8 — Slice 2: capture acceptance signal (did Seth reply? edit? ignore?)
```

## T0: Resolve design decisions
**AC:** None (input gathering)
**Effort:** XS (15 min conversation)
**Done when:** DECISION-1 (cadence), DECISION-2 (confidence threshold + gate shape), DECISION-3 (model tier), DECISION-4 (awake-hours source) from `design.md` have explicit chosen values, captured in this file or as separate ADR notes.

## T1: Skeleton subsystem with governor-only logic
**AC:** AC-1, AC-6, AC-7
**Files:** `src/agent/init_proposer.c`, `include/human/agent/init_proposer.h`, registered with cron scheduler in `src/main.c`
**Effort:** M (3-5 hrs)
**Done when:**
- `hu_init_proposer_create` / `hu_init_proposer_tick` / `hu_init_proposer_deinit` exist and link clean
- Tick fires per `tick_interval_sec` during autoresponder awake hours
- Tick performs governor pre-checks (quiet, daily-budget, per-contact-recency)
- Tick always returns SKIP (no LLM call yet)
- Every tick produces the `[init_proposer] tick id=N phase=... result=SKIP|GATED` log line
- Config flag `initiative.enabled` in `~/.human/config.json` gates the whole subsystem
- Test: `tests/test_init_proposer.c` covers (a) disabled config = no ticks, (b) quiet hours = GATED, (c) budget exhausted = GATED, (d) all-clear = SKIP

## T2: Context bundle assembly
**AC:** AC-2
**Files:** `src/agent/init_proposer.c` (add `assemble_context()`)
**Effort:** M (2-4 hrs)
**Done when:**
- The proposer assembles the SAME 25+ context fields as `hu_prompt_build_system`. Recommend extracting a shared helper from `agent_turn.c:3752+` rather than duplicating call sites.
- Also pulls: F30 curiosity topics (when available), F31 callback opportunities (when available), F129 disclosures (when available), last N inbound/outbound messages from Seth.
- Per-tick log line names byte counts per source: `[init_proposer] tick id=N phase=context_assembly fields=28 bytes_total=18742 f30=1 f31=2 ...`
- Test: a fake-context fixture exercises every field with non-empty content, verifies bundle contains all expected sections.

## T3: LLM call + decision gate
**AC:** AC-3, AC-4
**Files:** `src/agent/init_proposer.c` (add `call_proposer_llm()`, `parse_proposal()`)
**Effort:** M (2-4 hrs)
**Done when:**
- The proposer calls the configured `propose_model` (DECISION-3) with the assembled context + a propose-or-skip system prompt.
- LLM is required to return STRICT JSON: `{action, confidence, message, source_field, reason}`. If JSON parse fails, treat as SKIP and log a parse-error warning.
- Decision gate: if `action != propose` OR `confidence < threshold` (DECISION-2), result is SKIP.
- If pass: handoff to existing `hu_proactive_throttle_dedup_already_today` / per-contact recency for final check.
- Tick latency budget enforced: SKIP path completes <500ms (AC-4); LLM path completes <5s or returns ERROR.
- Test: fake provider returns canned proposals (high-conf propose, low-conf propose, skip, malformed JSON) — verify gate behavior in each case.

## T4: Wire to cron scheduler + iMessage channel
**AC:** AC-5
**Files:** `src/main.c` (register cron job), `src/agent/init_proposer.c` (send via channel)
**Effort:** S (1-2 hrs)
**Done when:**
- A new cron job named `initiative_proposer_tick` registered at `tick_interval_sec` cadence.
- When a proposal passes all gates, the message is sent via `agent->channels[imessage]` using the existing send path — same AX bridge, same response_guard, same delivery semantics as a normal outbound.
- The send is attributed to the same `default_target` as cron check-ins (or to a separately-configured `initiative.target` field).
- Test (manual): in a dev config, force a propose result via a debug-tier LLM mock, observe that Seth's phone receives the message.

## T5: Ship behind dry-run flag for one week
**AC:** none — observation
**Files:** Add `initiative.dry_run: true` config field. When set, the proposer runs the full pipeline INCLUDING the LLM call, but does NOT actually send the message. Logs the would-have-been message at INFO level.
**Effort:** S (1 hr)
**Done when:**
- One week's worth of dry-run logs accumulated.
- Spot-check: do the would-have-been-sent messages look like things Seth would appreciate? Or are they generic / spammy / wrong?
- Hard data: how many ticks per day, SKIP rate, LLM cost per day, confidence distribution.

## T6: Tune thresholds based on dry-run data
**AC:** AC-4 quantified
**Files:** Update DECISION-1 (cadence) and DECISION-2 (confidence threshold) based on observed data.
**Effort:** S (~1 hr of analysis + config tweak)
**Done when:**
- If SKIP rate < 90%, raise confidence threshold OR slow cadence.
- If SKIP rate > 98%, lower threshold (otherwise the system is functionally off).
- Document the chosen values + rationale in this directory.

## T7: Enable for Seth
**AC:** AC-1 through AC-7 verified live
**Files:** `~/.human/config.json::initiative.enabled = true`, `dry_run = false`
**Effort:** S (config + restart)
**Done when:**
- Seth has been receiving real initiative-proposed messages for one week without:
  (a) feeling spammed
  (b) receiving messages at wrong times
  (c) disabling the system
- Daemon log shows healthy tick cadence, sensible SKIP/PROPOSE distribution, no error spikes.

## T8: Capture acceptance signal (slice 2)
**AC:** Outside this spec, deferred
**Effort:** L (separate sprint)
**Done when:** A new spec lands at `docs/plans/2026-MM-DD-initiative-learning/` covering:
- Did Seth reply to the proposed message? (If yes within 24hr → +signal; if "stop" or ignored → −signal)
- Did Seth EDIT the proposed message before "sending it" (in some hypothetical assisted-send mode)? Quality signal.
- Connect to the DPO pair collection pipeline (once the judge audit's fixes are in — see `audit-lora-training-judge.md`).

---

## Out of this spec

- **Multi-user initiative** (Seth-only for v1)
- **Cross-channel** (iMessage only for v1)
- **Initiative-to-others on Seth's behalf** (different product)
- **Voice/image/video initiative** (text only)
- **Adaptive cadence based on Seth's mood/activity** — future tuning, not v1
