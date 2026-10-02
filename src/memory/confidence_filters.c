/* confidence_filters.c — the confidence boundary applied to each recall path
 * that can carry another contact's items into a reply prompt: recalled and
 * listed memory rows (semantic recall, episodic summaries, commitments) and
 * the personal model's facts. Contract: include/human/memory/confidence_boundary.h. */
#include "human/memory/confidence_boundary.h"

#include "human/core/log.h"
#include "human/memory/confidence_repo.h"
#include "human/memory/personal_model.h"

#include <stdio.h>
#include <string.h>

static void log_path(hu_gate_mode_t mode, hu_cb_path_t path, size_t considered, size_t hits) {
    hu_log_info("confidence-boundary", NULL,
                "[confidence-boundary %s] path=%s considered=%zu %s=%zu",
                mode == HU_GATE_LIVE ? "live" : "shadow", hu_cb_path_name(path), considered,
                mode == HU_GATE_LIVE ? "excluded" : "would_exclude", hits);
}

/* Provenance of one entry: the stored columns when the row is stored, else
 * the write-time rules applied to what the entry carries. */
static bool entry_excluded(hu_memory_t *mem, const hu_memory_entry_t *e, const char *contact,
                           size_t contact_len) {
    const char *key = e->key && e->key_len ? e->key : e->id;
    size_t key_len = e->key && e->key_len ? e->key_len : e->id_len;
    hu_share_level_t level = HU_SHARE_UNSET;
    char src[HU_CB_CONTACT_MAX];
    if (!key || !hu_confidence_repo_lookup(mem, key, key_len, &level, src, sizeof(src)) ||
        level == HU_SHARE_UNSET)
        level = hu_confidence_derive_row(key, key_len, e->session_id, e->session_id_len, e->source,
                                         e->source_len, NULL, 0, src, sizeof(src));
    return hu_confidence_excludes(level, src, strlen(src), contact, contact_len);
}

size_t hu_confidence_filter_entries(hu_memory_t *mem, hu_allocator_t *alloc, hu_cb_path_t path,
                                    hu_memory_entry_t **entries, size_t count, const char *contact,
                                    size_t contact_len) {
    hu_gate_mode_t mode = hu_confidence_mode();
    if (mode == HU_GATE_OFF || !alloc || !entries || !*entries || count == 0 || !contact ||
        contact_len == 0)
        return count;
    hu_memory_entry_t *e = *entries;
    size_t keep = 0, hits = 0;
    for (size_t i = 0; i < count; i++) {
        bool drop = entry_excluded(mem, &e[i], contact, contact_len);
        if (drop) {
            hits++;
            if (e[i].content && e[i].content_len)
                hu_confidence_ledger_note(contact, contact_len, e[i].content, e[i].content_len);
        }
        if (drop && mode == HU_GATE_LIVE) {
            hu_memory_entry_free_fields(alloc, &e[i]);
            continue;
        }
        if (keep != i)
            e[keep] = e[i];
        keep++;
    }
    log_path(mode, path, count, hits);
    if (keep == count)
        return count;
    if (keep == 0) {
        alloc->free(alloc->ctx, e, count * sizeof(hu_memory_entry_t));
        *entries = NULL;
        return 0;
    }
    hu_memory_entry_t *shrunk = (hu_memory_entry_t *)alloc->realloc(
        alloc->ctx, e, count * sizeof(hu_memory_entry_t), keep * sizeof(hu_memory_entry_t));
    if (shrunk) {
        *entries = shrunk;
        return keep;
    }
    /* Could not shrink: keep the array's size honest with zeroed tail slots. */
    memset(&e[keep], 0, (count - keep) * sizeof(hu_memory_entry_t));
    return count;
}

static void note_fact(const char *contact, size_t contact_len, const hu_heuristic_fact_t *f) {
    char text[3 * HU_FACT_MAX_FIELD + 3];
    int n = snprintf(text, sizeof(text), "%s %s %s", f->subject, f->predicate, f->object);
    if (n > 0)
        hu_confidence_ledger_note(contact, contact_len, text,
                                  (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
}

const hu_personal_model_t *hu_confidence_pm_view(hu_allocator_t *alloc,
                                                 const hu_personal_model_t *pm, const char *contact,
                                                 size_t contact_len, hu_personal_model_t **owned) {
    if (owned)
        *owned = NULL;
    hu_gate_mode_t mode = hu_confidence_mode();
    if (mode == HU_GATE_OFF || !pm || !contact || contact_len == 0 || pm->fact_count == 0)
        return pm;
    size_t hits = 0;
    bool drop[HU_PM_MAX_FACTS];
    for (size_t i = 0; i < pm->fact_count && i < HU_PM_MAX_FACTS; i++) {
        char src[HU_CB_CONTACT_MAX];
        hu_share_level_t level = hu_confidence_derive_fact(&pm->facts[i], src, sizeof(src));
        drop[i] = hu_confidence_excludes(level, src, strlen(src), contact, contact_len);
        if (drop[i]) {
            hits++;
            note_fact(contact, contact_len, &pm->facts[i]);
        }
    }
    log_path(mode, HU_CB_PATH_PM_FACTS, pm->fact_count, hits);
    if (mode != HU_GATE_LIVE || hits == 0 || !alloc || !owned)
        return pm;
    hu_personal_model_t *copy =
        (hu_personal_model_t *)alloc->alloc(alloc->ctx, sizeof(hu_personal_model_t));
    if (!copy)
        return NULL; /* fail closed: the caller renders no personal-model block */
    memcpy(copy, pm, sizeof(*copy));
    size_t keep = 0;
    for (size_t i = 0; i < pm->fact_count && i < HU_PM_MAX_FACTS; i++)
        if (!drop[i])
            copy->facts[keep++] = pm->facts[i];
    copy->fact_count = keep;
    *owned = copy;
    return copy;
}

void hu_confidence_pm_view_free(hu_allocator_t *alloc, hu_personal_model_t *owned) {
    if (alloc && owned)
        alloc->free(alloc->ctx, owned, sizeof(hu_personal_model_t));
}

void hu_confidence_stamp_write(hu_memory_t *mem, const char *key, size_t key_len,
                               const char *contact, size_t contact_len) {
    hu_confidence_repo_stamp_write(mem, key, key_len, contact, contact_len);
}
