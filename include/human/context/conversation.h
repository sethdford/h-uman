#ifndef HU_CONTEXT_CONVERSATION_H
#define HU_CONTEXT_CONVERSATION_H

#include "human/channel.h"
#include "human/channel_class.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/filler_recency.h"
#include "human/memory.h"
#include "human/persona.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Commitment detection with deadline parsing (F20) ───────────────────── */

/* Parse deadline from message. Returns unix timestamp or 0 if no deadline found. */
int64_t hu_conversation_parse_deadline(const char *msg, size_t msg_len, int64_t now_ts);

/* Detect if message contains a commitment. Fills description_out, who_out ("me"/"them").
 * from_me: true if speaker is the user/agent. Returns true if commitment detected. */
bool hu_conversation_detect_commitment(const char *msg, size_t msg_len, char *description_out,
                                       size_t desc_cap, char *who_out, size_t who_cap,
                                       bool from_me);

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
 * Caller owns returned string. Returns NULL if no entries.
 * *out_len is content length; free with *out_len + 1 bytes. */
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
 * Caller owns returned string; free with strlen(result) + 1 bytes. */
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

/* LLM-backed emotion analysis with heuristic fallback when no provider or low confidence. */
hu_emotional_state_t hu_conversation_detect_emotion_llm(hu_allocator_t *alloc,
                                                        hu_provider_t *provider, const char *model,
                                                        size_t model_len,
                                                        const hu_channel_history_entry_t *entries,
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

/* ── Micro-moment extraction (F18) ─────────────────────────────────────────── */

/* Extract small but significant details from a message. Returns count (0–3).
 * Heuristic extraction: named entities, places, preferences, life events.
 * Caller provides facts[][256] and significances[][128], max_facts typically 3. */
int hu_conversation_extract_micro_moments(const char *msg, size_t msg_len, char facts[][256],
                                          char significances[][128], size_t max_facts);

/* ── Inside joke detection (F19) ────────────────────────────────────────── */

/* Returns true if message suggests an inside joke: "remember when", "that time we",
 * "you always say", "[X] energy", "that's our thing", "classic [name]",
 * or shared phrase from history. */
bool hu_conversation_detect_inside_joke(const char *msg, size_t msg_len,
                                        const hu_channel_history_entry_t *entries, size_t count);

/* ── Emotional tone classification (F22) ───────────────────────────────── */

/* Classify emotional tone of a message using keyword heuristics.
 * Returns one of: "stressed", "excited", "neutral", "sad", "anxious", "happy", "frustrated".
 * Static string; do not free. */
const char *hu_conversation_classify_emotional_tone(const char *msg, size_t msg_len);

/* Extract first 2-3 significant words (skip stopwords) as topic. Writes into out, returns length.
 */
size_t hu_conversation_extract_topic(const char *msg, size_t msg_len, char *out, size_t cap);

/* ── Growth celebration detection (F24) ─────────────────────────────────── */

/* Detect when message contains positive outcome keywords (it went great, i got the job,
 * nailed it, crushed it, i passed, got promoted, it worked out, turned out well).
 * Extracts topic from context. Returns true if growth opportunity detected.
 * Fills topic_out and after_state_out for storage. */
bool hu_conversation_detect_growth_opportunity(const char *msg, size_t msg_len, char *topic_out,
                                               size_t topic_cap, char *after_state_out,
                                               size_t after_cap);

/* ── Avoidance pattern detection (F21) ─────────────────────────────────── */

/* Detect when last 2 user messages have different topics. Simple topic extraction:
 * first 2-3 significant words (skip "i", "the", "a", etc.). Returns true if
 * topic change detected; fills topic_before (older) and topic_after (newer). */
bool hu_conversation_detect_topic_change(const hu_channel_history_entry_t *entries, size_t count,
                                         char *topic_before, size_t before_cap, char *topic_after,
                                         size_t after_cap);

/* ── Call escalation (F49) ───────────────────────────────────────────────── */

/* Classify if message complexity + emotional intensity suggest a call would be better.
 * Score components: crisis keywords (0.4), complexity (0.3), emotional intensity (0.3).
 * should_suggest when score >= 0.6. Prefer false negatives; never escalate for
 * logistics, quick questions, or casual chat. */
typedef struct hu_call_escalation {
    bool should_suggest;
    float score;
} hu_call_escalation_t;

hu_call_escalation_t
hu_conversation_should_escalate_to_call(const char *msg, size_t msg_len,
                                        const hu_channel_history_entry_t *entries, size_t count);

/* Build [CALL: ...] directive for prompt injection when classifier triggers.
 * Writes into buf (up to cap bytes). Returns bytes written (0 if invalid args). */
size_t hu_conversation_build_call_directive(const char *msg, size_t msg_len, char *buf, size_t cap);

/* ── Linguistic mirroring (F28) ──────────────────────────────────────────── */
size_t hu_conversation_build_mirror_directive(const char *distinctive_words, size_t words_len,
                                              uint32_t seed, float probability, char *buf,
                                              size_t cap);

/* ── Delayed follow-up topic extraction (F8) ─────────────────────────────── */
size_t hu_conversation_extract_followup_topic(const char *msg, size_t msg_len, char *topic_out,
                                              size_t cap);

/* ── Double-text decision (F9) ───────────────────────────────────────────── */
bool hu_conversation_should_double_text(const char *last_response, size_t resp_len,
                                        const hu_channel_history_entry_t *entries, size_t count,
                                        uint8_t hour_local, uint32_t seed, float probability);

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

/* ── Comfort pattern directive (F27) ─────────────────────────────────── */

/* Build [COMFORT: This contact responds well to {response_type} when {emotion}.]
 * Writes into buf (up to cap bytes). Returns bytes written (0 if invalid args). */
size_t hu_conversation_build_comfort_directive(const char *response_type, size_t type_len,
                                               const char *emotion, size_t emotion_len, char *buf,
                                               size_t cap);

/* ── First-time vulnerability detection (F17) ─────────────────────────────── */

/* Extract vulnerability topic category from message. Returns static string
 * (e.g. "illness", "job_loss") or NULL if none detected. */
const char *hu_conversation_extract_vulnerability_topic(const char *msg, size_t msg_len);

/* Check if contact has ever had an emotional_moment with this topic before.
 * Returns true if count == 0 (first time). If memory/db NULL, returns true. */
#ifdef HU_ENABLE_SQLITE
bool hu_conversation_is_first_time_topic(hu_memory_t *memory, const char *contact_id,
                                         size_t contact_id_len, const char *topic,
                                         size_t topic_len);
#endif

typedef struct hu_vulnerability_state {
    bool first_time;
    const char *topic_category; /* static string, do not free */
    float intensity;
} hu_vulnerability_state_t;

hu_vulnerability_state_t hu_conversation_detect_first_time_vulnerability(const char *msg,
                                                                         size_t msg_len,
                                                                         hu_memory_t *memory,
                                                                         const char *contact_id,
                                                                         size_t contact_id_len);

size_t hu_conversation_build_vulnerability_directive(const hu_vulnerability_state_t *state,
                                                     char *buf, size_t cap);

/* ── Context modifiers (F16) ─────────────────────────────────────────────── */

/* Build [CONTEXT: ...] directives based on heavy topics, personal sharing,
 * high emotion, and early-turn detection. Uses persona context_modifiers when
 * non-NULL; otherwise defaults (0.4, 1.6, 1.5, 1.4). Writes into buf, returns
 * bytes written. */
size_t hu_conversation_build_context_modifiers(const hu_channel_history_entry_t *entries,
                                               size_t count, const hu_emotional_state_t *emo,
                                               const hu_context_modifiers_t *mods, char *buf,
                                               size_t cap);

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
 * max_chars: per-channel bubble size hint (e.g. from get_response_constraints).
 * When 0, uses the historical default (~300 chars, iMessage-style thresholds).
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
                                      size_t max_fragments, uint32_t max_chars);

