#ifndef SC_CONTEXT_CONVERSATION_H
#define SC_CONTEXT_CONVERSATION_H

#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Thread callback detection: identify opportunities to naturally return to
 * a topic from earlier in the conversation or from a previous session.
 * Scans history for topic transitions and identifies the best callback candidate.
 * Returns a context injection string or NULL if no good callback found.
 * Caller owns returned string. */
char *sc_conversation_build_callback(sc_allocator_t *alloc,
                                     const sc_channel_history_entry_t *entries, size_t count,
                                     size_t *out_len);

/* Build a complete conversation awareness context string from channel history.
 * Includes: conversation thread, emotional analysis, verbosity mirroring,
 * conversation phase, time-of-day triggers, detected user states.
 * Caller owns returned string. Returns NULL if no entries. */
char *sc_conversation_build_awareness(sc_allocator_t *alloc,
                                      const sc_channel_history_entry_t *entries, size_t count,
                                      size_t *out_len);

/* Conversation quality score (0-100). Evaluates a draft response for:
 * brevity, validation/reflection, repair/rephrase, and follow-up.
 * Qualitative: compares to their average length, energy level; needs_revision
 * only on gross mismatches. guidance describes the issue for prompt injection. */
typedef struct sc_quality_score {
    int total;
    int brevity;
    int validation;
    int warmth;
    int naturalness;
    bool needs_revision;
    char guidance[256]; /* human-readable issue description when mismatched */
} sc_quality_score_t;

sc_quality_score_t sc_conversation_evaluate_quality(const char *response, size_t response_len,
                                                    const sc_channel_history_entry_t *entries,
                                                    size_t count, uint32_t max_chars);

/* Honesty guardrail: detect "did you do X?" questions and inject honest context.
 * Returns a context string if an honesty injection is needed, NULL otherwise.
 * Caller owns returned string. */
char *sc_conversation_honesty_check(sc_allocator_t *alloc, const char *message, size_t message_len);

/* Narrative arc phase for the conversation.
 * Adds structure/climax/intervention guidance to the awareness buffer. */
typedef enum sc_narrative_phase {
    SC_NARRATIVE_OPENING = 0,
    SC_NARRATIVE_BUILDING,
    SC_NARRATIVE_APPROACHING_CLIMAX,
    SC_NARRATIVE_PEAK,
    SC_NARRATIVE_RELEASE,
    SC_NARRATIVE_CLOSING,
} sc_narrative_phase_t;

sc_narrative_phase_t sc_conversation_detect_narrative(const sc_channel_history_entry_t *entries,
                                                      size_t count);

/* Engagement level detection */
typedef enum sc_engagement_level {
    SC_ENGAGEMENT_HIGH = 0,
    SC_ENGAGEMENT_MODERATE,
    SC_ENGAGEMENT_LOW,
    SC_ENGAGEMENT_DISTRACTED,
} sc_engagement_level_t;

sc_engagement_level_t sc_conversation_detect_engagement(const sc_channel_history_entry_t *entries,
                                                        size_t count);

/* Emotional valence from conversation (-1.0 to 1.0, negative=distressed, positive=happy) */
typedef struct sc_emotional_state {
    float valence;
    float intensity;
    bool concerning;
    const char *dominant_emotion;
} sc_emotional_state_t;

sc_emotional_state_t sc_conversation_detect_emotion(const sc_channel_history_entry_t *entries,
                                                    size_t count);

/* ── Typo correction fragment (*meant) ─────────────────────────────────── */

/* Check if a typo was introduced and generate a correction fragment.
 * Compares original text with typo-applied text to find the changed word.
 * If a typo was found, writes a correction like "*meeting" into out_buf.
 * Returns length of correction (0 if no typo detected or no correction needed).
 * correction_chance is probability 0-100 of generating correction (suggest ~40). */
