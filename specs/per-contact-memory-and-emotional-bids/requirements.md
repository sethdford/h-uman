# Per-Contact Memory + Emotional Bids (PCMEB) — Requirements

> **Vision.** Bring h-uman's per-contact relational texting to **SOTA per
> 2025–2026 benchmarks** (TwinVoice Persona Tone + Memory Recall;
> PersoBench personalization; Catch-Me-If-You-Can implicit-style imitation).
> The gap was made concrete by the Jordan transcript (484-678-4914) where
> h-uman post-PCTT still:
>
> 1. Skipped *"I need cuddles 😩"* — an emotional bid — without acknowledgment
> 2. Pivoted *"tbh lucky! Enjoy the break! ✨"* away from the sleepover topic
> 3. Used coworker register with an intimate partner
> 4. Defaulted to generic emoji (🍿, ✨) instead of the pair's actual set
> 5. Used *"tbh"* grammatically wrong ("tbh lucky" is broken usage)
>
> Root cause: no model of *who the counterparty is to the user* — relationship
> type, register, style mirror, recent emotional state, topic memory.
> personal_model knows *the user*, not *the people the user talks to*.
>
> This spec ships Waves 1+2 of a larger relational-AI roadmap (Waves 3–5
> listed at the bottom as separate future specs).

## User stories

- **Jordan-style intimacy.** When the partner sends vulnerable content like
  *"I need cuddles 😩"*, the bot acknowledges the emotion FIRST, before any
  informational content — not skipping past it to the next message.
- **Topic continuity.** When the contact opens a topic (sleepover proposal),
  the bot *builds on it* ("wanna come over earlier?", "I'll grab snacks")
  rather than pivots to small-talk ("Enjoy the break!").
- **Relational register.** The bot tones every reply to the contact's
  relationship type — intimate partner / close friend / family /
  professional / acquaintance — and doesn't default to "coworker cheerful."
- **Style mirroring.** The bot's emoji, punctuation, capitalization, and
  message length with a given person mirror what the user actually uses
  with that person, not generic-positive defaults.
- **Topic memory.** The bot knows what the contact is currently caring
  about (last vacation, current stressors, recent wins) and surfaces it
  when natural — *"how'd the PM rollout end up going?"*
- **Self-disclosure depth matching.** When the partner is vulnerable, the
  bot can share back proportionally — *"yeah I've been feeling that too"* —
  not just empathize at arm's length.
- **Privacy by architecture.** Anything Jordan said in their chat is NEVER
  surfaced in a different chat. Cross-contact memory leak is structurally
  impossible, not policy-controlled.
- **Manageable contact profile.** I can set Jordan's relationship type, see
  her profile, and clear it entirely — all from CLI — without touching JSON.
- **Measurable persona quality.** I can run an eval that scores per-contact
  register fidelity (TwinVoice-style) and detect regressions over time.

## Acceptance criteria

### Wave 1 — Foundations (relationship schema + register)

- [ ] **AC-1 (Relationship type schema):** `hu_contact_profile_t` (in
  `include/human/persona.h`) gains a `relationship_type` field whose value
  is an enum `hu_relationship_type_t` with members `{HU_REL_UNKNOWN=0,
  HU_REL_INTIMATE, HU_REL_CLOSE_FRIEND, HU_REL_FAMILY, HU_REL_PROFESSIONAL,
  HU_REL_ACQUAINTANCE}`. Persona JSON round-trip preserves the field.
  Old persona files load with `relationship_type = HU_REL_UNKNOWN`.

- [ ] **AC-2 (CLI to set/show/forget type):** Three new subcommands:
  - `human persona contact set-type <persona> <contact_id> <type>` — sets
    and saves atomically. Type strings: `intimate`, `close-friend`,
    `family`, `professional`, `acquaintance`, `unknown`.
  - `human persona contact show <persona> <contact_id>` — prints the full
    profile (name, relationship_type, top emoji, current topics,
    style_profile fields).
  - `human persona contact forget <persona> <contact_id>` — zeroes the
    learned profile (keeps `name`, `contact_id`; resets the rest).
    Requires a confirmation echo before destructive action.

