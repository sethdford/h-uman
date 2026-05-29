#include "human/core/allocator.h"
#include "human/music.h"
#include "test_framework.h"
#include <string.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 1 — URL encoding (now with %XX for non-ASCII)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_music_url_encode_basic(void) {
    char buf[128];
    size_t n = hu_music_url_encode_query("Queen Bohemian Rhapsody", 23, buf, sizeof(buf));
    HU_ASSERT(n > 0);
    HU_ASSERT_STR_EQ(buf, "Queen+Bohemian+Rhapsody");
}

static void test_music_url_encode_special_chars(void) {
    char buf[128];
    size_t n = hu_music_url_encode_query("AC/DC Back in Black!", 20, buf, sizeof(buf));
    HU_ASSERT(n > 0);
    HU_ASSERT(strstr(buf, "AC%2FDC+Back+in+Black%21") != NULL);
}

static void test_music_url_encode_empty(void) {
    char buf[128];
    HU_ASSERT_EQ(hu_music_url_encode_query(NULL, 0, buf, sizeof(buf)), 0u);
    HU_ASSERT_EQ(hu_music_url_encode_query("", 0, buf, sizeof(buf)), 0u);
}

static void test_music_url_encode_small_buffer(void) {
    char buf[6];
    size_t n = hu_music_url_encode_query("Hello World", 11, buf, sizeof(buf));
    HU_ASSERT(n > 0);
    HU_ASSERT(n < sizeof(buf));
    HU_ASSERT(buf[n] == '\0');
}

static void test_music_url_encode_unicode(void) {
    /* "Björk" in UTF-8: 42 6a c3 b6 72 6b */
    char buf[128];
    size_t n = hu_music_url_encode_query("Bj\xc3\xb6rk", 6, buf, sizeof(buf));
    HU_ASSERT(n > 0);
    HU_ASSERT_STR_EQ(buf, "Bj%C3%B6rk");
}

static void test_music_url_encode_cjk(void) {
    /* CJK characters get percent-encoded as multi-byte sequences */
    char buf[128];
    size_t n = hu_music_url_encode_query("\xe4\xb8\xad", 3, buf, sizeof(buf));
    HU_ASSERT(n > 0);
    HU_ASSERT_STR_EQ(buf, "%E4%B8%AD");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 2 — iTunes JSON response parsing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_music_parse_full_response(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const char json[] =
        "{\"resultCount\":1,\"results\":[{"
        "\"trackName\":\"Bohemian Rhapsody\","
        "\"artistName\":\"Queen\","
        "\"collectionName\":\"A Night at the Opera\","
        "\"previewUrl\":\"https://audio-ssl.itunes.apple.com/preview.m4a\","
        "\"trackViewUrl\":\"https://music.apple.com/us/album/bohemian-rhapsody/1440650428\","
        "\"artworkUrl100\":\"https://is1-ssl.mzstatic.com/image/art100.jpg\","
        "\"primaryGenreName\":\"Rock\""
        "}]}";

    hu_music_result_t result = {0};
    hu_error_t err = hu_music_parse_search_response(&alloc, json, strlen(json), &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.source, (hu_music_source_t)HU_MUSIC_SOURCE_ITUNES);
    HU_ASSERT_STR_EQ(result.track_name, "Bohemian Rhapsody");
    HU_ASSERT_STR_EQ(result.artist_name, "Queen");
    HU_ASSERT_STR_EQ(result.album_name, "A Night at the Opera");
    HU_ASSERT_STR_EQ(result.preview_url, "https://audio-ssl.itunes.apple.com/preview.m4a");
    HU_ASSERT_STR_EQ(result.track_view_url,
                     "https://music.apple.com/us/album/bohemian-rhapsody/1440650428");
    HU_ASSERT_STR_EQ(result.genre, "Rock");

    hu_music_result_free(&alloc, &result);
}

static void test_music_parse_empty_results(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const char json[] = "{\"resultCount\":0,\"results\":[]}";
    hu_music_result_t result = {0};
    HU_ASSERT_EQ(hu_music_parse_search_response(&alloc, json, strlen(json), &result),
                 HU_ERR_NOT_FOUND);
}

static void test_music_parse_missing_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const char json[] =
        "{\"resultCount\":1,\"results\":[{\"trackName\":\"Imagine\",\"artistName\":\"John "
        "Lennon\"}]}";
    hu_music_result_t result = {0};
    HU_ASSERT_EQ(hu_music_parse_search_response(&alloc, json, strlen(json), &result), HU_OK);
    HU_ASSERT_STR_EQ(result.track_name, "Imagine");
    HU_ASSERT(result.preview_url == NULL);
    hu_music_result_free(&alloc, &result);
}

