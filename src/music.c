#include "human/music.h"
#include "human/core/string.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/io_secure.h"
#include "human/core/json.h"
#include "human/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if !defined(HU_IS_TEST)
#include <time.h>
#include <unistd.h>
#endif

/* ── Helpers ─────────────────────────────────────────────────────────── */

static char *dup_json_str(hu_allocator_t *alloc, const char *s) {
    if (!s)
        return NULL;
    size_t len = strlen(s);
    char *d = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = '\0';
    }
    return d;
}

static void free_str(hu_allocator_t *alloc, char **s) {
    if (*s) {
        alloc->free(alloc->ctx, *s, strlen(*s) + 1);
        *s = NULL;
    }
}

/* ── URL encoding (with proper %XX for non-ASCII) ────────────────────── */

static const char hex_chars[] = "0123456789ABCDEF";

size_t hu_music_url_encode_query(const char *query, size_t query_len, char *out, size_t out_cap) {
    if (!query || query_len == 0 || !out || out_cap < 2)
        return 0;
    size_t w = 0;
    for (size_t i = 0; i < query_len && w + 3 < out_cap; i++) {
        unsigned char c = (unsigned char)query[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.') {
            out[w++] = (char)c;
        } else if (c == ' ') {
            out[w++] = '+';
        } else {
            out[w++] = '%';
            out[w++] = hex_chars[c >> 4];
            out[w++] = hex_chars[c & 0x0f];
        }
    }
    out[w] = '\0';
    return w;
}

/* ── iTunes JSON response parsing (pure, always compiled) ────────────── */

hu_error_t hu_music_parse_search_response(hu_allocator_t *alloc, const char *json, size_t json_len,
                                          hu_music_result_t *out) {
    if (!alloc || !json || json_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &root);
    if (err != HU_OK)
        return err;

    hu_json_value_t *results = hu_json_object_get(root, "results");
    if (!results || results->type != HU_JSON_ARRAY || results->data.array.len == 0) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_FOUND;
    }

    hu_json_value_t *first = results->data.array.items[0];
    if (!first || first->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_FOUND;
    }

    out->track_name = dup_json_str(alloc, hu_json_get_string(first, "trackName"));
    out->artist_name = dup_json_str(alloc, hu_json_get_string(first, "artistName"));
    out->album_name = dup_json_str(alloc, hu_json_get_string(first, "collectionName"));
    out->preview_url = dup_json_str(alloc, hu_json_get_string(first, "previewUrl"));
    out->track_view_url = dup_json_str(alloc, hu_json_get_string(first, "trackViewUrl"));
    out->artwork_url = dup_json_str(alloc, hu_json_get_string(first, "artworkUrl100"));
    out->genre = dup_json_str(alloc, hu_json_get_string(first, "primaryGenreName"));
    out->source = HU_MUSIC_SOURCE_ITUNES;

    hu_json_free(alloc, root);

    if (!out->track_name && !out->artist_name) {
        hu_music_result_free(alloc, out);
        return HU_ERR_NOT_FOUND;
    }

    return HU_OK;
}

/* ── Spotify JSON response parsing (pure, always compiled) ───────────── */

