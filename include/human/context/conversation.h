#ifndef HU_CONTEXT_CONVERSATION_H
#define HU_CONTEXT_CONVERSATION_H

#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#ifdef HU_HAS_PERSONA
#include "human/persona.h"
#else
struct hu_persona;         /* opaque when persona not available */
struct hu_contact_profile; /* opaque when persona not available */
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Group chat prompt hint — prepended to conversation_context when is_group is true */
#define HU_GROUP_CHAT_PROMPT_HINT                     \
    "[GROUP CHAT] You are in a group conversation. "  \
    "Keep responses to 1-2 sentences. "               \
    "Don't explain — react. "                         \
    "Don't try to be helpful unless asked directly. " \
    "Match the group's energy. Don't dominate.\n\n"

/* Thread callback detection: identify opportunities to naturally return to
 * a topic from earlier in the conversation or from a previous session.
 * Scans history for topic transitions and identifies the best callback candidate.
 * Returns a context injection string or NULL if no good callback found.
 * Caller owns returned string. */
char *hu_conversation_build_callback(hu_allocator_t *alloc,
                                     const hu_channel_history_entry_t *entries, size_t count,
                                     size_t *out_len);

/* Build a complete conversation awareness context string from channel history.
 * Includes: conversation thread, emotional analysis, verbosity mirroring,
 * conversation phase, time-of-day triggers, detected user states.
 * When persona is non-NULL and has style_rules, uses those; otherwise uses built-in rules.
 * Caller owns returned string. Returns NULL if no entries. */
char *hu_conversation_build_awareness(hu_allocator_t *alloc,
                                      const hu_channel_history_entry_t *entries, size_t count,
                                      const struct hu_persona *persona, size_t *out_len);

/* Conversation quality score (0-100). Evaluates a draft response for:
 * brevity, validation/reflection, repair/rephrase, and follow-up.
 * Qualitative: compares to their average length, energy level; needs_revision
 * only on gross mismatches. guidance describes the issue for prompt injection. */
typedef struct hu_quality_score {
    int total;
    int brevity;
    int validation;
    int warmth;
    int naturalness;
    bool needs_revision;
    char guidance[256]; /* human-readable issue description when mismatched */
} hu_quality_score_t;

hu_quality_score_t hu_conversation_evaluate_quality(const char *response, size_t response_len,
                                                    const hu_channel_history_entry_t *entries,
                                                    size_t count, uint32_t max_chars);

/* Honesty guardrail: detect "did you do X?" questions and inject honest context.
 * Returns a context string if an honesty injection is needed, NULL otherwise.
 * Caller owns returned string. */
char *hu_conversation_honesty_check(hu_allocator_t *alloc, const char *message, size_t message_len);

/* Narrative arc phase for the conversation.
 * Adds structure/climax/intervention guidance to the awareness buffer. */
typedef enum hu_narrative_phase {
    HU_NARRATIVE_OPENING = 0,
    HU_NARRATIVE_BUILDING,
    HU_NARRATIVE_APPROACHING_CLIMAX,
    HU_NARRATIVE_PEAK,
    HU_NARRATIVE_RELEASE,
    HU_NARRATIVE_CLOSING,
} hu_narrative_phase_t;

hu_narrative_phase_t hu_conversation_detect_narrative(const hu_channel_history_entry_t *entries,
                                                      size_t count);

/* Engagement level detection */
typedef enum hu_engagement_level {
    HU_ENGAGEMENT_HIGH = 0,
    HU_ENGAGEMENT_MODERATE,
    HU_ENGAGEMENT_LOW,
    HU_ENGAGEMENT_DISTRACTED,
} hu_engagement_level_t;

hu_engagement_level_t hu_conversation_detect_engagement(const hu_channel_history_entry_t *entries,
                                                        size_t count);

/* Emotional valence from conversation (-1.0 to 1.0, negative=distressed, positive=happy) */
typedef struct hu_emotional_state {
    float valence;
    float intensity;
    bool concerning;
    const char *dominant_emotion;
} hu_emotional_state_t;

hu_emotional_state_t hu_conversation_detect_emotion(const hu_channel_history_entry_t *entries,
                                                    size_t count);

