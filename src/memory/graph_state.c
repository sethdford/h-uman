/* State-first read view over bitemporal relations. See graph_state.h.
 *
 * Pure: the only dependency is the single-valued predicate the write-side
 * conflict resolver already uses, so read and write agree on which relation
 * types can have exactly one current truth per source. */

#include "human/memory/graph_state.h"
#include "human/memory/conflict_resolver.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Event times are milliseconds, but 512 of 525 live rows were written in
 * seconds before hu_graph_normalize_timestamp_units existed; read a value
 * in [1e9, 1e11) as seconds so an un-migrated row (or a caller passing
 * time(NULL)) still resolves correctly. Smaller values are abstract test
 * ticks and compare as given. */
static int64_t gs_ms(int64_t t) {
    return (t >= 1000000000LL && t < 100000000000LL) ? t * 1000 : t;
}

bool hu_graph_state_is_current(const hu_graph_relation_t *r, int64_t as_of_ms) {
    if (!r)
        return false;
    int64_t as_of = gs_ms(as_of_ms);
    int64_t start = gs_ms(r->event_start);
    int64_t end = gs_ms(r->event_end);
    if (start > 0 && start > as_of)
        return false;
    return end == 0 || end > as_of;
}

/* Single-valued types compete per (source, type); multi-valued per (source,
 * type, target) so "knows Alice" and "knows Bob" never displace each other. */
static bool gs_same_group(const hu_graph_relation_t *a, const hu_graph_relation_t *b) {
    if (a->source_id != b->source_id || a->type != b->type)
        return false;
    if (hu_conflict_relation_is_single_valued(a->type))
        return true;
    return a->target_id == b->target_id;
}

static bool gs_starts_later(const hu_graph_relation_t *a, const hu_graph_relation_t *b) {
    if (gs_ms(a->event_start) != gs_ms(b->event_start))
        return gs_ms(a->event_start) > gs_ms(b->event_start);
    return a->id > b->id;
}

static bool gs_ends_later(const hu_graph_relation_t *a, const hu_graph_relation_t *b) {
    if (gs_ms(a->event_end) != gs_ms(b->event_end))
        return gs_ms(a->event_end) > gs_ms(b->event_end);
    return a->id > b->id;
}

/* Keep `r` when it is the head of its group as of `as_of_ms`: the latest-
 * starting current row, or — when the group has no current row at all — the
 * latest-ended one (rendered as history). */
static bool gs_is_group_head(const hu_graph_relation_t *rels, size_t n, size_t i, bool current,
                             int64_t as_of_ms) {
    const hu_graph_relation_t *r = &rels[i];
    for (size_t j = 0; j < n; j++) {
        if (j == i || !gs_same_group(&rels[j], r))
            continue;
        bool j_current = hu_graph_state_is_current(&rels[j], as_of_ms);
        if (current) {
            if (j_current && gs_starts_later(&rels[j], r))
                return false;
        } else if (j_current || gs_ends_later(&rels[j], r)) {
            return false;
        }
    }
    return true;
}

/* The row `r` replaced: the one its supersedes_id names when present, else
 * the latest-ended non-current row of the same group. NULL when neither. */
static const hu_graph_relation_t *gs_find_prev(const hu_graph_relation_t *rels, size_t n,
                                               const hu_graph_relation_t *r, int64_t as_of_ms) {
    const hu_graph_relation_t *prev = NULL;
    for (size_t j = 0; j < n; j++) {
        const hu_graph_relation_t *c = &rels[j];
        if (c == r)
            continue;
        if (r->supersedes_id > 0 && c->id == r->supersedes_id)
            return c;
        if (!gs_same_group(c, r) || c->event_end == 0 || hu_graph_state_is_current(c, as_of_ms))
            continue;
        if (!gs_ends_later(r, c) && r->event_end != 0)
            continue; /* ended after r: a successor, not a predecessor */
        if (!prev || gs_ends_later(c, prev))
            prev = c;
    }
    return prev;
}

hu_error_t hu_graph_state_resolve(hu_allocator_t *alloc, const hu_graph_relation_t *rels, size_t n,
                                  int64_t as_of_ms, hu_graph_state_entry_t **out,
                                  size_t *out_count) {
    if (!alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (!rels || n == 0)
        return HU_OK;

    hu_graph_state_entry_t *entries = alloc->alloc(alloc->ctx, n * sizeof(*entries));
    if (!entries)
        return HU_ERR_OUT_OF_MEMORY;
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        const hu_graph_relation_t *r = &rels[i];
        bool current = hu_graph_state_is_current(r, as_of_ms);
        if (!gs_is_group_head(rels, n, i, current, as_of_ms))
            continue;
        entries[count].rel = r;
        entries[count].prev = gs_find_prev(rels, n, r, as_of_ms);
        entries[count].current = current;
        count++;
    }
    if (count == 0) {
        alloc->free(alloc->ctx, entries, n * sizeof(*entries));
        return HU_OK;
    }
    if (count < n) {
        /* Shrink so the caller's free(count * sizeof) matches the allocation. */
        hu_graph_state_entry_t *exact = alloc->alloc(alloc->ctx, count * sizeof(*exact));
        if (exact) {
            memcpy(exact, entries, count * sizeof(*exact));
            alloc->free(alloc->ctx, entries, n * sizeof(*entries));
            entries = exact;
        } else {
            alloc->free(alloc->ctx, entries, n * sizeof(*entries));
            return HU_ERR_OUT_OF_MEMORY;
        }
    }
    *out = entries;
    *out_count = count;
    return HU_OK;
}

size_t hu_graph_state_format_month(int64_t ms, char *buf, size_t cap) {
    static const char *const k_month[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (ms <= 0 || !buf || cap < 9)
        return 0;
    time_t secs = (time_t)(gs_ms(ms) / 1000);
    struct tm tm_utc;
    memset(&tm_utc, 0, sizeof(tm_utc));
    if (!gmtime_r(&secs, &tm_utc))
        return 0;
    int w = snprintf(buf, cap, "%s %04d", k_month[tm_utc.tm_mon % 12], tm_utc.tm_year + 1900);
    return (w > 0 && (size_t)w < cap) ? (size_t)w : 0;
}