hu_error_t hu_music_parse_spotify_response(hu_allocator_t *alloc, const char *json, size_t json_len,
                                           hu_music_result_t *out) {
    if (!alloc || !json || json_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &root);
    if (err != HU_OK)
        return err;

    /* Spotify: { tracks: { items: [ { name, artists: [{name}], album: {name, images}, ... } ] } } */
    hu_json_value_t *tracks = hu_json_object_get(root, "tracks");
    if (!tracks) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_FOUND;
    }

    hu_json_value_t *items = hu_json_object_get(tracks, "items");
    if (!items || items->type != HU_JSON_ARRAY || items->data.array.len == 0) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_FOUND;
    }

    hu_json_value_t *first = items->data.array.items[0];
    if (!first || first->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_FOUND;
    }

    out->track_name = dup_json_str(alloc, hu_json_get_string(first, "name"));

    /* artists[0].name */
    hu_json_value_t *artists = hu_json_object_get(first, "artists");
    if (artists && artists->type == HU_JSON_ARRAY && artists->data.array.len > 0) {
        hu_json_value_t *a0 = artists->data.array.items[0];
        if (a0 && a0->type == HU_JSON_OBJECT)
            out->artist_name = dup_json_str(alloc, hu_json_get_string(a0, "name"));
    }

    /* album.name + album.images[0].url */
    hu_json_value_t *album = hu_json_object_get(first, "album");
    if (album && album->type == HU_JSON_OBJECT) {
        out->album_name = dup_json_str(alloc, hu_json_get_string(album, "name"));
        hu_json_value_t *images = hu_json_object_get(album, "images");
        if (images && images->type == HU_JSON_ARRAY && images->data.array.len > 0) {
            hu_json_value_t *img0 = images->data.array.items[0];
            if (img0 && img0->type == HU_JSON_OBJECT)
                out->artwork_url = dup_json_str(alloc, hu_json_get_string(img0, "url"));
        }
    }

    /* external_urls.spotify */
    hu_json_value_t *ext = hu_json_object_get(first, "external_urls");
    if (ext && ext->type == HU_JSON_OBJECT)
        out->track_view_url = dup_json_str(alloc, hu_json_get_string(ext, "spotify"));

    out->preview_url = dup_json_str(alloc, hu_json_get_string(first, "preview_url"));
    out->source = HU_MUSIC_SOURCE_SPOTIFY;

    hu_json_free(alloc, root);

    if (!out->track_name && !out->artist_name) {
        hu_music_result_free(alloc, out);
        return HU_ERR_NOT_FOUND;
    }

    return HU_OK;
}

/* ── Share text builder (pure, always compiled) ──────────────────────── */