/* Energy level for matching emotional energy of incoming message.
 * Used to inject [ENERGY: ...] directive into the prompt. */
typedef enum hu_energy_level {
    HU_ENERGY_NEUTRAL,
    HU_ENERGY_EXCITED,
    HU_ENERGY_SAD,
    HU_ENERGY_PLAYFUL,
    HU_ENERGY_ANXIOUS,
    HU_ENERGY_CALM,
} hu_energy_level_t;

hu_energy_level_t hu_conversation_detect_energy(const char *msg, size_t msg_len,
                                                const hu_channel_history_entry_t *entries,
                                                size_t count);

size_t hu_conversation_build_energy_directive(hu_energy_level_t energy, char *buf, size_t cap);

/* ── Emotional escalation detection (F14) ────────────────────────────────── */

/* Track emotional trajectory across multiple messages. When 3+ messages show
 * increasing negative sentiment, switch to de-escalation mode. */
typedef struct hu_escalation_state {
    bool escalating;
    int consecutive_negative;
    float trajectory; /* negative = worsening */
} hu_escalation_state_t;

hu_escalation_state_t hu_conversation_detect_escalation(const hu_channel_history_entry_t *entries,
                                                        size_t count);

size_t hu_conversation_build_deescalation_directive(char *buf, size_t cap);

/* ── Context modifiers (F16) ─────────────────────────────────────────────── */

/* Build [CONTEXT: ...] directives based on heavy topics, personal sharing,
 * high emotion, and early-turn detection. Uses persona context_modifiers when
 * non-NULL; otherwise defaults (0.4, 1.6, 1.5, 1.4). Writes into buf, returns
 * bytes written. */
#ifdef HU_HAS_PERSONA
size_t hu_conversation_build_context_modifiers(const hu_channel_history_entry_t *entries,
                                               size_t count, const hu_emotional_state_t *emo,
                                               const hu_context_modifiers_t *mods, char *buf,
                                               size_t cap);
#endif

/* ── Typo correction fragment (*meant) ─────────────────────────────────── */

/* Check if a typo was introduced and generate a correction fragment.
 * Compares original text with typo-applied text to find the changed word.
 * If a typo was found, writes a correction like "*meeting" into out_buf.
 * Returns length of correction (0 if no typo detected or no correction needed).
 * correction_chance is probability 0-100 of generating correction (suggest ~40). */
size_t hu_conversation_generate_correction(const char *original, size_t original_len,
                                           const char *typo_applied, size_t typo_applied_len,
                                           char *out_buf, size_t out_cap, uint32_t seed,
                                           uint32_t correction_chance);

/* ── Multi-message splitting ──────────────────────────────────────────── */

/* Split a single response into multiple message fragments for natural delivery.
 * Analyzes sentence boundaries, conjunctions, and interjections to break
 * a response into the kind of rapid-fire fragments real humans send.
 * Returns fragment count. Each fragment.text is a separately allocated copy;
 * caller must free each fragment.text via alloc->free(ctx, text, text_len+1).
 * Returns 0 on error or if response is empty/NULL. */
typedef struct hu_message_fragment {
    char *text;
    size_t text_len;
    uint32_t delay_ms; /* suggested inter-message delay before this fragment */
} hu_message_fragment_t;

size_t hu_conversation_split_response(hu_allocator_t *alloc, const char *response,
                                      size_t response_len, hu_message_fragment_t *fragments,
                                      size_t max_fragments);

/* ── Situational length calibration ───────────────────────────────────── */

/* Return max response characters based on incoming message length.
 * Formula: incoming_len * 2.0, capped at 300, minimum 15.
 * Use for max_response_chars to match response length within ~1.5x ratio. */
int hu_conversation_max_response_chars(size_t incoming_len);

/* Classify the last incoming message and produce human-level length guidance.
 * Analyzes message type (question, emotional, greeting, logistics, etc.) and
 * produces a short directive string for the prompt like:
 *   "CALIBRATION: They asked a direct question. Answer it, add one detail. 5-15 words."
 * Writes into buf (up to cap bytes). Returns bytes written (0 if nothing useful). */
