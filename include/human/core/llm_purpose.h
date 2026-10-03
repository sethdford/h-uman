#ifndef HU_CORE_LLM_PURPOSE_H
#define HU_CORE_LLM_PURPOSE_H

/* Per-request purpose tag + admission priority for local LLM / embedding calls.
 *
 * mlx-server serves replies, proactive drafts, judges and embeddings on ONE
 * worker. Its admission queue reads `X-HU-Priority`: only the value `batch` is
 * low priority; anything else, including no header, is live. So reply calls
 * stay unmarked and BACKGROUND calls send `X-HU-Priority: batch`, letting a
 * reply jump the queued background work. `X-HU-Purpose: <name>` names each
 * request ([a-z_]{1,24}, never message text) so the server can log it.
 *
 * Two thread-locals decide the headers, set where the work is known:
 *   - the current PURPOSE, set at a call site (save the return value, restore
 *     it afterwards);
 *   - a BACKGROUND LANE depth, entered around whole background ticks
 *     (housekeeping, the post-send deferral flush). Inside the lane every
 *     request is batch, whatever its purpose.
 * The purpose -> priority mapping lives in one place: hu_llm_purpose_headers. */

#include <stdbool.h>
#include <stddef.h>

typedef enum hu_llm_purpose {
    HU_LLM_PURPOSE_UNTAGGED = 0,
    /* Foreground: on a reply's critical path. No priority header. */
    HU_LLM_PURPOSE_REPLY,
    HU_LLM_PURPOSE_GUARD_RETRY,
    HU_LLM_PURPOSE_PLANNER,
    HU_LLM_PURPOSE_COMMITMENT_CHECK, /* commitment guard detector/rewrite (daemon) */
    HU_LLM_PURPOSE_MODERATION_CHECK, /* moderation context judge (SHIELD-004, reply path) */
    /* Label-only: priority follows the caller. The prospective judge runs both
     * in the reactive prompt build (pre-send) and from proactive ticks; fact
     * extraction runs inline or, deferred, after the send. Batch only when
     * made inside the background lane. */
    HU_LLM_PURPOSE_EXTRACT,
    HU_LLM_PURPOSE_JUDGE,
    /* Background: always X-HU-Priority: batch. */
    HU_LLM_PURPOSE_EMBED,
    HU_LLM_PURPOSE_PROACTIVE,
    HU_LLM_PURPOSE_BACKGROUND, /* generic: a foreground-default call made inside the lane */
    HU_LLM_PURPOSE__COUNT
} hu_llm_purpose_t;

/* Wire name, [a-z_]{1,24}. Out-of-range values name "untagged". */
const char *hu_llm_purpose_name(hu_llm_purpose_t purpose);

/* True for purposes that are batch on their own (independent of the lane). */
bool hu_llm_purpose_is_background(hu_llm_purpose_t purpose);

/* Calling thread's current purpose (UNTAGGED until set). */
hu_llm_purpose_t hu_llm_purpose_current(void);

/* Set the calling thread's purpose; returns the previous one to restore. */
hu_llm_purpose_t hu_llm_purpose_set(hu_llm_purpose_t purpose);

/* Set only when the thread is UNTAGGED (a caller's tag wins); returns the
 * previous purpose to restore either way. */
hu_llm_purpose_t hu_llm_purpose_set_if_untagged(hu_llm_purpose_t purpose);

/* Background lane (nestable). Inside it every request carries batch. */
void hu_llm_background_enter(void);
void hu_llm_background_exit(void);
bool hu_llm_background_active(void);

/* Write the extra-header block for a request made now, on this thread, with
 * `purpose`: "X-HU-Purpose: <name>\r\n" plus "X-HU-Priority: batch\r\n" when
 * the purpose is background or the lane is active. Inside the lane a REPLY or
 * UNTAGGED purpose is named "background". NUL-terminated; returns the length
 * (0 and buf[0]='\0' when cap is too small). */
size_t hu_llm_purpose_headers(hu_llm_purpose_t purpose, char *buf, size_t cap);

#endif /* HU_CORE_LLM_PURPOSE_H */