/* ── Situational length calibration ───────────────────────────────────── */

/* Return max response characters based on incoming message length.
 * Formula: incoming_len * 2.0, capped at 300, minimum 15.
 * Use for max_response_chars to match response length within ~1.5x ratio. */
int hu_conversation_max_response_chars(size_t incoming_len);

/* Like hu_conversation_max_response_chars, but scales length with relationship warmth
 * (session stage + optional persona contact hints). Group threads should pass contact=NULL
 * and rely on the caller forcing plain max for is_group. */
int hu_conversation_max_response_chars_relational(size_t incoming_len,
                                                  const hu_contact_profile_t *contact,
                                                  hu_relationship_stage_t session_stage);

/* When the daemon is in "brief mode", this is the hard character ceiling before truncation.
 * Group chats stay tight (50). Trusted 1:1 contacts get headroom so replies stay human. */
uint32_t hu_conversation_brief_char_cap(bool is_group, const hu_contact_profile_t *contact,
                                        hu_relationship_stage_t session_stage);

/* Classify the last incoming message and produce human-level length guidance.
 * Analyzes message type (question, emotional, greeting, logistics, etc.) and
 * produces a short directive string for the prompt like:
 *   "CALIBRATION: They asked a direct question. Answer it, add one detail. 5-15 words."
 * Writes into buf (up to cap bytes). Returns bytes written (0 if nothing useful). */
