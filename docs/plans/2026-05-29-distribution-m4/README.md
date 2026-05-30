---
title: M4 Distribution Plan — 0 → 100 DAU
description: Honest go-to-market for h-uman's first 100 daily-active users, gated on the humanness metric (#1) and aliveness (#4) being real. Grounded in the PRODUCT.md red-team.
status: draft
created: 2026-05-29
owner: seth
---

# M4 Distribution Plan — 0 → 100 DAU

> This is a plan, not code. It deliberately sequences AFTER the humanness
> metric (#1, done) and the aliveness send-tail (#4) because — per the
> PRODUCT.md red-team — h-uman's retention story lives entirely in *voice +
> presence*, not task execution. Shipping to users before those are real just
> burns the novelty window (AI apps churn 30% faster than non-AI; 21% annual
> retention).

## The one-sentence wedge

**"The AI that texts like *you*, runs on *your* Mac, and never phones home."**

Not "an assistant that does tasks" (commodity, can't beat Gemini/Cowork). The
felt product is: friends/family get replies that sound like you, drafted by
something that lives on your hardware and knows you because it learned you
locally — not because it read your Gmail in someone's cloud.

## Who first (the beachhead) — pick ONE, don't spray

| Segment | Why them | Why not yet |
|---|---|---|
| **Privacy-motivated Mac power users** ⭐ | Already own Apple Silicon (MLX requirement), value local-first structurally, tolerate setup friction, evangelize | — (recommended beachhead) |
| iMessage-heavy individuals | The Tier-1 channel is real and differentiated | Needs the macOS iMessage setup to be one-command |
| Developers (HuLa/SDK) | M5 SDK exists | Platform play is a later mission, not a DAU wedge |
| Mainstream consumers | TAM | Can't out-distribute Google's 2B devices; setup friction fatal |

**Recommendation:** privacy-motivated Mac power users. They are the only segment
where every one of our honest moats (local-first, on-device learning, iMessage)
is a *buying reason* rather than a *nice-to-have*, and the Apple-Silicon
requirement is already satisfied.

## The funnel (and where it currently breaks)

1. **Hear about it** — needs a credible privacy + "sounds like you" demo.
2. **Install** — `human onboard` exists (M4). ⚠️ *Gap:* still assumes cloud
   provider creds in config; the local-first path (MLX) must be the DEFAULT
   onboarding branch, not a power-user toggle.
3. **First "whoa"** — the persona must sound like them within the first
   session. ⚠️ *Gap:* starter persona is generic until they feed it data; the
   on-device learning loop (#3-fed DPO) needs ~50 conversations to bite.
4. **Daily use** — presence (#4 aliveness) is what converts a tool into a
   companion (41% DAU/MAU for companion AI vs 14% utility). ⚠️ *Blocked on #4.*
5. **Retain** — the humanness metric (#1) is the internal proxy; if nightly
   composite trends up per user, retention should follow.

The funnel's two structural breaks are **onboarding-defaults-to-cloud** and
**no-presence-yet**. Both are upstream of any marketing spend.

## The 0→100 motion (cheap, sequenced)

- **Phase A (pre-launch, gated on #4):** 5–10 hand-held installs with people
  who text Seth. Measure: does the humanness composite (#1) climb per user over
  2 weeks? Does anyone say "wait, that actually sounded like you"? This is the
  qualitative signal that the moat is felt, not just measured.
- **Phase B (10→40):** a single sharp artifact — a 90-second demo showing a
  real iMessage thread where the assistant's reply is indistinguishable from
  Seth, with the network monitor showing **zero outbound traffic**. The "zero
  packets" visual *is* the pitch. Post where privacy-Mac people are (HN, Lobsters,
  r/LocalLLaMA, Mastodon).
- **Phase C (40→100):** referral is intrinsic — the product's output (your
  texts) is seen by everyone you text. Add a one-line opt-in footer experiment
  ("sent via my local AI — github.com/...") A/B'd for conversion, *off by
  default* (it's outbound-visible; needs the same authorization discipline as #4).

## Success metric (M4)

100 DAU with **30% day-7 retention**. The leading indicator we can watch before
we have 100 users: **per-user humanness composite trend** (from #1's nightly).
If composite isn't climbing for a cohort, retention won't either — fix the
product (loop/persona), don't buy more users.

## What this plan explicitly refuses to do

- No paid acquisition before Phase A proves the "sounds like you" moment lands.
- No mainstream-consumer push (can't out-distribute Google).
- No "privacy as a settings toggle" messaging — privacy is the architecture or
  it's nothing (red-team finding: 81% care, 8-12% configure).

## Dependencies / sequencing

```
#1 humanness metric (DONE)  ──┐
#4 aliveness presence       ──┼──►  Phase A (hand-held installs)  ──► B ──► C
onboarding-defaults-to-local ─┘
#3 on-device learning (SOTA) ──► per-user composite actually climbs
```

## Immediate next actions (when M4 becomes active)

1. Make MLX/local the **default** onboarding branch (`src/onboard.c`) — the
   single highest-leverage funnel fix.
2. One-command macOS iMessage setup (collapse the current multi-step pairing).
3. Instrument per-user nightly humanness composite as the retention leading
   indicator.
4. Build the 90-second "zero packets" demo.
