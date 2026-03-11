#ifndef HU_INTELLIGENCE_FEEDBACK_H
#define HU_INTELLIGENCE_FEEDBACK_H

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

typedef enum hu_feedback_class {
    HU_FEEDBACK_CLASS_POSITIVE,
    HU_FEEDBACK_CLASS_NEGATIVE,
    HU_FEEDBACK_CLASS_NEUTRAL
} hu_feedback_class_t;

hu_feedback_class_t hu_feedback_classify(int64_t response_time_ms, size_t response_length_chars,
                                        bool has_emoji, bool has_tapback, bool topic_continued,
                                        bool has_laughter_words, bool topic_changed,
                                        bool ignored_question);

#ifdef HU_ENABLE_SQLITE
hu_error_t hu_feedback_record(sqlite3 *db, const char *behavior_type, size_t bt_len,
                             const char *contact_id, size_t cid_len, const char *signal,
                             size_t sig_len, const char *context, size_t ctx_len,
                             int64_t timestamp);
#endif

const char *hu_feedback_class_str(hu_feedback_class_t signal);

#endif /* HU_INTELLIGENCE_FEEDBACK_H */