size_t hu_conversation_calibrate_length(const char *last_msg, size_t last_msg_len,
                                        const hu_channel_history_entry_t *entries, size_t count,
                                        char *buf, size_t cap);

/* ── Texting style analysis ───────────────────────────────────────────── */

/* Analyze the other person's texting style from conversation history.
 * Produces a style context string for the prompt with capitalization,
 * punctuation, fragmentation, and abbreviation patterns.
 * When persona is non-NULL and has anti_patterns, uses those; otherwise uses built-in rules.
 * Caller owns returned string. Returns NULL if insufficient data. */
char *hu_conversation_analyze_style(hu_allocator_t *alloc,
                                    const hu_channel_history_entry_t *entries, size_t count,
                                    const struct hu_persona *persona, size_t *out_len);

/* ── Typing quirk post-processing ─────────────────────────────────────── */

/* Apply typing quirks to a response as deterministic post-processing.
 * Supported quirks: "lowercase", "no_periods", "no_commas",
 * "no_apostrophes", "double_space_to_newline", "variable_punctuation".
 * Modifies the buffer in-place. Returns the new length (may shrink). */
size_t hu_conversation_apply_typing_quirks(char *buf, size_t len, const char *const *quirks,
                                           size_t quirks_count);

/* Apply realistic typo simulation to a response. Introduces at most 1 typo
 * per message segment. Controlled by seed for determinism in tests.
 * Never introduces typos into proper nouns (words starting with uppercase
 * that aren't the first word of a sentence).
 * Returns the new length (may grow by at most 1 char for transposition).
 * buf must have capacity for len+1 chars. */
size_t hu_conversation_apply_typos(char *buf, size_t len, size_t cap, uint32_t seed);

/* ── Response action classification ───────────────────────────────────── */

typedef enum hu_response_action {
    HU_RESPONSE_FULL = 0,     /* generate full response */
    HU_RESPONSE_BRIEF = 1,    /* ultra-short: "yeah", "lol", "nice" */
    HU_RESPONSE_SKIP = 2,     /* don't respond at all */
    HU_RESPONSE_DELAY = 3,    /* full response but with extra delay */
    HU_RESPONSE_THINKING = 4, /* two-phase: send filler first, then real response after delay */
} hu_response_action_t;

typedef struct hu_thinking_response {
    char filler[64]; /* "hmm", "that's a good question", "let me think about that" */
    size_t filler_len;
    uint32_t delay_ms; /* delay before the real response (30000-60000ms) */
} hu_thinking_response_t;

/* Classify whether this message warrants a "thinking" response.
 * Returns true if the message is complex/emotional enough.
 * Fills out the thinking response with an appropriate filler message. */
bool hu_conversation_classify_thinking(const char *msg, size_t msg_len,
                                       const hu_channel_history_entry_t *entries,
                                       size_t entry_count, hu_thinking_response_t *out,
                                       uint32_t seed);

/* Classify how to respond, factoring in conversation context. */
hu_response_action_t hu_conversation_classify_response(const char *msg, size_t msg_len,
                                                       const hu_channel_history_entry_t *entries,
                                                       size_t entry_count,
                                                       uint32_t *delay_extra_ms);

/* Natural conversation drop-off: returns skip probability 0-100.
 * Caller rolls (seed % 100) < prob to decide SKIP. Used when action is FULL/BRIEF. */
int hu_conversation_classify_dropoff(const char *message, size_t message_len,
                                     const hu_channel_history_entry_t *entries, size_t entry_count,
                                     uint32_t seed);

/* ── URL extraction and link-sharing detection ───────────────────────── */

/* Extract URLs from a text message. Returns count of URLs found.
 * Each URL is written as a separate entry in urls[] (up to max_urls).
 * Caller provides pre-allocated url_buf entries. */
typedef struct hu_url_extract {
    const char *start; /* pointer into the original text */
    size_t len;
} hu_url_extract_t;

size_t hu_conversation_extract_urls(const char *text, size_t text_len, hu_url_extract_t *urls,
                                    size_t max_urls);

