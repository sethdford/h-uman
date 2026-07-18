/* Tapback timing bands (roadmap #18) — see include/human/behavior/tapback_band.h.
 *
 * Humans tapback within seconds-to-minutes or not at all; the 2026-07-18
 * audit saw a "Loved" echo ~100 min late. The dispatch gate here drops
 * stale tapbacks entirely: a missing tapback is invisible, a late one (or
 * its SMS `Loved "..."` text rendering) is a tell. */

#include "human/behavior/tapback_band.h"

#include "human/core/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t band_cap_ms(const hu_tapback_band_t *band) {
    if (band && band->valid && band->p90_ms > 0)
        return band->p90_ms;
    return HU_TAPBACK_DEFAULT_CAP_MS;
}

bool hu_tapback_within_band(int64_t now_ms, int64_t target_msg_ms, const hu_tapback_band_t *band) {
    if (target_msg_ms <= 0)
        return true; /* origin unknown — never drop on missing data */
    int64_t age_ms = now_ms - target_msg_ms;
    if (age_ms < 0)
        return true; /* clock skew — not stale */
    return age_ms <= band_cap_ms(band);
}

bool hu_tapback_dispatch_within_band(int64_t now_sec, int64_t msg_timestamp_sec,
                                     const hu_tapback_band_t *band) {
    if (msg_timestamp_sec <= 0)
        return true; /* hu_channel_loop_msg_t: 0 = fresh at poll time */
    return hu_tapback_within_band(now_sec * 1000, msg_timestamp_sec * 1000, band);
}

static void band_from_json(const hu_json_value_t *obj, hu_tapback_band_t *out) {
    out->valid = true;
    out->rate = hu_json_get_number(obj, "rate", 0.0);
    out->p50_ms = (int64_t)hu_json_get_number(obj, "p50_ms", 0.0);
    out->p90_ms = (int64_t)hu_json_get_number(obj, "p90_ms", 0.0);
}

hu_error_t hu_tapback_band_parse(hu_allocator_t *alloc, const char *json, size_t json_len,
                                 const char *contact_id, hu_tapback_band_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!alloc || !json || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &root);
    if (err != HU_OK || !root)
        return err != HU_OK ? err : HU_ERR_PARSE;

    const hu_json_value_t *chosen = NULL;
    if (contact_id && contact_id[0]) {
        hu_json_value_t *contacts = hu_json_object_get(root, "contacts");
        if (contacts)
            chosen = hu_json_object_get(contacts, contact_id);
    }
    if (!chosen)
        chosen = hu_json_object_get(root, "default");
    if (chosen)
        band_from_json(chosen, out);

    hu_json_free(alloc, root);
    return HU_OK;
}

hu_error_t hu_tapback_band_load(hu_allocator_t *alloc, const char *path, const char *contact_id,
                                hu_tapback_band_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!alloc || !path || !out)
        return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_NOT_FOUND;
    /* Bands files are small (~100 contact bands fit well under 16 KB); a
     * bounded stack read keeps the dispatch path allocation-free. Oversized
     * files degrade safely: HU_ERR_IO leaves out invalid, so the predicate
     * falls back to the 15-min default cap. */
    char buf[16384];
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    bool truncated = (rd == sizeof(buf) - 1) && fgetc(f) != EOF;
    fclose(f);
    if (rd == 0 || truncated)
        return HU_ERR_IO;
    buf[rd] = '\0';
    return hu_tapback_band_parse(alloc, buf, rd, contact_id, out);
}

const char *hu_tapback_bands_default_path(char *buf, size_t cap) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)buf, (void)cap;
    return NULL; /* tests never touch the real home dir */
#else
    const char *home = getenv("HOME");
    if (!home || !home[0] || !buf || cap == 0)
        return NULL;
    int n = snprintf(buf, cap, "%s/.human/tapback_bands.json", home);
    return (n > 0 && (size_t)n < cap) ? buf : NULL;
#endif
}
