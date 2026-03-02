#include "seaclaw/memory/lifecycle.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>

/* Parse ISO8601 timestamp (YYYY-MM-DDTHH:MM:SSZ) to seconds since epoch.
 * Returns 0 on parse failure (treat as very old). */
static uint64_t parse_timestamp_epoch(const char *ts, size_t ts_len) {
    if (!ts || ts_len < 19) return 0;
    int y, mo, d, h, mi, s;
    if (sscanf(ts, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6)
        return 0;
    struct tm tm = {0};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = 0;
#if defined(_BSD_SOURCE) || defined(_DEFAULT_SOURCE) || defined(__APPLE__)
    time_t t = timegm(&tm);
#else
    time_t t = mktime(&tm);
#endif
    return (uint64_t)(t > 0 ? t : 0);
}

/* Compare entries by content for deduplication */
static bool content_equal(const sc_memory_entry_t *a, const sc_memory_entry_t *b) {
    if (a->content_len != b->content_len) return false;
    if (!a->content || !b->content) return a->content == b->content;
    return memcmp(a->content, b->content, a->content_len) == 0;
}

/* Compare timestamps; return true if a is newer than b */
static bool timestamp_newer(const sc_memory_entry_t *a, const sc_memory_entry_t *b) {
    uint64_t ta = parse_timestamp_epoch(a->timestamp, a->timestamp_len);
    uint64_t tb = parse_timestamp_epoch(b->timestamp, b->timestamp_len);
    return ta > tb;
}

sc_error_t sc_memory_hygiene_run(sc_allocator_t *alloc, sc_memory_t *memory,
    const sc_hygiene_config_t *config, sc_hygiene_stats_t *stats) {
    if (!alloc || !memory || !memory->vtable || !config || !stats)
        return SC_ERR_INVALID_ARGUMENT;

    memset(stats, 0, sizeof(*stats));

    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = memory->vtable->list(memory->ctx, alloc, NULL, NULL, 0, &entries, &count);
    if (err != SC_OK) return err;
    if (!entries || count == 0) {
        stats->entries_scanned = 0;
        return SC_OK;
    }

    stats->entries_scanned = count;
    bool *to_remove = (bool *)alloc->alloc(alloc->ctx, count * sizeof(bool));
    if (!to_remove) {
        for (size_t i = 0; i < count; i++) sc_memory_entry_free_fields(alloc, &entries[i]);
        alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(to_remove, 0, count * sizeof(bool));

    uint64_t now = (uint64_t)time(NULL);

    for (size_t i = 0; i < count; i++) {
        sc_memory_entry_t *e = &entries[i];
        if (config->max_entry_size > 0 && e->content_len > config->max_entry_size) {
            to_remove[i] = true;
            stats->oversized_removed++;
            continue;
        }
        if (config->max_age_seconds > 0 && e->timestamp && e->timestamp_len > 0) {
            uint64_t t = parse_timestamp_epoch(e->timestamp, e->timestamp_len);
            if (t > 0 && (now - t) > config->max_age_seconds) {
                to_remove[i] = true;
                stats->expired_removed++;
                continue;
            }
        }
    }

    if (config->deduplicate) {
        for (size_t i = 0; i < count; i++) {
            if (to_remove[i]) continue;
            for (size_t j = i + 1; j < count; j++) {
                if (to_remove[j]) continue;
                if (!content_equal(&entries[i], &entries[j])) continue;
                if (timestamp_newer(&entries[i], &entries[j]))
                    to_remove[j] = true;
                else
                    to_remove[i] = true;
                stats->duplicates_removed++;
                break;
            }
        }
    }

    if (config->max_entries > 0 && count > config->max_entries) {
        size_t to_evict = count - config->max_entries;
        for (size_t n = 0; n < to_evict; n++) {
            size_t oldest = count;
            uint64_t oldest_ts = UINT64_MAX;
            for (size_t i = 0; i < count; i++) {
                if (to_remove[i]) continue;
                uint64_t t = entries[i].timestamp && entries[i].timestamp_len
                    ? parse_timestamp_epoch(entries[i].timestamp, entries[i].timestamp_len)
                    : 0;
                if (t < oldest_ts) {
                    oldest_ts = t;
                    oldest = i;
                }
            }
            if (oldest < count)
                to_remove[oldest] = true;
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (!to_remove[i]) continue;
        bool deleted = false;
        const char *key = entries[i].key;
        size_t key_len = entries[i].key_len;
        if (key && key_len > 0) {
            memory->vtable->forget(memory->ctx, key, key_len, &deleted);
            if (deleted) stats->entries_removed++;
        }
    }

    for (size_t i = 0; i < count; i++)
        sc_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
    alloc->free(alloc->ctx, to_remove, count * sizeof(bool));

    return SC_OK;
}
