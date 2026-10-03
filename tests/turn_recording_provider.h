/* tests/turn_recording_provider.h — scripted provider that records every request.
 *
 * Characterization harness for hu_agent_turn
 * (docs/superpowers/specs/2026-09-30-agent-turn-carve-design.md §4.1). Every
 * chat() / chat_with_system() call is serialized INTO THE LOG AT CALL TIME:
 * the turn builds request messages in a per-iteration arena that is reset
 * right after the call, so keeping pointers would read freed memory. The
 * serialization is the deep copy.
 *
 * Replies come from a caller-owned script. Scripted content and tool calls are
 * allocated with the allocator chat() receives, because hu_agent_turn frees the
 * response with hu_chat_response_free on that allocator. chat_with_system()
 * (side calls: orchestrator, verifiers) never consumes the script; it always
 * answers "ok".
 *
 * The log and trp_scrub()'s result use plain malloc/free: test-only memory,
 * never handed to the code under test.
 */
#ifndef HU_TESTS_TURN_RECORDING_PROVIDER_H
#define HU_TESTS_TURN_RECORDING_PROVIDER_H

#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

#define TRP_MAX_TOOL_CALLS 4

typedef struct trp_tool_call {
    const char *id;
    const char *name;
    const char *arguments;
} trp_tool_call_t;

typedef struct trp_step {
    hu_error_t err;      /* returned by chat(); content/tool_calls used only when HU_OK */
    const char *content; /* NULL = no content */
    trp_tool_call_t tool_calls[TRP_MAX_TOOL_CALLS];
    size_t tool_calls_count;
} trp_step_t;

typedef struct trp {
    const trp_step_t *script;
    size_t script_count;
    size_t next; /* next script step */
    const char
        *off_script_content; /* reply once the script is spent; NULL = HU_ERR_INVALID_ARGUMENT */
    size_t calls;            /* chat() + chat_with_system() calls seen */
    char *log;               /* NUL-terminated; malloc'd */
    size_t log_len;
    size_t log_cap;
    bool oom; /* an append failed: the log is incomplete and must not be compared */
} trp_t;

void trp_init(trp_t *t, const trp_step_t *script, size_t script_count,
              const char *off_script_content);
void trp_deinit(trp_t *t);
hu_provider_t trp_provider(trp_t *t);

void trp_log_raw(trp_t *t, const char *s, size_t n);
/* Appends s with \\ \n \r \t and other control bytes escaped; NULL logs "(null)". */
void trp_log_escaped(trp_t *t, const char *s, size_t n);
void trp_log_fmt(trp_t *t, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Replaces time-shaped tokens (dates, clock times, weekdays, months with a
 * following day, ordinal days, 9–13 digit epochs, times of day) with <DATE>
 * <TIME> <DOW> <MON> <DOM> <EPOCH> <TOD>. A backslash escape (\\n, \\t, \\xHH)
 * is a word boundary. Returns a malloc'd NUL-terminated string (caller frees)
 * or NULL on allocation failure. */
char *trp_scrub(const char *in, size_t in_len, size_t *out_len);

#endif /* HU_TESTS_TURN_RECORDING_PROVIDER_H */
