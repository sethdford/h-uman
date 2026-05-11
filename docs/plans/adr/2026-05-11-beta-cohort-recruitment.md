---
title: "ADR — Beta cohort recruitment: friends-and-family first, gated public signup later"
created: 2026-05-11
status: accepted
deciders: product, engineering
parent: ../2026-05-10-sota-roadmap-6mo.md
related:
  - ../2026-05-10-sota-roadmap-6mo.md
  - ../../standards/security/data-privacy.md
  - ../../standards/security/threat-model.md
---

# ADR: Beta cohort recruitment — friends-and-family first, gated public signup at C6

## Context

The SOTA roadmap (Phase C5.3, C6.1) requires a closed-beta cohort of 20+ users at month 5 and 100 DAU at month 6. Recruitment posture has direct privacy and quality implications:

- **Wider cohort earlier** → noisier signal, more support burden, higher PII exposure
- **Smaller / known cohort** → quieter signal but biased toward friend-positivity, slower learning

The product thesis ("privacy by architecture, not by settings") raises the bar: the recruitment flow must itself respect the thesis. Public-form signup without legal review is incompatible with that.

## Decision

**Two-phase recruitment.**

Phase 1 — friends-and-family (Months 4–5)

- Invite-only via direct outreach. Target: 20–30 users.
- Each invitee signs an explicit consent form covering:
  - What local data is collected (event log, friction events, sentiment signals — already local-only per C5.1).
  - What is shared back to engineering (only with explicit opt-in; default is **no data sharing**).
  - Retention measurement methodology.
  - Right to delete; binary uninstall + `~/.human/` removal as the canonical secure-erase path.
- Consent form lives at `docs/legal/beta-consent.md` (to be created at C5 kickoff) and links to `docs/standards/security/data-privacy.md`.
- Cohort tracking in a private Linear project — no PII in issue text.
- **Bias acknowledgement:** friends-and-family will skew positive on satisfaction metrics. Retention is the gate, not satisfaction. Day-7 retention is harder to fake than satisfaction.

Phase 2 — gated public signup (Month 6, C6.1)

- Public landing page with a waitlist form, NOT an open-install path.
- Waitlist invites batched (10–20 per week) to keep support load bounded.
- Same consent form, plus an explicit privacy disclosure on the landing page itself.
- Legal review required before the waitlist form goes live. Review covers:
  - Consent language compliance (CCPA, GDPR-equivalent best practices).
  - Data residency claims (local-only).
  - Deletion / portability claims.
- After legal sign-off, ship.

Operational rules for both phases:

- **No support PII in code or repo.** All user-facing identifiers are local-only.
- **Telemetry stays local.** Phase C5.1 already requires this; cohort recruitment doesn't relax it.
- **Friction-event reporting** is opt-in and redacts identifiers before any cross-machine transit.
- **Disable / pause** must be a one-command operation (`human beta opt-out`) that survives a daemon restart.

## Consequences

- **Positive:** privacy posture intact; cohort behavior is observable through retention (objective) rather than survey (subjective and biased); slower scale is a feature, not a bug — better signal per user.
- **Negative:** retention sample size at month 6 is bounded by waitlist throughput. The 100 DAU target may slip into month 7 if waitlist throughput is conservative. Mitigation: phase C6.4 retention measurement runs even at < 100 DAU; we publish whatever number we hit honestly.
- **Legal:** legal review is on the critical path for C6.1. Schedule it at the start of month 5, not month 6.
- **Documented in:** `docs/legal/beta-consent.md` (TBD), landing page copy (TBD), `human beta` CLI subcommand (to be added).

## Status

Accepted. The friends-and-family list is bootstrapped at C5 kickoff; legal review request goes out at start of month 5. If legal review slips past month 5.5, Phase C6.1 reschedules to month 7 and the 100 DAU target is communicated as a stretch goal in the program report.
