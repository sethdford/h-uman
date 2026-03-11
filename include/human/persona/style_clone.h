#ifndef HU_PERSONA_STYLE_CLONE_H
#define HU_PERSONA_STYLE_CLONE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hu_style_fingerprint {
    char *contact_id; /* NULL for aggregate across all contacts */
    size_t contact_id_len;

    /* Capitalization (ratios 0.0-1.0 among own messages) */
    double lowercase_ratio;     /* fraction of messages that are all lowercase */
    double sentence_case_ratio; /* fraction starting with uppercase, rest lower */

    /* Punctuation */
    double period_ending_ratio;      /* messages ending with '.' */
    double exclamation_ending_ratio; /* messages ending with '!' */
    double ellipsis_ratio;           /* messages containing "..." */

    /* Laughter style (ratios among all laughter tokens, sum to ~1.0) */
    double haha_ratio;
    double lol_ratio;
    double lmao_ratio;

    /* Length */
    double avg_message_length; /* average character count of own messages */
    double msg_length_stddev;

    /* Emoji */
    double emoji_per_message; /* average emoji codepoints per message */

    /* Behavioral */
    double double_text_ratio; /* fraction of messages followed by another from us before reply */
    double question_ratio;    /* fraction of messages containing '?' */

    uint32_t sample_count; /* total messages analyzed */
    uint64_t computed_at;  /* timestamp */
} hu_style_fingerprint_t;

hu_error_t hu_style_build_query(const char *contact_id, size_t contact_id_len, char *buf,
                                 size_t cap, size_t *out_len);

hu_error_t hu_style_analyze_messages(hu_allocator_t *alloc, const char **messages,
                                     size_t msg_count, hu_style_fingerprint_t *out);

hu_error_t hu_style_fingerprint_to_prompt(hu_allocator_t *alloc,
                                         const hu_style_fingerprint_t *fp, char **out,
                                         size_t *out_len);

void hu_style_fingerprint_deinit(hu_allocator_t *alloc, hu_style_fingerprint_t *fp);

#endif
