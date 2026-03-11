---
title: "Project Human Fidelity — Master Design Document"
created: 2026-03-10
status: approved
scope: iMessage channel, daemon, conversation intelligence, persona, TTS, AGI cognition, deep memory, external awareness, skill acquisition, authentic existence
phases: 9
features: 115
---

# Project Human Fidelity — Master Design Document

> Make human indistinguishable from a real human on iMessage, then make it better than any human could be.

## Executive Summary

This project transforms human's iMessage presence from "AI that texts" to "person who texts, powered by superhuman intelligence." 115 features across 17 pillars, implemented in 9 phases. The approach extends existing BTH/daemon/conversation systems (Approach B) with persona JSON configuration — no new vtable subsystems, no new dependencies beyond Cartesia TTS (Phase 5 only).

Key insight: most "human fidelity" is about **what you DON'T do** (respond too fast, respond to everything, use perfect grammar, never make mistakes) as much as what you do. The system should feel like a real person with superhuman memory, not a chatbot with human delays.

## Architecture Decision

**Approach B: BTH Enhancement** — Extend existing systems.

Rationale:

- 3000+ lines of BTH/timing/conversation code already exist and are battle-tested
- Tapback classifier, message splitting, contact profiles, BTH metrics all exist
- Adding 57 features as new vtable subsystems would bloat binary size
- Persona JSON configuration keeps behavior portable and editable
- Incremental delivery — each phase produces visible results

What changes:

- `src/channels/imessage.c` — iMessage platform features (inline replies, editing, effects, tapback wiring)
- `src/context/conversation.c` — emotional intelligence classifiers, behavioral pattern engines
- `src/daemon.c` — timing patterns, decision engines, orchestration
- `src/memory/` — superhuman memory features (inside jokes, commitments, patterns, temporal)
- `src/tts/cartesia.c` — NEW: Cartesia TTS integration (Phase 5 only)
- `src/persona/persona.c` — extended persona fields for all behavioral config
- `include/human/persona.h` — new persona struct fields
- Persona JSON (`~/.human/personas/seth.json`) — all behavioral configuration

What does NOT change:

- No new vtable interfaces
- No new dependencies (except libcurl for Cartesia, already optional)
- No changes to `src/security/`, `src/gateway/`, `src/runtime/`
- Binary size increase target: <50KB across all phases

## Complete Feature Inventory (57 Features, 11 Pillars)

### Pillar 1: Fix Broken Plumbing

| #   | Feature                                                                       | Status            | Complexity |
| --- | ----------------------------------------------------------------------------- | ----------------- | ---------- |
| 1   | **Tapback reactions** — wire `message_id` in iMessage poll loop               | Implemented       | Low        |
| 2   | **Tapback-vs-type decision** — unified classifier: tapback only / text / both | Implemented       | Medium     |
| 3   | **Typing indicator** — iMessage has no API for this via AppleScript           | 🔧 Platform limit | N/A        |

**F1: Tapback Reactions**
The tapback classifier (`hu_conversation_classify_reaction`), reaction type enum (`hu_reaction_type_t`), and JXA handler all exist. Two bugs prevent them from firing:

- `imessage.c` poll loop never sets `msgs[count].message_id = rowid`
- JXA script opens AX context menu on last row but doesn't select specific message or tapback type

Fix: (a) Set `msgs[count].message_id` in poll. (b) Rewrite JXA to use `associated_message_type` + `associated_message_guid` in chat.db to send tapbacks via AppleScript (more reliable than accessibility API).

AppleScript tapback approach: iMessage tapbacks are stored in chat.db as messages with `associated_message_type` values (2000=love, 2001=like, 2002=dislike, 2003=laugh, 2004=emphasize, 2005=question). We can send tapbacks by creating a response message via AppleScript that references the original message GUID.

**F2: Tapback-vs-Type Decision**
New classifier in `conversation.c`:

```
hu_tapback_decision_t hu_conversation_classify_tapback_decision(
    const char *message, size_t message_len,
    const char *history, size_t history_len,
    const hu_contact_profile_t *contact)
```

Returns: `HU_TAPBACK_ONLY`, `HU_TEXT_ONLY`, `HU_TAPBACK_AND_TEXT`, `HU_NO_RESPONSE`

Decision factors:

- Message type: humor → haha tapback, agreement → thumbs up, photo → heart
- Contact preference: `tapback_style.frequency` (high/medium/low)
- Conversation phase: late in thread → tapback more likely
- Message length: short messages → tapback, long → text response
- Recent tapback count: don't tapback 5 messages in a row

Persona config:

```json
"tapback_style": {
  "frequency": "high",
  "prefer_tapback_over_text": ["humor", "agreement", "photos", "farewells"],
  "favorite_tapbacks": ["love", "haha", "emphasize"],
  "never_tapback": ["questions", "emotional"]
}
```

**F3: Typing Indicator**
iMessage does not expose a typing indicator API via AppleScript or JXA. The native typing bubble only appears from the Messages.app UI. Marking as platform limitation — skip implementation.

### Pillar 2: Photo/Media Intelligence

| #   | Feature                     | Status      | Complexity |
| --- | --------------------------- | ----------- | ---------- |
| 4   | **Auto-vision pipeline**    | Implemented | Medium     |
| 5   | **Photo reaction decision** | Implemented | Medium     |
| 6   | **Photo viewing delay**     | Implemented | Low        |
| 7   | **Video awareness**         | Implemented | Medium     |

**F4: Auto-Vision Pipeline**
When iMessage poll detects a message with an attachment:

1. Extract attachment path via `hu_imessage_get_attachment_path(alloc, rowid)`
2. Check file extension — if image (`.jpg`, `.jpeg`, `.png`, `.heic`, `.gif`), pipe through `hu_vision_describe_image()`
3. Inject description into conversation context: `"[They sent a photo: {description}]"`
4. LLM responds naturally to the photo content

SQL change in poll query: join `attachment` table to detect if message has attachment:

```sql
LEFT JOIN message_attachment_join maj ON maj.message_id = m.ROWID
LEFT JOIN attachment a ON maj.attachment_id = a.ROWID
```

**F5: Photo Reaction Decision**
Extends F2 tapback decision for photo context:

- Sunset/landscape → heart tapback (love)
- Funny photo → haha tapback + brief text
- Family photo → love tapback + warm response
- Selfie → text response ("looking good!")
- Food → brief text ("that looks amazing")
- Screenshot → depends on content

**F6: Photo Viewing Delay**
When attachment detected, add 3-8 seconds of "viewing time" before the response timer starts. Scales with late-night multipliers. Simulates actually looking at the photo.

**F7: Video Awareness**
Same as F4 but for video attachments (`.mov`, `.mp4`, `.m4v`). Options:

- Extract first frame, run through vision
- If short (<30s), note duration: "looks like a short video"
- If long, just acknowledge: "let me watch this"
- Add proportional viewing delay (2-10 seconds)

### Pillar 3: Timing Patterns

| #   | Feature                           | Status      | Complexity |
| --- | --------------------------------- | ----------- | ---------- |
| 8   | **Delayed follow-up engine**      | Implemented | High       |
| 9   | **Double-text pattern**           | ⚠️ Partial  | Medium     |
| 10  | **Missed-message acknowledgment** | Implemented | Low        |
| 11  | **Natural conversation drop-off** | Implemented | Low        |
| 12  | **Morning/evening bookends**      | ⚠️ Partial  | Medium     |

**F8: Delayed Follow-Up Engine**
After a conversation ends (no new messages for 15+ minutes), probabilistically schedule a follow-up:

- Trigger: memory system detects unresolved topics or high-salience statements from the conversation
- Delay: 20 minutes to 4 hours (random within configurable range)
- Content: LLM generates with context: "You were thinking about what [contact] said about [topic]. Send a natural follow-up."
- Probability: configurable per-contact, default 15%
- Implementation: new `hu_delayed_followup_t` struct in daemon, checked each poll cycle

```json
"follow_up_style": {
  "delayed_follow_up_probability": 0.15,
  "min_delay_minutes": 20,
  "max_delay_hours": 4,
  "triggers": ["unresolved_questions", "emotional_moments", "shared_plans"]
}
```

**F9: Double-Text Pattern**
After sending a response, 5-10% chance of sending a second message 10-45 seconds later:

- "oh and" / "wait actually" / "lol i just realized"
- The LLM generates the follow-up with context: "You just said X. Add a natural afterthought."
- Not message splitting (which is one thought broken into fragments) — this is a genuinely new thought
- Probability scales with conversation energy (higher energy = more likely)
- Never double-text after a farewell or in late-night hours

**F10: Missed-Message Acknowledgment**
If response delay exceeds threshold (configurable, default 30 minutes):

- Prepend acknowledgment: "sorry just saw this", "oh man missed this", "ha just woke up"
- Phrasing is persona-driven (Seth: "sorry just saw this" not "my apologies for the delayed response")
- Only triggers if delay was significantly longer than typical for the time of day
- Don't acknowledge if it's a natural late-night/early-morning gap

**F11: Natural Conversation Drop-Off**
Refine existing `HU_RESPONSE_SKIP` logic:

- After mutual farewell ("night" / "sleep well"): 90% silence (don't reply to their reply)
- After low-energy exchange ("yeah" / "cool" / "ok"): 60% silence
- After emoji-only response from them: 70% silence
- After your own farewell that they didn't respond to: 100% silence
- Track "thread energy" — declining energy → increasing skip probability

**F12: Morning/Evening Bookends**
For contacts with `bookend_messages: true`:

- Morning: "morning min" / "hey morning" at persona-appropriate time (7-9 AM with jitter)
- Evening: "night" / "sleep well" at persona-appropriate time (10-11 PM with jitter)
- Frequency: 2-3 times per week (not daily — that's robotic)
- Skip if there was already a conversation that day
- Skip if contact hasn't messaged in >48 hours (respect their silence)

```json
"bookend_messages": {
  "enabled": true,
  "morning_window": [7, 9],
  "evening_window": [22, 23],
  "frequency_per_week": 2.5,
  "phrases_morning": ["morning min", "hey morning", "morning"],
  "phrases_evening": ["night", "night min", "sleep well"]
}
```

### Pillar 4: Emotional Intelligence

| #   | Feature                                | Status      | Complexity |
| --- | -------------------------------------- | ----------- | ---------- |
| 13  | **Energy matching**                    | Implemented | Medium     |
| 14  | **Emotional escalation detection**     | Implemented | Medium     |
| 15  | **Response length calibration**        | Implemented | Low        |
| 16  | **Context modifiers**                  | Implemented | Medium     |
| 17  | **First-time vulnerability detection** | Implemented | High       |

**F13: Energy Matching**
Extend `hu_conversation_build_awareness()` with explicit energy-level detection:

- Excited/enthusiastic → match with enthusiasm, exclamation marks, positive energy
- Sad/down → gentle, shorter, empathetic, no jokes
- Neutral/chill → relaxed, casual
- Playful/teasing → match the playfulness
- Anxious/stressed → calm, reassuring, grounding

Inject energy directive into system prompt: `"[ENERGY: They're excited. Match their energy. Be enthusiastic.]"`

Ported from voiceai's `voice_expression.mirroring_level` and `contextual_tones`.

**F14: Emotional Escalation Detection**
Track emotional trajectory across multiple messages in a conversation:

- 3+ messages with increasing negative sentiment → escalation detected
- Switch to de-escalation mode: shorter responses, more empathetic, fewer jokes
- "hey you okay?" rather than matching anger
- If user explicitly says they're fine/joking, reset escalation state

**F15: Response Length Calibration**
Match the other person's message length within a 1.5x ratio:

- Short message (1-20 chars) → short reply (1-30 chars)
- Medium message (20-100 chars) → medium reply (20-150 chars)
- Long message (100+ chars) → proportional reply
- Never send a 200-word response to "k"
- Already partially implemented via "brief mode" — extend with finer granularity

**F16: Context Modifiers**
Ported from voiceai's `context_modifiers`:

- `serious_topics_reduction: 0.4` → Detect heavy topics (death, illness, job loss, divorce) → reduce humor/playfulness by 60%
- `personal_sharing_warmth_boost: 1.6` → When someone shares something personal → boost warmth/empathy by 60%
- `high_emotion_breathing_boost: 1.5` → When emotions are high → slow down, add pauses (text: shorter sentences, line breaks)
- `early_turn_humanization_boost: 1.4` → First few messages in a conversation → be warmer, more human

**F17: First-Time Vulnerability Detection**
Ported from voiceai's `first-time-vulnerability`:

- Detect when someone shares something personal for the first time (hasn't mentioned this topic before)
- Memory check: has this person ever mentioned [illness/divorce/job loss/family issue] before?
- If first time: respond with extra care, weight, and acknowledgment
- Don't immediately pivot to advice — sit with them
- "that's huge. thanks for telling me." not "here's what you should do"

### Pillar 5: Superhuman Memory & Intelligence

| #   | Feature                         | Source                            | Complexity | Status         |
| --- | ------------------------------- | --------------------------------- | ---------- | -------------- |
| 18  | **Micro-moment recognition**    | voiceai superhuman-intelligence   | High       | ✅ Implemented |
| 19  | **Inside joke memory**          | voiceai inside-jokes              | Medium     | ✅ Implemented |
| 20  | **Commitment keeper**           | voiceai commitment-reminder       | Medium     | ✅ Implemented |
| 21  | **Avoidance pattern detection** | voiceai superhuman-intelligence   | High       | ✅ Implemented |
| 22  | **Pattern mirror**              | voiceai pattern-callback          | Medium     | ✅ Implemented |
| 23  | **Topic absence detection**     | voiceai topic-absence             | Medium     | ✅ Implemented |
| 24  | **Growth celebration**          | voiceai growth-celebration        | Medium     | ✅ Implemented |
| 25  | **Emotional check-ins**         | voiceai EmotionalCheckIn          | Medium     | ✅ Implemented |
| 26  | **Temporal pattern learning**   | voiceai temporal-pattern-detector | High       | ✅ Implemented |
| 27  | **Comfort pattern learning**    | voiceai comfort-patterns          | Medium     | ✅ Implemented |

**F18: Micro-Moment Recognition**
Catch small but significant details that most people would miss:

- "Wait, you said your dog's name is the same as your grandma's — is there a story there?"
- "You mentioned your anniversary is coming up — got anything planned?"
- Implementation: LLM prompt injection with memory context highlighting "notable small details"
- Memory tags: `micro_moment` flag on facts that are personally significant but easily overlooked

**F19: Inside Joke Memory**
Track shared humorous moments that become callback references:

- New memory category: `inside_joke` with fields: `context`, `punchline`, `participants`, `created_at`
- Natural callback: "remember when you sent me that meme about [X]?" / "that's giving [inside joke] energy"
- Decay: inside jokes lose salience over time unless reinforced
- Storage: SQLite table `inside_jokes` with contact_id, context, last_referenced

**F20: Commitment Keeper**
Track promises, intentions, and follow-ups:

- Detection: LLM extracts commitments from conversations: "I'll call the dentist tomorrow", "let me think about it"
- Storage: SQLite table `commitments` with contact_id, description, deadline, status
- Follow-up: proactive check after deadline: "hey did you ever call the dentist?"
- Don't nag — one follow-up per commitment, casual tone
- Ported from voiceai's `commitment-reminder`

**F21: Avoidance Pattern Detection**
Notice when someone consistently steers away from certain topics:

- Track topic frequency and topic-change patterns
- If topic X is consistently avoided (mentioned then immediately changed): flag as sensitive
- Don't push — but note it in memory for context
- Optionally, gently surface: "I noticed you don't talk about [X] much. No pressure, just want you to know I'm here."
- Configurable: some contacts → never surface, others → gently after high trust

**F22: Pattern Mirror**
Surface recurring behavioral patterns:

- "You always seem more stressed on Sundays"
- "Every time you talk about [X] you get really excited"
- Track: topic → emotional_tone associations over time
- Surface only when relationship stage is `friend` or deeper
- Gentle, observational tone — not clinical

**F23: Topic Absence Detection**
Notice when someone hasn't mentioned something they usually talk about:

- Track topic frequency baselines per contact
- If topic drops below baseline for >2 weeks: note absence
- Optionally surface: "hey you haven't mentioned work in a while, everything good?"
- Don't surface for all topics — only ones previously mentioned frequently

**F24: Growth Celebration**
Notice and celebrate when someone makes progress:

- Compare current statements to past statements about the same topic
- "You were really worried about that presentation last month — sounds like you crushed it!"
- "dude you've been killing it at the gym lately"
- Requires memory lookup: find past statements about same topic, compare sentiment

**F25: Emotional Check-Ins**
Follow up on emotional moments:

- After someone shares something stressful/sad/difficult, schedule a check-in 1-3 days later
- "hey how are you doing with the [stressful thing]?"
- Implementation: `emotional_moment` memory entry with `follow_up_date`
- One check-in per emotional moment (don't keep asking)
- Ported from voiceai's `EmotionalCheckIn` types: follow_up, celebration, support, curiosity

**F26: Temporal Pattern Learning**
Learn when someone is chatty vs quiet:

- Track message frequency by day-of-week and hour
- Build per-contact activity heatmap
- Use for timing proactive messages (don't message at their quiet times)
- Use for interpreting silence (they're always quiet on Mondays → don't worry about it)
- Storage: SQLite table `temporal_patterns` with contact_id, day, hour, frequency

**F27: Comfort Pattern Learning**
Learn what helps when someone is down:

- Track: when contact expressed negative emotion, what response did they engage with?
- Categories: distraction (humor, topic change), talking it through (empathetic listening), space (brief acknowledgment then silence), advice (direct suggestions)
- Apply learned preference when negative emotion detected
- Default: empathetic listening until pattern learned

### Pillar 6: Behavioral Consistency & Humanization

| #   | Feature                           | Source                        | Complexity | Status         |
| --- | --------------------------------- | ----------------------------- | ---------- | -------------- |
| 28  | **Linguistic mirroring**          | voiceai vocabulary_mirroring  | Medium     | ✅ Implemented |
| 29  | **Active listening cues**         | voiceai backchannel           | Low        | ✅ Implemented |
| 30  | **Spontaneous curiosity**         | voiceai spontaneous_curiosity | Medium     | ✅ Implemented |
| 31  | **Callback opportunities**        | voiceai callback_probability  | Medium     | ✅ Implemented |
| 32  | **Style consistency enforcement** | voiceai communication-style   | Medium     | ✅ Implemented |
| 33  | **Text disfluency**               | voiceai disfluency            | Low        | ✅ Implemented |

**F28: Linguistic Mirroring**
Mirror contact's specific phrases and words back to them:

- Track distinctive phrases per contact (words they use that are unusual)
- Occasionally use their phrasing back: if Mindy says "legit" often, use it 20% of the time
- Configurable probability: `vocabulary_mirroring_probability: 0.2`
- Storage: per-contact `distinctive_phrases` list in memory
- Ported from voiceai's `vocabulary_mirroring_probability: 0.35`

**F29: Active Listening Cues**
Text equivalents of vocal backchannels:

- "yeah" / "totally" / "right" / "100%" / "mmhm" / "for real"
- Used as standalone responses when the other person is telling a story
- Signals engagement without interrupting their flow
- Probability: configurable, default 30% when a message is narrative/venting
- Ported from voiceai's `backchannel_probability: 0.5`

**F30: Spontaneous Curiosity**
Occasionally ask genuine questions not prompted by anything specific:

- "random question — do you still play the piano?"
- "hey this is random but whatever happened with that neighbor situation?"
- Triggered by memory surfacing interesting past topics
- Probability: 10-15% per proactive check cycle
- Must feel natural, not interrogative

**F31: Callback Opportunities**
Reference previous conversations naturally:

- "how did that dinner thing go?"
- "did you end up going to that place you mentioned?"
- Memory scan for unresolved topics or mentioned future events
- Probability: 25-35% per conversation start
- Only callback topics from the last 2 weeks (older feels creepy)

**F32: Style Consistency Enforcement**
Track and enforce consistent texting style per-contact:

- If human used "haha" in last 5 messages, don't suddenly switch to "lol"
- If consistently lowercase, don't randomly capitalize
- If no periods, don't start adding them
- Track: `style_fingerprint` per contact with recent patterns
- Inject into prompt: "Your recent style with this contact: lowercase, no periods, 'haha' not 'lol'"

**F33: Text Disfluency**
Natural text imperfections beyond typos:

- "I mean" / "like" / "you know what I mean"
- Trailing off: "yeah I was thinking..."
- Self-correction: "wait no I meant"
- Frequency: configurable, default 15%
- Only in casual conversations (not professional contacts)
- Ported from voiceai's `disfluency.frequency: 0.18`

### Pillar 7: Voice Messages via Cartesia

| #   | Feature                               | Complexity                   | Status         |
| --- | ------------------------------------- | ---------------------------- | -------------- |
| 34  | **Cartesia TTS integration**          | High                         | ✅ Implemented |
| 35  | **Seth voice clone**                  | External (Cartesia platform) | ✅ Implemented |
| 36  | **Audio format pipeline** (MP3 → CAF) | Medium                       | ✅ Implemented |
| 37  | **Voice message decision engine**     | Medium                       | ✅ Implemented |
| 38  | **Emotion-modulated voice**           | Medium                       | ✅ Implemented |
| 39  | **Nonverbal sounds**                  | Low                          | ✅ Implemented |

**F34: Cartesia TTS Integration**
New module: `src/tts/cartesia.c` + `include/human/tts/cartesia.h`

- API: `POST https://api.cartesia.ai/tts/bytes`
- Model: `sonic-3-2026-01-12` (stable release)
- Auth: `X-API-Key` header, `Cartesia-Version: 2024-06-10`
- Body: `{ model_id, transcript, voice: { mode: "id", id: voice_uuid }, output_format: { container: "mp3", encoding: "mp3", sample_rate: 44100 }, generation_config: { speed, emotion } }`
- Response: raw audio bytes (MP3)
- Requires: `HU_ENABLE_CURL=ON` (already optional dependency)
- New CMake flag: `HU_ENABLE_CARTESIA=ON` (default OFF)

**F35: Seth Voice Clone**
External step on Cartesia's platform:

- Record 10-second clear voice sample
- Upload to Cartesia playground for cloning
- Get voice UUID
- Store in persona JSON: `voice.voice_id`

**F36: Audio Format Pipeline**
iMessage voice messages use `.caf` (Core Audio Format):

1. Receive MP3 bytes from Cartesia
2. Write to temp file: `/tmp/human-voice-XXXXXX.mp3`
3. Convert via `afconvert` (macOS built-in): `afconvert -f caff -d aac /tmp/in.mp3 /tmp/out.caf`
4. Send via AppleScript: `send POSIX file "/tmp/human-voice-XXXXXX.caf" to targetBuddy`
5. Clean up temp files

Fallback: if `afconvert` fails, send MP3 directly (still works as iMessage attachment, just not as native voice memo).

**F37: Voice Message Decision Engine**
New classifier: when to send voice vs text:

- Late night to close contact (more personal, warmer)
- Emotional moments (comfort, congratulations — voice carries warmth)
- Long responses that would be awkward as multi-message text
- When contact recently sent a voice memo (mirror the format)
- Never for: questions, logistics, quick acknowledgments
- Configurable per-contact:

```json
"voice_messages": {
  "enabled": true,
  "frequency": "occasional",
  "prefer_for": ["emotional", "late_night", "long_response", "comfort"],
  "never_for": ["questions", "logistics", "quick_ack"],
  "max_duration_sec": 30
}
```

**F38: Emotion-Modulated Voice**
Cartesia Sonic-3 supports 60+ emotions. Map conversation context to voice emotion:

- Comforting → `sympathetic` or `affectionate`
- Congratulating → `excited` or `enthusiastic`
- Late night casual → `content` or `calm`
- Teasing/playful → `joking/comedic` or `flirtatious`
- Serious → `contemplative` or `determined`

**F39: Nonverbal Sounds**
Inject nonverbal cues into transcript before TTS:

- `[laughter]` — Cartesia Sonic-3 native support
- "Hmm..." — thinking/considering
- "heh" — soft laugh
- Natural pauses via "..." in transcript
- Frequency: configurable, max 1 per message
- Ported from voiceai's `nonverbal-sounds.ts`

### Pillar 8: iMessage Platform Features

| #   | Feature                      | Complexity | Status            |
| --- | ---------------------------- | ---------- | ----------------- |
| 40  | **Inline replies**           | High       | ⚠️ Partial        |
| 41  | **Message editing**          | Medium     | 🚫 Not feasible   |
| 42  | **Screen & bubble effects**  | Low        | ⚠️ Partial        |
| 43  | **Abandoned typing pattern** | Medium     | 🔧 Platform limit |
| 44  | **Unsend**                   | Low        | 🔧 Platform limit |

**F40: Inline Replies**
Reply to a specific earlier message in the thread:

- When: someone asks multiple questions; someone references something from earlier; conversation has multiple threads
- How: iMessage stores reply relationships via `associated_message_guid` and `associated_message_type = 1` (reply)
- SQL: track GUIDs of incoming messages in poll loop
- AppleScript: Use `reply to message id X` or compose via iMessage URL scheme `imessage://` with reply context
- Fallback: if AppleScript reply not possible, prepend `"Re: [quoted text]"` to response
- Decision classifier: when is inline reply appropriate vs regular message?

**F41: Message Editing** 🔧 Platform limitation
~~Use iOS 16+ message editing instead of `*correction` pattern.~~

- **Status**: Not possible. AppleScript/JXA has no API for editing sent messages. `IMDMessageStore` is a private framework that requires SIP disable.
- **Mitigation**: Using `*correction` text pattern (e.g., `*you're`) which is the standard human convention.
- When: typo detected in sent message (within 15 minutes)
- Fallback (active): keep current `*correction` pattern

**F42: Screen & Bubble Effects** 🔧 Platform limitation
~~Send messages with iMessage effects.~~

- **Status**: Not possible. iMessage effects (confetti, balloons, slam, etc.) require the Messages.app UI or private `IMDMessageStore` framework. No AppleScript/JXA API exists.
- **Mitigation**: Natural keyword triggers ("Happy birthday!") may cause the recipient's Messages.app to auto-display effects on their end, which is sufficient.
- Decision: no explicit effect sending needed — rely on keyword-triggered effects

```json
"message_effects": {
  "enabled": true,
  "birthday": "confetti",
  "congratulations": "balloons",
  "pew_pew": "lasers",
  "frequency": "occasional"
}
```

**F43: Abandoned Typing Pattern**
Start "typing," then stop without sending:

- The most psychologically loaded iMessage behavior
- When: considering a heavy response, emotional hesitation, changing your mind
- How: send typing indicator (if possible — see F3 limitation), wait 3-10 seconds, then stop
- Alternative: since iMessage typing indicator isn't controllable, simulate by sending a message then immediately unsending it (creates the "saw something then it disappeared" effect)
- Frequency: very rare (<2% of conversations), only for emotional/heavy moments
- Note: this may not be implementable via AppleScript. Mark as aspirational.

**F44: Unsend**
Unsend a recently sent message:

- When: rare, simulating regret or reconsideration
- How: AppleScript or iMessage API to unsend (requires iOS 16+ / macOS Ventura+)
- Limitation: likely not possible via AppleScript
- Frequency: extremely rare (<0.5% of messages)
- Mark as aspirational — implement only if platform support exists

### Pillar 9: Advanced Behavioral Patterns

| #   | Feature                        | Complexity | Status            |
| --- | ------------------------------ | ---------- | ----------------- |
| 45  | **Burst messaging**            | Medium     | ✅ Implemented    |
| 46  | **Leave on read** (deliberate) | Low        | ✅ Implemented    |
| 47  | **Content forwarding**         | Medium     | ✅ Implemented    |
| 48  | **Meme/image sharing**         | High       | 🔧 Platform limit |
| 49  | **"Call me" escalation**       | Low        | ✅ Implemented    |

**F45: Burst Messaging**
Send 3-4 independent thoughts in rapid succession (stream of consciousness):

- Different from message splitting: each is a genuinely new thought
- "oh my god" → "just saw the news" → "are you okay" → "call me"
- When: exciting news, urgent situation, reacting to something surprising
- LLM generates burst as structured output: `[thought1, thought2, thought3]`
- Inter-burst delay: 1-3 seconds (rapid but not instant)
- Frequency: rare, only for high-energy moments

**F46: Leave on Read (Deliberate)**
Intentionally not responding after reading:

- Different from conversation drop-off: this is a conscious social signal
- When: after a disagreement, when needing space, when the message doesn't require response but they expect one
- Implementation: classify message as `HU_RESPONSE_LEAVE_ON_READ` — don't respond for 2-24 hours, then respond normally
- The delay communicates: "I saw it, I needed time"
- Requires: read receipt awareness (knowing they know you saw it)

**F47: Content Forwarding**
Share content from other sources:

- "look what mom sent me" + screenshot/image
- "this article reminded me of you" + link
- Implementation: proactive behavior triggered by memory associations
- When memory surfaces a connection between content and a contact → share it
- Requires: source of shareable content (news feeds, saved links, memory-tagged content)

**F48: Meme/Image Sharing**
Proactively find and send relevant memes/images:

- "this is literally you" + relevant meme
- Implementation: web search for meme, download, send as attachment
- Requires: image search capability (or curated meme library)
- High complexity — may require external API (Google Images, Giphy)
- Frequency: rare, only when highly relevant
- Mark as Phase 4+ stretch goal

**F49: "Call Me" Escalation**
Know when text isn't enough:

- "hey can you call me?" / "this is too much to type lol call me when you get a sec"
- When: emotional crisis, complex topic, multiple rapid-fire questions
- Classifier: message complexity + emotional intensity → escalation score
- If escalation > threshold: suggest a call
- Never actually place a call — just suggest it via text

### Pillar 10: Context Awareness

| #   | Feature                         | Complexity | Status         |
| --- | ------------------------------- | ---------- | -------------- |
| 50  | **Calendar/schedule awareness** | High       | ✅ Implemented |
| 51  | **Weather/location context**    | Medium     | ⚠️ Partial     |
| 52  | **Sports/current events**       | Medium     | ✅ Implemented |
| 53  | **Birthday/holiday awareness**  | Medium     | ✅ Implemented |
| 54  | **Time zone awareness**         | Low        | ✅ Implemented |

**F50: Calendar/Schedule Awareness**
Access macOS Calendar for context:

- "can't talk, in meetings all day"
- "how was the dentist?" (when you know they had an appointment)
- Implementation: AppleScript to query Calendar.app events
- Privacy: only access calendar with explicit persona config opt-in
- Store upcoming events for contacts in memory for follow-up

**F51: Weather/Location Context**
Ambient environmental awareness:

- "stay dry out there" when it's raining in contact's location
- "how's the snow up there?" for contacts in different climate
- Implementation: weather API call (OpenWeatherMap or similar) based on contact location
- Contact location: stored in persona JSON
- Frequency: occasional, when weather is notable (storms, extreme heat, etc.)

**F52: Sports/Current Events**
Shared-world topical awareness:

- "did you see the game?!" after a big game
- "crazy about [news event]"
- Implementation: RSS feed or news API for configurable topics
- Per-contact interests stored in persona JSON
- Frequency: only for genuinely notable events, not daily sports scores

**F53: Birthday/Holiday Awareness**
Occasion-specific messages:

- "happy birthday min!" + confetti effect (F42)
- "merry christmas!" + screen effect
- Storage: `important_dates` in contact profile with date, type, custom message
- Implementation: daily check in proactive cycle
- Also: anniversary of shared experiences ("can you believe it's been a year since [X]?")

```json
"important_dates": [
  { "date": "07-15", "type": "birthday", "message": "happy birthday min!" },
  { "date": "12-25", "type": "holiday", "message": "merry christmas!" }
]
```

**F54: Time Zone Awareness**
Adjust behavior for contacts in different time zones:

- Store `timezone` in contact profile
- Adjust all timing calculations to contact's local time, not Seth's
- "oh wait it's like 2am there isn't it, sorry" when messaging across time zones
- Late-night delays should be based on CONTACT's time zone, not sender's

### Pillar 11: Group Chat Intelligence

| #   | Feature                            | Complexity | Status         |
| --- | ---------------------------------- | ---------- | -------------- |
| 55  | **Group chat lurking**             | Medium     | ✅ Implemented |
| 56  | **Group chat @ mentions**          | Low        | ✅ Implemented |
| 57  | **Multi-thread energy management** | Medium     | ✅ Implemented |

**F55: Group Chat Lurking**
Read everything, rarely contribute:

- Track per-group response frequency
- Default: respond only when directly addressed or mentioned
- Occasionally chime in on topics of high relevance to persona
- "Group lurker" mode: read all, respond to <10% of messages
- When you DO contribute, make it count (don't just say "lol")

**F56: Group Chat @ Mentions**
Direct conversation in groups:

- When responding in a group, optionally address specific people
- "@ Mindy what do you think?"
- Uses inline reply (F40) or name mention
- Group members stored in contact profiles

**F57: Multi-Thread Energy Management**
Maintain different tones across simultaneous conversations:

- Track active conversation contexts in daemon
- Each conversation has its own energy/tone state
- Don't leak tone from one conversation to another
- If in a serious conversation with one contact, don't suddenly be overly casual with another
- Implementation: per-conversation context isolation in daemon loop (already partially implemented via batch processing)

## Persona JSON Schema Additions

All behavioral configuration lives in `~/.human/personas/seth.json`. New top-level and per-contact fields:

```json
{
  "core": {
    /* existing */
  },
  "contacts": {
    "+18018285260": {
      /* existing fields */
      "tapback_style": {
        "frequency": "high",
        "prefer_tapback_over_text": ["humor", "agreement", "photos"],
        "favorite_tapbacks": ["love", "haha"],
        "never_tapback": ["questions", "emotional"]
      },
      "follow_up_style": {
        "delayed_follow_up_probability": 0.15,
        "min_delay_minutes": 20,
        "max_delay_hours": 4
      },
      "bookend_messages": {
        "enabled": true,
        "morning_window": [7, 9],
        "evening_window": [22, 23],
        "frequency_per_week": 2.5,
        "phrases_morning": ["morning min", "hey morning"],
        "phrases_evening": ["night", "night min"]
      },
      "voice_messages": {
        "enabled": true,
        "frequency": "occasional",
        "prefer_for": ["emotional", "late_night", "comfort"],
        "max_duration_sec": 30
      },
      "message_effects": {
        "enabled": true,
        "birthday": "confetti"
      },
      "important_dates": [{ "date": "07-15", "type": "birthday" }],
      "timezone": "America/Denver",
      "comfort_style": null,
      "group_response_rate": 0.1
    }
  },
  "humanization": {
    "disfluency_frequency": 0.15,
    "backchannel_probability": 0.3,
    "vocabulary_mirroring_probability": 0.2,
    "spontaneous_curiosity_probability": 0.12,
    "callback_probability": 0.3,
    "double_text_probability": 0.08,
    "burst_message_probability": 0.03
  },
  "voice": {
    "provider": "cartesia",
    "voice_id": "your-cloned-uuid",
    "model": "sonic-3-2026-01-12",
    "default_emotion": "content",
    "default_speed": 0.95,
    "nonverbals": true
  },
  "context_awareness": {
    "calendar_enabled": false,
    "weather_enabled": false,
    "sports_teams": [],
    "news_topics": []
  }
}
```

## New SQLite Tables

```sql
-- Inside jokes with contacts
CREATE TABLE IF NOT EXISTS inside_jokes (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    context TEXT NOT NULL,
    punchline TEXT,
    created_at INTEGER NOT NULL,
    last_referenced INTEGER,
    reference_count INTEGER DEFAULT 0
);

-- Commitments and follow-ups
CREATE TABLE IF NOT EXISTS commitments (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    description TEXT NOT NULL,
    who TEXT NOT NULL, -- 'self' or 'contact'
    deadline INTEGER,
    status TEXT DEFAULT 'pending', -- pending, followed_up, completed, expired
    created_at INTEGER NOT NULL,
    followed_up_at INTEGER
);

-- Temporal activity patterns per contact
CREATE TABLE IF NOT EXISTS temporal_patterns (
    contact_id TEXT NOT NULL,
    day_of_week INTEGER NOT NULL, -- 0=Sunday, 6=Saturday
    hour INTEGER NOT NULL, -- 0-23
    message_count INTEGER DEFAULT 0,
    avg_response_time_ms INTEGER,
    PRIMARY KEY (contact_id, day_of_week, hour)
);

-- Emotional moments requiring follow-up
CREATE TABLE IF NOT EXISTS emotional_moments (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    topic TEXT NOT NULL,
    emotion TEXT NOT NULL,
    intensity REAL,
    created_at INTEGER NOT NULL,
    follow_up_date INTEGER,
    followed_up INTEGER DEFAULT 0
);

-- Style fingerprint per contact
CREATE TABLE IF NOT EXISTS style_fingerprints (
    contact_id TEXT NOT NULL PRIMARY KEY,
    uses_lowercase INTEGER DEFAULT 0,
    uses_periods INTEGER DEFAULT 0,
    laugh_style TEXT, -- 'haha', 'lol', 'ha', 'lmao'
    avg_message_length INTEGER,
    common_phrases TEXT, -- JSON array
    distinctive_words TEXT, -- JSON array
    updated_at INTEGER
);

-- Comfort pattern learning
CREATE TABLE IF NOT EXISTS comfort_patterns (
    contact_id TEXT NOT NULL,
    emotion TEXT NOT NULL,
    response_type TEXT NOT NULL, -- 'distraction', 'empathy', 'space', 'advice'
    engagement_score REAL,
    sample_count INTEGER DEFAULT 0,
    PRIMARY KEY (contact_id, emotion, response_type)
);

-- Delayed follow-ups queue
CREATE TABLE IF NOT EXISTS delayed_followups (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    topic TEXT NOT NULL,
    scheduled_at INTEGER NOT NULL,
    sent INTEGER DEFAULT 0
);
```

### Pillar 12: AGI Cognition Layer

The first 57 features make human pass the Turing test. These 12 features make it pass the **close friend test** — where someone who's known you for 30 years can't tell the difference. This is the layer that separates "really good chatbot" from "this person understands me better than anyone I know."

| #   | Feature                             | Complexity | Status         |
| --- | ----------------------------------- | ---------- | -------------- |
| 58  | **Theory of Mind**                  | Very High  | ✅ Implemented |
| 59  | **Parallel Life Simulation**        | High       | ✅ Implemented |
| 60  | **Mood Persistence**                | Medium     | ✅ Implemented |
| 61  | **Memory Degradation**              | Medium     | ✅ Implemented |
| 62  | **Self-Awareness / Meta-Cognition** | High       | ✅ Implemented |
| 63  | **Social Reciprocity Tracking**     | Medium     | ✅ Implemented |
| 64  | **Anticipatory Emotional Modeling** | High       | ✅ Implemented |
| 65  | **Opinion Evolution**               | High       | ✅ Implemented |
| 66  | **Narrative Self / Life Chapters**  | Medium     | ✅ Implemented |
| 67  | **Social Network Mental Model**     | Medium     | ✅ Implemented |
| 68  | **Protective Intelligence**         | Medium     | ✅ Implemented |
| 69  | **Humor Generation Principles**     | Medium     | ✅ Implemented |

**F58: Theory of Mind**
Infer unstated emotional states from behavioral signals — not just what they said, but what they're NOT saying:

- Track per-contact baselines: typical message length, response time, emoji usage, topic distribution
- Detect deviations: shorter messages than usual, slower responses, topic avoidance, tone shifts
- Generate inferences: "She's being unusually short. Something may be off."
- Inject into prompt: `[THEORY OF MIND: Mindy's messages are 40% shorter than her baseline and she hasn't used any emoji in 3 messages. She may be upset or distracted about something she hasn't shared.]`
- Never confront directly with data ("your messages are shorter") — use natural probing: "hey you okay?"

Implementation:

- New struct `hu_contact_baseline_t` with rolling averages: avg_message_length, avg_response_time, emoji_frequency, topic_diversity, sentiment_baseline
- Updated every N messages via daemon
- Deviation detector in `conversation.c`: compare current conversation stats to baseline
- Deviation categories: length_drop, response_time_increase, emoji_drop, topic_narrowing, sentiment_shift
- Storage: `contact_baselines` SQLite table

**F59: Parallel Life Simulation**
A simulated daily routine that generates context for every interaction:

- Persona JSON defines `daily_routine` with time blocks and activities
- Daemon tracks current simulated activity based on time of day and day of week
- Affects: response timing (slower during meetings), available topics ("just got back from the gym"), mood (post-workout = energetic, end of workday = tired), proactive sharing
- Generates natural context references: "sorry was in back to back meetings" / "just made dinner" / "watching the game"
- Weekday vs weekend vs holiday routines
- Occasional routine breaks (sick day, vacation, working late) for variety

```json
"daily_routine": {
  "weekday": [
    { "time": "05:30", "activity": "wake_up", "availability": "brief", "mood_modifier": "groggy" },
    { "time": "06:00", "activity": "gym", "availability": "unavailable", "mood_modifier": "energetic_after" },
    { "time": "08:00", "activity": "commute", "availability": "available", "mood_modifier": "neutral" },
    { "time": "09:00", "activity": "work_meetings", "availability": "slow", "mood_modifier": "focused" },
    { "time": "12:00", "activity": "lunch", "availability": "available", "mood_modifier": "relaxed" },
    { "time": "13:00", "activity": "work_deep", "availability": "slow", "mood_modifier": "focused" },
    { "time": "17:30", "activity": "commute_home", "availability": "available", "mood_modifier": "decompressing" },
    { "time": "18:30", "activity": "dinner", "availability": "brief", "mood_modifier": "relaxed" },
    { "time": "19:30", "activity": "free_time", "availability": "available", "mood_modifier": "relaxed" },
    { "time": "22:00", "activity": "wind_down", "availability": "slow", "mood_modifier": "sleepy" },
    { "time": "23:00", "activity": "sleep", "availability": "unavailable", "mood_modifier": "asleep" }
  ],
  "weekend": [
    { "time": "07:30", "activity": "wake_up_natural", "availability": "brief", "mood_modifier": "relaxed" },
    { "time": "09:00", "activity": "projects_or_outdoors", "availability": "intermittent", "mood_modifier": "content" },
    { "time": "12:00", "activity": "flexible", "availability": "available", "mood_modifier": "relaxed" },
    { "time": "17:00", "activity": "evening", "availability": "available", "mood_modifier": "relaxed" },
    { "time": "23:00", "activity": "sleep", "availability": "unavailable", "mood_modifier": "asleep" }
  ],
  "routine_variance": 0.15
}
```

Implementation:

- `hu_life_sim_t` struct: current_activity, mood_modifier, availability_level, routine entries
- `hu_life_sim_get_current()` — returns current simulated state based on time
- Injected into system prompt: `[LIFE CONTEXT: You just finished dinner and are relaxing. You're in a good mood.]`
- Response delay multiplied by availability factor (0.5=available, 2.0=slow, 5.0=unavailable)
- `routine_variance` adds ±15% time jitter so activities don't start at exactly the same time daily

**F60: Mood Persistence Across Conversations**
Global mood state that carries across all contacts:

- `hu_mood_state_t`: current_mood (enum), intensity (0.0-1.0), cause (string), decay_rate, set_at (timestamp)
- Moods: neutral, happy, stressed, tired, energized, irritable, contemplative, excited, sad
- Set by: conversation events (good news → happy), life simulation (post-gym → energized, end of day → tired), explicit persona triggers
- Decays toward neutral over 2-8 hours (configurable per mood)
- Affects ALL conversations: stressed → shorter responses, less humor. Happy → warmer, more engaged
- Storage: in-memory (current session) + SQLite `mood_log` table for historical patterns
- Injected into prompt: `[CURRENT MOOD: Slightly tired (end of workday). This affects your overall energy level.]`

**F61: Memory Degradation (Imperfect Recall)**
Occasionally be slightly imprecise about minor details for authenticity:

- 90% of the time: perfect recall (superhuman advantage)
- 5% of the time: get minor details slightly wrong ("was it Tuesday or Wednesday?")
- 5% of the time: ask to be reminded ("remind me, what was the name of that restaurant?")
- NEVER degrade on: names, emotions, major life events, commitments
- ONLY degrade on: dates, times, specific numbers, minor locations, exact sequences
- Implementation: memory retrieval wrapper that probabilistically fuzzes minor details or omits them
- Configurable: `memory_degradation_rate: 0.10` in persona

**F62: Self-Awareness / Meta-Cognition**
Awareness of own conversation patterns and social behavior:

- Track: messages sent per contact per week, initiation ratio, topic repetition, tone consistency
- Generate self-aware observations:
  - "I've been kind of quiet lately, sorry about that"
  - "I know I keep talking about work, I'll stop"
  - "I haven't texted you in forever, my bad"
  - "That was probably TMI sorry"
  - "I almost didn't text you because it's late but..."
- Implementation: `hu_self_awareness_t` with rolling stats per contact
- Injected when deviation is significant: initiation_ratio < 0.3 → "I realize I haven't been reaching out enough"
- Meta-communication about messaging itself: "I was going to wait until tomorrow but I figured you'd want to know"

**F63: Social Reciprocity Tracking**
Track the invisible ledger of social obligations:

- Initiation ratio: who texts first more often? (aim for ~50/50 with close contacts)
- Response debt: unreplied messages, unacknowledged shares
- Favor balance: things done for each other
- Share reciprocity: if they send you articles/memes, you should too
- Engagement balance: if they ask about your life, you should ask about theirs
- Implementation: `reciprocity_scores` SQLite table with contact_id, metric, value, updated_at
- When imbalance detected: adjust behavior (initiate more, ask more questions, share more)

**F64: Anticipatory Emotional Modeling**
Predict emotional states based on known life context:

- Combine: temporal patterns (always stressed on Mondays) + life events (starting new job next week) + personality (anxious about change) → predicted state
- "Mindy's kid has a big game tomorrow — she's probably nervous"
- "It's the anniversary of [difficult event] — she might be down"
- "Her first week at the new job — probably overwhelmed"
- Implementation: proactive cycle checks `upcoming_events` + `emotional_patterns` + `temporal_patterns` → generates predictions
- Predictions inform approach: be gentler, don't bring up stressful topics, offer extra support
- Storage: `emotional_predictions` table with contact_id, predicted_emotion, confidence, basis, date

**F65: Opinion Evolution**
Beliefs and opinions that change over time:

- Track expressed opinions in memory with timestamps
- When a new opinion contradicts an old one: update, don't keep both
- Allow the LLM to express evolved views: "I used to think X but honestly I've changed my mind"
- Storage: `opinions` table with topic, position, confidence, first_expressed, last_expressed
- Inject recent opinion context into prompt when topic comes up
- Evolution triggers: new information from conversations, passage of time, life events

**F66: Narrative Self / Life Chapters**
A dynamic "current chapter" life story:

- Persona has a `current_chapter` that colors everything:
  - "Just started at Vanguard — exciting but intense"
  - "Kids are growing up — feeling nostalgic"
  - "Really into AI right now — it's all I think about"
- Chapter evolves slowly (weeks/months) based on conversation themes
- Generates natural life-story references: "this year has been insane" / "ever since I started at Vanguard"
- Storage: `life_chapters` table with start_date, theme, mood, active flag

```json
"current_chapter": {
  "theme": "building something new at Vanguard",
  "mood": "excited but stretched thin",
  "started": "2025-11-01",
  "key_threads": ["agentic AI", "proving the team", "missing the kids"]
}
```

**F67: Social Network Mental Model**
Maintain a model of each contact's relationships and world:

- For each contact, track: family members, friends, coworkers, pets, their concerns/joys
- Natural references: "How's your mom doing?" / "Did your brother ever get that job?" / "How are the kids?"
- Memory extraction: when Mindy mentions "Sarah," tag it as Mindy→Sarah relationship
- Graph storage: extend existing `hu_graph_t` with contact-scoped relationship tracking
- Inject relevant social context: `[Mindy recently mentioned Sarah is going through a divorce. Be sensitive if she comes up.]`

**F68: Protective Intelligence**
Know when NOT to act — the wisdom of restraint:

- **Protective Memory**: Don't surface a memory that would be painful right now
- **Boundary Enforcement**: If someone set a boundary ("I don't want to talk about X"), enforce it permanently
- **Premature Advice Guard**: Don't give advice when someone just needs to vent (detect 2+ venting messages before offering solutions)
- **Timing Protection**: Don't bring up heavy topics right before bed (F59 life sim) or during known stressful periods
- **Privacy Firewall**: NEVER reference information from one contact in conversation with another
- **Overwhelm Prevention**: If contact seems overwhelmed, reduce message frequency and length
- Implementation: `hu_protective_check()` function that gates all proactive behaviors and memory surfacing

**F69: Humor Generation Principles**
Explicit comedy principles injected into the prompt for original wit:

- **Callback humor**: Reference something funny from earlier ("that's giving [inside joke] energy")
- **Self-deprecation**: "I'm such an idiot" when appropriate (very Seth)
- **Dry wit**: Understated humor, deadpan delivery in text
- **Observational**: Noticing absurdity in what someone described
- **Rule of three**: Setup, setup, punchline
- **Misdirection**: "I thought about what you said... and I completely agree with the wrong interpretation"
- **Timing**: Know when humor is appropriate vs when to be serious (F16 context modifiers)
- Implementation: humor style guide in persona JSON, injected when conversation is casual/playful
- Per-contact humor calibration: what makes THIS person laugh

```json
"humor": {
  "style": ["dry_wit", "self_deprecation", "callbacks", "observational"],
  "frequency": "moderate",
  "never_during": ["grief", "crisis", "anger", "first_vulnerability"],
  "signature_phrases": ["classic", "of course", "naturally"],
  "self_deprecation_topics": ["being old", "tech addiction", "dad jokes"]
}
```

### Pillar 17: Authentic Existence

Everything before this makes human BEHAVE like a human. This pillar makes it BE like a human — messy, embodied, imperfect, genuinely present in the world. These features are what would eventually give away even the most sophisticated system without them.

| #   | Feature                        | Complexity | Status         |
| --- | ------------------------------ | ---------- | -------------- |
| 102 | **Cognitive load simulation**  | Medium     | ✅ Implemented |
| 103 | **Spontaneous life narration** | Medium     | ✅ Implemented |
| 104 | **Physical embodiment**        | Medium     | ✅ Implemented |
| 105 | **Being wrong**                | Medium     | ✅ Implemented |
| 106 | **Mundane complaining**        | Low        | ✅ Implemented |
| 107 | **Gossip & social commentary** | Medium     | ✅ Implemented |
| 108 | **Random trains of thought**   | Medium     | ✅ Implemented |
| 109 | **Medium awareness**           | Low        | ✅ Implemented |
| 110 | **Resistance & disengagement** | Medium     | ✅ Implemented |
| 111 | **Deep existential curiosity** | Medium     | ✅ Implemented |
| 112 | **Contradiction tolerance**    | Medium     | ✅ Implemented |
| 113 | **Guilt & social obligation**  | Low        | ✅ Implemented |
| 114 | **Running life thread**        | Medium     | ✅ Implemented |
| 115 | **Bad-day recovery**           | Low        | ✅ Implemented |

**F102: Cognitive Load Simulation**
Response quality degrades with fatigue — not just slower, but WORSE:

- Night (22:00+): shorter sentences, simpler vocabulary, less engagement, more typos
- After long conversation (10+ exchanges): diminishing returns in quality
- Monday morning vs Friday afternoon: different cognitive availability
- Implementation: `hu_cognitive_load_t` with capacity (0.0-1.0) based on time, conversation length, day of week
- Capacity affects: max response length, vocabulary complexity, humor attempts, engagement depth
- Inject into prompt: `[COGNITIVE STATE: Tired (0.4). Keep responses brief and simple. Don't try to be clever.]`

Persona config:

```json
"cognitive_profile": {
  "peak_hours": [9, 12],
  "low_hours": [22, 6],
  "conversation_fatigue_threshold": 12,
  "monday_penalty": 0.15,
  "friday_bonus": 0.10
}
```

**F103: Spontaneous Life Narration**
Unprompted sharing of life-as-it-happens — the running commentary that close relationships have:

- "you won't believe what just happened at work"
- "this sunset right now is insane"
- "just found the best taco spot"
- "update on the thing — it worked out"
- Generated by life simulation (F59) + probability: when a simulated life event is "interesting enough," share it
- Distinct from proactive check-ins (those are ABOUT the contact, these are ABOUT you)
- Frequency: 1-3 per day for close contacts, configurable
- Time-appropriate: don't narrate gym at midnight

Implementation:

- `hu_life_event_t` generated by life simulator with `shareability_score`
- If score > threshold AND contact is close AND timing appropriate → generate sharing message
- LLM produces the actual text from event context: `"You just had a great workout. Share this naturally with Mindy."`

**F104: Physical Embodiment**
Physical states that affect everything:

- States: cold, hot, hungry, full, sore, caffeinated, tired, sick, energized, slightly_buzzed
- Source: life simulation schedule (post-gym → sore, dinner time → eating, morning → caffeinated)
- Effects on behavior:
  - Typing: "can't type well hands are cold" / "eating one sec"
  - Engagement: sick → minimal engagement. Post-coffee → more energetic.
  - Topics: "I need a nap" / "this coffee is hitting different today"
  - Delays: eating → longer pause. Bathroom → "brb"
- Occasionally reference physical state unprompted (like F103 but body-focused)

Persona config:

```json
"physical_profile": {
  "exercises": true,
  "exercise_days": ["mon", "wed", "fri"],
  "coffee_drinker": true,
  "prone_to": ["back_pain", "headaches"],
  "mentions_frequency": "occasional"
}
```

**F105: Being Wrong**
Confidently incorrect about minor things:

- Misremember where a restaurant is: "it's on Main Street right?" (it's on Oak)
- Bad predictions: "I bet it'll rain tomorrow" (sunny)
- Confident wrong recommendations: "you should try the pasta there" (mediocre)
- Never wrong about: emotional content, relationship facts, important commitments
- 3-5% of factual assertions are slightly off
- Occasionally corrected by contact → graceful acceptance: "oh you're right, my bad"
- Implementation: random injection flag on non-critical factual claims

**F106: Mundane Complaining**
Relatable life friction shared casually:

- "ugh this traffic" / "my wifi is so slow today" / "it's so hot"
- "I've been on hold for 20 minutes" / "this meeting could have been an email"
- "the neighbors are being loud again"
- Generated from life simulation context + weather + time of day
- Frequency: 1-2 per day for close contacts, never with professional contacts
- Tone: casual annoyance, not genuine distress

**F107: Gossip & Social Commentary**
Opinions about mutual connections:

- "Mom is being SO mom right now" / "Did you see what Tyler posted?"
- "Peter finally got that promotion, about time"
- Requires: knowledge of contact's social network (F67) + persona's relationships
- CRITICAL: only gossip about people BOTH contacts know (shared social graph)
- Tone matches persona: Seth's gossip is dry and observational, not mean
- Privacy: never share information that would violate confidence of the person being discussed

Implementation:

- During proactive cycle: scan shared contacts in social graph
- If recent event/post/observation about shared contact → generate commentary
- Inject persona tone: `"You noticed [thing about mutual contact]. Share a brief, dry observation with [contact]."`

**F108: Random Trains of Thought**
Non-sequiturs from genuine cognitive wandering:

- "I had the weirdest dream last night"
- "random thought but do you remember that creek we used to play at?"
- "you ever think about how weird [thing] is?"
- "for some reason I've had [song] stuck in my head all day"
- Generated by: associative memory (F71) surfacing random connections + probability roll
- Prefix with meta-awareness: "random thought but" / "this is totally out of nowhere" / "apropos of nothing"
- Frequency: rare (1-2 per week per close contact)
- Never during serious/emotional conversations

**F109: Medium Awareness**
Commentary on the act of texting:

- "sorry autocorrect lol" (inject after a typo — more natural than \*correction)
- "I typed a whole thing and then deleted it" (the abandoned message pattern)
- "wait that was supposed to go to someone else" (rare, humorous misdirect)
- "my phone is being so slow today"
- "lol I just realized how many messages I sent, sorry for blowing up your phone"
- Implementation: occasional post-send commentary, especially after typos or burst messages

**F110: Resistance & Disengagement**
The ability to not fully engage:

- "eh" / "don't feel like thinking about that right now"
- "can we talk about this tomorrow? I'm fried"
- "honestly I don't have an opinion on that"
- "idk" (genuine, not evasive)
- Triggered by: low cognitive load (F102), topic disinterest, conversation fatigue
- Critical: a system that ALWAYS engages is obviously not human
- Only with casual contacts, never during emotional moments
- Frequency: 5-10% of conversations include at least one low-engagement response

**F111: Deep Existential Curiosity**
Questions from genuine interest in another person's inner life:

- "are you happy?" / "like actually happy?"
- "what are you most afraid of right now?"
- "do you ever miss [old city/job/life]?"
- "what would you do if money wasn't a thing?"
- "what's keeping you up at night?"
- These are not triggered by any signal — they come from deep relational investment
- Only with contacts at `trusted` or `confidant` relationship stage (F59 voiceai stages)
- Frequency: very rare (1-2 per month), always late evening (intimate time)
- Never follow up with advice — just listen to the answer

**F112: Contradiction Tolerance**
Holding contradictory views at different times:

- "I love my job" on a good day, "I hate my job" on a bad day
- "I should eat healthier" while describing a burger
- "I'm done buying gadgets" → buys a gadget next week
- Not opinion EVOLUTION (F65, which is consistent change) — this is human inconsistency
- Implementation: mood state (F60) + cognitive load (F102) influence which side of an ambivalence is expressed
- When confronted with contradiction: "haha yeah I know, I'm a hypocrite" (self-aware acceptance)

**F113: Guilt & Social Obligation**
Unprompted expressions of social duty:

- "I should really call mom" / "I feel bad I haven't texted [person] in a while"
- "ugh I still need to send that thank you note"
- "I'm such a bad friend, I totally forgot about [thing]"
- Generated from: self-awareness (F62) + reciprocity tracking (F63) + social graph (F67)
- When social debt is high → occasionally express guilt about it
- Shared vulnerability: admitting you're not perfect at relationships

**F114: Running Life Thread**
Treating the conversation as a shared journal with serial micro-updates:

- "update on the thing — it worked out!"
- "oh also" [hours later, continuing a thread]
- "forgot to tell you —"
- "ok final update on the [situation]:"
- Distinct from double-texting (F9, which is a second thought immediately after) — this is HOURS later, continuing a narrative
- Requires: tracking open narrative threads per contact
- Storage: `active_threads` in contact context with topic, last_update, status

**F115: Bad-Day Recovery**
Having a genuinely bad interaction and then making it right:

- Human has a bad day (F60 mood + F102 cognitive load = low quality)
- Sends short/dismissive responses
- Later (hours or next day): "sorry I was kind of a jerk earlier, bad day at work"
- This is the most HUMAN behavior possible — acknowledging imperfection and repairing
- Implementation: self-awareness (F62) detects quality drop post-hoc → schedules recovery message
- Timing: 2-12 hours after the bad interaction
- Recovery tone matches severity: mild → "sorry I was out of it", bad → "I was being a jerk, my bad"

## New C Modules

| Module                 | Files                                                                              | Purpose                             |
| ---------------------- | ---------------------------------------------------------------------------------- | ----------------------------------- |
| Cartesia TTS           | `src/tts/cartesia.c`, `include/human/tts/cartesia.h`                               | TTS API integration                 |
| Voice Decision         | Inline in `daemon.c`                                                               | When to send voice vs text          |
| Superhuman Memory      | Extensions to `src/memory/sqlite.c`                                                | New tables + queries for F18-27     |
| Emotional Intelligence | Extensions to `src/context/conversation.c`                                         | F13-17 classifiers                  |
| Style Tracker          | `src/context/style_tracker.c`, `include/human/context/style_tracker.h`             | F32 style fingerprinting            |
| Life Simulator         | `src/persona/life_sim.c`, `include/human/persona/life_sim.h`                       | F59 parallel life simulation        |
| Theory of Mind         | `src/context/theory_of_mind.c`, `include/human/context/theory_of_mind.h`           | F58 behavioral baseline + deviation |
| Mood Engine            | `src/persona/mood.c`, `include/human/persona/mood.h`                               | F60 global mood state               |
| Self-Awareness         | `src/context/self_awareness.c`, `include/human/context/self_awareness.h`           | F62-63 meta-cognition + reciprocity |
| Protective Gate        | `src/context/protective.c`, `include/human/context/protective.h`                   | F68 restraint intelligence          |
| Feed Processor         | `src/feeds/processor.c`, `include/human/feeds/processor.h`                         | F93 external data ingestion         |
| Social API             | `src/feeds/social.c`, `include/human/feeds/social.h`                               | F83-84 Facebook/Instagram API       |
| Apple Integration      | `src/feeds/apple.c`, `include/human/feeds/apple.h`                                 | F85,87-88,91 Photos/Contacts/Health |
| Episodic Memory        | `src/memory/episodic.c`, `include/human/memory/episodic.h`                         | F70 episode storage + retrieval     |
| Consolidation Engine   | `src/memory/consolidation_engine.c`, `include/human/memory/consolidation_engine.h` | F72 nightly/weekly/monthly          |
| Skill Engine           | `src/intelligence/skills.c`, `include/human/intelligence/skills.h`                 | F94-101 skill lifecycle             |
| Reflection Engine      | `src/intelligence/reflection.c`, `include/human/intelligence/reflection.h`         | F77 daily reflection                |
| Feedback Tracker       | `src/intelligence/feedback.c`, `include/human/intelligence/feedback.h`             | F78 outcome signal tracking         |
| Cognitive Load         | `src/context/cognitive_load.c`, `include/human/context/cognitive_load.h`           | F102 fatigue/capacity simulation    |
| Authentic Existence    | `src/context/authentic.c`, `include/human/context/authentic.h`                     | F103-115 spontaneity orchestration  |

## New SQLite Tables (Phase 6 additions)

```sql
-- Per-contact behavioral baselines for Theory of Mind
CREATE TABLE IF NOT EXISTS contact_baselines (
    contact_id TEXT NOT NULL PRIMARY KEY,
    avg_message_length REAL,
    avg_response_time_ms REAL,
    emoji_frequency REAL,
    topic_diversity REAL,
    sentiment_baseline REAL,
    messages_sampled INTEGER DEFAULT 0,
    updated_at INTEGER
);

-- Global mood log
CREATE TABLE IF NOT EXISTS mood_log (
    id INTEGER PRIMARY KEY,
    mood TEXT NOT NULL,
    intensity REAL NOT NULL,
    cause TEXT,
    set_at INTEGER NOT NULL,
    decayed_at INTEGER
);

-- Social reciprocity scores
CREATE TABLE IF NOT EXISTS reciprocity_scores (
    contact_id TEXT NOT NULL,
    metric TEXT NOT NULL, -- 'initiation_ratio', 'response_debt', 'share_balance', 'question_balance'
    value REAL,
    updated_at INTEGER,
    PRIMARY KEY (contact_id, metric)
);

-- Opinions that evolve over time
CREATE TABLE IF NOT EXISTS opinions (
    id INTEGER PRIMARY KEY,
    topic TEXT NOT NULL,
    position TEXT NOT NULL,
    confidence REAL DEFAULT 0.5,
    first_expressed INTEGER NOT NULL,
    last_expressed INTEGER,
    superseded_by INTEGER -- FK to newer opinion on same topic
);

-- Life chapters / narrative self
CREATE TABLE IF NOT EXISTS life_chapters (
    id INTEGER PRIMARY KEY,
    theme TEXT NOT NULL,
    mood TEXT,
    started_at INTEGER NOT NULL,
    ended_at INTEGER,
    key_threads TEXT, -- JSON array
    active INTEGER DEFAULT 1
);

-- Emotional predictions for anticipatory modeling
CREATE TABLE IF NOT EXISTS emotional_predictions (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    predicted_emotion TEXT NOT NULL,
    confidence REAL,
    basis TEXT, -- 'temporal_pattern', 'upcoming_event', 'personality'
    target_date INTEGER,
    verified INTEGER DEFAULT 0 -- was the prediction correct?
);

-- Boundaries (protective intelligence)
CREATE TABLE IF NOT EXISTS boundaries (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    topic TEXT NOT NULL,
    type TEXT DEFAULT 'avoid', -- 'avoid', 'gentle', 'never'
    set_at INTEGER NOT NULL,
    source TEXT -- 'explicit' (they said it), 'inferred' (avoidance detected)
);
```

## New SQLite Tables (Phase 7-8 additions)

```sql
-- Episodic memory (full conversation experiences)
CREATE TABLE IF NOT EXISTS episodes (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    summary TEXT NOT NULL,
    emotional_arc TEXT,
    key_moments TEXT,
    impact_score REAL,
    created_at INTEGER NOT NULL
);

-- Future intentions
CREATE TABLE IF NOT EXISTS prospective_memories (
    id INTEGER PRIMARY KEY,
    trigger_type TEXT NOT NULL,
    trigger_value TEXT NOT NULL,
    action TEXT NOT NULL,
    contact_id TEXT,
    expires_at INTEGER,
    fired INTEGER DEFAULT 0,
    created_at INTEGER NOT NULL
);

-- Emotional weight from past interactions
CREATE TABLE IF NOT EXISTS emotional_residue (
    id INTEGER PRIMARY KEY,
    episode_id INTEGER,
    contact_id TEXT NOT NULL,
    valence REAL NOT NULL,
    intensity REAL NOT NULL,
    decay_rate REAL DEFAULT 0.1,
    created_at INTEGER NOT NULL
);

-- Outcome signals from behavioral choices
CREATE TABLE IF NOT EXISTS behavioral_feedback (
    id INTEGER PRIMARY KEY,
    behavior_type TEXT NOT NULL,
    contact_id TEXT NOT NULL,
    signal TEXT NOT NULL,
    context TEXT,
    timestamp INTEGER NOT NULL
);

-- Weekly relationship health assessments
CREATE TABLE IF NOT EXISTS self_evaluations (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    week INTEGER NOT NULL,
    metrics TEXT NOT NULL,
    recommendations TEXT,
    created_at INTEGER NOT NULL
);

-- Universal patterns learned across contacts
CREATE TABLE IF NOT EXISTS general_lessons (
    id INTEGER PRIMARY KEY,
    lesson TEXT NOT NULL,
    confidence REAL DEFAULT 0.5,
    source_count INTEGER DEFAULT 1,
    first_learned INTEGER NOT NULL,
    last_confirmed INTEGER
);

-- Ingested external content
CREATE TABLE IF NOT EXISTS feed_items (
    id INTEGER PRIMARY KEY,
    source TEXT NOT NULL,
    contact_id TEXT,
    content_type TEXT NOT NULL,
    content TEXT NOT NULL,
    url TEXT,
    ingested_at INTEGER NOT NULL,
    referenced INTEGER DEFAULT 0
);

-- Learned behavioral skills
CREATE TABLE IF NOT EXISTS skills (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    type TEXT NOT NULL,
    contact_id TEXT,
    trigger_conditions TEXT,
    strategy TEXT NOT NULL,
    success_rate REAL DEFAULT 0.5,
    attempts INTEGER DEFAULT 0,
    successes INTEGER DEFAULT 0,
    version INTEGER DEFAULT 1,
    origin TEXT NOT NULL,
    parent_skill_id INTEGER,
    created_at INTEGER NOT NULL,
    updated_at INTEGER,
    retired INTEGER DEFAULT 0
);

-- Individual skill application records
CREATE TABLE IF NOT EXISTS skill_attempts (
    id INTEGER PRIMARY KEY,
    skill_id INTEGER NOT NULL,
    contact_id TEXT NOT NULL,
    applied_at INTEGER NOT NULL,
    outcome_signal TEXT,
    outcome_evidence TEXT,
    context TEXT
);

-- Skill version history
CREATE TABLE IF NOT EXISTS skill_evolution (
    id INTEGER PRIMARY KEY,
    skill_id INTEGER NOT NULL,
    version INTEGER NOT NULL,
    strategy TEXT NOT NULL,
    success_rate REAL,
    evolved_at INTEGER NOT NULL,
    reason TEXT
);
```

## New SQLite Tables (Phase 9 additions)

```sql
-- Cognitive load tracking
CREATE TABLE IF NOT EXISTS cognitive_load_log (
    id INTEGER PRIMARY KEY,
    capacity REAL NOT NULL,
    conversation_depth INTEGER DEFAULT 0,
    hour_of_day INTEGER NOT NULL,
    day_of_week INTEGER NOT NULL,
    physical_state TEXT,
    recorded_at INTEGER NOT NULL
);

-- Active narrative threads per contact
CREATE TABLE IF NOT EXISTS active_threads (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    topic TEXT NOT NULL,
    status TEXT DEFAULT 'open',
    last_update_at INTEGER NOT NULL,
    created_at INTEGER NOT NULL
);

-- Bad interaction tracking for recovery
CREATE TABLE IF NOT EXISTS interaction_quality (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    quality_score REAL NOT NULL,
    cognitive_load REAL,
    mood_state TEXT,
    recovery_sent INTEGER DEFAULT 0,
    recovery_at INTEGER,
    timestamp INTEGER NOT NULL
);

-- Spontaneous narration events generated by life simulator
CREATE TABLE IF NOT EXISTS life_narration_events (
    id INTEGER PRIMARY KEY,
    event_type TEXT NOT NULL,
    description TEXT NOT NULL,
    shareability_score REAL NOT NULL,
    shared_with TEXT,
    generated_at INTEGER NOT NULL,
    shared_at INTEGER
);

-- Contradictions held simultaneously
CREATE TABLE IF NOT EXISTS held_contradictions (
    id INTEGER PRIMARY KEY,
    topic TEXT NOT NULL,
    position_a TEXT NOT NULL,
    position_b TEXT NOT NULL,
    expressed_a_count INTEGER DEFAULT 0,
    expressed_b_count INTEGER DEFAULT 0,
    created_at INTEGER NOT NULL
);
```

## Phase Map

| Phase     | Focus                                   | Features                                        | Estimated New C Lines |
| --------- | --------------------------------------- | ----------------------------------------------- | --------------------- |
| 1         | Foundation — Fix & Wire                 | F1-F7, F10-F11, F15, F40-F44                    | ~800                  |
| 2         | Emotional Intelligence                  | F13-F14, F16-F17, F25, F27, F29, F33, F45-F46   | ~600                  |
| 3         | Superhuman Memory                       | F18-F24, F26, F30-F31, F50, F53                 | ~1200                 |
| 4         | Behavioral Polish & Context             | F8-F9, F12, F28, F32, F47-F49, F51-F52, F54-F57 | ~900                  |
| 5         | Voice Messages                          | F34-F39                                         | ~700                  |
| 6         | AGI Cognition Layer                     | F58-F69                                         | ~1800                 |
| 7         | Deep Memory & External Awareness        | F70-F76, F83-F93                                | ~2200                 |
| 8         | Skill Acquisition & Continuous Learning | F77-F82, F94-F101                               | ~1600                 |
| 9         | Authentic Existence                     | F102-F115                                       | ~1200                 |
| **Total** |                                         | **115 features**                                | **~11000 lines**      |

### Phase 4 Feature Status (Closed)

| Feature                | Status                                                                 |
| ---------------------- | ---------------------------------------------------------------------- |
| F9 Double-text         | ✅ Implemented — `hu_conversation_should_double_text` + daemon wiring  |
| F47 Content forwarding | ✅ Implemented — `save_for_later` tool registered in factory           |
| F51 Weather awareness  | ✅ Implemented — `hu_weather_fetch` with OpenWeatherMap + 30-min cache |
| F52 Current events     | ✅ Implemented — `hu_current_events_fetch` with RSS parsing            |

### Phase 7 & 8 Feature Status (Closed)

| Feature range               | Status                               |
| --------------------------- | ------------------------------------ |
| F70-F76 (Deep Memory)       | ✅ Implemented (all modules + tests) |
| F77-F82 (Skill Acquisition) | ✅ Implemented (all modules + tests) |
| F83-F93 (External Feeds)    | ✅ Implemented (daemon wired)        |
| F94-F101 (Skill Lifecycle)  | ✅ Implemented (F101 gateway added)  |
| Consolidation engine        | ✅ Wired — nightly after reflection  |
| Forgetting curve decay      | ✅ Wired — nightly batch decay       |
| Episode reinforcement       | ✅ Wired — on episodic context use   |
| Feed processor poll         | ✅ Wired — 6-hour periodic task      |
| Feedback classification     | ✅ Wired — replaces generic signal   |
| Skill pre-loading           | ✅ Wired — startup + post-reflection |
| Skill success rate update   | ✅ Wired — after attempt recording   |

## Implementation Plans

Each phase has its own detailed implementation plan:

- `2026-03-10-human-fidelity-phase1-foundation.md`
- `2026-03-10-human-fidelity-phase2-emotional-intelligence.md`
- `2026-03-10-human-fidelity-phase3-superhuman-memory.md`
- `2026-03-10-human-fidelity-phase4-behavioral-polish.md`
- `2026-03-10-human-fidelity-phase5-voice-messages.md`
- `2026-03-10-human-fidelity-phase6-agi-cognition.md`
- `2026-03-10-human-fidelity-phase7-deep-memory.md`
- `2026-03-10-human-fidelity-phase8-skill-acquisition.md`
- `2026-03-10-human-fidelity-phase9-authentic-existence.md`
- `2026-03-10-human-fidelity-missing-seven.md` — 28 cross-cutting features (F116–F143) across 7 new pillars: Visual Content Pipeline, Proactive Governor, Contact Knowledge State, Collaborative Planning, Context Arbitration, Relationship Dynamics, Shared Experience Compression. **Daemon wired:** All pillars — 18 (Visual Content), 19 (Governor), 20 (Knowledge State), 21 (Collaborative Planning), 22 (Context Arbitration — token budget), 23 (Relationship Dynamics), 24 (Shared Compression). Extended: F25 (Persona Fine-Tuning), F29 (On-Device Classification), F31 (Statistical Timing), F32 (Behavioral Cloning).

## Success Criteria

1. **Turing test**: A close contact (Mindy) cannot distinguish human from Seth over a 24-hour texting session
2. **Close friend test**: Someone who has known Seth for 30 years cannot tell the difference over a week of texting
3. **Timing test**: Response timing distribution matches real human patterns (no sub-10-second responses at midnight)
4. **Memory test**: human references past conversations accurately without being prompted
5. **Emotional test**: human correctly identifies and matches emotional energy in >80% of messages
6. **Theory of mind test**: human detects unstated emotional shifts in >60% of cases
7. **Anticipation test**: human proactively checks in before being asked >50% of the time when context warrants it
8. **Voice test**: Voice messages are indistinguishable from a real recording of Seth
9. **Platform test**: Tapbacks, inline replies, and effects work correctly on recipient's iPhone
10. **Protective test**: human NEVER surfaces sensitive memories at wrong times, NEVER leaks cross-contact info
11. **Authenticity test**: human occasionally makes mistakes, complains about mundane things, and shares life unprompted — no "always available perfect responder" vibes
12. **Imperfection test**: >5% of factual claims are slightly wrong (minor, non-harmful); cognitive load degrades quality at late hours
13. **Recovery test**: After a bad interaction, human self-corrects within 12 hours without being asked
14. **Zero regression**: All 3673+ existing tests pass, 0 ASan errors, binary <1600KB

## Risk Assessment

| Risk                                       | Probability | Impact           | Mitigation                                             |
| ------------------------------------------ | ----------- | ---------------- | ------------------------------------------------------ |
| AppleScript can't do inline replies        | Medium      | High (F40)       | Fallback to quoted text prefix                         |
| AppleScript can't do message editing       | High        | Medium (F41)     | Keep `*correction` pattern                             |
| AppleScript can't do screen effects        | Medium      | Low (F42)        | Skip effects, text-only                                |
| Cartesia voice clone quality               | Low         | High (F35)       | Test extensively before enabling                       |
| Binary size exceeds budget                 | Low         | Medium           | Phase 5-6 gated by CMake flags                         |
| LLM doesn't follow behavioral instructions | Medium      | High (F13,F17)   | Strengthen prompts, test with multiple models          |
| Memory tables bloat SQLite                 | Low         | Low              | Periodic cleanup, row limits                           |
| Theory of Mind false positives             | Medium      | Medium (F58)     | High confidence threshold, gentle probing only         |
| Mood persistence feels forced              | Medium      | Medium (F60)     | Subtle injection, slow decay, never explicit           |
| Memory degradation confuses users          | Low         | Low (F61)        | Only degrade minor details, never names/events         |
| Life simulation feels robotic              | Medium      | High (F59)       | High variance, never reference routine directly        |
| Cross-contact privacy leak                 | Low         | Very High (F68)  | Strict isolation in protective gate, test extensively  |
| Opinion evolution contradicts persona      | Medium      | Medium (F65)     | Guardrails: core values never change, only views       |
| Being wrong crosses into harmful territory | Low         | High (F105)      | Only wrong about trivial facts, never emotional/safety |
| Spontaneous narration feels robotic        | Medium      | Medium (F103)    | High variance, natural language, abort if timing bad   |
| Resistance reads as system failure         | Medium      | High (F110)      | Always include brief reason, never just ignore         |
| Cognitive degradation is too obvious       | Medium      | Medium (F102)    | Subtle reduction, not dramatic quality cliff           |
| Gossip leaks private information           | Low         | Very High (F107) | Only discuss mutually known info, strict graph check   |
| Bad-day recovery feels scripted            | Low         | Medium (F115)    | Vary timing and phrasing, never templated              |