- [ ] **AC-3 (Register → prompt hint):** At dispatch, when the chat's
  counterparty has a known `relationship_type`, the system prompt adds a
  one-line register hint matching that type:
  - `HU_REL_INTIMATE` → *"You are responding as an intimate partner.
    Warmth and emotional acknowledgement come before information. Stay on
    topics the partner opens; don't pivot to small-talk."*
  - `HU_REL_CLOSE_FRIEND` → *"Close friend register — match their energy;
    acknowledge emotion; build on stated topics."*
  - `HU_REL_FAMILY` → *"Family register — warm, casual, informal."*
  - `HU_REL_PROFESSIONAL` → *"Professional register — concise, polite,
    task-focused."*
  - `HU_REL_ACQUAINTANCE` → *"Acquaintance register — friendly but not
    familiar."*
  - `HU_REL_UNKNOWN` → no augmentation. Test asserts presence/absence per
    type.

- [ ] **AC-4 (Recent thread context window):** Prompt includes the last 10
  messages from THIS specific chat (both directions, chronological).
  Bounded — never more than 10 even when history is longer, to control
  prompt bloat. Test asserts only the last 10 from a 30-message history
  appear in the prompt.

### Wave 2 — Style mirroring + topic memory

- [ ] **AC-5 (Per-contact emoji corpus):** `hu_contact_profile_t` gains a
  `top_emoji[]` array (max 10 strings, single-emoji-glyph each, soft cap
  with parse-time warning matching PR #78's pattern). Surfaced as
  prompt hint when non-empty. Round-trip preserved.

- [ ] **AC-6 (Per-contact style profile):** `hu_contact_profile_t` gains
  a nested `style_profile` substruct with fields `avg_msg_len_chars`
  (size_t), `punctuation_density` (float 0–1, fraction of msgs ending
  with `.!?`), `caps_usage` (float 0–1, fraction of msgs containing
  ≥1 capitalized word past the first character), `emoji_per_msg` (float).
  Surfaced as a one-line prompt hint when any field is non-zero:
  *"Style with this contact: avg N chars/msg, lowercase {usually|often|rarely},
  punctuation {minimal|moderate|formal}, emoji {none|sparse|frequent}."*
  Buckets are deterministic; thresholds: avg_msg_len_chars: <40=short,
  40–120=medium, >120=long. The fields are *learned* (AC-9 wave-2 also
  enables learning), but for this AC the fields are settable+round-trippable.

- [ ] **AC-7 (Topic memory):** `hu_contact_profile_t` gains `current_topics[]`
  — array of up to 5 short strings (each ≤80 chars) representing topics
  the contact has discussed in the last 30 days, ordered by recency.
  Surfaced as prompt hint: *"This contact recently talked about: [topics]."*
  Round-trip preserved. For this AC, topics are settable manually via CLI;
  auto-extraction is roadmap.

### Wave 2 — Emotional bid detection + gated augmentation

- [ ] **AC-8 (Emotional-bid detector):** New function
  `hu_error_t hu_cognition_detect_bid(const char *msg, size_t msg_len,
  hu_bid_result_t *out)` (in `src/cognition/bid.c`, header
  `include/human/cognition/bid.h`) classifies into
  `hu_bid_type_t {HU_BID_NONE, HU_BID_VULNERABILITY, HU_BID_EXCITEMENT,
  HU_BID_DISTRESS}` with a `confidence` field (0.0–1.0).
  Rules:
  - VULNERABILITY: "i need", "i feel", "i miss", "can you" + emotional emoji
    (😩 😭 🥺 💔 🤍) or standalone vulnerable phrases ("hold me", "cuddles",
    "lonely", "hurts").
  - EXCITEMENT: "omg yes", "🙌", "✨ amazing", multiple "!" runs, caps bursts.
  - DISTRESS: "overwhelmed", "exhausted", "scared", "freaking out", 😢 😟.
  Confidence ≥ 0.6 required for non-NONE. Latency <100µs/message.
  Test corpus: ≥10 positive cases per type + ≥5 hard negatives per type
  (e.g., "I need to grab milk" must NOT trigger VULNERABILITY).

- [ ] **AC-9 (Bid → prompt augmentation, gated):** When bid is non-NONE
  AND counterparty relationship_type ∈ {INTIMATE, CLOSE_FRIEND, FAMILY},
  prompt is augmented with: *"The user has expressed [bid_type_lowercase].
  Respond to that emotion FIRST, before any informational content. Do not
  pivot to small-talk."* Suppressed for PROFESSIONAL / ACQUAINTANCE / UNKNOWN.
  Test asserts presence for intimate+VULNERABILITY and absence for
  professional+VULNERABILITY with identical input message.

- [ ] **AC-10 (Self-disclosure depth matching):** When bid type =
  VULNERABILITY and `hu_personal_model_t` has recent emotional content
  about the user (heuristic: a fact in the personal_model whose text
  contains "i feel", "i've been", "lately"), the prompt is augmented with:
  *"User has been feeling: [up to 3 most-recent personal_model emotional
  facts]. Share back proportionally if it feels natural — don't force it."*
  Suppressed when personal_model has nothing matching, or when
  relationship_type ∉ {INTIMATE, CLOSE_FRIEND}. Test asserts presence/absence.