/* Detect if a message context suggests sharing a link.
 * Returns true if the LLM should be prompted to find and share a URL.
 * Triggers on: recommendations, "check this out", "have you seen", "look at this",
 * "you should try", "here's a link" */
bool hu_conversation_should_share_link(const char *msg, size_t msg_len,
                                       const hu_channel_history_entry_t *entries,
                                       size_t entry_count);

/* ── Attachment context for prompts ──────────────────────────────────── */

/* Generate context for the prompt when attachments are detected in history.
 * Scans for [Photo shared], [Attachment shared], [image or attachment], etc.
 * Returns allocated string; caller must free. NULL if no attachments found. */
char *hu_conversation_attachment_context(hu_allocator_t *alloc,
                                         const hu_channel_history_entry_t *entries, size_t count,
                                         size_t *out_len);

/* ── Anti-repetition detection ───────────────────────────────────────── */

/* Analyze recent "from_me" messages for repetitive patterns (openers,
 * always ending with questions, filler words). Writes guidance into buf.
 * Returns bytes written (0 if no patterns detected). */
size_t hu_conversation_detect_repetition(const hu_channel_history_entry_t *entries, size_t count,
                                         char *buf, size_t cap);

/* ── Relationship-tier calibration ───────────────────────────────────── */

/* Map relationship metadata to calibration modifiers for the prompt.
 * Pass NULL for any field not available. Returns bytes written. */
size_t hu_conversation_calibrate_relationship(const char *relationship_stage,
                                              const char *warmth_level,
                                              const char *vulnerability_level, char *buf,
                                              size_t cap);

/* ── Group chat classifier ───────────────────────────────────────────── */

typedef enum hu_group_response {
    HU_GROUP_RESPOND = 0, /* full response */
    HU_GROUP_BRIEF = 1,   /* short acknowledgment */
    HU_GROUP_SKIP = 2,    /* don't respond */
} hu_group_response_t;

/* Classify whether to respond in a group chat context.
 * bot_name is used to detect direct addressing. */
hu_group_response_t hu_conversation_classify_group(const char *msg, size_t msg_len,
                                                   const char *bot_name, size_t bot_name_len,
                                                   const hu_channel_history_entry_t *entries,
                                                   size_t count);

/* ── Inline reply classifier (iMessage quoted text fallback) ───────────────── */

/* Decide whether to use quoted-text inline reply: "> {original}\n\n{response}".
 * Returns true when: multiple questions pending, conversation diverged topics,
 * or they referenced something from earlier ("you said", "earlier", "what about").
 * Returns false for single-topic conversations. */
bool hu_conversation_should_inline_reply(const hu_channel_history_entry_t *entries, size_t count,
                                         const char *last_msg, size_t last_msg_len);

/* ── Tapback-vs-text decision engine ─────────────────────────────────────── */

/* Decide whether to send tapback only, text only, both, or no response.
 * Runs before hu_conversation_classify_reaction; only call classify_reaction
 * when decision is TAPBACK_ONLY or TAPBACK_AND_TEXT. */
typedef enum hu_tapback_decision {
    HU_TAPBACK_ONLY,     /* send tapback, no text */
    HU_TEXT_ONLY,        /* send text, no tapback */
    HU_TAPBACK_AND_TEXT, /* both */
    HU_NO_RESPONSE,      /* neither */
} hu_tapback_decision_t;

hu_tapback_decision_t hu_conversation_classify_tapback_decision(
    const char *message, size_t message_len, const hu_channel_history_entry_t *entries,
    size_t entry_count, const struct hu_contact_profile *contact, uint32_t seed);

/* ── Reaction classifier ───────────────────────────────────────────────── */

/* Classify whether to send a reaction instead of (or in addition to) a text reply.
 * Returns HU_REACTION_NONE if a text reply is more appropriate.
 * Takes into account message content, conversation flow, and randomness via seed.
 * Only call when tapback decision is TAPBACK_ONLY or TAPBACK_AND_TEXT. */
hu_reaction_type_t hu_conversation_classify_reaction(const char *msg, size_t msg_len, bool from_me,
                                                     const hu_channel_history_entry_t *entries,
                                                     size_t entry_count, uint32_t seed);

