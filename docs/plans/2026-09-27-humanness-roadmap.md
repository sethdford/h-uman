---
title: Humanness roadmap — memory, context, timing, voice
date: 2026-09-27
status: draft
---

# Humanness roadmap — memory, context, timing, voice

**Question:** h-uman texts as Seth. What stands between it and reading (and
sounding) like him?

**Short answer:** not a shortage of features. The live daemon runs 36 feature
flags (24 live, 10 shadow, 2 off). The gaps are (1) a stalled measurement loop,
so nothing in shadow can be promoted and nothing live has a current human
verdict; (2) several LIVE features that do nothing in production; (3) a few
real mechanism gaps — reply timing, "what we talked about last time", things
Seth has already told someone, voice memos that never get chosen, and inbound
voice memos that cannot be understood.

**Evidence levels.** *Verified* = re-checked first-hand while writing this
(query, file read, or live env). *Audit* = reported by one of the eight
read-only research agents with file:line citations, not re-checked. Several
audit claims were wrong on review (see §7), so treat *Audit* items as leads.

---

## 0. The measurement loop is stalled — fix this first

Everything else in this document is promoted or retired by the blind A/B loop
(`.claude/rules/feature-gate-requires-measurement.md`). Right now it cannot
decide anything.

| Fact | Level |
|---|---|
| `doctor`: human verdict is 60 days old (limit 45) | Verified |
| Last human verdict (2026-07-29, n=40, PASS) measured adapter `seth-glm-air-v5`, not the one served now | Audit |
| Current drip sheet: 4 of 48 rows rated since 09-06; ~09-05→09-20 sends went to a dead alias; `score.py` only runs when every row is rated | Audit |
| The gating A/B is whole-model only. Per-feature preference sheets exist (`make_rating_sheet.py --mode preference`) but are non-gating, and both are unrated (48 + 54 rows) | Audit |
| `doctor` stale limit is 45 days, `scripts/blind_ab_gate.py` uses 30 | Audit |