### Wave 2 — Privacy + eval

- [ ] **AC-11 (Cross-contact privacy invariant test):** New test
  `tests/test_contact_privacy_isolation.c` constructs two contacts (Jordan
  with `relationship_type=INTIMATE`, top_emoji=`[🥺,💕]`, topics=`[PM rollout]`;
  and "Acme Boss" with `relationship_type=PROFESSIONAL`, top_emoji=`[]`,
  topics=`[Q3 review]`). Dispatches a message to each; asserts:
  - Jordan's prompt contains `🥺` AND `PM rollout` AND intimate hint
  - Boss's prompt does NOT contain `🥺`, `PM rollout`, or the intimate hint
  - Jordan's prompt does NOT contain `Q3 review` or the professional hint
  Validates the architectural invariant that contact data is looked up by
  the active chat's counterparty id only.

- [ ] **AC-12 (Topic-stay observability hook):** New post-generation guard
  `hu_cognition_check_topic_stay(const char *inbound, const char *outbound,
  hu_topic_stay_result_t *out)` returns a `pivot_detected` flag and a
  `confidence` score. Heuristic: extract top-2 content nouns from `inbound`;
  if neither appears in `outbound`'s first sentence, flag pivot. Result is
  logged to stderr as a warning only — NOT a send-block — for observability.
  This is the "Enjoy the break!" → sleepover anti-pivot guard. Test with
  the literal Jordan exchange asserts pivot_detected=true.

- [ ] **AC-13 (Persona-tone eval suite):** New eval scenario
  `data/eval_persona_tone.json` with 5 synthetic contacts (one per
  relationship_type non-UNKNOWN) × 5 message types (vulnerability,
  excitement, distress, info-request, neutral) = 25 cases. Test
  `tests/test_persona_tone_eval.c` asserts for each case:
  - Bid detection result matches expected
  - Assembled prompt contains the correct register hint
  - Augmentation presence matches gating logic
  No LLM call — pure prompt-content assertions. Forms the regression suite
  for future Wave-3+ work.

- [ ] **AC-14 (Jordan-transcript regression):** `data/eval_jordan_pcmeb.json`
  carries the literal Jordan messages quoted in this spec preamble.
  `tests/test_jordan_pcmeb_regression.c` drives them through the new
  pipeline and asserts:
  - *"I need cuddles 😩"* → bid=VULNERABILITY, confidence ≥ 0.6
  - *"let's do a sleepover tomorrow night"* → bid=NONE (proposal, not bid)
  - *"I don't have class today and tomorrow 🙌🏻"* → bid=EXCITEMENT
  - With Jordan's contact profile set to INTIMATE, the prompt for cuddles
    contains the AC-9 augmentation
  - With contact profile set to PROFESSIONAL, the augmentation is absent
  - The literal bot reply *"tbh lucky! Enjoy the break! ✨"* tested against
    the AC-12 topic-stay guard returns pivot_detected=true

- [ ] **AC-15 (Privacy CLI):** `human persona contact forget <persona>
  <contact_id>` clears the profile (relationship_type → UNKNOWN, top_emoji
  → empty, current_topics → empty, style_profile → zeroed; preserves
  `name` and `contact_id`). Saves atomically. Asks for confirmation
  (`y/N` prompt) unless `--yes` flag passed. Test covers both paths.

- [ ] **AC-16 (Response-shape decision stub):** New module
  `src/cognition/response_shape.c` (header
  `include/human/cognition/response_shape.h`) exposing
  `hu_response_shape_t {HU_SHAPE_TEXT, HU_SHAPE_TAPBACK, HU_SHAPE_SILENCE}`
  and `hu_response_shape_t hu_cognition_decide_response_shape(...)`. This
  spec ships the stub that ALWAYS returns `HU_SHAPE_TEXT` (current
  behavior). Tests assert the stub returns TEXT and the dispatch path is
  wired through it. The real decision logic is in Wave 3 roadmap.

## Non-goals (deferred to later specs)

