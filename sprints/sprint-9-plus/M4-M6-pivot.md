# M4 / M6 Pivot Sketch — Post-Sprint-8

**Status:** Sketch (Sprint 9+ candidate; not yet planned)
**Context:** Sprint 7 advanced M2 (Personal Model) and M3 (Private Learning).
Sprint 8 closes the M3 loop honestly. M4 (Ship to Users) hasn't moved since the
PCTT effort; M6 (Channel Focus) is still 31 channels of roughly equivalent depth.

This file sketches what M4 / M6 sprints would look like **after Sprint 8's
honest-loop work lands**. The order matters: Sprint 8 must close the loop
before we can ship it to users.

---

## M4 — Ship to Users (target: 100 DAU with 30% day-7 retention)

### Where we are honestly

The current onboarding path (`human onboard` from US-7.x precedent) is
**plausibly first-run-ready but unverified end-to-end on a fresh machine**.
The honest test: clone the repo on a brand-new Mac, run `human onboard`,
have a chat — does it produce something a non-developer would keep using
for a week?

We don't know. PCTT was the last time anyone ran this fresh-machine flow,
and PCTT's success criterion was "the wizard runs," not "the user stays."

### M4 sprint candidates

#### M4.S1 — Fresh-machine smoke (1 week, 5 stories)

**Goal:** Prove the install-to-first-chat flow on a clean Mac without
developer assumptions.

| Story | What | Why |
|---|---|---|
| M4.1 | Document the exact install steps in a top-level INSTALL.md | Reproduce-ability across users |
| M4.2 | `human onboard` runs through happy path on macOS without xcode | Most operators won't have xcode |
| M4.3 | `human onboard` detects missing GGUF/MLX weights and offers download | First-run download dialog |
| M4.4 | Default provider config that works without API keys | Provider fallback to local-only |
| M4.5 | First-chat smoke: agent responds in <5s on fresh install | TTFT budget |

Success: a freshly-cloned repo + `human onboard` + 1 chat in <30 min wall-clock
on a fresh Mac that has never run h-uman before.

#### M4.S2 — Local-inference parity (2-3 weeks)

**Prereq:** Sprint 8's US-8.2 (NLL backend) lands first.

The digital-twin story only holds if production inference uses the
LoRA'd Gemma. Today most chat goes through Vertex / Anthropic / OpenAI;
the local llamacpp provider is technically there but operationally untested.

| Story | What | Why |
|---|---|---|
| M4.S2.1 | Default `provider: llamacpp` in fresh config | Honest local-first |
| M4.S2.2 | Bridge B.1 fully wired — MLX provider for inference | Apple Silicon path |
| M4.S2.3 | Speculative decode via aligned draft (E2B → 31B) | SOTA roadmap N5 (≥35 tok/s) |
| M4.S2.4 | Personalization adapter auto-loaded from W14 latest | Closes Sprint 8 loop in production |
| M4.S2.5 | Doctor + status surface the active provider/adapter to user | "Yes I'm chatting with my own LoRA" |

Success: opening a chat on iMessage with a fresh persona + 100 corrections
mined → the response is measurably more Seth-like than the cloud baseline,
and the user can SEE this via dashboard.

#### M4.S3 — User retention loop (3-4 weeks; only after S1+S2)

**Prereq:** S2 must show real persona fidelity lift before this is meaningful.

| Story | What | Why |
|---|---|---|
| M4.S3.1 | Weekly retention email-or-notification surfacing what the agent learned | Visibility = trust |
| M4.S3.2 | "Why did the agent say that?" diagnostic surface | Trust calibration |
| M4.S3.3 | Easy correction loop (long-press a message → mark as "not me") | High-quality preference signal |
| M4.S3.4 | Persona snapshot history with rollback UI | Safety net for adapter regressions |

Success: 30% day-7 retention on the 100-user cohort.

### M4 honest blockers

