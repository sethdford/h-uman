#ifndef HU_MOMENT_H
#define HU_MOMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "human/core/error.h" /* hu_error_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations of types this module depends on. The full struct
   definitions live in their existing headers. */
struct hu_agent_t;
struct hu_contact_t;
struct hu_persona_t;
struct hu_persona_overlay_t;
struct hu_conversation_history_t;

typedef enum {
    HU_MOMENT_PHASE_DEEP_NIGHT,    /* 00:00–05:30 local */
    HU_MOMENT_PHASE_EARLY_MORNING, /* 05:30–07:30 */
    HU_MOMENT_PHASE_MORNING,       /* 07:30–11:00 */
    HU_MOMENT_PHASE_MIDDAY,        /* 11:00–14:00 */
    HU_MOMENT_PHASE_AFTERNOON,     /* 14:00–17:30 */
    HU_MOMENT_PHASE_EVENING,       /* 17:30–21:30 */
    HU_MOMENT_PHASE_NIGHT,         /* 21:30–24:00 */
} hu_moment_phase_t;

typedef enum {
    HU_MOMENT_OPEN_NONE,
    HU_MOMENT_OPEN_ACKNOWLEDGE_GAP,
    HU_MOMENT_OPEN_GREET_MORNING,
    HU_MOMENT_OPEN_GREET_NIGHT,
    HU_MOMENT_OPEN_RECONNECT,
} hu_moment_open_t;

typedef enum {
    HU_MOMENT_BREVITY_MIRROR,
    HU_MOMENT_BREVITY_TERSE,
    HU_MOMENT_BREVITY_SHORT,
    HU_MOMENT_BREVITY_MEDIUM,
    HU_MOMENT_BREVITY_LONG,
} hu_moment_brevity_t;

typedef enum {
    HU_MOMENT_TONE_UNKNOWN,
    HU_MOMENT_TONE_TERSE,
    HU_MOMENT_TONE_WARM,
    HU_MOMENT_TONE_EXCITED,
    HU_MOMENT_TONE_QUIET,
    HU_MOMENT_TONE_DISTRESSED,
} hu_moment_tone_t;

typedef struct {
    /* timing (seconds; -1 means "never") */
    int64_t time_since_their_last_msg_s;
    int64_t time_since_our_last_msg_s;

    /* phases */
    hu_moment_phase_t phase_local;
    hu_moment_phase_t phase_theirs;
    bool it_is_unusual_hour_for_them;

    /* thread state */
    bool thread_is_continuation;
    bool topic_still_open;
    char topic_hint[128]; /* empty string = no topic inferred */

    /* their style (computed from last N inbound messages) */
    int their_avg_length_words;
    int their_p90_length_words;
    hu_moment_tone_t their_recent_tone;
    bool they_use_lowercase;
    bool they_use_emoji;
    bool they_use_punctuation_eol;

    /* decisions */
    hu_moment_open_t suggested_open;
    hu_moment_open_t suggested_close;
    hu_moment_brevity_t suggested_brevity;
    int64_t defer_send_until_s; /* 0 = send now */

    /* provenance */
    int64_t composed_at_s;
    uint32_t source_flags; /* bitmask of HU_MOMENT_SRC_* */
} hu_moment_t;

/* source_flags bits — which inputs were available at compose time. */
#define HU_MOMENT_SRC_PERSONA       (1u << 0)
#define HU_MOMENT_SRC_OVERLAY       (1u << 1)
#define HU_MOMENT_SRC_HISTORY       (1u << 2)
#define HU_MOMENT_SRC_LAST_THEIR_TS (1u << 3)
#define HU_MOMENT_SRC_LAST_OUR_TS   (1u << 4)
#define HU_MOMENT_SRC_CONTACT_TZ    (1u << 5)

/* Public composer — loads inputs from the agent, then calls the pure inner. */
hu_error_t hu_moment_compose(const struct hu_agent_t *agent, const struct hu_contact_t *contact,
                             const char *channel_id, int64_t now_s, hu_moment_t *out);

/* Pure-predicate inner — no I/O, no globals, no clock reads. Tests use this. */
hu_error_t hu_moment_compose_from_inputs(
    const struct hu_persona_t *persona, const struct hu_persona_overlay_t *overlay,
    const struct hu_conversation_history_t *history, int64_t last_their_ts_s, int64_t last_our_ts_s,
    const char *contact_tz, /* IANA name, e.g. "America/Los_Angeles"; NULL = use local */
    int64_t now_s, hu_moment_t *out);

/* Render struct → prompt fragment (≤ ~256 chars). */
hu_error_t hu_moment_render_prompt(const hu_moment_t *moment, char *buf, size_t buf_cap,
                                   size_t *out_len);

/* Sample N most-recent outbound turns from history as in-context style anchors. */
hu_error_t hu_moment_render_self_exemplars(const hu_moment_t *moment,
                                           const struct hu_conversation_history_t *history,
                                           size_t max_exemplars, char *buf, size_t buf_cap,
                                           size_t *out_len);

/* Pure predicates for code consumers. Each unit-testable in isolation. */
bool hu_moment_should_defer_send(const hu_moment_t *m);
bool hu_moment_should_trigger_followup(const hu_moment_t *m, int64_t silence_threshold_s);
int hu_moment_brevity_cap_words(const hu_moment_t *m);

#ifdef __cplusplus
}
#endif

#endif /* HU_MOMENT_H */