size_t hu_music_build_share_text(const hu_music_result_t *result, const char *casual_msg,
                                 size_t casual_msg_len, char *buf, size_t cap) {
    if (!result || !buf || cap < 32)
        return 0;

    const char *url = result->track_view_url;
    int n;

    if (casual_msg && casual_msg_len > 0 && url) {
        n = snprintf(buf, cap, "%.*s %s", (int)casual_msg_len, casual_msg, url);
    } else if (casual_msg && casual_msg_len > 0) {
        n = snprintf(buf, cap, "%.*s", (int)casual_msg_len, casual_msg);
    } else if (url) {
        n = snprintf(buf, cap, "%s - %s %s", result->artist_name ? result->artist_name : "Unknown",
                     result->track_name ? result->track_name : "Unknown", url);
    } else {
        n = snprintf(buf, cap, "%s - %s", result->artist_name ? result->artist_name : "Unknown",
                     result->track_name ? result->track_name : "Unknown");
    }

    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* ── LLM suggestion parser (pure, always compiled) ───────────────────── */

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void trim_inplace(char *s) {
    size_t len = strlen(s);
    while (len > 0 && is_ws(s[len - 1]))
        s[--len] = '\0';
    size_t start = 0;
    while (start < len && is_ws(s[start]))
        start++;
    if (start > 0)
        memmove(s, s + start, len - start + 1);
}

bool hu_music_parse_suggestion(const char *suggestion, size_t suggestion_len, char *query_out,
                               size_t query_cap, char *msg_out, size_t msg_cap) {
    if (!suggestion || suggestion_len == 0 || !query_out || query_cap < 8 || !msg_out ||
        msg_cap < 8)
        return false;

    query_out[0] = '\0';
    msg_out[0] = '\0';

    while (suggestion_len > 0 && is_ws(suggestion[suggestion_len - 1]))
        suggestion_len--;
    if (suggestion_len == 0)
        return false;

    const char *pipe = NULL;
    for (size_t i = 0; i + 2 < suggestion_len; i++) {
        if (suggestion[i] == ' ' && suggestion[i + 1] == '|' && suggestion[i + 2] == ' ') {
            pipe = suggestion + i;
            break;
        }
    }

    if (pipe) {
        size_t query_len = (size_t)(pipe - suggestion);
        if (query_len >= query_cap)
            query_len = query_cap - 1;
        memcpy(query_out, suggestion, query_len);
        query_out[query_len] = '\0';

        const char *msg_start = pipe + 3;
        size_t msg_len = suggestion_len - (size_t)(msg_start - suggestion);
        if (msg_len >= msg_cap)
            msg_len = msg_cap - 1;
        memcpy(msg_out, msg_start, msg_len);
        msg_out[msg_len] = '\0';
    } else {
        size_t query_len = suggestion_len;
        if (query_len >= query_cap)
            query_len = query_cap - 1;
        memcpy(query_out, suggestion, query_len);
        query_out[query_len] = '\0';
    }

    trim_inplace(query_out);
    trim_inplace(msg_out);

    return query_out[0] != '\0';
}

/* ── Preference detection (pure, always compiled) ────────────────────── */

hu_music_source_t hu_music_detect_preference(const char *const *texts, const size_t *lens,
                                              size_t count) {
    if (!texts || !lens || count == 0)
        return HU_MUSIC_SOURCE_ITUNES;

    int spotify_count = 0;
    int apple_count = 0;

    for (size_t i = 0; i < count; i++) {
        if (!texts[i])
            continue;
        size_t tlen = lens[i];
        if (hu_str_contains_ci_cstr(texts[i], tlen, "open.spotify.com") ||
            hu_str_contains_ci_cstr(texts[i], tlen, "spotify.link"))
            spotify_count++;
        if (hu_str_contains_ci_cstr(texts[i], tlen, "music.apple.com"))
            apple_count++;
    }

    return spotify_count > apple_count ? HU_MUSIC_SOURCE_SPOTIFY : HU_MUSIC_SOURCE_ITUNES;
}

/* ── Taste learning (in-memory, parallel to GIF calibration) ─────────── */

typedef struct {
    char contact_id[128];
    char artists[HU_MUSIC_TASTE_HISTORY][64];
    char titles[HU_MUSIC_TASTE_HISTORY][128];
    int sends;
    int reactions;
    int head; /* circular index */
} music_taste_entry_t;

static music_taste_entry_t s_taste[HU_MUSIC_TASTE_MAX_CONTACTS];
static int s_taste_count;

static music_taste_entry_t *find_or_create_taste(const char *contact_id, size_t cid_len) {
    if (!contact_id || cid_len == 0 || cid_len >= 127)
        return NULL;

    for (int i = 0; i < s_taste_count; i++) {
        if (strncmp(s_taste[i].contact_id, contact_id, cid_len) == 0 &&
            s_taste[i].contact_id[cid_len] == '\0')
            return &s_taste[i];
    }

    if (s_taste_count >= HU_MUSIC_TASTE_MAX_CONTACTS) {
        /* Evict oldest (slot 0), shift down */
        memmove(&s_taste[0], &s_taste[1],
                (size_t)(HU_MUSIC_TASTE_MAX_CONTACTS - 1) * sizeof(music_taste_entry_t));
        s_taste_count--;
    }

    music_taste_entry_t *e = &s_taste[s_taste_count++];
    memset(e, 0, sizeof(*e));
    memcpy(e->contact_id, contact_id, cid_len);
    e->contact_id[cid_len] = '\0';
    return e;
}

void hu_music_taste_record_send(const char *contact_id, size_t cid_len, const char *artist,
                                const char *title) {
    music_taste_entry_t *e = find_or_create_taste(contact_id, cid_len);
    if (!e)
        return;

    int idx = e->head % HU_MUSIC_TASTE_HISTORY;
    if (artist) {
        size_t al = strlen(artist);
        if (al >= 64)
            al = 63;
        memcpy(e->artists[idx], artist, al);
        e->artists[idx][al] = '\0';
    }
    if (title) {
        size_t tl = strlen(title);
        if (tl >= 128)
            tl = 127;
        memcpy(e->titles[idx], title, tl);
        e->titles[idx][tl] = '\0';
    }
    e->head++;
    e->sends++;
}

void hu_music_taste_record_reaction(const char *contact_id, size_t cid_len) {
    music_taste_entry_t *e = find_or_create_taste(contact_id, cid_len);
    if (e)
        e->reactions++;
}

float hu_music_taste_hit_rate(const char *contact_id, size_t cid_len) {
    if (!contact_id || cid_len == 0)
        return 0.0f;
    for (int i = 0; i < s_taste_count; i++) {
        if (strncmp(s_taste[i].contact_id, contact_id, cid_len) == 0 &&
            s_taste[i].contact_id[cid_len] == '\0') {
            if (s_taste[i].sends == 0)
                return 0.0f;
            float rate = (float)s_taste[i].reactions / (float)s_taste[i].sends;
            return rate > 1.0f ? 1.0f : rate;
        }
    }
    return 0.0f;
}

size_t hu_music_taste_build_prompt(const char *contact_id, size_t cid_len, char *buf, size_t cap) {
    if (!contact_id || cid_len == 0 || !buf || cap < 64)
        return 0;

    music_taste_entry_t *e = NULL;
    for (int i = 0; i < s_taste_count; i++) {
        if (strncmp(s_taste[i].contact_id, contact_id, cid_len) == 0 &&
            s_taste[i].contact_id[cid_len] == '\0') {
            e = &s_taste[i];
            break;
        }
    }
    if (!e || e->sends == 0)
        return 0;

    /* Collect unique artists from recent sends */
    char seen[HU_MUSIC_TASTE_HISTORY][64];
    int seen_count = 0;

    int total = e->head < HU_MUSIC_TASTE_HISTORY ? e->head : HU_MUSIC_TASTE_HISTORY;
    for (int i = 0; i < total && seen_count < 6; i++) {
        int idx = (e->head - 1 - i + HU_MUSIC_TASTE_HISTORY * 2) % HU_MUSIC_TASTE_HISTORY;
        if (e->artists[idx][0] == '\0')
            continue;
        bool dup = false;
        for (int j = 0; j < seen_count; j++) {
            if (strcmp(seen[j], e->artists[idx]) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            size_t al = strlen(e->artists[idx]);
            if (al >= 64)
                al = 63;
            memcpy(seen[seen_count], e->artists[idx], al);
            seen[seen_count][al] = '\0';
            seen_count++;
        }
    }

    if (seen_count == 0)
        return 0;

    size_t w = 0;
    int n = snprintf(buf + w, cap - w, "Previously shared artists they liked: ");
    if (n > 0 && w + (size_t)n < cap)
        w += (size_t)n;

    for (int i = 0; i < seen_count && w + 1 < cap; i++) {
        n = snprintf(buf + w, cap - w, "%s%s", i > 0 ? ", " : "", seen[i]);
        if (n > 0 && w + (size_t)n < cap)
            w += (size_t)n;
    }

    float rate = (e->sends > 0) ? (float)e->reactions / (float)e->sends : 0.0f;
    if (rate > 0.0f && w + 1 < cap) {
        n = snprintf(buf + w, cap - w, " (%.0f%% positive reaction rate)", rate * 100.0f);
        if (n > 0 && w + (size_t)n < cap)
            w += (size_t)n;
    }

    return w;
}

/* ── Taste persistence (JSON file) ────────────────────────────────────── */

static void taste_write_escaped(FILE *f, const char *s) {
    for (; *s; s++) {
        if (*s == '"')
            fputs("\\\"", f);
        else if (*s == '\\')
            fputs("\\\\", f);
        else if ((unsigned char)*s < 0x20)
            fprintf(f, "\\u%04x", (unsigned)*s);
        else
            fputc(*s, f);
    }
}

hu_error_t hu_music_taste_save(const char *path, size_t path_len) {
    if (!path || path_len == 0 || s_taste_count == 0)
        return HU_ERR_INVALID_ARGUMENT;

    char path_buf[512];
    if (path_len >= sizeof(path_buf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(path_buf, path, path_len);
    path_buf[path_len] = '\0';

    /* Music taste file at caller-supplied path (typically
     * ~/.human/music_taste.json). Path is user/config derived so we
     * need the traversal guard; contents are user data, not secrets,
     * so 0644 is appropriate. */
    FILE *f = NULL;
    if (hu_io_secure_open(path_buf, HU_IO_PERM_USER, "w", &f) != HU_OK || !f) {
        /* Try creating parent directory (e.g. ~/.human/) */
        char *last_slash = strrchr(path_buf, '/');
        if (last_slash && last_slash != path_buf) {
            *last_slash = '\0';
            (void)mkdir(path_buf, 0700);
            *last_slash = '/';
            (void)hu_io_secure_open(path_buf, HU_IO_PERM_USER, "w", &f);
        }
        if (!f)
            return HU_ERR_IO;
    }

    fprintf(f, "[\n");
    for (int i = 0; i < s_taste_count; i++) {
        music_taste_entry_t *e = &s_taste[i];
        fprintf(f, "  {\"contact\":\"");
        taste_write_escaped(f, e->contact_id);
        fprintf(f, "\",\"sends\":%d,\"reactions\":%d,\"head\":%d,\"artists\":[",
                e->sends, e->reactions, e->head);
        int total = e->head < HU_MUSIC_TASTE_HISTORY ? e->head : HU_MUSIC_TASTE_HISTORY;
        bool first = true;
        for (int j = 0; j < total; j++) {
            int idx = (e->head - 1 - j + HU_MUSIC_TASTE_HISTORY * 2) % HU_MUSIC_TASTE_HISTORY;
            if (e->artists[idx][0]) {
                fprintf(f, "%s\"", first ? "" : ",");
                taste_write_escaped(f, e->artists[idx]);
                fputc('"', f);
                first = false;
            }
        }
        fprintf(f, "]}%s\n", i + 1 < s_taste_count ? "," : "");
    }
    fprintf(f, "]\n");
    fclose(f);
    return HU_OK;
}

hu_error_t hu_music_taste_load(const char *path, size_t path_len) {
    if (!path || path_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    char path_buf[512];
    if (path_len >= sizeof(path_buf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(path_buf, path, path_len);
    path_buf[path_len] = '\0';

    FILE *f = fopen(path_buf, "r");
    if (!f)
        return HU_ERR_NOT_FOUND;

    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0)
        return HU_ERR_NOT_FOUND;
    buf[n] = '\0';

    s_taste_count = 0;
    const char *p = buf;
    while ((p = strstr(p, "\"contact\":\"")) != NULL && s_taste_count < HU_MUSIC_TASTE_MAX_CONTACTS) {
        p += 11;
        const char *end = strchr(p, '"');
        if (!end)
            break;
        music_taste_entry_t *e = &s_taste[s_taste_count];
        memset(e, 0, sizeof(*e));
        size_t cl = (size_t)(end - p);
        if (cl >= sizeof(e->contact_id) - 1)
            cl = sizeof(e->contact_id) - 1;
        memcpy(e->contact_id, p, cl);
        e->contact_id[cl] = '\0';
        p = end + 1;

        /* Find the closing brace for this entry to limit search scope */
        const char *entry_end = strchr(p, '}');
        if (!entry_end)
            entry_end = buf + n;
        size_t scope = (size_t)(entry_end - p);

        const char *sends = strstr(p, "\"sends\":");
        if (sends && (size_t)(sends - p) < scope)
            e->sends = atoi(sends + 8);
        const char *reacts = strstr(p, "\"reactions\":");
        if (reacts && (size_t)(reacts - p) < scope)
            e->reactions = atoi(reacts + 12);
        const char *head_s = strstr(p, "\"head\":");
        if (head_s && (size_t)(head_s - p) < scope)
            e->head = atoi(head_s + 7);

        /* Parse artists array */
        const char *artists = strstr(p, "\"artists\":[");
        if (artists && (size_t)(artists - p) < scope) {
            const char *ap = artists + 11;
            const char *arr_end = strchr(ap, ']');
            if (!arr_end || arr_end > entry_end)
                arr_end = entry_end;
            int ai = 0;
            while (ap < arr_end && ai < HU_MUSIC_TASTE_HISTORY) {
                const char *q1 = strchr(ap, '"');
                if (!q1 || q1 >= arr_end)
                    break;
                q1++;
                const char *q2 = strchr(q1, '"');
                if (!q2 || q2 >= arr_end)
                    break;
                size_t al = (size_t)(q2 - q1);
                if (al >= sizeof(e->artists[0]) - 1)
                    al = sizeof(e->artists[0]) - 1;
                /* Save in reverse order (newest first in file) */
                int slot = (e->head - 1 - ai + HU_MUSIC_TASTE_HISTORY * 2) % HU_MUSIC_TASTE_HISTORY;
                memcpy(e->artists[slot], q1, al);
                e->artists[slot][al] = '\0';
                ai++;
                ap = q2 + 1;
            }
        }

        p = entry_end;
        s_taste_count++;
    }

    return s_taste_count > 0 ? HU_OK : HU_ERR_NOT_FOUND;
}

/* ── Result cleanup ──────────────────────────────────────────────────── */

void hu_music_result_free(hu_allocator_t *alloc, hu_music_result_t *r) {
    if (!alloc || !r)
        return;
    free_str(alloc, &r->track_name);
    free_str(alloc, &r->artist_name);
    free_str(alloc, &r->album_name);
    free_str(alloc, &r->preview_url);
    free_str(alloc, &r->track_view_url);
    free_str(alloc, &r->artwork_url);
    free_str(alloc, &r->genre);
}

/* ── Network functions ───────────────────────────────────────────────── */

#if !defined(HU_IS_TEST) && defined(HU_HTTP_CURL)

#include "human/core/http.h"

hu_error_t hu_music_search(hu_allocator_t *alloc, const char *query, size_t query_len,
                           hu_music_result_t *out) {
    if (!alloc || !query || query_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;

    char encoded[512];
    size_t enc_len = hu_music_url_encode_query(query, query_len, encoded, sizeof(encoded));
    if (enc_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    char url[768];
    int n = snprintf(url, sizeof(url),
                     "https://itunes.apple.com/search?term=%s&media=music&entity=song&limit=1",
                     encoded);
    if (n < 0 || (size_t)n >= sizeof(url))
        return HU_ERR_INVALID_ARGUMENT;

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url, NULL, &resp);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return err;
    }

    if (resp.status_code != 200 || !resp.body || resp.body_len == 0) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }

    err = hu_music_parse_search_response(alloc, resp.body, resp.body_len, out);
    hu_http_response_free(alloc, &resp);
    return err;
}

/* ── Spotify: client credentials token + search (with caching) ────────── */

static char s_spotify_token[512];
static time_t s_spotify_token_expiry;

static hu_error_t spotify_get_token(hu_allocator_t *alloc, const char *client_id,
                                    const char *client_secret, char *token_out, size_t token_cap) {
    /* Return cached token if still valid (with 60s safety margin) */
    if (s_spotify_token[0] && time(NULL) < s_spotify_token_expiry - 60) {
        size_t tl = strlen(s_spotify_token);
        if (tl < token_cap) {
            memcpy(token_out, s_spotify_token, tl + 1);
            return HU_OK;
        }
    }

    /* Build Basic auth: base64(client_id:client_secret) */
    size_t id_len = strlen(client_id);
    size_t sec_len = strlen(client_secret);
    size_t raw_len = id_len + 1 + sec_len;
    char raw[256];
    if (raw_len >= sizeof(raw))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(raw, client_id, id_len);
    raw[id_len] = ':';
    memcpy(raw + id_len + 1, client_secret, sec_len);
    raw[raw_len] = '\0';

    /* Simple base64 encode */
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char b64_buf[384];
    size_t bi = 0;
    for (size_t i = 0; i < raw_len && bi + 4 < sizeof(b64_buf); i += 3) {
        unsigned int v = (unsigned int)((unsigned char)raw[i]) << 16;
        if (i + 1 < raw_len)
            v |= (unsigned int)((unsigned char)raw[i + 1]) << 8;
        if (i + 2 < raw_len)
            v |= (unsigned int)((unsigned char)raw[i + 2]);
        b64_buf[bi++] = b64[(v >> 18) & 0x3f];
        b64_buf[bi++] = b64[(v >> 12) & 0x3f];
        b64_buf[bi++] = (i + 1 < raw_len) ? b64[(v >> 6) & 0x3f] : '=';
        b64_buf[bi++] = (i + 2 < raw_len) ? b64[v & 0x3f] : '=';
    }
    b64_buf[bi] = '\0';

    static const char body[] = "grant_type=client_credentials";

    char headers[768];
    snprintf(headers, sizeof(headers),
             "Content-Type: application/x-www-form-urlencoded\nAuthorization: Basic %s", b64_buf);

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_request(alloc, "https://accounts.spotify.com/api/token", "POST",
                                     headers, body, sizeof(body) - 1, &resp);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return err;
    }

    if (resp.status_code != 200 || !resp.body) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_PROVIDER_AUTH;
    }

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &root);
    hu_http_response_free(alloc, &resp);
    if (err != HU_OK)
        return err;

    const char *tok = hu_json_get_string(root, "access_token");
    if (!tok || strlen(tok) >= token_cap) {
        hu_json_free(alloc, root);
        return HU_ERR_PROVIDER_AUTH;
    }

    size_t tl = strlen(tok);
    memcpy(token_out, tok, tl + 1);

    /* Cache the token with its expiry */
    if (tl < sizeof(s_spotify_token)) {
        memcpy(s_spotify_token, tok, tl + 1);
        int expires_in = (int)hu_json_get_number(root, "expires_in", 3600.0);
        s_spotify_token_expiry = time(NULL) + expires_in;
    }

    hu_json_free(alloc, root);
    return HU_OK;
}

hu_error_t hu_music_search_spotify(hu_allocator_t *alloc, const char *client_id,
                                   const char *client_secret, const char *query, size_t query_len,
                                   hu_music_result_t *out) {
    if (!alloc || !client_id || !client_secret || !query || query_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;

    char token[512];
    hu_error_t err = spotify_get_token(alloc, client_id, client_secret, token, sizeof(token));
    if (err != HU_OK)
        return err;

    char encoded[512];
    size_t enc_len = hu_music_url_encode_query(query, query_len, encoded, sizeof(encoded));
    if (enc_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    char url[768];
    int n = snprintf(url, sizeof(url),
                     "https://api.spotify.com/v1/search?q=%s&type=track&limit=1", encoded);
    if (n < 0 || (size_t)n >= sizeof(url))
        return HU_ERR_INVALID_ARGUMENT;

    char auth[576];
    snprintf(auth, sizeof(auth), "Bearer %s", token);

    hu_http_response_t resp = {0};
    err = hu_http_get(alloc, url, auth, &resp);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return err;
    }

    if (resp.status_code != 200 || !resp.body || resp.body_len == 0) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }

    err = hu_music_parse_spotify_response(alloc, resp.body, resp.body_len, out);
    hu_http_response_free(alloc, &resp);
    return err;
}

/* ── File downloads (preview + artwork, reusable) ────────────────────── */

static hu_error_t download_to_temp(hu_allocator_t *alloc, const char *url, const char *suffix,
                                   char *path_out, size_t path_cap) {
    if (!alloc || !url || !path_out || path_cap < 64)
        return HU_ERR_INVALID_ARGUMENT;

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url, NULL, &resp);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return err;
    }

    if (resp.status_code != 200 || !resp.body || resp.body_len == 0) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }

    char *tmp_dir = hu_platform_get_temp_dir(alloc);
    if (!tmp_dir) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }

    int suffix_len = (int)strlen(suffix);
    char tmpl[256];
    int tn = snprintf(tmpl, sizeof(tmpl), "%s/hu_music_XXXXXX%s", tmp_dir, suffix);
    alloc->free(alloc->ctx, tmp_dir, strlen(tmp_dir) + 1);
    if (tn < 0 || (size_t)tn >= sizeof(tmpl)) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }

    int fd = mkstemps(tmpl, suffix_len);
    if (fd < 0) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }

    FILE *f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(tmpl);
        hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }

    size_t expected = resp.body_len;
    size_t written = fwrite(resp.body, 1, expected, f);
    fclose(f);
    hu_http_response_free(alloc, &resp);

    if (written != expected) {
        unlink(tmpl);
        return HU_ERR_IO;
    }

    size_t path_len = strlen(tmpl);
    if (path_len >= path_cap) {
        unlink(tmpl);
        return HU_ERR_INVALID_ARGUMENT;
    }

    memcpy(path_out, tmpl, path_len + 1);
    return HU_OK;
}

