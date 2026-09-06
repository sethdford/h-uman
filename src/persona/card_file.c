/* Shared loader plumbing for the measured persona cards — see
 * include/human/persona/card_file.h. */
#include "human/persona/card_file.h"

#include "human/core/file.h"
#include "human/persona.h"

#include <stdio.h>

#define HU_PERSONA_CARD_PATH_MAX 512

hu_error_t hu_persona_card_slurp(hu_allocator_t *alloc, const char *name, size_t name_len,
                                 const char *suffix, char **buf, size_t *len) {
    if (!alloc || !name || name_len == 0 || !suffix || !buf || !len)
        return HU_ERR_INVALID_ARGUMENT;
    *buf = NULL;
    *len = 0;
    char base[HU_PERSONA_CARD_PATH_MAX];
    if (!hu_persona_base_dir(base, sizeof(base)))
        return HU_ERR_NOT_FOUND;
    char path[HU_PERSONA_CARD_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%.*s%s", base, (int)name_len, name, suffix);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;
    return hu_file_slurp(alloc, path, HU_PERSONA_CARD_MAX_BYTES, buf, len);
}

hu_error_t hu_persona_card_parse_object(hu_allocator_t *alloc, const char *json, size_t len,
                                        hu_json_value_t **root) {
    if (!root)
        return HU_ERR_INVALID_ARGUMENT;
    *root = NULL;
    if (!alloc || !json)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t err = hu_json_parse(alloc, json, len, root);
    if (err != HU_OK) {
        *root = NULL;
        return err;
    }
    if (!*root || (*root)->type != HU_JSON_OBJECT) {
        if (*root)
            hu_json_free(alloc, *root);
        *root = NULL;
        return HU_ERR_INVALID_ARGUMENT;
    }
    return HU_OK;
}

static void copy_date(char *dst, size_t cap, const hu_json_value_t *obj, const char *key) {
    const char *s = hu_json_get_string(obj, key);
    if (s)
        snprintf(dst, cap, "%s", s);
}

void hu_persona_card_copy_window(const hu_json_value_t *root, char *start, size_t start_cap,
                                 char *end, size_t end_cap) {
    const hu_json_value_t *window = root ? hu_json_object_get(root, "window") : NULL;
    if (!window || window->type != HU_JSON_OBJECT)
        return;
    copy_date(start, start_cap, window, "start");
    copy_date(end, end_cap, window, "end");
}