size_t hu_conversation_calibrate_length(const char *last_msg, size_t last_msg_len,
                                        const hu_channel_history_entry_t *entries, size_t count,
                                        char *buf, size_t cap);

/* Same as hu_conversation_calibrate_length, but uses relational caps for DMs when is_group
 * is false; when is_group is true, uses the neutral 2x ratio only. */
size_t hu_conversation_calibrate_length_for_contact(const char *last_msg, size_t last_msg_len,
                                                    const hu_channel_history_entry_t *entries,
                                                    size_t count, bool is_group,
                                                    const hu_contact_profile_t *contact,
                                                    hu_relationship_stage_t session_stage,
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
    HU_RESPONSE_LEAVE_ON_READ = 5, /* deliberate non-response as social signal (<2%) */
} hu_response_action_t;

typedef struct hu_thinking_response {
    char filler[64]; /* "hmm", "that's a good question", "let me think about that" */
    size_t filler_len;
    uint32_t delay_ms; /* delay before the real response (30000-60000ms) */
} hu_thinking_response_t;

/* Context for persona-driven thinking filler selection (PCTT Task 4). */
typedef struct hu_thinking_context {
    const hu_persona_t *persona; /* active persona, may be NULL */
    const char *channel_name;    /* e.g. "imessage", "slack" */
    const char *chat_id;         /* send_target — stable chat identifier */
    size_t chat_id_len;
    hu_filler_recency_t *recency; /* may be NULL — caller's recency LRU */
    uint32_t seed;                /* random seed */
} hu_thinking_context_t;

/* Classify whether this message warrants a "thinking" response.
 * Returns true if the message is complex enough and a persona filler bank
 * is available for the channel.  Fills out with the chosen filler + delay. */
bool hu_conversation_classify_thinking(const hu_thinking_context_t *ctx, const char *msg,
                                       size_t msg_len, const hu_channel_history_entry_t *entries,
                                       size_t entry_count, hu_thinking_response_t *out);

/* Classify how to respond, factoring in conversation context. */
hu_response_action_t hu_conversation_classify_response(const char *msg, size_t msg_len,
                                                       const hu_channel_history_entry_t *entries,
                                                       size_t entry_count,
                                                       uint32_t *delay_extra_ms);

/* Leave-on-read classifier (F46): intentionally not responding as a social signal.
 * Returns true when: disagreement/space/low-content AND seed%100 < threshold_pct.
 * Never for: direct questions (?), emotional crisis ("help me"), concerning emotion, group chats.
 * Caller must pass is_group=false; never call for group chats.
 * threshold_pct: 0 = use default (10%). */
bool hu_conversation_should_leave_on_read(const char *msg, size_t msg_len,
                                          const hu_channel_history_entry_t *entries, size_t count,
                                          uint32_t seed, uint8_t threshold_pct);

