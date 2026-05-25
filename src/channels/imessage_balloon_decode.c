/* src/channels/imessage_balloon_decode.c
 *
 * Phase 5 of docs/plans/2026-05-18-imessage-sota.md: per-balloon-type
 * payload decoders. See header for the privacy contract.
 *
 * Each decoder walks the bplist00 plist with hu_bplist_get_string_at_path,
 * trying multiple known key paths to be tolerant of the Apple key-name
 * drift across iOS versions. The key paths come from imessage-exporter
 * docs + Apple's archived-struct conventions (MapKit, MusicKit,
 * STPaymentService). Where Apple wraps payloads in NSKeyedArchiver
 * ($objects array), the top-level dict lookup returns 0 — the caller
 * falls back to generic narration, which is the right behavior. */

#include "human/channels/imessage_balloon_decode.h"

#include "human/channels/imessage_ingest.h"
#include "human/memory/personal_model.h"
#include "human/util/bplist.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal helpers ─────────────────────────────────────────────── */

/* Try a sequence of single-key paths against the parsed plist and write
 * the first non-empty value into `out`. Returns bytes written, 0 if no
 * key resolved. `keys` is NULL-terminated. */
static size_t try_keys(const hu_bplist_t *p, const char *const *keys, char *out, size_t cap) {
    if (!p || !out || cap < 2)
        return 0;
    for (size_t i = 0; keys[i] != NULL; i++) {
        const char *path[2] = {keys[i], NULL};
        size_t n = hu_bplist_get_string_at_path(p, path, out, cap);
        if (n > 0)
            return n;
    }
    out[0] = '\0';
    return 0;
}

/* Parse + close wrapper. Returns the parsed bplist or NULL on failure;
 * caller MUST hu_bplist_free. */
static hu_bplist_t *parse_payload(const unsigned char *payload, size_t payload_len) {
    if (!payload || payload_len < 32)
        return NULL;
    hu_bplist_t *p = NULL;
    if (hu_bplist_parse(payload, payload_len, &p) != HU_OK)
        return NULL;
    return p;
}

/* ── URL preview ──────────────────────────────────────────────────── */

size_t hu_imessage_decode_url_preview(const unsigned char *payload, size_t payload_len,
                                      char *detail_out, size_t cap) {
    if (!detail_out || cap < 2)
        return 0;
    detail_out[0] = '\0';
    hu_bplist_t *p = parse_payload(payload, payload_len);
    if (!p)
        return 0;
    /* og:title comes first because it's the most human-readable; URL is
     * the last-resort fallback. */
    const char *keys[] = {"title", "summary", "originalURL", "URL", NULL};
    size_t n = try_keys(p, keys, detail_out, cap);
    hu_bplist_free(p);
    return n;
}

/* ── Apple Pay ────────────────────────────────────────────────────── */

/* Privacy contract enforced STRUCTURALLY: this function never references
 * "amount", "currency", "value" keys. Even if the caller passes a payload
 * whose dict contains those, we don't read them. */
size_t hu_imessage_decode_apple_pay(const unsigned char *payload, size_t payload_len,
                                    char *detail_out, size_t cap) {
    if (!detail_out || cap < 2)
        return 0;
    detail_out[0] = '\0';
    hu_bplist_t *p = parse_payload(payload, payload_len);
    if (!p)
        return 0;
    /* Recipient / sender handle ONLY. Never an amount. */
    const char *keys[] = {"recipient", "sender", "recipientHandle", "handle", NULL};
    size_t n = try_keys(p, keys, detail_out, cap);
    hu_bplist_free(p);
    return n;
}

/* ── Placemark ────────────────────────────────────────────────────── */

/* Privacy contract: never reads `latitude` / `longitude` / coordinate
 * keys. Only place names. */
size_t hu_imessage_decode_placemark(const unsigned char *payload, size_t payload_len,
                                    char *detail_out, size_t cap) {
    if (!detail_out || cap < 2)
        return 0;
    detail_out[0] = '\0';
    hu_bplist_t *p = parse_payload(payload, payload_len);
    if (!p)
        return 0;

    /* Try a sequence of single-key paths first. */
    const char *simple_keys[] = {"name", NULL};
    size_t n = try_keys(p, simple_keys, detail_out, cap);
    if (n > 0) {
        hu_bplist_free(p);
        return n;
    }

    /* Then try "City, Country" composition from nested mapItem.placemark. */
    char locality[96] = {0};
    char country[96] = {0};
    const char *locality_path[] = {"mapItem", "placemark", "locality", NULL};
    const char *country_path[] = {"mapItem", "placemark", "country", NULL};
    size_t lo_n = hu_bplist_get_string_at_path(p, locality_path, locality, sizeof(locality));
    size_t co_n = hu_bplist_get_string_at_path(p, country_path, country, sizeof(country));
    if (lo_n > 0 && co_n > 0) {
        int written = snprintf(detail_out, cap, "%s, %s", locality, country);
        hu_bplist_free(p);
        return (written > 0 && (size_t)written < cap) ? (size_t)written : 0;
    }
    if (lo_n > 0) {
        int written = snprintf(detail_out, cap, "%s", locality);
        hu_bplist_free(p);
        return (written > 0 && (size_t)written < cap) ? (size_t)written : 0;
    }

    hu_bplist_free(p);
    return 0;
}

/* ── Music ────────────────────────────────────────────────────────── */

