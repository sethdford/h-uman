#ifndef HU_MEMORY_MINJA_GUARD_H
#define HU_MEMORY_MINJA_GUARD_H

#include "human/memory/trust.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * SOTA-2026 init-09 §2.6: broadened MINJA / memory-injection detector.
 *
 * `hu_minja_detect` returns true when a third-party-tier message looks
 * like an instruction-rewrite / identity-overwrite / capability-unlock
 * attempt. The detector applies three preprocessing stages before the
 * pattern scan:
 *
 *   1. NFKC-equivalent normalization (fullwidth → narrow, drop combining
 *      marks, Cyrillic-confusable folding). ASCII-only fallback when ICU
 *      is not linked.
 *   2. Leetspeak decode in place (1→i, 3→e, 0→o, 5→s, @→a).
 *   3. Non-locale-language reject: if the user's locale is "en" and the
 *      normalized message is ≥40% non-ASCII, short-circuit to quarantine.
 *
 * The pattern table is tiered (instruction-rewrite, identity-overwrite,
 * capability-unlock) for diagnostics. Scan window: 1 KB.
 *
 * `user_locale` may be NULL; when NULL the locale-mismatch reject is
 * disabled.
 */
bool hu_minja_detect(const char *text, size_t len, const char *user_locale);

/* Append a quarantine event (JSONL) to ~/.human/private/quarantine.log.
 * Snippet truncated to 64 bytes to bound disk growth and avoid logging
 * the full adversarial payload. The path is overridable via the
 * `HUMAN_PM_QUARANTINE_PATH` env var (used by tests). */
void hu_minja_quarantine_log(const char *text, size_t len,
                             const hu_provenance_t *prov);

#endif /* HU_MEMORY_MINJA_GUARD_H */