/* Leave-on-read state-machine decision (F46): combines the per-contact active-period
 * status, group-chat policy, and the heuristic classifier's verdict into a single
 * decision the daemon can act on.
 *
 * - Groups never leave-on-read (we always respond), regardless of other flags.
 * - An existing active period takes precedence over a new helper-triggered period;
 *   we do not extend a running silence by re-rolling.
 *
 * Pure predicate over three booleans so the policy can be unit-tested without
 * driving the daemon's ring-buffer state. Per .claude/rules/security-predicate-extraction.md,
 * inputs are facts (not pointers to mutable state) and output is a small enum. */
typedef enum {
    HU_LOR_RESPOND = 0,       /* respond normally; do not skip */
    HU_LOR_ALREADY_IN_PERIOD, /* skip; an active leave-on-read window is still running */
    HU_LOR_TRIGGER_NEW,       /* skip and start a new leave-on-read period */
} hu_leave_on_read_decision_t;

hu_leave_on_read_decision_t hu_leave_on_read_decide(bool is_group_chat,
                                                    bool already_in_active_period,
                                                    bool helper_says_should_leave);

/* Natural conversation drop-off: returns skip probability 0-100.
 * Caller rolls (seed % 100) < prob to decide SKIP. Used when action is FULL/BRIEF. */
int hu_conversation_classify_dropoff(const char *message, size_t message_len,
                                     const hu_channel_history_entry_t *entries, size_t entry_count,
                                     uint32_t seed);

/* ── Active listening backchannels (F29) ───────────────────────────────── */

/* Returns true if message is narrative/venting AND probability roll passes (seed-based).
 * Narrative: length > 80 chars, no question, contains "and then", "so i", "anyway",
 * "long story", or first person ("i ", "my ", "me "). Or: last 2–3 their messages
 * are long and we haven't replied yet. */
bool hu_conversation_should_backchannel(const char *msg, size_t msg_len,
                                        const hu_channel_history_entry_t *entries, size_t count,
                                        uint32_t seed, float probability);

/* Pick a backchannel phrase at random using seed. Returns length written. */
size_t hu_conversation_pick_backchannel(uint32_t seed, char *buf, size_t cap);

/* ── Burst messaging (F45) ────────────────────────────────────────────── */

/* Returns true when message suggests urgency/excitement AND energy is EXCITED or ANXIOUS
 * AND probability roll passes. Triggers: "omg", "oh my god", "just saw", "did you see",
 * "holy shit", "emergency", "are you okay", "!!!" (3+ exclamation marks). */
bool hu_conversation_should_burst(const char *msg, size_t msg_len,
                                  const hu_channel_history_entry_t *entries, size_t count,
                                  uint32_t seed, float probability);

/* Build burst-mode prompt directive. Writes into buf, returns bytes written. */
size_t hu_conversation_build_burst_prompt(char *buf, size_t cap);

/* Parse JSON array from LLM burst response. Extracts up to max_messages strings into
 * messages[][256]. Returns count. Simple parsing: find [, then quoted strings. */
int hu_conversation_parse_burst_response(const char *response, size_t resp_len,
                                         char messages[][256], size_t max_messages);

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

/* ── Behavior configuration ──────────────────────────────────────────── */

/* Load conversation word lists from embedded JSON data files.
 * Call once at startup. Falls back to hardcoded defaults on failure. */
hu_error_t hu_conversation_data_init(hu_allocator_t *alloc);
void hu_conversation_data_cleanup(void);

/* Set configurable behavior thresholds.
 * Call this after loading config to apply user-defined values.
 * Pass 0 to keep default for any threshold. */
void hu_conversation_set_thresholds(uint32_t consecutive_limit, uint32_t participation_pct,
                                    uint32_t max_response_chars, uint32_t min_response_chars);

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

/* ── Text disfluency (F33) ───────────────────────────────────────────── */

/* Apply natural text imperfections: "i mean", "like", "you know", trailing "...",
 * self-correction "wait no", "actually". Casual conversations only.
 * Skip if contact has relationship_type "coworker" or formality "formal".
 * frequency: 0.0–1.0 (default 0.15). Only one disfluency per call. */