size_t hu_imessage_decode_music(const unsigned char *payload, size_t payload_len, char *detail_out,
                                size_t cap) {
    if (!detail_out || cap < 2)
        return 0;
    detail_out[0] = '\0';
    hu_bplist_t *p = parse_payload(payload, payload_len);
    if (!p)
        return 0;

    char song[128] = {0}, artist[128] = {0};
    const char *song_path[] = {"songName", NULL};
    const char *track_path[] = {"trackName", NULL};
    const char *artist_path[] = {"artistName", NULL};
    size_t sn = hu_bplist_get_string_at_path(p, song_path, song, sizeof(song));
    if (sn == 0)
        sn = hu_bplist_get_string_at_path(p, track_path, song, sizeof(song));
    size_t an = hu_bplist_get_string_at_path(p, artist_path, artist, sizeof(artist));

    int written = 0;
    if (sn > 0 && an > 0)
        written = snprintf(detail_out, cap, "%s by %s", song, artist);
    else if (sn > 0)
        written = snprintf(detail_out, cap, "%s", song);
    else {
        /* Fallback: album name only. */
        const char *album_keys[] = {"albumName", NULL};
        size_t fallback = try_keys(p, album_keys, detail_out, cap);
        hu_bplist_free(p);
        return fallback;
    }
    hu_bplist_free(p);
    return (written > 0 && (size_t)written < cap) ? (size_t)written : 0;
}

/* ── Poll ─────────────────────────────────────────────────────────── */

size_t hu_imessage_decode_poll(const unsigned char *payload, size_t payload_len, char *detail_out,
                               size_t cap) {
    if (!detail_out || cap < 2)
        return 0;
    detail_out[0] = '\0';
    hu_bplist_t *p = parse_payload(payload, payload_len);
    if (!p)
        return 0;
    const char *keys[] = {"question", "prompt", "title", NULL};
    size_t n = try_keys(p, keys, detail_out, cap);
    hu_bplist_free(p);
    return n;
}

/* ── Dispatch + row-ingest ────────────────────────────────────────── */

hu_imessage_balloon_kind_t hu_imessage_balloon_decode(const char *bundle_id,
                                                      const unsigned char *payload,
                                                      size_t payload_len, char *detail_out,
                                                      size_t cap) {
    hu_imessage_balloon_kind_t kind = hu_imessage_balloon_kind_from_bundle_id(bundle_id);
    if (detail_out && cap > 0)
        detail_out[0] = '\0';

    switch (kind) {
    case HU_IMESSAGE_BALLOON_URL_PREVIEW:
        hu_imessage_decode_url_preview(payload, payload_len, detail_out, cap);
        return kind;
    case HU_IMESSAGE_BALLOON_APPLE_PAY:
        hu_imessage_decode_apple_pay(payload, payload_len, detail_out, cap);
        return kind;
    case HU_IMESSAGE_BALLOON_PLACEMARK:
        hu_imessage_decode_placemark(payload, payload_len, detail_out, cap);
        return kind;
    case HU_IMESSAGE_BALLOON_MUSIC:
        hu_imessage_decode_music(payload, payload_len, detail_out, cap);
        return kind;
    case HU_IMESSAGE_BALLOON_POLL:
        hu_imessage_decode_poll(payload, payload_len, detail_out, cap);
        return kind;
    case HU_IMESSAGE_BALLOON_AUDIO_TRANSCRIPT:
        if (detail_out && cap > 0)
            hu_imessage_extract_audio_transcript(payload, payload_len, detail_out, cap);
        return kind;
    case HU_IMESSAGE_BALLOON_UNKNOWN:
    default:
        return HU_IMESSAGE_BALLOON_UNKNOWN;
    }
}

hu_error_t hu_imessage_ingest_balloon_row(struct hu_personal_model *model, const char *bundle_id,
                                          const unsigned char *payload, size_t payload_len,
                                          const char *sender_handle, bool is_from_me,
                                          int64_t timestamp_unix, bool in_group_chat) {
    if (!model)
        return HU_ERR_INVALID_ARGUMENT;
    char detail[512];
    hu_imessage_balloon_kind_t kind =
        hu_imessage_balloon_decode(bundle_id, payload, payload_len, detail, sizeof(detail));
    if (kind == HU_IMESSAGE_BALLOON_UNKNOWN)
        return HU_OK;
    hu_error_t balloon_err =
        hu_imessage_ingest_balloon(model, sender_handle, is_from_me, kind,
                                   detail[0] ? detail : NULL, timestamp_unix, in_group_chat);
    /* B5 wire: voice messages carry tone signal (speech rate) that the
     * transcript alone discards. Extract the duration from the same
     * payload, classify via hu_audio_tone_classify, and ingest the
     * resulting "<handle>'s voice message sounded <label>." fact as a
     * SECOND personal-model entry alongside the transcript. No-op when:
     *   - is_from_me (we model others' tone, not the user's own)
     *   - duration extraction returned 0.0 (older iOS / archiver-wrapped)
     *   - transcript was empty (no word_count signal)
     *   - classifier returned UNKNOWN
     * Failure of the tone ingest does NOT propagate — the transcript
     * ingest's success/failure is what the caller cares about. */
    if (kind == HU_IMESSAGE_BALLOON_AUDIO_TRANSCRIPT && detail[0]) {
        double duration = hu_imessage_extract_audio_duration(payload, payload_len);
        (void)hu_imessage_ingest_audio_tone(model, sender_handle, is_from_me, detail, duration,
                                            timestamp_unix, in_group_chat);
    }
    return balloon_err;
}