- **Provider cost** — local Gemma inference costs $0 marginal; cloud is $X/turn. Today most chat goes cloud. Switching to local-first changes the cost model AND the latency budget.
- **First-run weights download** — Gemma-4-E4B is ~3GB, Gemma-4-31B is ~17GB. The "5-minute install" promise breaks if first chat is gated on a 30-min download.
- **Battery / heat on user devices** — local inference on M-series is fast but hot. Continuous chat at 35 tok/s pegs the GPU. Need power-aware throttling.

---

## M6 — Channel Focus (Tier-1: Telegram, iMessage, Slack, Discord)

### Where we are honestly

31 channels exist in `src/channels/`. Persona overlays (channel-specific
formality/length/emoji controls) exist but are not measurably better on
ANY channel than the cloud baseline. The "Telegram-casual vs Slack-formal"
story is asserted in design but not yet **proven** in chat fidelity.

### M6 sprint candidates

#### M6.S1 — Tier-1 channel persona-fidelity baselines (1 sprint)

**Goal:** Establish quantitative baselines for the 4 Tier-1 channels so we
can target the gap.

| Story | What | Why |
|---|---|---|
| M6.1 | Per-channel fidelity dashboard (extends the Sprint 7 fidelity tile) | Visibility |
| M6.2 | Per-channel held-out judgment PPL (extends US-7.6's NLL backend) | Quantitative gap |
| M6.3 | A/B compare: persona-overlay ON vs OFF per channel, score the lift | Validate the overlay machinery actually works |
| M6.4 | Identify the weakest channel and document why | Sprint 9 target |

Success: a dashboard tile per channel with current fidelity score; a
follow-up plan for the worst-performing one.

#### M6.S2 — MoLoRA channel-specific adapters (1-2 sprints; uses Sprint 7 US-7.8)

**Prereq:** Sprint 7 US-7.8 static router shipped. Phase 2 of Init #02.

| Story | What | Why |
|---|---|---|
| M6.S2.1 | Train per-channel LoRAs on per-channel subsets of chat.db | Specialized adapters |
| M6.S2.2 | Wire MoLoRA static router to load channel adapters at chat time | Composition with US-7.7 best-of-N |
| M6.S2.3 | Measure persona-fidelity per channel with channel-adapter ON vs base adapter only | Prove the per-channel adapter actually helps |
| M6.S2.4 | If proven: ship per-channel adapter as default for Tier-1 | Operationalize the win |

Success: each Tier-1 channel scores at least 10% higher persona-fidelity
with its channel-specific adapter than with the single global adapter.

### M6 honest blockers

- **Per-channel data sparsity** — slack-formal might have 500 messages; iMessage-warm 5000. Training a LoRA on 500 messages is risky; might overfit.
- **MoLoRA cold-start** — 4 adapters × ~50MB each = 200MB to keep warm at chat time. Memory budget on iOS/older Macs is tight.
- **Channel-id reliability** — `agent->active_channel` is set by the channel handler. If a future refactor breaks it, MoLoRA silently degrades to default.

---

## Sequencing recommendation

```
Sprint 8 (next):       close honest loop (5 stories)
Sprint 9 (after):      M4.S1 — fresh-machine smoke (5 stories)
Sprint 10 (parallel):  M6.S1 — per-channel fidelity baselines (4 stories)
Sprint 11:             M4.S2 OR M6.S2 (depending on what S1 surfaces)
Sprint 12+:            M4.S3 retention loop (only after S2 proves fidelity lift)
```

Don't ship to 100 DAU before the fidelity lift is real. Don't ship per-channel
adapters before the single-adapter lift is real. The temptation will be to
parallelize ambitiously; the honest path is sequenced.

---

## When to drop M4/M6 from the roadmap

If Sprint 8's e2e DPO vs SFT compare shows **delta ≤ 0.05** (the goal metric
unmet), M4/M6 are premature. The digital-twin story doesn't ship to users
until it produces a measurably better adapter on real data. In that case,
Sprint 9 is "diagnose why DPO didn't help," not "ship to users."

The smoke run that just kicked off (Sprint 7 close + this session's E2E
attempt) is the first data point on whether Sprint 8 should advance toward
M4/M6 or pivot to deeper ML diagnostics.