/* Classify photo reaction from vision description. When the message is a photo (with vision
 * description), decide: heart tapback for sunset/family/selfie, haha for funny, etc.
 * Returns HU_REACTION_NONE if text response preferred (e.g. food, screenshot, error).
 * Use seed for probabilistic variation. */
hu_reaction_type_t hu_conversation_classify_photo_reaction(const char *vision_description,
                                                           size_t desc_len,
                                                           const struct hu_contact_profile *contact,
                                                           uint32_t seed);

/* Extract vision description from combined content containing "[They sent a photo: {desc}]".
 * If found, sets *out_start and *out_len. Returns false if not found. */
bool hu_conversation_extract_vision_description(const char *combined, size_t combined_len,
                                                const char **out_start, size_t *out_len);

/* ── Filler word injection (Rime-research placement) ──────────────────── */

/* Probabilistically inject context-appropriate fillers into a response.
 * Placement: utterance start, after first word, before complex words.
 * Channel type controls filler intensity (messaging=high, email=none).
 * Modifies buf in-place. cap is buffer capacity. Returns new length. */
size_t hu_conversation_apply_fillers(char *buf, size_t len, size_t cap, uint32_t seed,
                                     const char *channel_type, size_t channel_type_len);

/* ── Stylometric variance ─────────────────────────────────────────────── */

/* Post-process response to vary sentence structure and vocabulary complexity.
 * Occasionally shortens long sentences, contracts "it is" -> "it's" etc.
 * Modifies buf in-place. Returns new length. */
size_t hu_conversation_vary_complexity(char *buf, size_t len, uint32_t seed);

/* ── Bidirectional sentiment momentum ─────────────────────────────────── */

/* Analyze recent message sentiment trend and build a mood-matching directive.
 * Returns allocated string or NULL. Caller frees. */
char *hu_conversation_build_sentiment_momentum(hu_allocator_t *alloc,
                                               const hu_channel_history_entry_t *entries,
                                               size_t count, size_t *out_len);

/* ── Conversation depth signal ────────────────────────────────────────── */

/* Build depth-based persona reinforcement for long conversations.
 * Returns allocated string or NULL. Caller frees. */
char *hu_conversation_build_depth_signal(hu_allocator_t *alloc,
                                         const hu_channel_history_entry_t *entries, size_t count,
                                         size_t *out_len);

/* ── Topic tangent/callback engine ────────────────────────────────────── */

/* With ~6% probability per turn, inject a callback to an earlier topic.
 * Uses STM-style topic tracking from conversation history.
 * Returns allocated string or NULL. Caller frees. */
char *hu_conversation_build_tangent_callback(hu_allocator_t *alloc,
                                             const hu_channel_history_entry_t *entries,
                                             size_t count, uint32_t seed, size_t *out_len);

/* ── AI disclosure detection (pre-send guardrail) ─────────────────────── */

/* Check if a response accidentally reveals AI identity.
 * Returns true if the response contains AI self-identification patterns. */
bool hu_conversation_check_ai_disclosure(const char *response, size_t response_len);

/* ── Banned AI phrases post-processor ─────────────────────────────────── */

/* Strip or replace common AI-giveaway phrases from a response.
 * "Great question" -> removed, "crucial" -> "important", etc.
 * Modifies buf in-place. Returns new length. */
size_t hu_conversation_strip_ai_phrases(char *buf, size_t len);

/* ── iMessage effect classifier (keyword-triggered, client-side) ───────── */

/* Classify if a message contains iMessage effect trigger phrases.
 * Returns effect name (static string) or NULL. Effect is applied automatically
 * on recipient's device when the trigger phrase is sent as plain text. */
const char *hu_conversation_classify_effect(const char *msg, size_t msg_len);

/* ── Media-type awareness ─────────────────────────────────────────────── */

/* Detect if the last user message was a photo/attachment.
 * Returns true if the message or recent history contains attachment markers. */
bool hu_conversation_is_media_message(const char *msg, size_t msg_len,
                                      const hu_channel_history_entry_t *entries, size_t count);

#endif /* HU_CONTEXT_CONVERSATION_H */
