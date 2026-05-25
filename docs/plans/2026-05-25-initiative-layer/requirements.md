# Initiative Layer — Requirements

**Status:** Draft (2026-05-25)
**Owner:** TBD
**Motivation:** During the 2026-05-25 "why isn't h-uman human-like yet" audit, the gap most cleanly attributable to architecture (not training data, not model quality) was the absence of an **initiative layer** — h-uman never proposes outreach you didn't first prompt. The perception layer (25+ injected context fields), the response layer (director→agent_turn→AX), and the cron-scheduled proactive check-ins exist. What's missing is the loop that asks "given everything I know about Seth's life right now, should I bring something up?"

## Goals

1. **Periodic self-initiated outreach.** A subsystem ticks during awake hours, examines the full context (recent conversations, calendar, feeds, commitments, relationship hygiene), and decides whether to propose a message to Seth — even if no inbound event triggered it.

2. **Coherent with existing affordances.** The F30 (spontaneous curiosity), F31 (callback opportunities), F129 (disclosure) compute layers documented in `2026-05-25-proactive-ext-completion.md` are the INPUTS this scheduler consumes. The cron-based proactive check-ins to family contacts continue as-is.

3. **Restrained by design.** It is FAR more important that h-uman not become spammy than that it be chatty. A wrong proposal at 10:14am during a meeting is worse than a right proposal that never fires. The governor's job is to err on the side of silence.

4. **Observable.** When the scheduler decides to fire, the daemon logs WHY (which context fields tipped the decision, what model verdict was returned, what governor checks passed). When it decides NOT to fire, the daemon logs the dominant reason in one line. No silent gating.

## Non-Goals

- **Replacing the cron-based proactive check-ins.** Those serve a different purpose (relationship maintenance with family). The initiative layer is about Seth-facing outreach.
- **Adding new context affordances.** F30/F31/F129 ARE the new affordances; their data-ingest layer is the responsibility of the `2026-05-25-proactive-ext-completion.md` plan. This spec consumes their output.
- **Fixing the reactive-reply or training pipeline bugs.** Those have their own specs (`2026-05-24-reactive-imessage-recovery/`, the LoRA judge audit in this directory).
- **Inventing initiative for OTHER contacts.** Outreach to Mindy/Betty/Annie stays cron-driven for now. The initiative layer's first user is Seth.

## Acceptance Criteria

**AC-1: Scheduler ticks during awake hours.**
A new subsystem (call it `hu_init_proposer`) runs on a configurable cadence during awake hours (per autoresponder.json quiet hours). Each tick is logged with `tick_id`, `awake_window_ok` (bool), and `governor_budget_remaining`. Default cadence and quiet-hours behavior are design decisions noted in design.md.

**AC-2: Proposer consumes the rich context.**
On each tick where governor budget allows, the proposer assembles the SAME 25+ context fields the agent_turn prompt builder uses, PLUS the F30/F31/F129 affordances (once they're wired by `2026-05-25-proactive-ext-completion.md`), and sends them to the analytical-tier LLM with a "propose-or-skip" prompt. Verified by capturing one tick's prompt and confirming all expected fields are present.

**AC-3: Proposer respects an explicit "should I interrupt" governor.**
Before sending any proposed message, the governor checks: quiet hours, daily budget cap (from `hu_proactive_budget`), per-contact recency (don't text Seth if he texted h-uman <10 min ago), and a "high-confidence-only" gate (the LLM must return a structured proposal with confidence >= threshold, not free-text). The governor's verdict is logged per-tick.

**AC-4: SKIP is the default outcome and is fast.**
For 9 out of 10 ticks (target — tune via Seth's actual usage), the proposer returns SKIP and no message is sent. SKIP ticks should complete in <500ms and not incur significant LLM cost. Verified by measuring tick-to-decision time and per-tick token spend over a week.

**AC-5: Non-SKIP proposals are delivered via existing channels.**
When the proposer returns a non-SKIP proposal, the message routes through the existing iMessage channel path (AX bridge, response_guard, etc.) — no new transport. Verified by Seth receiving an h-uman-initiated text on his phone within 60s of a tick deciding to fire.

**AC-6: Loud failure on silent gating.**
If any governor check disables the proposer entirely (e.g., budget exhausted for the day, quiet hours always-on), the daemon logs ONE warning at WARN level naming WHICH check disabled it and HOW to re-enable. Following the project's `~/.claude/rules/silent-config-gated-subsystems.md` discipline.

**AC-7: Reversible kill switch.**
A single config field (`initiative.enabled: false` in `~/.human/config.json`) disables the scheduler entirely. Daemon logs the disabled state at startup. Restart-free if SIGHUP-driven config reload is wired (separately tracked).

## Out of Scope (Explicit)

- **Multi-user initiative.** Seth-only for v1. Multi-user is a v2 concern that requires per-user context isolation.
- **Cross-channel initiative.** iMessage only for v1. Telegram/Discord/Slack initiative is a slice for later.
- **Initiative on behalf of Seth to OTHER people** (e.g., "h-uman drafts a text to Mindy and asks Seth to approve"). That's a different product shape (assistant-driven outreach) and deserves its own spec.
- **Learning from initiative outcomes** (did Seth reply? did he like the proposal?). Outcome capture for slice 2 — first ship the proposer, then close the learning loop.
- **Voice / image / video initiative.** Text only for v1.

## Risks

- **R-1: Spam aversion failure.** A wrong proposal lands during a meeting → Seth disables the whole system. Mitigation: AC-3's governor + AC-4's SKIP-default + AC-7's kill switch. Plus: start with VERY conservative confidence threshold (e.g., 0.85) and tune down only after Seth confirms the proposals are wanted.
- **R-2: Cost explosion.** Analytical-tier LLM on every tick adds up. Mitigation: AC-4's SKIP path uses reflexive tier; only proposals worthy of consideration go to analytical. Plus: daily token budget cap.
- **R-3: Context staleness.** F30/F31/F129 affordances aren't wired yet per `2026-05-25-proactive-ext-completion.md`. Without them, the proposer has thin signal. Mitigation: this spec depends on that one. Don't ship initiative without at least one of the three affordances live.
- **R-4: Premature optimization of "human-ness."** Spending engineering on initiative before reactive replies are reliable is misordered. Mitigation: this spec is BLOCKED on reactive recovery (`2026-05-24-reactive-imessage-recovery/`) reaching at least AC-1 — h-uman must reliably reply before it should reliably propose.
