#ifndef HU_PERSONA_CARD_FILE_H
#define HU_PERSONA_CARD_FILE_H

/* Shared plumbing for the MEASURED persona cards (style_card.c,
 * emotion_card.c): locate and slurp <persona dir>/<name><suffix>, parse the
 * document as a JSON object, copy the measurement window. Extracted
 * 2026-09-06 when the emotion card would otherwise have cloned the style
 * card's loader line for line (clone-ratchet.md). Any future card (length,
 * timing, …) uses these and keeps only its own axes. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cards are small JSON files; anything larger is not a card. */
#define HU_PERSONA_CARD_MAX_BYTES ((size_t)256 * 1024)

/* Read <hu_persona_base_dir()>/<name><suffix> (suffix like
 * ".style-card.json") into a NUL-terminated buffer of *len bytes, owned by
 * the caller (free with alloc->free(ctx, buf, len + 1)). HU_ERR_NOT_FOUND
 * when the persona dir cannot be resolved or the file is absent. */
hu_error_t hu_persona_card_slurp(hu_allocator_t *alloc, const char *name, size_t name_len,
                                 const char *suffix, char **buf, size_t *len);

/* hu_json_parse plus the "top level must be an object" check every card
 * shares. On any failure *root is NULL and nothing is leaked. */
hu_error_t hu_persona_card_parse_object(hu_allocator_t *alloc, const char *json, size_t len,
                                        hu_json_value_t **root);

/* Copy root.window.start / .end (YYYY-MM-DD) into the two buffers; leaves
 * them untouched when the window is absent or not an object. */
void hu_persona_card_copy_window(const hu_json_value_t *root, char *start, size_t start_cap,
                                 char *end, size_t end_cap);

#ifdef __cplusplus
}
#endif

#endif /* HU_PERSONA_CARD_FILE_H */
