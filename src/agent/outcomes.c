#include "human/agent/outcomes.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t outcome_now_ms(void) {
#ifdef HU_IS_TEST
    return 1000000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}

void hu_outcome_tracker_init(hu_outcome_tracker_t *tracker, bool auto_apply_feedback) {
    if (!tracker)
        return;
    memset(tracker, 0, sizeof(*tracker));
    tracker->auto_apply_feedback = auto_apply_feedback;
}

static void record_entry(hu_outcome_tracker_t *tracker, hu_outcome_type_t type,
                         const char *tool_name, const char *summary) {
    size_t idx = tracker->write_idx % HU_OUTCOME_MAX_RECENT;
    hu_outcome_entry_t *e = &tracker->entries[idx];
    e->type = type;
    e->timestamp_ms = outcome_now_ms();

    if (tool_name) {
        strncpy(e->tool_name, tool_name, sizeof(e->tool_name) - 1);
        e->tool_name[sizeof(e->tool_name) - 1] = '\0';
    } else {
        e->tool_name[0] = '\0';
    }

    if (summary) {
        strncpy(e->summary, summary, sizeof(e->summary) - 1);
        e->summary[sizeof(e->summary) - 1] = '\0';
    } else {
        e->summary[0] = '\0';
    }

    tracker->write_idx++;
    tracker->total++;
}

void hu_outcome_record_tool(hu_outcome_tracker_t *tracker, const char *tool_name, bool success,
                            const char *summary) {
    if (!tracker)
        return;
    record_entry(tracker, success ? HU_OUTCOME_TOOL_SUCCESS : HU_OUTCOME_TOOL_FAILURE, tool_name,
                 summary);
    if (success)
        tracker->tool_successes++;
    else
        tracker->tool_failures++;
}

void hu_outcome_record_correction(hu_outcome_tracker_t *tracker, const char *original,
                                  const char *correction) {
    if (!tracker)
        return;
    record_entry(tracker, HU_OUTCOME_USER_CORRECTION, NULL, correction ? correction : original);
    tracker->corrections++;
}

void hu_outcome_record_positive(hu_outcome_tracker_t *tracker, const char *context) {
    if (!tracker)
        return;
    record_entry(tracker, HU_OUTCOME_USER_POSITIVE, NULL, context);
    tracker->positives++;
}

const hu_outcome_entry_t *hu_outcome_get_recent(const hu_outcome_tracker_t *tracker,
                                                size_t *count) {
    if (!tracker || !count)
        return NULL;
    *count = tracker->total < HU_OUTCOME_MAX_RECENT ? tracker->total : HU_OUTCOME_MAX_RECENT;
    return tracker->entries;
}

char *hu_outcome_build_summary(const hu_outcome_tracker_t *tracker, hu_allocator_t *alloc,
                               size_t *out_len) {
    if (!tracker || !alloc)
        return NULL;

    if (tracker->total == 0)
        return NULL;

    char buf[512];
    int n = snprintf(
        buf, sizeof(buf),
        "## Learning Outcomes\n"
        "- Tool calls: %llu succeeded, %llu failed\n"
        "- User corrections: %llu\n"
        "- Positive feedback: %llu\n",
        (unsigned long long)tracker->tool_successes, (unsigned long long)tracker->tool_failures,
        (unsigned long long)tracker->corrections, (unsigned long long)tracker->positives);

    if (n <= 0)
        return NULL;

    size_t len = (size_t)n;
    char *result = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!result)
        return NULL;
    memcpy(result, buf, len);
    result[len] = '\0';
    if (out_len)
        *out_len = len;
    return result;
}

bool hu_outcome_detect_repeated_failure(const hu_outcome_tracker_t *tracker, const char *tool_name,
                                        size_t threshold) {
    if (!tracker || !tool_name || threshold == 0)
        return false;

    size_t count = 0;
    size_t n = tracker->total < HU_OUTCOME_MAX_RECENT ? tracker->total : HU_OUTCOME_MAX_RECENT;

    for (size_t i = 0; i < n; i++) {
        const hu_outcome_entry_t *e = &tracker->entries[i];
        if (e->type == HU_OUTCOME_TOOL_FAILURE && strcmp(e->tool_name, tool_name) == 0)
            count++;
    }

    return count >= threshold;
}