hu_error_t hu_music_download_preview(hu_allocator_t *alloc, const char *preview_url,
                                     char *path_out, size_t path_cap) {
    return download_to_temp(alloc, preview_url, ".m4a", path_out, path_cap);
}

hu_error_t hu_music_download_artwork(hu_allocator_t *alloc, const char *artwork_url,
                                     char *path_out, size_t path_cap) {
    return download_to_temp(alloc, artwork_url, ".jpg", path_out, path_cap);
}

#else /* HU_IS_TEST || !HU_HTTP_CURL */

hu_error_t hu_music_search(hu_allocator_t *alloc, const char *query, size_t query_len,
                           hu_music_result_t *out) {
    (void)alloc;
    (void)query;
    (void)query_len;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_music_search_spotify(hu_allocator_t *alloc, const char *client_id,
                                   const char *client_secret, const char *query, size_t query_len,
                                   hu_music_result_t *out) {
    (void)alloc;
    (void)client_id;
    (void)client_secret;
    (void)query;
    (void)query_len;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_music_download_preview(hu_allocator_t *alloc, const char *preview_url,
                                     char *path_out, size_t path_cap) {
    (void)alloc;
    (void)preview_url;
    (void)path_out;
    (void)path_cap;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_music_download_artwork(hu_allocator_t *alloc, const char *artwork_url,
                                     char *path_out, size_t path_cap) {
    (void)alloc;
    (void)artwork_url;
    (void)path_out;
    (void)path_cap;
    return HU_ERR_NOT_SUPPORTED;
}

#endif