static void test_music_parse_invalid_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_music_result_t result = {0};
    HU_ASSERT(hu_music_parse_search_response(&alloc, "{broken", 7, &result) != HU_OK);
}

static void test_music_parse_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_music_result_t result = {0};
    HU_ASSERT_EQ(hu_music_parse_search_response(NULL, "{}", 2, &result), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_music_parse_search_response(&alloc, NULL, 0, &result), HU_ERR_INVALID_ARGUMENT);
}

static void test_music_parse_no_track_or_artist(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const char json[] =
        "{\"resultCount\":1,\"results\":[{\"collectionName\":\"Some Album\"}]}";
    hu_music_result_t result = {0};
    HU_ASSERT_EQ(hu_music_parse_search_response(&alloc, json, strlen(json), &result),
                 HU_ERR_NOT_FOUND);
}

static void test_music_parse_multiple_results_takes_first(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const char json[] = "{\"resultCount\":2,\"results\":["
                               "{\"trackName\":\"First\",\"artistName\":\"Artist1\"},"
                               "{\"trackName\":\"Second\",\"artistName\":\"Artist2\"}"
                               "]}";
    hu_music_result_t result = {0};
    HU_ASSERT_EQ(hu_music_parse_search_response(&alloc, json, strlen(json), &result), HU_OK);
    HU_ASSERT_STR_EQ(result.track_name, "First");
    hu_music_result_free(&alloc, &result);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 3 — Spotify JSON response parsing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_music_parse_spotify_full(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const char json[] =
        "{\"tracks\":{\"items\":[{"
        "\"name\":\"Creep\","
        "\"artists\":[{\"name\":\"Radiohead\"}],"
        "\"album\":{\"name\":\"Pablo Honey\",\"images\":[{\"url\":\"https://i.scdn.co/art.jpg\"}]},"
        "\"external_urls\":{\"spotify\":\"https://open.spotify.com/track/abc123\"},"
        "\"preview_url\":null"
        "}]}}";

    hu_music_result_t result = {0};
    hu_error_t err = hu_music_parse_spotify_response(&alloc, json, strlen(json), &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.source, (hu_music_source_t)HU_MUSIC_SOURCE_SPOTIFY);
    HU_ASSERT_STR_EQ(result.track_name, "Creep");
    HU_ASSERT_STR_EQ(result.artist_name, "Radiohead");
    HU_ASSERT_STR_EQ(result.album_name, "Pablo Honey");
    HU_ASSERT_STR_EQ(result.track_view_url, "https://open.spotify.com/track/abc123");
    HU_ASSERT_STR_EQ(result.artwork_url, "https://i.scdn.co/art.jpg");
    HU_ASSERT(result.preview_url == NULL);

    hu_music_result_free(&alloc, &result);
}

static void test_music_parse_spotify_empty_items(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const char json[] = "{\"tracks\":{\"items\":[]}}";
    hu_music_result_t result = {0};
    HU_ASSERT_EQ(hu_music_parse_spotify_response(&alloc, json, strlen(json), &result),
                 HU_ERR_NOT_FOUND);
}

static void test_music_parse_spotify_no_tracks(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const char json[] = "{\"other\":{}}";
    hu_music_result_t result = {0};
    HU_ASSERT_EQ(hu_music_parse_spotify_response(&alloc, json, strlen(json), &result),
                 HU_ERR_NOT_FOUND);
}

static void test_music_parse_spotify_null_args(void) {
    hu_music_result_t result = {0};
    HU_ASSERT_EQ(hu_music_parse_spotify_response(NULL, "{}", 2, &result), HU_ERR_INVALID_ARGUMENT);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 4 — Share text builder
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_music_share_text_with_message_and_url(void) {
    hu_music_result_t r = {.track_name = "Imagine",
                           .artist_name = "John Lennon",
                           .track_view_url = "https://music.apple.com/us/album/imagine/123"};
    char buf[256];
    const char *msg = "this one always hits different";
    size_t n = hu_music_build_share_text(&r, msg, strlen(msg), buf, sizeof(buf));
    HU_ASSERT(n > 0);
    HU_ASSERT(strstr(buf, "this one always hits different") != NULL);
    HU_ASSERT(strstr(buf, "music.apple.com") != NULL);
}

static void test_music_share_text_spotify_url(void) {
    hu_music_result_t r = {.track_name = "Creep",
                           .artist_name = "Radiohead",
                           .track_view_url = "https://open.spotify.com/track/abc123",
                           .source = HU_MUSIC_SOURCE_SPOTIFY};
    char buf[256];
    const char *msg = "haunting stuff";
    size_t n = hu_music_build_share_text(&r, msg, strlen(msg), buf, sizeof(buf));
    HU_ASSERT(n > 0);
    HU_ASSERT(strstr(buf, "open.spotify.com") != NULL);
}

static void test_music_share_text_no_url(void) {
    hu_music_result_t r = {.track_name = "Imagine", .artist_name = "John Lennon"};
    char buf[256];
    size_t n = hu_music_build_share_text(&r, NULL, 0, buf, sizeof(buf));
    HU_ASSERT(n > 0);
    HU_ASSERT(strstr(buf, "John Lennon") != NULL);
}

static void test_music_share_text_null_result(void) {
    char buf[256];
    HU_ASSERT_EQ(hu_music_build_share_text(NULL, NULL, 0, buf, sizeof(buf)), 0u);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 5 — LLM suggestion parser
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_music_parse_suggestion_standard(void) {
    const char *s = "Queen - Bohemian Rhapsody | this one always hits different";
    char query[128], msg[128];
    bool ok = hu_music_parse_suggestion(s, strlen(s), query, sizeof(query), msg, sizeof(msg));
    HU_ASSERT(ok);
    HU_ASSERT_STR_EQ(query, "Queen - Bohemian Rhapsody");
    HU_ASSERT_STR_EQ(msg, "this one always hits different");
}

static void test_music_parse_suggestion_no_pipe(void) {
    const char *s = "Queen - Bohemian Rhapsody";
    char query[128], msg[128];
    bool ok = hu_music_parse_suggestion(s, strlen(s), query, sizeof(query), msg, sizeof(msg));
    HU_ASSERT(ok);
    HU_ASSERT_STR_EQ(query, "Queen - Bohemian Rhapsody");
    HU_ASSERT_STR_EQ(msg, "");
}

static void test_music_parse_suggestion_empty(void) {
    char query[128], msg[128];
    HU_ASSERT(!hu_music_parse_suggestion(NULL, 0, query, sizeof(query), msg, sizeof(msg)));
    HU_ASSERT(!hu_music_parse_suggestion("", 0, query, sizeof(query), msg, sizeof(msg)));
}

static void test_music_parse_suggestion_trims_whitespace(void) {
    const char *s = "  Radiohead - Creep  | haunting stuff  ";
    char query[128], msg[128];
    bool ok = hu_music_parse_suggestion(s, strlen(s), query, sizeof(query), msg, sizeof(msg));
    HU_ASSERT(ok);
    HU_ASSERT_STR_EQ(query, "Radiohead - Creep");
    HU_ASSERT_STR_EQ(msg, "haunting stuff");
}

static void test_music_parse_suggestion_trailing_newline(void) {
    const char *s = "Radiohead - Creep | such a classic\n";
    char query[128], msg[128];
    bool ok = hu_music_parse_suggestion(s, strlen(s), query, sizeof(query), msg, sizeof(msg));
    HU_ASSERT(ok);
    HU_ASSERT_STR_EQ(query, "Radiohead - Creep");
    HU_ASSERT_STR_EQ(msg, "such a classic");
}

static void test_music_parse_suggestion_crlf_and_tabs(void) {
    const char *s = "\tQueen - Radio Ga Ga | banger \r\n";
    char query[128], msg[128];
    bool ok = hu_music_parse_suggestion(s, strlen(s), query, sizeof(query), msg, sizeof(msg));
    HU_ASSERT(ok);
    HU_ASSERT_STR_EQ(query, "Queen - Radio Ga Ga");
    HU_ASSERT_STR_EQ(msg, "banger");
}

static void test_music_parse_suggestion_only_whitespace(void) {
    const char *s = "  \n\t  ";
    char query[128], msg[128];
    HU_ASSERT(!hu_music_parse_suggestion(s, strlen(s), query, sizeof(query), msg, sizeof(msg)));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 6 — Preference detection
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_music_preference_default_itunes(void) {
    HU_ASSERT_EQ(hu_music_detect_preference(NULL, NULL, 0),
                 (hu_music_source_t)HU_MUSIC_SOURCE_ITUNES);
}

static void test_music_preference_spotify_wins(void) {
    const char *t[] = {
        "check this out https://open.spotify.com/track/abc",
        "nice",
        "https://open.spotify.com/track/def",
        "love it https://music.apple.com/us/album/xyz",
    };
    size_t l[] = {49, 4, 38, 45};
    HU_ASSERT_EQ(hu_music_detect_preference(t, l, 4), (hu_music_source_t)HU_MUSIC_SOURCE_SPOTIFY);
}

static void test_music_preference_apple_wins(void) {
    const char *t[] = {
        "https://music.apple.com/us/album/one",
        "https://music.apple.com/us/album/two",
        "https://open.spotify.com/track/abc",
    };
    size_t l[] = {36, 36, 34};
    HU_ASSERT_EQ(hu_music_detect_preference(t, l, 3), (hu_music_source_t)HU_MUSIC_SOURCE_ITUNES);
}

static void test_music_preference_no_urls(void) {
    const char *t[] = {"hey whats up", "not much"};
    size_t l[] = {12, 8};
    HU_ASSERT_EQ(hu_music_detect_preference(t, l, 2), (hu_music_source_t)HU_MUSIC_SOURCE_ITUNES);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 7 — Taste learning
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_music_taste_empty(void) {
    HU_ASSERT(hu_music_taste_hit_rate("nobody", 6) < 0.01f);

    char buf[256];
    HU_ASSERT_EQ(hu_music_taste_build_prompt("nobody", 6, buf, sizeof(buf)), 0u);
}

static void test_music_taste_record_and_rate(void) {
    hu_music_taste_record_send("user_a", 6, "Radiohead", "Creep");
    hu_music_taste_record_send("user_a", 6, "Bon Iver", "Skinny Love");
    hu_music_taste_record_reaction("user_a", 6);

    float rate = hu_music_taste_hit_rate("user_a", 6);
    HU_ASSERT(rate > 0.0f);
    HU_ASSERT(rate <= 1.0f);
}

static void test_music_taste_prompt_has_artists(void) {
    hu_music_taste_record_send("user_b", 6, "Queen", "Bohemian Rhapsody");
    hu_music_taste_record_send("user_b", 6, "Pink Floyd", "Wish You Were Here");
    hu_music_taste_record_reaction("user_b", 6);

    char buf[512];
    size_t n = hu_music_taste_build_prompt("user_b", 6, buf, sizeof(buf));
    HU_ASSERT(n > 0);
    HU_ASSERT(strstr(buf, "Pink Floyd") != NULL || strstr(buf, "Queen") != NULL);
}

static void test_music_taste_null_args(void) {
    hu_music_taste_record_send(NULL, 0, NULL, NULL);
    HU_ASSERT(hu_music_taste_hit_rate(NULL, 0) < 0.01f);

    char buf[64];
    HU_ASSERT_EQ(hu_music_taste_build_prompt(NULL, 0, buf, sizeof(buf)), 0u);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 7b — Taste persistence
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_music_taste_save_and_load(void) {
    hu_music_taste_record_send("+15551234567", 12, "Radiohead", "Creep");
    hu_music_taste_record_send("+15551234567", 12, "Nirvana", "Come As You Are");
    hu_music_taste_record_reaction("+15551234567", 12);

    const char *path = "/tmp/hu_test_music_taste.json";
    size_t path_len = strlen(path);
    HU_ASSERT_EQ(hu_music_taste_save(path, path_len), HU_OK);

    /* Load replaces in-memory state; verify artists roundtrip */
    HU_ASSERT_EQ(hu_music_taste_load(path, path_len), HU_OK);

    /* After load, the prompt should still contain the saved artists */
    char prompt[256];
    size_t pl = hu_music_taste_build_prompt("+15551234567", 12, prompt, sizeof(prompt));
    HU_ASSERT(pl > 0);
    HU_ASSERT(strstr(prompt, "Radiohead") != NULL || strstr(prompt, "Nirvana") != NULL);

    /* Hit rate should survive: 1 reaction / 2 sends = 0.5 */
    float rate = hu_music_taste_hit_rate("+15551234567", 12);
    HU_ASSERT(rate > 0.4f && rate < 0.6f);

    (void)unlink(path);
}

static void test_music_taste_save_escaped_json(void) {
    hu_music_taste_record_send("+15559876543", 12, "Guns N' Roses", "Sweet Child O' Mine");
    hu_music_taste_record_send("+15559876543", 12, "AC/DC", "Back In \"Black\"");

    const char *path = "/tmp/hu_test_taste_esc.json";
    size_t path_len = strlen(path);
    HU_ASSERT_EQ(hu_music_taste_save(path, path_len), HU_OK);
    HU_ASSERT_EQ(hu_music_taste_load(path, path_len), HU_OK);

    char prompt[256];
    size_t pl = hu_music_taste_build_prompt("+15559876543", 12, prompt, sizeof(prompt));
    HU_ASSERT(pl > 0);

    (void)unlink(path);
}

static void test_music_taste_save_null_args(void) {
    HU_ASSERT_EQ(hu_music_taste_save(NULL, 0), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_music_taste_save("", 0), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_music_taste_load(NULL, 0), HU_ERR_INVALID_ARGUMENT);
}

static void test_music_taste_load_missing_file(void) {
    HU_ASSERT_EQ(hu_music_taste_load("/tmp/hu_nonexistent_taste.json", 30), HU_ERR_NOT_FOUND);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 7 — Result matching
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_music_result_matches_exact_artist_and_title_passes(void) {
    hu_music_result_t r = {0};
    r.artist_name = "Queen";
    r.track_name = "Bohemian Rhapsody (Remastered 2011)";
    HU_ASSERT(hu_music_result_matches("Queen - Bohemian Rhapsody", &r));
}

static void test_music_result_matches_wrong_artist_rejected(void) {
    hu_music_result_t r = {0};
    r.artist_name = "Panic! at the Disco";
    r.track_name = "Bohemian Rhapsody";
    HU_ASSERT(!hu_music_result_matches("Queen - Bohemian Rhapsody", &r));
}

static void test_music_result_matches_wrong_title_rejected(void) {
    hu_music_result_t r = {0};
    r.artist_name = "Radiohead";
    r.track_name = "Creep";
    HU_ASSERT(!hu_music_result_matches("Radiohead - Karma Police", &r));
}

static void test_music_result_matches_feat_and_parenthetical_normalized(void) {
    hu_music_result_t r = {0};
    r.artist_name = "Calvin Harris";
    r.track_name = "Feel So Close (Radio Edit)";
    HU_ASSERT(hu_music_result_matches("Calvin Harris ft. Example - Feel So Close", &r));
}

static void test_music_result_matches_empty_inputs_rejected(void) {
    hu_music_result_t r = {0};
    r.artist_name = "Queen";
    r.track_name = "Bohemian Rhapsody";
    HU_ASSERT(!hu_music_result_matches("", &r));
    HU_ASSERT(!hu_music_result_matches("Queen - Bohemian Rhapsody", NULL));
    hu_music_result_t empty = {0};
    HU_ASSERT(!hu_music_result_matches("Queen - Bohemian Rhapsody", &empty));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 8 — Network stubs + cleanup
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_music_search_returns_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_music_result_t result = {0};
    HU_ASSERT_EQ(hu_music_search(&alloc, "test", 4, &result), HU_ERR_NOT_SUPPORTED);
}

static void test_music_search_spotify_returns_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_music_result_t result = {0};
    HU_ASSERT_EQ(hu_music_search_spotify(&alloc, "id", "secret", "test", 4, &result),
                 HU_ERR_NOT_SUPPORTED);
}

static void test_music_download_returns_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[256];
    HU_ASSERT_EQ(
        hu_music_download_preview(&alloc, "https://example.com/preview.m4a", path, sizeof(path)),
        HU_ERR_NOT_SUPPORTED);
}

static void test_music_download_artwork_returns_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[256];
    HU_ASSERT_EQ(
        hu_music_download_artwork(&alloc, "https://example.com/art.jpg", path, sizeof(path)),
        HU_ERR_NOT_SUPPORTED);
}

static void test_music_result_free_zeroed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_music_result_t result = {0};
    hu_music_result_free(&alloc, &result);
    hu_music_result_free(NULL, &result);
    hu_music_result_free(&alloc, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Suite registration
 * ═══════════════════════════════════════════════════════════════════════════ */

void run_music_tests(void) {
    HU_TEST_SUITE("Music");

    /* URL encoding */
    HU_RUN_TEST(test_music_url_encode_basic);
    HU_RUN_TEST(test_music_url_encode_special_chars);
    HU_RUN_TEST(test_music_url_encode_empty);
    HU_RUN_TEST(test_music_url_encode_small_buffer);
    HU_RUN_TEST(test_music_url_encode_unicode);
    HU_RUN_TEST(test_music_url_encode_cjk);

    /* iTunes parsing */
    HU_RUN_TEST(test_music_parse_full_response);
    HU_RUN_TEST(test_music_parse_empty_results);
    HU_RUN_TEST(test_music_parse_missing_fields);
    HU_RUN_TEST(test_music_parse_invalid_json);
    HU_RUN_TEST(test_music_parse_null_args);
    HU_RUN_TEST(test_music_parse_no_track_or_artist);
    HU_RUN_TEST(test_music_parse_multiple_results_takes_first);

    /* Spotify parsing */
    HU_RUN_TEST(test_music_parse_spotify_full);
    HU_RUN_TEST(test_music_parse_spotify_empty_items);
    HU_RUN_TEST(test_music_parse_spotify_no_tracks);
    HU_RUN_TEST(test_music_parse_spotify_null_args);

    /* Share text */
    HU_RUN_TEST(test_music_share_text_with_message_and_url);
    HU_RUN_TEST(test_music_share_text_spotify_url);
    HU_RUN_TEST(test_music_share_text_no_url);
    HU_RUN_TEST(test_music_share_text_null_result);

    /* Suggestion parser */
    HU_RUN_TEST(test_music_parse_suggestion_standard);
    HU_RUN_TEST(test_music_parse_suggestion_no_pipe);
    HU_RUN_TEST(test_music_parse_suggestion_empty);
    HU_RUN_TEST(test_music_parse_suggestion_trims_whitespace);
    HU_RUN_TEST(test_music_parse_suggestion_trailing_newline);
    HU_RUN_TEST(test_music_parse_suggestion_crlf_and_tabs);
    HU_RUN_TEST(test_music_parse_suggestion_only_whitespace);

    /* Preference detection */
    HU_RUN_TEST(test_music_preference_default_itunes);
    HU_RUN_TEST(test_music_preference_spotify_wins);
    HU_RUN_TEST(test_music_preference_apple_wins);
    HU_RUN_TEST(test_music_preference_no_urls);

    /* Taste learning */
    HU_RUN_TEST(test_music_taste_empty);
    HU_RUN_TEST(test_music_taste_record_and_rate);
    HU_RUN_TEST(test_music_taste_prompt_has_artists);
    HU_RUN_TEST(test_music_taste_null_args);

    /* Taste persistence */
    HU_RUN_TEST(test_music_taste_save_and_load);
    HU_RUN_TEST(test_music_taste_save_escaped_json);
    HU_RUN_TEST(test_music_taste_save_null_args);
    HU_RUN_TEST(test_music_taste_load_missing_file);

    /* Result matching */
    HU_RUN_TEST(test_music_result_matches_exact_artist_and_title_passes);
    HU_RUN_TEST(test_music_result_matches_wrong_artist_rejected);
    HU_RUN_TEST(test_music_result_matches_wrong_title_rejected);
    HU_RUN_TEST(test_music_result_matches_feat_and_parenthetical_normalized);
    HU_RUN_TEST(test_music_result_matches_empty_inputs_rejected);

    /* Network stubs + cleanup */
    HU_RUN_TEST(test_music_search_returns_not_supported);
    HU_RUN_TEST(test_music_search_spotify_returns_not_supported);
    HU_RUN_TEST(test_music_download_returns_not_supported);
    HU_RUN_TEST(test_music_download_artwork_returns_not_supported);
    HU_RUN_TEST(test_music_result_free_zeroed);
}
