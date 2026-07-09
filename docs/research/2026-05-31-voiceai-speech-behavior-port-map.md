# voiceai → h-uman: Speech-Behavior Port-Map (VERIFIED)

**Date:** 2026-05-31
**Author:** Seth + Claude (Opus 4.8)
**Source repos:** voiceai/Ferni `/Users/sethford/Documents/voiceai` @ `321a48c4b` · h-uman `/Users/sethford/Projects/h-uman` @ `eb97f0f1` (branch `feat/e2e-gold-and-scale-guard`)
**Method:** 2 parallel read-only cartographers (one per repo) + a focused verification pass on every UNCONFIRMED/DISCARDED claim (`audit-verify-before-allege`).

---

## TL;DR — this is an *activation* problem, not a *porting* problem

The question that started this: *"voiceai has so many human-like speech behaviors and quirks — how do we bring that SOTA work over to h-uman?"*

The verified answer: **h-uman already has nearly all of it, and more of it is LIVE than even a careful first-pass audit assumed.** Of ~19 speech-behavior categories, the h-uman audit found **0 genuinely ABSENT** on the text path. The work is overwhelmingly (a) flip the few dormant ones, (b) build a small number of genuinely-thin text behaviors, and (c) **measure** — not re-implement voiceai's code.

> ⚠️ **Why this doc is labeled VERIFIED.** A first-pass cartographer guessed two behaviors were missing/dormant. A verification pass proved both were already **LIVE** in production. Persisting the unverified map would have sent the first sprint to "wire up" things that already ship. Every state below that says LIVE/DISCARDED carries a `file:line` proof.

---

## The big correction (first-pass guess → verified truth)

| Behavior | First-pass guess | **Verified state** | Proof |
|---|---|---|---|
| Backchannels ("mhm/yeah") | COMPUTED-BUT-DISCARDED | **LIVE** (text) | decision `conversation.c::hu_conversation_should_backchannel` @ `daemon.c:5645`; sent via channel vtable @ `daemon.c:10325-10336` |
| Filler / discourse-marker weaving | UNCONFIRMED | **LIVE** (text **and** TTS) | text: `hu_conversation_apply_fillers` @ `daemon.c:12410`; TTS: `pick_discourse_marker` @ `transcript_prep.c:1173` |
| Self-repair disfluency | UNCONFIRMED | **LIVE** (10% sub-branch of disfluency) | `conversation.c:6010` injects `" wait no "` / `" *meant it"`; gated by `disfluency_frequency` @ `daemon.c:12438` |
| `texts_in_bursts` contact field | UNCONFIRMED | **COMPUTED-BUT-DISCARDED** (field is doc-only; burst *behavior* is LIVE on a different gate) | field only populates prompt text @ `persona.c:721`; real bursts gate on `burst_message_probability` @ `daemon.c:10537-10643` |
| Turn-taking / interruption (text) | UNCONFIRMED | **ABSENT on text** (exists in `src/voice/` only) | no wired handler in text path; `HU_RESPONSE_SKIP` is the only "should I reply" gate @ `daemon.c:5481/5512/5613` |

---

## Channel asymmetry — the real fault line

voiceai is a **real-time voice** product; h-uman is **iMessage text-in + TTS-out**. This determines portability more than anything else.

- **~18 voiceai behaviors are VOICE-INPUT-ONLY**: breath-pattern detection, voice tremor (jitter/shimmer), volume/energy dynamics, multi-signal laughter detection, FFT/prosodic feature extraction, prosody-aware turn prediction, micro-interruption/barge-in, ambient-sound awareness, avatar micro-expressions. **h-uman has no real-time audio input loop**, so these don't "port" — they'd require a new capability h-uman has no socket for today.
- **h-uman's TTS *output* realism already matches voiceai's** where it counts: `tts/transcript_prep.c` + `tts/emotion_map.c` do SSML `<break>` injection + emotion-derived speed/volume **LIVE** by default.
- **The text-expressible behaviors are where the action is** — and h-uman already ships most of them.

---

## The port-map (three tiers, by ROI)

### Tier A — Built in h-uman, dormant; flip/wire (hours)
After verification this tier is **much smaller than the first pass implied** — most "Tier A candidates" turned out already-LIVE.

| Behavior | Verified state | Work | Notes |
|---|---|---|---|
| Salience-ranked directives | **SHADOW** (`HU_SALIENCE_SHADOW`) | calibrate → flip ON | already on the SOTA roadmap; gate on blind-A/B |
| Graph-grounded memory recall | **GATED OFF** (`HU_GRAPH_GROUNDING`) | calibrate OFF→SHADOW→ON | the "better-than-human" memory moat; roadmap #1 |
| `texts_in_bursts` contact preference | **DISCARDED field** (behavior is LIVE on `burst_message_probability`) | optionally honor the per-contact field as a gate | cosmetic; burst behavior already works |

### Tier B — Genuinely thin/absent on text; real builds (medium)