size_t hu_conversation_apply_disfluency(char *buf, size_t len, size_t cap, uint32_t seed,
                                        float frequency, const struct hu_contact_profile *contact,
                                        const char *formality, size_t formality_len);

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

/* Strip hallucinated model artifacts from responses: ChatML tokens (<|word|>,
 * <|word>...<word|>), sequence markers (</s>, [INST]), and XML reasoning
 * wrappers (<thinking>...</thinking>, <analysis>...</analysis>, etc.).
 * Modifies buf in-place. Returns new length. */
size_t hu_conversation_strip_channel_tags(char *buf, size_t len);

/* Strip formal structure tells from casual-channel messages: numbered lists,
 * em-dashes (→ comma), en-dashes (→ hyphen). In-place. Returns new length. */
size_t hu_conversation_strip_formal_structure(char *buf, size_t len);

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

/* ── Nonverbal sound injection (F39) ───────────────────────────────── */

/* Inject nonverbal sounds into transcript for TTS. Max 1 injection per message.
 * Modifies buf in-place. Returns new length. */
size_t hu_conversation_inject_nonverbals(char *buf, size_t len, size_t cap, uint32_t seed,
                                         bool enabled);

/* F57: Multi-thread energy management — track per-conversation energy to
   prevent tone leakage across simultaneous chats. */
#define HU_MAX_CONCURRENT_CHATS 16

typedef struct hu_thread_energy_entry {
    char contact_id[128];
    hu_energy_level_t energy;
    uint64_t last_updated_ms;
} hu_thread_energy_entry_t;

typedef struct hu_thread_energy_tracker {
    hu_thread_energy_entry_t entries[HU_MAX_CONCURRENT_CHATS];
    size_t count;
} hu_thread_energy_tracker_t;

void hu_thread_energy_init(hu_thread_energy_tracker_t *tracker);
void hu_thread_energy_update(hu_thread_energy_tracker_t *tracker, const char *contact_id,
                             size_t cid_len, hu_energy_level_t energy, uint64_t now_ms);
hu_energy_level_t hu_thread_energy_get(const hu_thread_energy_tracker_t *tracker,
                                       const char *contact_id, size_t cid_len);
size_t hu_thread_energy_build_isolation_hint(const hu_thread_energy_tracker_t *tracker,
                                             const char *contact_id, size_t cid_len, char *buf,
                                             size_t cap);

/* ── Cold restart detection ──────────────────────────────────────────── */

/* Detect when there's been a significant time gap (>4 hours) between messages.
 * Injects a directive telling the LLM to start fresh instead of picking up
 * a stale conversation thread. Writes into buf, returns bytes written. */
size_t hu_conversation_build_cold_restart_hint(const hu_channel_history_entry_t *entries,
                                               size_t count, char *buf, size_t cap);

/* ── Self-reaction on own messages ───────────────────────────────────── */

/* Occasionally react to your own sent message (~2% chance).
 * Self-deprecating haha on jokes/awkward messages, emphasis on strong statements.
 * Only call for from_me=true messages. Returns HU_REACTION_NONE most of the time. */
hu_reaction_type_t hu_conversation_classify_self_reaction(const char *msg, size_t msg_len,
                                                          uint32_t seed);

/* ── Group chat participant mention ──────────────────────────────────── */

/* Build a hint for group chats that includes the sender's first name,
 * encouraging natural name-addressing. Writes into buf, returns bytes written. */
size_t hu_conversation_build_group_mention_hint(const char *first_name, size_t first_name_len,
                                                bool is_group, char *buf, size_t cap);

/* ── Link content awareness ──────────────────────────────────────────── */

/* Detect URLs in inbound message and build prompt context for natural reactions.
 * Writes into buf, returns bytes written. */
size_t hu_conversation_build_link_context(const char *msg, size_t msg_len, char *buf, size_t cap);

/* ── GIF decision engine ─────────────────────────────────────────────── */

/* Decide whether to respond with a GIF instead of (or alongside) text.
 * Returns true when message is humor/excitement/reaction-worthy AND
 * probability roll passes. gif_probability: 0.0-1.0 (suggest 0.08-0.15). */
