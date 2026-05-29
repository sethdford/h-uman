#ifndef HU_MUSIC_H
#define HU_MUSIC_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Music search, preview download, and preference detection
 *
 * iTunes Search API (no auth) provides song lookup + 30s DRM-free .m4a preview.
 * Spotify Web API (client credentials) provides song lookup + share links
 * (preview_url is null with client credentials — use iTunes for audio).
 *
 * The daemon combines both: iTunes .m4a preview for the audio attachment,
 * and the user's preferred service link (detected from conversation history).
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_music_source { HU_MUSIC_SOURCE_ITUNES, HU_MUSIC_SOURCE_SPOTIFY } hu_music_source_t;

typedef struct hu_music_result {
    char *track_name;     /* "Bohemian Rhapsody" */
    char *artist_name;    /* "Queen" */
    char *album_name;     /* "A Night at the Opera" */
    char *preview_url;    /* https://audio-ssl.itunes.apple.com/... (.m4a) */
    char *track_view_url; /* https://music.apple.com/... or https://open.spotify.com/... */
    char *artwork_url;    /* https://is1-ssl.mzstatic.com/... (100x100) */
    char *genre;          /* "Rock" */
    hu_music_source_t source;
} hu_music_result_t;

/* ── iTunes (no auth) ────────────────────────────────────────────────── */

hu_error_t hu_music_search(hu_allocator_t *alloc, const char *query, size_t query_len,
                           hu_music_result_t *out);

/* ── Spotify (client credentials) ────────────────────────────────────── */

/** Search Spotify for a song. Requires client_id and client_secret.
 *  Note: preview_url will be NULL (Spotify limitation with client credentials).
 *  track_view_url will be the Spotify share link. */
hu_error_t hu_music_search_spotify(hu_allocator_t *alloc, const char *client_id,
                                   const char *client_secret, const char *query, size_t query_len,
                                   hu_music_result_t *out);

/** Parse a Spotify search JSON response. Exposed for testing. */
hu_error_t hu_music_parse_spotify_response(hu_allocator_t *alloc, const char *json, size_t json_len,
                                           hu_music_result_t *out);

/* ── Downloads ───────────────────────────────────────────────────────── */

/** Download the 30s preview .m4a to a temp file. Caller must unlink(). */
hu_error_t hu_music_download_preview(hu_allocator_t *alloc, const char *preview_url, char *path_out,
                                     size_t path_cap);

/** Download album artwork to a temp file (.jpg). Caller must unlink(). */
hu_error_t hu_music_download_artwork(hu_allocator_t *alloc, const char *artwork_url, char *path_out,
                                     size_t path_cap);

/* ── Utilities ───────────────────────────────────────────────────────── */

void hu_music_result_free(hu_allocator_t *alloc, hu_music_result_t *result);

size_t hu_music_build_share_text(const hu_music_result_t *result, const char *casual_msg,
                                 size_t casual_msg_len, char *buf, size_t cap);

/** URL-encode a search query (spaces → +, non-ASCII → %XX). */
size_t hu_music_url_encode_query(const char *query, size_t query_len, char *out, size_t out_cap);

hu_error_t hu_music_parse_search_response(hu_allocator_t *alloc, const char *json, size_t json_len,
                                          hu_music_result_t *out);

bool hu_music_parse_suggestion(const char *suggestion, size_t suggestion_len, char *query_out,
                               size_t query_cap, char *msg_out, size_t msg_cap);

/** True if an iTunes/Spotify result plausibly matches the LLM-suggested
 *  "Artist - Title" string. Normalizes both sides (lowercase; strip
 *  punctuation, "feat."/"ft." segments, trailing parentheticals) then
 *  requires BOTH a shared artist token AND a shared title token. Defends
 *  against iTunes fuzzy-matching the wrong song from a hallucinated
 *  suggestion. Returns false on NULL/empty inputs. */
bool hu_music_result_matches(const char *suggested, const hu_music_result_t *result);

/* ── Preference detection ────────────────────────────────────────────── */

/** Scan conversation history for Spotify vs Apple Music URLs.
 *  Returns HU_MUSIC_SOURCE_SPOTIFY if user has shared more Spotify links,
 *  HU_MUSIC_SOURCE_ITUNES otherwise (default). */
hu_music_source_t hu_music_detect_preference(const char *const *texts, const size_t *lens,
                                             size_t count);

/* ── Taste learning ──────────────────────────────────────────────────── */

#define HU_MUSIC_TASTE_MAX_CONTACTS 32
#define HU_MUSIC_TASTE_HISTORY      16

/** Record that we sent a music share to this contact. */
void hu_music_taste_record_send(const char *contact_id, size_t cid_len, const char *artist,
                                const char *title);

/** Record a positive tapback on a music share from this contact. */
void hu_music_taste_record_reaction(const char *contact_id, size_t cid_len);

/** Get the positive reaction rate for music shares to this contact (0.0-1.0). */
float hu_music_taste_hit_rate(const char *contact_id, size_t cid_len);

/** Build a taste profile snippet for the LLM prompt.
 *  e.g. "They've reacted positively to: Radiohead, Bon Iver. Genres: Rock, Indie."
 *  Returns bytes written or 0 if no data. */
size_t hu_music_taste_build_prompt(const char *contact_id, size_t cid_len, char *buf, size_t cap);

/** Persist taste data to ~/.human/music_taste.json. */
hu_error_t hu_music_taste_save(const char *path, size_t path_len);

/** Load taste data from disk. */
hu_error_t hu_music_taste_load(const char *path, size_t path_len);

#endif