| voiceai behavior | h-uman gap | Value | voiceai reference |
|---|---|---|---|
| **Intent-aware response-type selection** — classify "needs-to-be-heard / seeking-advice / vulnerable-share / small-talk" from signal patterns, route listen-vs-advise-vs-share | h-uman has `arbitrator.c` + mood but **no explicit intent classifier** | **HIGH** — biggest "feels human" lever h-uman truly lacks | `src/personality/timing-intelligence.ts`, `domain/services/timing-calculator.ts` |
| **Over-feedback coordinator** — don't backchannel + ask + warmth-mark in the same turn | not found in h-uman; relevant now that backchannels are confirmed LIVE | Medium (prevents the newly-confirmed backchannel from saturating) | `src/speech/feedback-coordinator.ts` |
| **Structured cognitive-quirk library** — per-persona triggers, frequency, signature transition phrases, internal-monologue style | h-uman has `narrative_self.c` / `genuine_boundaries.c` but less structured | Medium (refinement) | `src/personas/cognitive-quirks.ts` |
| **Proactive meaningful-silence + vulnerability callbacks** — user goes quiet → surface a *shared memory*; first vulnerable share → check in days later | h-uman has `follow_up.c` (delayed, warmth-gated) but not these proactive shapes | Medium | `src/personas/meaningful-silence.ts`, `personality/.../vulnerability-deposit.ts` |
| Turn-taking / interruption (text) | **ABSENT** on text | Low for text (texting is async; less of a "quirk") | `src/speech/enhanced-turn-prediction.ts` (voice) |

### Tier C — Voice-input-only; deferred unless h-uman grows a real-time voice-in loop
Breath/tremor/laughter detection, prosody turn-prediction, barge-in, ambient awareness, avatar layer. **Only relevant if real-time voice *input* lands on the roadmap.** h-uman's `src/voice/` has interrupt/duplex scaffolding but no humanness-grade audio analysis.

---

## Already-LIVE on h-uman text (do NOT rebuild)

Confirmed reaching the send path on default config: disfluency + self-repair (`daemon.c:12438`), **backchannels** (`daemon.c:10325`), **fillers/discourse markers** (`daemon.c:12410`), humor generation w/ 6 styles + safety gate (`cognition/humor.c`), topic callbacks (`conversation.c:722`), warmth-gated delayed follow-ups (`follow_up.c` + `daemon_follow_up_watcher.c`), reply cadence by warmth + chronotype (`follow_up.c:40`), typing quirks/typos when overlay-enabled (`conversation.c::hu_conversation_apply_typing_quirks`), multi-message bursts (`daemon.c:10537`), mood model 9-state SQLite-backed (`context/mood.c`, `persona/mood.c`), circadian shifts (`persona/circadian.c`), style mirroring (`behavioral.c:316`, `style_learner.c`), formality/register shifts + emoji control (`persona/render.c:282`), sycophancy/authenticity guard (`security/sycophancy_guard.c`), arbitration (`agent/arbitrator.c`). TTS output: emotion-derived SSML breaks + speed/volume (`tts/transcript_prep.c`, `tts/emotion_map.c`).

---

## Recommended sequencing

The strategic constraint (from prior sessions): **gate every behavior change on the blind-A/B indistinguishability measure** (`scripts/blind_ab/`), which is still unmeasured and is the actual SOTA bottleneck. More behaviors without that number is motion, not progress.

1. **Measurement first** — run `scripts/blind_ab/` end-to-end with Seth's real sent messages + raters who know him → establish the baseline number. Without it, nothing below is provably "more human."
2. **Flip the moat (gated on #1)** — `HU_GRAPH_GROUNDING` OFF→SHADOW→ON with calibration; this is the better-than-human (perfect-recall) lever, not a quirk.
3. **Tier B #1 — intent-aware response-type selection** — the one genuinely-missing high-value text behavior. Spec → build → A/B.
4. **Over-feedback coordinator** — now that backchannels + fillers are confirmed LIVE, ensure they don't co-fire and over-humanize a single turn.
5. **Tier B refinements** (cognitive-quirk library, proactive callbacks) as A/B shows headroom.
6. **Tier C / voice-in** — only if a real-time voice input loop is on the roadmap.

---

## Open items (not exhaustively verified — do not treat as settled)

- Minimal acknowledgments ("got it/cool") and register shifts *beyond* formality — believed LLM-prompt-driven, not separately wired; not deep-traced.
- voiceai subsystems mapped but not detailed: `services/superhuman/*` (45 capabilities), `intelligence/cross-persona-intelligence.ts`, pronunciation-memory, response-anticipation caching (a latency feature, not a quirk).
- Whether honoring `texts_in_bursts` per-contact (vs the global `burst_message_probability`) measurably improves realism — untested.

---

## Provenance

- voiceai inventory agent: 45–50 behaviors across `src/speech/`, `src/personas/`, `src/personality/`, `src/config/voice-humanization-flags.ts` (~82k LOC / 276 files).
- h-uman activation audit + verification: states cited with `file:line`; 5 re-verified items corrected 2 first-pass errors (see "big correction" table).
- Related memory: `project_h-uman_voiceai_gap_REALITY`, `project_h-uman_PICKUP_PLAN`, `project_h-uman_sota_roadmap`, `project_h-uman_salience`.
