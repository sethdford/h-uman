#ifndef HU_STYLE_MIRROR_H
#define HU_STYLE_MIRROR_H

/*
 * Sprint 6 US-19: Post-generation case/punctuation mirroring.
 *
 * Reads partner's recent messages and enforces case + punctuation style on
 * Seth's outbound buffer — regardless of whether the frontier model honored
 * the system-prompt directive.
 */

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* Summary of edits applied by hu_style_mirror_apply */
typedef struct hu_style_mirror_report {
    bool lowercased_applied; /* sentence-start lowercasing was applied */
    bool periods_stripped;   /* trailing period(s) were stripped */
    size_t edits;            /* total character positions modified */
} hu_style_mirror_report_t;

/*
 * hu_style_mirror_apply — post-process Seth's outbound buffer to match
 * the partner's observed case + punctuation style.
 *
 * buf            : NUL-terminated outbound text buffer (modified in-place).
 * inout_len      : on entry, byte length of buf (excluding NUL); updated on exit.
 * partner_recent : array of n_partner_recent NUL-terminated partner message strings.
 * n_partner_recent: number of entries in partner_recent.
 * report         : optional; populated with what was changed (may be NULL).
 *
 * Rules:
 *   - Skip if n_partner_recent < 2 (insufficient signal).
 *   - If >= 70% of partner messages START with a lowercase letter:
 *       lowercase the first letter of each sentence-start in buf, BUT only
 *       if that word is 1-3 characters long (avoids clobbering proper nouns
 *       like "Jordan" which are 4+ letters).
 *   - If >= 70% of partner messages do NOT end with '.' (after whitespace trim):
 *       strip a single trailing '.' from buf (not '?' or '!').
 *
 * Returns HU_OK on success, HU_ERR_INVALID_ARGUMENT if buf or inout_len is NULL.
 */
hu_error_t hu_style_mirror_apply(char *buf, size_t *inout_len, const char *const *partner_recent,
                                 size_t n_partner_recent, hu_style_mirror_report_t *report);

#endif /* HU_STYLE_MIRROR_H */
