---
title: Expressive iMessage — self-healing plumbing and a director that chooses how to respond
date: 2026-09-28
status: approved — Seth 2026-09-28 ("Get it all fixed, self healing … all the responses need to be human like and used appropriately; we should think through what and how to respond")
---

# Expressive iMessage

## Why (measured 2026-09-28)

Audit of the live daemon (chat.db, `outbound_sends`, 133k-line service log,
`imsg status`, two code audits):

| Capability | State |
|---|---|
| IMCore bridge | **down** on every daemon start in the log (`bridge=no`); `imsg launch` fixed it once, by hand |
| Accessibility (AX) | **lost after every reinstall** (new cdhash): "all typing tiers failed" on 09-25/26/27/28 |
| typing "…" | works only while AX holds |
| tapbacks | 56 ok / 61 failed (UI-automation fallback while the bridge was down) |
| threaded replies | **0 real**; 20 degraded to flat ("ax_unavailable") |
| effects, stickers, edit/unsend, polls | never sent; `imsg` 0.15.9 supports all of them via the bridge |
| GIFs | code exists (Tenor → attachment); **no Tenor key**, never sent |
| timing | `llm_decides=true`: the director sets delay; the heuristic layer (leave-on-read, bursts, tempo) is bypassed |
| caps | probed once per process and cached forever: a bridge that comes back is never seen |

The daemon had most of the vocabulary and almost none of it reached anyone.
The fix is two-sided: make the plumbing heal itself, and make ONE place decide
how Seth would respond — then hold every expressive move to human frequency.

## Principles

1. **One brain decides the form.** The scene director (already LIVE,
   `llm_decides`) chooses the whole response: text, voice memo, tapback, GIF,
   silence — plus optional effect and whether to thread. No subsystem rolls
   its own dice on top.
2. **Deterministic guards around the brain.** Budgets, appropriateness
   (never an effect or GIF on grief, crisis, conflict), family lists, groups
   — enforced in C, testable, whatever the model says.
3. **Every move degrades gracefully.** Bridge down → AppleScript path →
   plain text. A failed flourish never costs the reply.
4. **Self-healing, human-aware.** Re-probe, repair, and report — but never
   restart Messages while Seth is using the Mac.
5. **OFF → SHADOW → LIVE** per the project's feature-gate rule.

## Phase 1 — Self-healing plumbing

- **Caps re-probe.** `hu_imessage_caps_cached` re-probes when the cached
  answer is older than 10 min *or* a bridge verb just failed. Pure policy:
  `hu_imessage_caps_should_reprobe(now, probed_at, bridge_ok, verb_failed)`.
- **Bridge auto-repair.** When a probe finds `bridge=no` (SIP off, imsg ≥
  0.15), run `imsg launch` — only when the Mac has been idle ≥ 5 min
  (HIDIdleTime) and at most once per 30 min; log the outcome. Pure policy:
  `hu_imessage_bridge_repair_due(bridge_ok, sip_off, idle_s, since_last_s)`.
  Gate `HU_IMSG_SELF_HEAL=off|shadow|live` (shadow logs "would relaunch").
- **Accessibility loss is surfaced, once.** When typing tiers fail for AX,
  post one macOS notification per process ("h-uman lost Accessibility —
  System Settings → Privacy & Security → Accessibility") and make `doctor`
  report it as an error, not a buried INFO line.

## Phase 2 — The director chooses how to respond

Director output grows (backward compatible — missing fields = today):

```
action:<text|voice|tapback|gif|silence>
  [|reaction:<love|like|laugh|emphasize|question|dislike>]
  [|effect:<none|impact|loud|gentle|invisibleink|confetti|lasers>]   (the ids imsg 0.15.9 documents)
  [|reply_to:<true|false>]   thread onto their message
  [|gif:<search words>]       with action:gif
  [|delay_s:N][|burst:true][|direction:...]
```

The director is told what is *available* this turn (voice allowed and why,
bridge up/down, budgets left) so it never asks for the impossible, and it is
given the appropriateness policy:

| Move | When a person does it | Hard guard (C) |
|---|---|---|
| voice | they sent audio; a heartfelt moment; a real question worth talking through | voice-first rules (family list, 3 h spacing, never groups) |
| tapback | photos, pure reactions, acknowledgments, closers | ≤ 1 per inbound message; `question` only when genuinely unsure |
| effect | birthdays, big news, a joke that lands — rarely | ≤ 1 per contact per 7 days; never when emotion is grief/sad/anger/crisis; never in groups |
| GIF | playful back-and-forth with a close contact | ≤ 1 per contact per day; casual relationships only; never serious emotion |
| threaded reply | answering an older message when newer ones came in between, or a group | only when ≥ 2 messages arrived since the target |
| silence | toxic, or after 3+ low-effort pings | unchanged |

The voice-first predicate becomes an *input* the director sees ("voice
available: yes — they sent audio") instead of a second decision-maker; the
deterministic guards still hold. The director's "brevity" rule no longer fights
the memo directive: when it picks voice, its length cue is dropped.

## Phase 3 — Executors on the bridge

- effect → `imsg send-rich --effect`; fallback: plain send.
- threaded → `imsg send-rich --reply-to <guid>`; fallback: plain send.
- tapback → `imsg tapback --kind`; fallback: `imsg react` → text.
- GIF → Tenor search (needs `providers.tenor` key from Seth) → attachment.
- sticker → `imsg send-sticker` (Phase 3b, needs a curated sticker set).
- self-correction → `imsg edit` for a real mistake, never a fake typo.

## Phase 4 — Timing

The heuristic timing signals (time of day, message count, photo viewing, the
learned reply-delay model) become inputs to the director's `delay_s`, instead
of a parallel path it bypasses.

## Measurement

Per move per contact from `outbound_sends` + chat.db: reply rate and latency
after each move vs plain text; tapback success rate; bridge uptime; AX-loss
count. A move stays LIVE only while it does not lower reply rates.

## Order and scope for this session

Phase 1 fully. Phase 2 + the effect / threaded / tapback executors of Phase 3,
shipped in SHADOW (director decides and logs; sends unchanged) so Seth can read
a day of decisions before any of it goes LIVE. GIF waits on a Tenor key;
stickers and Phase 4 are follow-ups.
