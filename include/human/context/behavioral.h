#ifndef HU_CONTEXT_BEHAVIORAL_H
#define HU_CONTEXT_BEHAVIORAL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- F9: Double-text --- */
typedef struct hu_double_text_config {
    double probability;       /* default 0.15 */
    uint32_t min_gap_seconds; /* min time before double-texting, default 30 */
    uint32_t max_gap_seconds; /* max, default 300 */
    bool only_close_friends;  /* default true */
} hu_double_text_config_t;

bool hu_should_double_text(double closeness, uint64_t last_msg_ms, uint64_t now_ms,
                           const hu_double_text_config_t *config, uint32_t seed);
hu_error_t hu_double_text_build_prompt(hu_allocator_t *alloc, char **out, size_t *out_len);

/* --- F12: Bookend Messages --- */
typedef enum hu_bookend_type {
    HU_BOOKEND_NONE = 0,
    HU_BOOKEND_MORNING,
    HU_BOOKEND_EVENING,
    HU_BOOKEND_GOODNIGHT
} hu_bookend_type_t;

hu_bookend_type_t hu_bookend_check(uint32_t hour, bool contact_is_close, bool already_sent_today,
                                   uint32_t seed);
const char *hu_bookend_type_str(hu_bookend_type_t t);
hu_error_t hu_bookend_build_prompt(hu_allocator_t *alloc, hu_bookend_type_t type, char **out,
                                   size_t *out_len);

/* --- F28: Linguistic Mirroring --- */
typedef struct hu_mirror_analysis {
    bool uses_lowercase;     /* they type in all lowercase */
    bool uses_abbreviations; /* "u" "ur" "rn" */
    bool uses_exclamation;   /* frequent "!" */
    bool uses_ellipsis;      /* frequent "..." */
    double avg_msg_length;   /* their average */
    bool uses_emoji;         /* frequent emoji */
} hu_mirror_analysis_t;

hu_error_t hu_mirror_build_directive(hu_allocator_t *alloc, const hu_mirror_analysis_t *analysis,
                                     char **out, size_t *out_len);

/* Filter a mirror directive against persona identity constraints.
 * Removes mirroring suggestions that conflict with avoided_vocab or character_invariants.
 * Returns a new filtered directive (caller frees), or NULL with *out_len=0 if fully filtered. */
struct hu_persona;
hu_error_t hu_mirror_check_identity_bounds(hu_allocator_t *alloc, const char *directive,
                                           size_t directive_len, const struct hu_persona *persona,
                                           char **out, size_t *out_len);

/* Check if a mirror analysis conflicts with persona identity constraints.
 * Returns true if mirroring the analyzed style would violate avoided_vocab
 * or character_invariants (e.g., persona says "never uses abbreviations"
 * but user uses abbreviations). */
bool hu_mirror_conflicts_with_identity(const hu_mirror_analysis_t *analysis,
                                       const struct hu_persona *persona);

/* Build mirror directive with persona identity filtering.
 * Convenience: builds directive then filters through identity bounds.
 * Returns filtered directive, or NULL with *out_len=0 if fully filtered. */
hu_error_t hu_mirror_build_directive_persona(hu_allocator_t *alloc,
                                             const hu_mirror_analysis_t *analysis,
                                             const struct hu_persona *persona, char **out,
                                             size_t *out_len);

/* --- F54: Timezone Awareness --- */
typedef struct hu_timezone_info {
    int offset_hours;       /* UTC offset, e.g. -5 for EST */
    uint32_t local_hour;    /* their current hour */
    bool is_sleeping_hours; /* 23-7 */
    bool is_work_hours;     /* 9-17 */
} hu_timezone_info_t;

hu_timezone_info_t hu_timezone_compute(int offset_hours, uint64_t utc_now_ms);
hu_error_t hu_timezone_build_directive(hu_allocator_t *alloc, const hu_timezone_info_t *tz,
                                       const char *contact_name, size_t name_len, char **out,
                                       size_t *out_len);

#endif /* HU_CONTEXT_BEHAVIORAL_H */