size_t sc_conversation_generate_correction(const char *original, size_t original_len,
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
typedef struct sc_message_fragment {
    char *text;
    size_t text_len;
    uint32_t delay_ms; /* suggested inter-message delay before this fragment */
} sc_message_fragment_t;

size_t sc_conversation_split_response(sc_allocator_t *alloc, const char *response,
                                      size_t response_len, sc_message_fragment_t *fragments,
                                      size_t max_fragments);

/* ── Situational length calibration ───────────────────────────────────── */

/* Classify the last incoming message and produce human-level length guidance.
 * Analyzes message type (question, emotional, greeting, logistics, etc.) and
 * produces a short directive string for the prompt like:
 *   "CALIBRATION: They asked a direct question. Answer it, add one detail. 5-15 words."
 * Writes into buf (up to cap bytes). Returns bytes written (0 if nothing useful). */
size_t sc_conversation_calibrate_length(const char *last_msg, size_t last_msg_len,
                                        const sc_channel_history_entry_t *entries, size_t count,
                                        char *buf, size_t cap);

/* ── Texting style analysis ───────────────────────────────────────────── */

/* Analyze the other person's texting style from conversation history.
 * Produces a style context string for the prompt with capitalization,
 * punctuation, fragmentation, and abbreviation patterns.
 * Caller owns returned string. Returns NULL if insufficient data. */
char *sc_conversation_analyze_style(sc_allocator_t *alloc,
                                    const sc_channel_history_entry_t *entries, size_t count,
                                    size_t *out_len);

/* ── Typing quirk post-processing ─────────────────────────────────────── */

/* Apply typing quirks to a response as deterministic post-processing.
 * Supported quirks: "lowercase", "no_periods", "no_commas",
 * "no_apostrophes", "double_space_to_newline", "variable_punctuation".
 * Modifies the buffer in-place. Returns the new length (may shrink). */
size_t sc_conversation_apply_typing_quirks(char *buf, size_t len, const char *const *quirks,
                                           size_t quirks_count);

/* Apply realistic typo simulation to a response. Introduces at most 1 typo
 * per message segment. Controlled by seed for determinism in tests.
 * Never introduces typos into proper nouns (words starting with uppercase
 * that aren't the first word of a sentence).
 * Returns the new length (may grow by at most 1 char for transposition).
 * buf must have capacity for len+1 chars. */
size_t sc_conversation_apply_typos(char *buf, size_t len, size_t cap, uint32_t seed);

/* ── Response action classification ───────────────────────────────────── */

typedef enum sc_response_action {
    SC_RESPONSE_FULL = 0,     /* generate full response */
    SC_RESPONSE_BRIEF = 1,    /* ultra-short: "yeah", "lol", "nice" */
    SC_RESPONSE_SKIP = 2,     /* don't respond at all */
    SC_RESPONSE_DELAY = 3,    /* full response but with extra delay */
    SC_RESPONSE_THINKING = 4, /* two-phase: send filler first, then real response after delay */
} sc_response_action_t;

typedef struct sc_thinking_response {
    char filler[64]; /* "hmm", "that's a good question", "let me think about that" */
    size_t filler_len;
    uint32_t delay_ms; /* delay before the real response (30000-60000ms) */
} sc_thinking_response_t;

/* Classify whether this message warrants a "thinking" response.
 * Returns true if the message is complex/emotional enough.
 * Fills out the thinking response with an appropriate filler message. */
bool sc_conversation_classify_thinking(const char *msg, size_t msg_len,
                                       const sc_channel_history_entry_t *entries,
                                       size_t entry_count, sc_thinking_response_t *out,
                                       uint32_t seed);

/* Classify how to respond, factoring in conversation context. */
sc_response_action_t sc_conversation_classify_response(const char *msg, size_t msg_len,
                                                       const sc_channel_history_entry_t *entries,
                                                       size_t entry_count,
                                                       uint32_t *delay_extra_ms);

/* ── URL extraction and link-sharing detection ───────────────────────── */

/* Extract URLs from a text message. Returns count of URLs found.
 * Each URL is written as a separate entry in urls[] (up to max_urls).
 * Caller provides pre-allocated url_buf entries. */
typedef struct sc_url_extract {
    const char *start; /* pointer into the original text */
    size_t len;
} sc_url_extract_t;

size_t sc_conversation_extract_urls(const char *text, size_t text_len, sc_url_extract_t *urls,
                                    size_t max_urls);

/* Detect if a message context suggests sharing a link.
 * Returns true if the LLM should be prompted to find and share a URL.
 * Triggers on: recommendations, "check this out", "have you seen", "look at this",
 * "you should try", "here's a link" */
bool sc_conversation_should_share_link(const char *msg, size_t msg_len,
                                       const sc_channel_history_entry_t *entries,
                                       size_t entry_count);

/* ── Attachment context for prompts ──────────────────────────────────── */

/* Generate context for the prompt when attachments are detected in history.
 * Scans for [Photo shared], [Attachment shared], [image or attachment], etc.
 * Returns allocated string; caller must free. NULL if no attachments found. */
char *sc_conversation_attachment_context(sc_allocator_t *alloc,
                                         const sc_channel_history_entry_t *entries, size_t count,
                                         size_t *out_len);

/* ── Anti-repetition detection ───────────────────────────────────────── */

/* Analyze recent "from_me" messages for repetitive patterns (openers,
 * always ending with questions, filler words). Writes guidance into buf.
 * Returns bytes written (0 if no patterns detected). */
size_t sc_conversation_detect_repetition(const sc_channel_history_entry_t *entries, size_t count,
                                         char *buf, size_t cap);

/* ── Relationship-tier calibration ───────────────────────────────────── */

/* Map relationship metadata to calibration modifiers for the prompt.
 * Pass NULL for any field not available. Returns bytes written. */
size_t sc_conversation_calibrate_relationship(const char *relationship_stage,
                                              const char *warmth_level,
                                              const char *vulnerability_level, char *buf,
                                              size_t cap);

/* ── Group chat classifier ───────────────────────────────────────────── */

typedef enum sc_group_response {
    SC_GROUP_RESPOND = 0, /* full response */
    SC_GROUP_BRIEF = 1,   /* short acknowledgment */
    SC_GROUP_SKIP = 2,    /* don't respond */
} sc_group_response_t;

/* Classify whether to respond in a group chat context.
 * bot_name is used to detect direct addressing. */
sc_group_response_t sc_conversation_classify_group(const char *msg, size_t msg_len,
                                                   const char *bot_name, size_t bot_name_len,
                                                   const sc_channel_history_entry_t *entries,
                                                   size_t count);

/* ── Reaction classifier ───────────────────────────────────────────────── */

/* Classify whether to send a reaction instead of (or in addition to) a text reply.
 * Returns SC_REACTION_NONE if a text reply is more appropriate.
 * Takes into account message content, conversation flow, and randomness via seed. */
sc_reaction_type_t sc_conversation_classify_reaction(const char *msg, size_t msg_len, bool from_me,
                                                     const sc_channel_history_entry_t *entries,
                                                     size_t entry_count, uint32_t seed);

/* ── Filler word injection (Rime-research placement) ──────────────────── */

/* Probabilistically inject context-appropriate fillers into a response.
 * Placement: utterance start, after first word, before complex words.
 * Channel type controls filler intensity (messaging=high, email=none).
 * Modifies buf in-place. cap is buffer capacity. Returns new length. */
size_t sc_conversation_apply_fillers(char *buf, size_t len, size_t cap, uint32_t seed,
                                     const char *channel_type, size_t channel_type_len);

/* ── Stylometric variance ─────────────────────────────────────────────── */

/* Post-process response to vary sentence structure and vocabulary complexity.
 * Occasionally shortens long sentences, contracts "it is" -> "it's" etc.
 * Modifies buf in-place. Returns new length. */
size_t sc_conversation_vary_complexity(char *buf, size_t len, uint32_t seed);

/* ── Bidirectional sentiment momentum ─────────────────────────────────── */

/* Analyze recent message sentiment trend and build a mood-matching directive.
 * Returns allocated string or NULL. Caller frees. */
char *sc_conversation_build_sentiment_momentum(sc_allocator_t *alloc,
                                               const sc_channel_history_entry_t *entries,
                                               size_t count, size_t *out_len);

/* ── Conversation depth signal ────────────────────────────────────────── */

/* Build depth-based persona reinforcement for long conversations.
 * Returns allocated string or NULL. Caller frees. */
char *sc_conversation_build_depth_signal(sc_allocator_t *alloc,
                                         const sc_channel_history_entry_t *entries, size_t count,
                                         size_t *out_len);

/* ── Topic tangent/callback engine ────────────────────────────────────── */

/* With ~6% probability per turn, inject a callback to an earlier topic.
 * Uses STM-style topic tracking from conversation history.
 * Returns allocated string or NULL. Caller frees. */
char *sc_conversation_build_tangent_callback(sc_allocator_t *alloc,
                                             const sc_channel_history_entry_t *entries,
                                             size_t count, uint32_t seed, size_t *out_len);

/* ── AI disclosure detection (pre-send guardrail) ─────────────────────── */

/* Check if a response accidentally reveals AI identity.
 * Returns true if the response contains AI self-identification patterns. */
bool sc_conversation_check_ai_disclosure(const char *response, size_t response_len);

/* ── Banned AI phrases post-processor ─────────────────────────────────── */

/* Strip or replace common AI-giveaway phrases from a response.
 * "Great question" -> removed, "crucial" -> "important", etc.
 * Modifies buf in-place. Returns new length. */
size_t sc_conversation_strip_ai_phrases(char *buf, size_t len);

/* ── Media-type awareness ─────────────────────────────────────────────── */

/* Detect if the last user message was a photo/attachment.
 * Returns true if the message or recent history contains attachment markers. */
bool sc_conversation_is_media_message(const char *msg, size_t msg_len,
                                      const sc_channel_history_entry_t *entries, size_t count);

#endif /* SC_CONTEXT_CONVERSATION_H */
