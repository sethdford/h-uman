/* Per-contact wiki page: the pure half of better-than-human item 4.
 * See include/human/memory/wiki_page.h. */
#include "human/memory/wiki_page.h"

#include "human/core/paths.h"

#include <stdio.h>
#include <string.h>

bool hu_wiki_contact_id_is_safe(const char *contact_id, size_t len) {
    if (!contact_id || len == 0 || len > 96 || contact_id[0] == '.')
        return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)contact_id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '+' || c == '@' || c == '.' || c == '_' || c == '-';
        if (!ok)
            return false;
        if (c == '.' && i + 1 < len && contact_id[i + 1] == '.')
            return false;
    }
    return true;
}

size_t hu_wiki_page_slice(const char *page, size_t page_len, size_t budget) {
    if (!page || page_len == 0 || budget == 0)
        return 0;
    if (page_len <= budget)
        return page_len;
    for (size_t i = budget; i > 0; i--) {
        if (page[i - 1] == '\n')
            return i;
    }
    return 0;
}

size_t hu_wiki_recall_cap(size_t max_context_chars, size_t wiki_len) {
    if (wiki_len == 0)
        return max_context_chars;
    size_t floor_cap = max_context_chars / 2;
    if (wiki_len >= max_context_chars - floor_cap)
        return floor_cap;
    return max_context_chars - wiki_len;
}

hu_error_t hu_wiki_page_read(hu_allocator_t *alloc, const char *contact_id, size_t cid_len,
                             size_t budget, char **out, size_t *out_len) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (out_len)
        *out_len = 0;
    if (!hu_wiki_contact_id_is_safe(contact_id, cid_len))
        return HU_ERR_INVALID_ARGUMENT;

    char path[1024];
    int n = hu_paths_state(path, sizeof(path), "wiki/%.*s.md", (int)cid_len, contact_id);
    if (n < 0 || (size_t)n >= sizeof(path))
        return HU_ERR_IO;

    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_NOT_FOUND;
    char *raw = (char *)alloc->alloc(alloc->ctx, HU_WIKI_PAGE_MAX_BYTES + 1);
    if (!raw) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t got = fread(raw, 1, HU_WIKI_PAGE_MAX_BYTES, f);
    fclose(f);

    size_t keep = hu_wiki_page_slice(raw, got, budget);
    while (keep > 0 && (raw[keep - 1] == '\n' || raw[keep - 1] == '\r'))
        keep--;
    if (keep == 0) {
        alloc->free(alloc->ctx, raw, HU_WIKI_PAGE_MAX_BYTES + 1);
        return HU_OK;
    }
    char *res = (char *)alloc->alloc(alloc->ctx, keep + 1);
    if (!res) {
        alloc->free(alloc->ctx, raw, HU_WIKI_PAGE_MAX_BYTES + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(res, raw, keep);
    res[keep] = '\0';
    alloc->free(alloc->ctx, raw, HU_WIKI_PAGE_MAX_BYTES + 1);
    *out = res;
    if (out_len)
        *out_len = keep;
    return HU_OK;
}
