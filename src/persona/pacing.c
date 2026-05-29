/* Feature-test macros must precede the first include so libc's <features.h>
 * exposes the right symbols. On musl (alpine docker build) arc4random_uniform
 * (<stdlib.h>) is declared only under `_BSD_SOURCE || _GNU_SOURCE` — NOT
 * _DEFAULT_SOURCE — and usleep (<unistd.h>) likewise needs a BSD/XOPEN macro;
 * strict -std=c11 (__STRICT_ANSI__) otherwise suppresses both. _GNU_SOURCE is
 * the portable choice (enables them on musl and glibc without the -Werror
 * deprecation warning bare _BSD_SOURCE triggers on glibc). macOS declares
 * arc4random unconditionally. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "human/persona/pacing.h"
#include "human/core/time.h"
#include <stdlib.h> /* arc4random_uniform */
#include <unistd.h> /* usleep */

void hu_persona_pace_reply_start(uint64_t *start_ms_out) {
    if (start_ms_out)
        *start_ms_out = hu_time_get_current_ms();
}

void hu_persona_pace_reply_finish(const hu_persona_t *persona, uint64_t start_ms) {
    if (!persona)
        return;
    int64_t min_delay_ms = persona->min_reply_delay_ms;
    int64_t variance_ms = persona->reply_delay_variance_ms;
    if (min_delay_ms <= 0)
        return; /* pacing disabled */

    /* Target wall-clock = (min_delay_ms * 1.2) + uniform jitter in
     * [-variance, +variance]. The 1.2x floor ensures replies never feel
     * suspiciously crisp even at the minimum. */
    int64_t target_ms = (min_delay_ms * 12) / 10;
    if (variance_ms > 0) {
        int64_t jitter = (int64_t)arc4random_uniform((uint32_t)(variance_ms * 2)) - variance_ms;
        target_ms += jitter;
    }
    if (target_ms < (min_delay_ms * 12) / 10)
        target_ms = (min_delay_ms * 12) / 10;

    uint64_t now_ms = hu_time_get_current_ms();
    int64_t elapsed_ms = (int64_t)(now_ms - start_ms);
    int64_t remaining_ms = target_ms - elapsed_ms;
    if (remaining_ms > 0) {
        /* usleep takes microseconds. Cap at 10 seconds to avoid hangs. */
        if (remaining_ms > 10000)
            remaining_ms = 10000;
        usleep((useconds_t)(remaining_ms * 1000));
    }
}