bool hu_conversation_should_send_gif(const char *msg, size_t msg_len,
                                     const hu_channel_history_entry_t *entries, size_t count,
                                     uint32_t seed, float gif_probability);

/* Build a prompt for the LLM to generate a GIF search query.
 * Returns a prompt string asking for 2-4 word search terms. */
size_t hu_conversation_build_gif_search_prompt(const char *msg, size_t msg_len, char *buf,
                                               size_t cap);

/* Adjust GIF send probability based on relationship type (friend, coworker, parent, etc.).
 * Returns adjusted probability clamped to [0.0, 1.0]. */
float hu_conversation_adjust_gif_probability(float base_probability, const char *relationship_type,
                                             size_t rel_len);

/* Build a GIF style hint based on relationship type.
 * E.g. "absurd meme" for friends, "wholesome" for family. */
size_t hu_conversation_build_gif_style_hint(const char *relationship_type, size_t rel_len,
                                            char *buf, size_t cap);

/* Per-contact GIF rate limiting. Returns true if sending a GIF is allowed.
 * max_per_day=0 defaults to 5, min_gap_ms=0 defaults to 10 minutes. */
bool hu_conversation_gif_rate_allow(const char *contact_id, size_t cid_len, uint64_t now_ms,
                                    uint32_t max_per_day, uint64_t min_gap_ms);

/* Record that a GIF was sent to contact_id at now_ms. */
void hu_conversation_gif_rate_record(const char *contact_id, size_t cid_len, uint64_t now_ms);

/* Seen behavior classification result. */
typedef enum hu_seen_action {
    HU_SEEN_RESPOND_NOW,
    HU_SEEN_DELAY_THEN_RESPOND,
    HU_SEEN_IGNORE_FOR_NOW,
} hu_seen_action_t;

/* Classify whether to respond immediately or delay (modeling "seen" behavior).
 * hour_local is 0-23 local time. out_delay_ms receives recommended delay (can be NULL). */
hu_seen_action_t hu_conversation_classify_seen_behavior(const char *msg, size_t msg_len,
                                                        uint8_t hour_local, uint32_t seed,
                                                        uint32_t *out_delay_ms);

/* GIF humor calibration: track sends and reactions per contact.
 * Record a GIF send (with search query), then record if the contact reacted.
 * hit_rate returns reaction/send ratio (0.5 default for <3 samples). */
void hu_conversation_gif_cal_record_send(const char *contact_id, size_t cid_len, const char *query,
                                         size_t query_len);
void hu_conversation_gif_cal_record_reaction(const char *contact_id, size_t cid_len);
float hu_conversation_gif_cal_hit_rate(const char *contact_id, size_t cid_len);

/* Build a hint about tapback reactions they sent on our messages.
 * Tells the LLM not to explicitly acknowledge tapbacks. */
size_t hu_conversation_build_reaction_received_hint(const hu_channel_history_entry_t *entries,
                                                    size_t count, char *buf, size_t cap);

/* Build an emoji frequency matching directive based on their message patterns.
 * If they use lots of emoji, encourage it; if they rarely do, suggest restraint. */
size_t hu_conversation_build_emoji_mirror_hint(const hu_channel_history_entry_t *entries,
                                               size_t count, char *buf, size_t cap);

/* Build awareness hint for edited or unsent messages.
 * Tells the LLM to respond to the corrected version or pretend unsent was never seen. */
size_t hu_conversation_build_edit_awareness_hint(bool message_was_edited, bool message_was_unsent,
                                                 char *buf, size_t cap);

/* Sticker-like reaction images: decide whether to send a sticker based on
 * message content and probability. Returns true if a sticker should be sent. */
bool hu_conversation_should_send_sticker(const char *msg, size_t msg_len, const char *last_response,
                                         size_t resp_len, uint32_t seed, float probability);

/* Select a sticker matching the message content from the sticker directory.
 * Writes the full path to out_path. Returns path length or 0 if no match. */
