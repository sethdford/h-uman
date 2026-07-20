#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/memory/retrieval.h"
#include <string.h>

#define HU_CONTACT_KEY_PREFIX     "contact:"
#define HU_CONTACT_KEY_PREFIX_LEN 8

hu_error_t hu_retrieval_check_namespace(const hu_retrieval_options_t *opts) {
    if (!opts)
        return HU_OK;
    if (!opts->require_contact_namespace)
        return HU_OK;
    if (!opts->contact_id || opts->contact_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}

bool hu_retrieval_entry_in_contact_scope(const hu_memory_entry_t *e, const char *contact_id,
                                         size_t contact_id_len) {
    if (!e || !contact_id || contact_id_len == 0)
        return false;

    const char *key = e->key && e->key_len > 0 ? e->key : e->id;
    size_t key_len = e->key && e->key_len > 0 ? e->key_len : e->id_len;
    if (!key || key_len == 0)
        return false;

    /* Expected: contact:<id>:... */
    size_t need = HU_CONTACT_KEY_PREFIX_LEN + contact_id_len + 1;
    if (key_len < need)
        return false;
    if (memcmp(key, HU_CONTACT_KEY_PREFIX, HU_CONTACT_KEY_PREFIX_LEN) != 0)
        return false;
    if (memcmp(key + HU_CONTACT_KEY_PREFIX_LEN, contact_id, contact_id_len) != 0)
        return false;
    if (key[HU_CONTACT_KEY_PREFIX_LEN + contact_id_len] != ':')
        return false;
    return true;
}

static bool entry_matches_session(const hu_memory_entry_t *e, const char *session_id,
                                  size_t session_id_len) {
    if (!session_id || session_id_len == 0)
        return true;
    if (!e->session_id || e->session_id_len == 0)
        return false;
    return e->session_id_len == session_id_len &&
           memcmp(e->session_id, session_id, session_id_len) == 0;
}

hu_error_t hu_retrieval_filter_by_namespace(hu_allocator_t *alloc, hu_retrieval_result_t *r,
                                            const hu_retrieval_options_t *opts) {
    if (!alloc || !r || !opts)
        return HU_OK;
    if ((!opts->contact_id || opts->contact_id_len == 0) &&
        (!opts->session_id || opts->session_id_len == 0))
        return HU_OK;
    if (!r->entries || r->count == 0)
        return HU_OK;

    size_t keep = 0;
    for (size_t i = 0; i < r->count; i++) {
        bool ok = true;
        if (opts->contact_id && opts->contact_id_len > 0)
            ok = hu_retrieval_entry_in_contact_scope(&r->entries[i], opts->contact_id,
                                                     opts->contact_id_len);
        if (ok)
            ok = entry_matches_session(&r->entries[i], opts->session_id, opts->session_id_len);
        if (ok) {
            if (keep != i) {
                r->entries[keep] = r->entries[i];
                if (r->scores)
                    r->scores[keep] = r->scores[i];
            }
            keep++;
        } else {
            hu_memory_entry_free_fields(alloc, &r->entries[i]);
        }
    }

    if (keep == 0) {
        alloc->free(alloc->ctx, r->entries, r->count * sizeof(hu_memory_entry_t));
        if (r->scores)
            alloc->free(alloc->ctx, r->scores, r->count * sizeof(double));
        r->entries = NULL;
        r->scores = NULL;
        r->count = 0;
        return HU_OK;
    }

    if (keep < r->count) {
        hu_memory_entry_t *te = (hu_memory_entry_t *)alloc->realloc(
            alloc->ctx, r->entries, r->count * sizeof(hu_memory_entry_t),
            keep * sizeof(hu_memory_entry_t));
        if (te)
            r->entries = te;
        if (r->scores) {
            double *ts = (double *)alloc->realloc(alloc->ctx, r->scores, r->count * sizeof(double),
                                                  keep * sizeof(double));
            if (ts)
                r->scores = ts;
        }
        r->count = keep;
    }
    return HU_OK;
}