- **Auto-learning relationship_type from chat patterns.** Manual classification
  only this wave. Pattern-learning is roadmap.
- **Auto-extracting current_topics from inbound message content.** Topics
  are settable manually via JSON edit / CLI for now. Auto-extraction is
  roadmap.
- **Auto-populating style_profile from outbound corpus.** Fields are
  settable+round-trippable; auto-population is roadmap.
- **Real response-shape decisions (tapback / silence).** Stub ships;
  real logic is Wave 3.
- **Multi-message fragmentation.** One block per response remains current
  behavior. Splitting is Wave 3.
- **Proactive recall (bot texts first).** Out of scope.
- **Cross-channel coherence (same person across iMessage + Slack).** Out
  of scope.
- **Rupture/repair detection.** `src/cognition/rupture_repair.c` exists;
  connecting it to contact profiles is Wave 4.
- **ML-based bid detection.** Heuristic only this wave; ML is roadmap.
- **The LLM's actual generated response.** We augment prompts; the model
  still writes. Output quality is an LLM property, not a spec property.

## Constraints

- **Build:** C11, `-Wall -Wextra -Wpedantic -Werror`, ASan clean on dev preset.
- **Forward compat:** Pre-PCMEB persona JSONs load cleanly; missing fields
  default to safe zero-equivalents.
- **Privacy invariant (structural):** Contact profile data is looked up by
  active-chat counterparty id ONLY. Code reviewers must reject any change
  that allows cross-contact data injection. AC-11 is the test lock-in.
- **Bid detector latency:** <100µs per message. Hot path.
- **Topic-stay guard:** <50µs per outbound message. Observability path,
  not send-blocking.
- **Memory:** All new heap allocations freed; ASan clean across full
  suite (10,247+ baseline).
- **CLI pattern reuse:** New `contact` subcommand follows the same
  hand-rolled strcmp dispatch as `filler` (PCTT Task 7) for consistency.
- **No new dependencies.** No ML libraries, no embedding stores, no
  external runtime deps.

## Risk callouts

- **Bid detector false positives.** "I need a coffee" must NOT register
  VULNERABILITY. AC-8 has hard-negative cases. If real-world chat shows
  false positives, threshold can be tightened post-merge.

- **Privacy invariant fragility.** AC-11 tests the structural property but
  a future refactor could introduce a bleed. Recommend adding a code-review
  fence (CODEOWNERS entry on `src/cognition/bid.c` + privacy test file).

- **Register hints may over-constrain LLM creativity.** The instruction
  "don't pivot to small-talk" might reduce reply variety. Monitor real
  usage; tune wording if responses feel forced.

- **CLI confirmation prompts (AC-15) may break test automation.** The
  `--yes` flag bypasses; tests must use it.

- **Topic-stay guard is heuristic.** Top-2-noun extraction is naive.
  Wave 3 will replace with an embedding-distance check, but we ship the
  observability hook now so the eval has a measurement target.

## Roadmap (separate future specs, NOT in this spec)

These are *signposted*, not promised. Each becomes its own spec when ready.

### Wave 3 — Conversational competence
- Response-shape decision (replace AC-16 stub with real logic): tapback /
  silence / text / question / self-disclosure.
- Multi-message fragmentation: split one logical reply into 2–4 bursts
  with timing.
- Topic-stay enforcement (not just observability): block sends that pivot
  off-topic without explicit human-initiated topic shift.
- *"What's the plan?"* anti-pattern suppression: when a proposal is
  self-explanatory, just say yes.

### Wave 4 — Auto-learning + relationship dynamics
- Auto-extract `current_topics[]` from inbound messages.
- Auto-populate `style_profile` from outbound corpus.
- Auto-classify `relationship_type` from message cadence + content +
  emoji density.
- Rupture / repair detection: connect existing `src/cognition/rupture_repair.c`
  to per-contact thread analytics.
- Cross-channel coherence: unify Jordan-in-iMessage with Jordan-in-Slack.

### Wave 5 — Proactive + measured
- Proactive recall: bot occasionally texts first based on topic-recency.
- Persona-tone scoring against a golden corpus (TwinVoice-style).
- A/B framework for register variants.
- Relationship-health metric (running thread sentiment).
- User-facing privacy dashboard: audit log of what was learned about whom.

### Wave 6 — Governance + safety
- Per-contact opt-out of learning.
- "Sensitive topic" tagging (financial, medical, legal) with stricter
  privacy invariants.
- Periodic profile-decay (information older than N months loses weight).