**Do:**
1. Finish the current drip (44 ratings, ~15 min of Seth's time) → fresh verdict.
2. Rate the two per-feature preference sheets. They are the only per-flag
   human signal that exists.
3. Make a per-feature human verdict a first-class, gating artifact, so a flag
   can be promoted on its own evidence instead of riding a whole-model PASS.

---

## 1. Highest-leverage humanness gaps

Ranked by how strongly each reads as "not Seth".

### 1.1 Reply timing — replies ~20× faster than Seth (Audit)
- In production (`llm_decides=true`) the delay is the director LLM's
  `delay_s` (`daemon.c:3635`, capped at 120 s): n=151, median ≈4.6 s.
- Seth's real history (`~/.human/reply_delay_model.json`, n=1915): median
  ≈105 s, p75 ≈14 min, p90 ≈2.4 h. The 120 s cap makes his long tail
  impossible.
- A chat.db-learned per-contact model exists (`agent/timing.c:386`) but only
  feeds the heuristic path that `llm_decides` skips. `HU_REPLY_DELAY_MODEL` is
  off because its held-out MAE was worse than a global median; its LIVE mode
  is not wired to any send decision (`reply_delay.h:22`).
- **Do:** measure the director's delays against Seth's distribution first
  (it has never been scored). Then drive the delay from his per-contact
  distribution, with the director allowed to shorten only for urgency.

### 1.2 "What we talked about last time" (Audit)
- The raw iMessage thread is loaded but not rendered in `llm_decides` mode
  (`daemon_reactive_prompt.c:1259`); the model sees the session store (last
  100) plus memory blocks.
- Session summaries are neither stored (`daemon.c:8418`) nor loaded
  (`daemon.c:5049`) in production; memory.db has 0 `_ep:` rows.
- Inside jokes, micro-moments, avoidance patterns, growth milestones and the
  graph/ToM block are all gated behind `!llm_decides` (`daemon.c:5056-5236`,
  `4860`), and so is cross-channel context (`daemon_reactive_context.c:393`).
- **Do:** audit the `llm_decides` gates as one decision. The director mode
  appears to have silently switched off a large part of the relationship
  memory built earlier. Re-enable the pieces that belong in the prompt.

### 1.3 Things Seth has already told this person (Audit, verified missing by grep)
No per-contact record of self-disclosures or life updates Seth has shared, so
he can repeat the same story to the same friend. voiceai tracks `storiesTold`
(`deep-humanization.ts:250`).
- **Do:** record what each outbound message discloses, per contact, and check
  it before the prompt.

### 1.4 Other people's plans and dates (Audit)
- `HU_LIFE_EVENTS` covers Seth's own life only, and its data is two months
  stale (newest `as_of` 2026-07-28, nothing updates it).
- Important dates are one global persona list; `proactive.c:689` ignores
  `contact_id`. **Possible bug:** a persona date message could go to every
  eligible contact that day — verify before anything else here.
- **Do:** per-contact dates learned from conversation (yearly repeat,
  lookahead, sensitive-date opt-out). Refresh life events automatically.

### 1.5 Inside-joke reuse runs backwards (Audit)
The pick list sorts `last_referenced DESC` (`superhuman.c:1386`), so a joke
just used is the next one picked — the opposite of a cooldown. Punchlines are
saved empty. **Do:** 30-day cooldown, topic matching, save the actual line.

### 1.6 Memory that feels human (Audit)
- Old details recalled with perfect precision; hedging ("wasn't that in like
  March?") is random (`authentic.c:105`) rather than tied to
  `forgetting_curve.c` decay.
- No learning of whether memory callbacks land (voiceai `CallbackEffectiveness`).

---

## 2. Voice track

### 2.1 Voice memos never get chosen (Verified)
Since 2026-09-21 the daemon made 70 voice decisions: **0 sends**. Reasons:
`response_short` 31, `incoming_question` 21, `no_prefer_boost` 16,
`logistics` 1, `roll_miss` 1. Native delivery for family went live today
(`HU_VOICE_DELIVERY=messages`, `HU_VOICE_DELIVERY_ONLY` = the four family
handles), but it only matters once a memo is chosen.
- **Decide (Seth):** how often should family hear his voice? The policy is one
  global `frequency: rare` rule. Options: a per-contact frequency override for
  family, or loosening `response_short` for warm replies to family.
- Speech-rewrite shadow data (`~/.human/voice/shadow/`) cannot accumulate
  until memos are chosen (Audit: the directory does not exist yet).

### 2.2 Inbound voice memos are not understood (Verified)
- The inbound path transcribes audio through `hu_multimodal_process_audio`,
  which picks Gemini/OpenAI by the *reply provider's* name and key. The live
  reply provider is local, so a memo from family would reach the reply as
  `[Audio]`.
- `src/voice/local_stt.c` (whisper.cpp / OpenAI-compatible) exists but the
  inbound path does not use it; `voice.local_stt_endpoint` is unset.
  `mlx_whisper` and `whisper` are installed.
- 0 inbound audio messages in the last 90 days — this starts mattering now
  that Seth sends memos.
- **Do (design proposed 2026-09-27, awaiting approval):** local Whisper server
  under launchd → `voice.local_stt_endpoint` → inbound path tries local STT
  first, cloud route only as fallback. Check first whether iOS's own memo
  transcription is already present in chat.db.

### 2.3 Speech realism (Audit, voiceai speech-stack comparison)
Top ports, by audible impact:
1. **Laugh rendering.** h-uman always emits Cartesia's `[laughter]`; voiceai
   deliberately writes text laughs because stock laughter "doesn't match
   persona voice". A/B the two on the clone.
2. **Sentence gaps by topic weight** (150/250/400 ms) instead of a flat
   350 ms (`transcript_prep.c:822-830`).
3. **Spoken abbreviations** (btw, fyi, imo, jk, w/, vs, aka, asap) and
   😂/🤣 → laughter cue.
4. **Laugh spacing across memos** (spec'd in F1, not built), an
   inbound-laughing cue, and wire `prev_turn_emotion` (code exists, never set).
5. **Self-corrections, restarts and varied sentence length** in the S1
   rewrite prompt (`k_speak_it`).

### 2.4 Cartesia usage
| Item | Status |
|---|---|
| Model: persona sets `sonic-3.6`; `cartesia.c:65` fallback `sonic-3-2026-01-12` is stale but unused when the persona sets a model | Verified — update the fallback constant |
| Inline `<speed ratio>` / `<volume ratio>` / `<emotion>` / `<break>` tags | Verified — already emitted |
| Emotion vocabulary is ~6 keyword-mapped values (`emotion_map.c`) | Audit |
| Clone quality: current clone is an instant clone from April; the MV7 re-clone (W2) is waiting on Seth's recording | Verified (W2 pending) |
| Pro clone / similarity controls, custom pronunciations for contact names | Audit — worth a look after W2 |

---

## 3. LIVE features that do nothing in production (Audit)

| Flag | Why it is inert |
|---|---|
| `HU_SALIENCE_LIVE` | 148/148 ranking events suppressed nothing |
| `HU_GRAPH_GROUNDING` | Only injects on analytical turns; graph context empty on 2,323/2,323 turns |
| `HU_LLM_PRIORITY` | Header only sent on the streaming path; replies don't stream |
| `HU_OPINION_HOLD` | `evolved_opinions` has 0 rows, so the loop never runs |
| `HU_AGENT_FACTS` | 0 graph facts written in production |
| `HU_REPLY_DELAY_MODEL` / `HU_IMESSAGE_BB_EVENTS` | LIVE mode is identical to SHADOW (not wired to a send decision) |
| `HU_BANDIT_HUMANIZATION` | 6 of 9 arms never updated; the disfluency choice is discarded (`daemon.c:3485`) |
| `HU_INSIGHT_STREAM` | 2,035 tokens injected over 70 replies, 4 surfaced (0.2%) |

**Do:** each gets fix-or-retire. A flag that is live and inert costs prompt
bytes and misleads every future audit.

## 4. Shadow promotion queue (Audit)

| Flag | Blocker |
|---|---|
| `HU_PROACTIVE_REACHABILITY` | n≥30 met; run the named FIR reading, then promote |
| `HU_WIKI_HEAD` | Most friend-like memory; run the named bytes/specificity A/B |
| `HU_EMOTION_REGISTER` | JSD 0.167 vs 0.15 target; tune, then blind A/B |
| `HU_HARD_MOMENT` | 0 would-fires in 44 1:1 batches — check the classifier before any A/B; not called on the non-stream retry path |
| `HU_SUBSTANTIVE_REGISTER` | Three prompt variants moved nothing; tune or retire |
| `HU_PROACTIVE_CONTEXTUAL` | 1 detection in 8 days; needs data |
| `HU_IMMERSIVE_HUMANNESS` | Fires; needs a per-feature sheet |
| `HU_SPEECH_REWRITE` | Needs memos to be chosen (§2.1) |

## 5. Possible bugs to check first (Audit, cheap to verify)

1. Persona date message may fan out to every eligible contact (`proactive.c:689`).
2. Commitments due query: deadline 0 stored as 0 not NULL, so the query returns
   the 3 oldest no-deadline rows, and does not filter `who` (`superhuman.c:218,242`);
   167 commitments pending, 0 followed up.
3. `HU_TOM_DIRECTIVE` shadow appends to the prompt exactly like live (`agent_turn.c:417-499`).
4. LLM-extracted facts carry no contact field and may pool across contacts
   (`fact_extract.h:31-35`).
5. Live-with-no-measurement: terseness, style governor, warmth tone, follow-up
   compose were promoted without their named human verdict.

## 6. Suggested order

1. §0 measurement loop (unblocks everything).
2. §5 bug checks (cheap; two could send wrong messages).
3. §2.2 inbound STT and §2.1 voice frequency decision (voice is live for
   family now).
4. §1.1 timing, §1.2 `llm_decides` memory gates — the two biggest tells.
5. §3 fix-or-retire inert flags; §4 promotions as verdicts arrive.
6. §1.3–1.6 new memory mechanics; §2.3 speech ports; W2 re-clone.

## 7. Audit claims that were wrong on review

- "h-uman sends the deprecated `sonic-3-2026-01-12`": the persona's
  `sonic-3.6` overrides it (`voice_reply.c:70-71`).
- "No inline `<speed>`/`<volume>` tags": emitted at `transcript_prep.c:1150,1162`.

Recorded so the next audit doesn't re-open them.