size_t hu_conversation_select_sticker(const char *msg, size_t msg_len, uint32_t seed,
                                      const char *sticker_dir, size_t dir_len, char *out_path,
                                      size_t out_cap);

/* Music teaser (iMessage rich previews): decide whether to send an Apple Music / Spotify link.
 * history: recent channel messages; avoids repeats when a music URL appeared in the last 10. */
bool hu_conversation_should_send_music(const char *incoming, size_t incoming_len,
                                       const hu_channel_history_entry_t *history,
                                       size_t history_count, uint32_t seed, float probability);

/* Build user prompt for the LLM to pick a song + Apple Music search URL for the teaser. */
size_t hu_conversation_build_music_prompt(const char *incoming, size_t incoming_len, char *out,
                                          size_t out_cap);

/* Build context hint for inline replies. original_text is the text of the message
 * they are replying to. Tells the LLM to respond in context of that original. */
size_t hu_conversation_build_inline_reply_hint(const char *original_text, size_t original_len,
                                               char *buf, size_t cap);

/* Persist GIF calibration data to a JSON file. Load restores it on startup.
 * File format: [{"contact":"id","sent":N,"reacted":N}, ...] */
hu_error_t hu_conversation_gif_cal_save(const char *path, size_t path_len);
hu_error_t hu_conversation_gif_cal_load(const char *path, size_t path_len);

/* Split a long response into multiple separate text messages at sentence boundaries.
 * Each chunk fits in chunks[][512]. Returns the number of chunks written.
 * Designed for the "double-text" pattern where humans send multiple short messages. */
size_t hu_conversation_split_into_texts(const char *response, size_t resp_len, size_t max_chunk,
                                        char chunks[][512], size_t max_chunks);

/* Schedule a message for future delivery. deliver_at_ms is absolute epoch millis.
 * Call hu_conversation_flush_scheduled periodically to get due messages.
 * _on variants include channel_name for routing to the correct channel. */
hu_error_t hu_conversation_schedule_message(const char *contact_id, size_t cid_len,
                                            const char *message, size_t msg_len,
                                            uint64_t deliver_at_ms);
hu_error_t hu_conversation_schedule_message_on(const char *contact_id, size_t cid_len,
                                               const char *channel_name, size_t ch_len,
                                               const char *message, size_t msg_len,
                                               uint64_t deliver_at_ms);
size_t hu_conversation_flush_scheduled(uint64_t now_ms, char *out_contact, size_t contact_cap,
                                       char *out_message, size_t message_cap);
size_t hu_conversation_flush_scheduled_on(uint64_t now_ms, char *out_contact, size_t contact_cap,
                                          char *out_channel, size_t channel_cap, char *out_message,
                                          size_t message_cap);
/* Channel-filtered flush: only returns messages matching channel_filter (or any if NULL/empty). */
size_t hu_conversation_flush_scheduled_for(uint64_t now_ms, const char *channel_filter,
                                           size_t filter_len, char *out_contact, size_t contact_cap,
                                           char *out_channel, size_t channel_cap, char *out_message,
                                           size_t message_cap);

/* Persist scheduled messages to a JSON file. Load restores on startup. */
hu_error_t hu_conversation_sched_save(const char *path, size_t path_len);
hu_error_t hu_conversation_sched_load(const char *path, size_t path_len);

#define HU_SCHED_MAX 16

typedef struct hu_sched_slot {
    char contact_id[128];
    char channel_name[32];
    char message[512];
    size_t msg_len;
    uint64_t deliver_at_ms;
    bool active;
} hu_sched_slot_t;

/* Access a scheduled slot by index (0..HU_SCHED_MAX-1). Returns NULL if out of range. */
hu_sched_slot_t *hu_conversation_sched_slot(size_t index);

/* Resolve the macOS Contacts/AddressBook contact photo path.
 * Queries AddressBook-v22.abcddb for phone/email → record → image.
 * Returns path length or 0 if not available. */
size_t hu_conversation_contact_photo_path(const char *contact_id, size_t cid_len, char *out_path,
                                          size_t out_cap);

#endif /* HU_CONTEXT_CONVERSATION_H */
