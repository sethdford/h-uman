---
title: Valid, Human Media Inspirations — Implementation Plan
date: 2026-05-29
status: active
---

# Valid, Human Media Inspirations — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make proactive media inspirations (music / YouTube / TikTok) send only *verified-real* links, each framed by a persona-voiced human line in its own bubble so the platform rich-preview still renders.

**Architecture:** One per-turn "share an inspiration?" roll → pick medium from conversation cues → resolve the LLM's *search intent* against a real source (iTunes/Spotify, YouTube Data API, TikTok tag URL) → verify the result → send as two bubbles (human line, then bare link) on unfurling channels. The LLM never produces an identifier we send; it only produces a search intent. Verify-failure → silent skip.

**Tech Stack:** C11, libcurl HTTP (`hu_http_get`), cJSON (`hu_json_*`), existing `hu_music_*` + channel vtable. New modules `src/youtube.c`, `src/inspiration.c`. No new build flag; no config schema change (YouTube key via existing generic provider-key map).

**Spec:** [docs/superpowers/specs/2026-05-29-media-inspiration-share-design.md](../specs/2026-05-29-media-inspiration-share-design.md)

---

## Grounded facts (verified against the tree, 2026-05-29)

- HTTP: `hu_error_t hu_http_get(hu_allocator_t*, const char *url, const char *auth_or_NULL, hu_http_response_t*)`; response fields `body`, `body_len`, `status_code`, `owned`; free with `hu_http_response_free(alloc, &resp)`. Network code guarded `#if !defined(HU_IS_TEST) && defined(HU_HTTP_CURL)` (see `src/music.c:618`).
- JSON: `hu_json_*` from `human/core/json.h` (e.g. `hu_json_get_string`), used in `src/music.c`.
- Word-boundary CI match: `bool hu_str_contains_word_ci_n(const char *hay, size_t hlen, const char *needle)` (`src/core/string.c:190`, header `human/core/string.h`).
- Channel send: `vtable->send(ctx, target, target_len, message, message_len, media, media_count)`; `bool hu_channel_supports_link_unfurl(const hu_channel_t*)` inline in `include/human/channel.h:194`.
- URL guard: `hu_error_t hu_tool_validate_url(const char *url)` (`include/human/tools/validation.h:61`).
- Music result struct + `hu_music_search` + `hu_music_parse_suggestion(suggestion,len,query_out,query_cap,msg_out,msg_cap)` + `hu_music_url_encode_query` in `include/human/music.h`.
- Existing music share block to replace: `src/daemon.c:13663-13938`. Decision fn `hu_conversation_should_send_music` + `hu_conversation_build_music_prompt` at `src/context/conversation.c:7814,7870`.
- Persona: `agent->persona->humanization.gif_probability` (already used in daemon); formality string lives on the humanization/overlay config (`char *formality;`). Top-level `hu_persona_t` has `char *name`, `char **traits` + `traits_count`.
- Test harness: `tests/test_*.c` with `run_<name>_tests(void)` registered in `tests/test_main.c`; suites via `HU_TEST_SUITE` / `HU_RUN_TEST`. New source+test pairs must respect `.claude/rules/test-source-gate-symmetry.md` (no new `HU_ENABLE_*` flag here → register unconditionally).

---

# SLICE 1 — Validity predicate (fixes "wrong song" on the live path)

### Task 1: `hu_music_result_matches` predicate

**Files:**
- Modify: `include/human/music.h` (add prototype after `hu_music_parse_suggestion`, ~line 78)
- Modify: `src/music.c` (add near other pure helpers, above the HTTP `#if` block)
- Test: `tests/test_music.c` (add suite cases + register)

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_music.c`:

```c
static void music_match_exact_artist_and_title_passes(void) {
    hu_music_result_t r = {0};
    r.artist_name = "Queen";
    r.track_name = "Bohemian Rhapsody (Remastered 2011)";
    HU_ASSERT_TRUE(hu_music_result_matches("Queen - Bohemian Rhapsody", &r));
}

static void music_match_wrong_artist_rejected(void) {
    /* iTunes fuzzy-matched a cover by a different artist → must reject */
    hu_music_result_t r = {0};
    r.artist_name = "Panic! at the Disco";
    r.track_name = "Bohemian Rhapsody";
    HU_ASSERT_FALSE(hu_music_result_matches("Queen - Bohemian Rhapsody", &r));
}

static void music_match_wrong_title_rejected(void) {
    hu_music_result_t r = {0};
    r.artist_name = "Radiohead";
    r.track_name = "Creep";
    HU_ASSERT_FALSE(hu_music_result_matches("Radiohead - Karma Police", &r));
}

static void music_match_feat_and_parenthetical_normalized(void) {
    hu_music_result_t r = {0};
    r.artist_name = "Calvin Harris";
    r.track_name = "Feel So Close (Radio Edit)";
    HU_ASSERT_TRUE(hu_music_result_matches("Calvin Harris ft. Example - Feel So Close", &r));
}

static void music_match_empty_inputs_rejected(void) {
    hu_music_result_t r = {0};
    r.artist_name = "Queen";
    r.track_name = "Bohemian Rhapsody";
    HU_ASSERT_FALSE(hu_music_result_matches("", &r));
    HU_ASSERT_FALSE(hu_music_result_matches("Queen - Bohemian Rhapsody", NULL));
    hu_music_result_t empty = {0};
    HU_ASSERT_FALSE(hu_music_result_matches("Queen - Bohemian Rhapsody", &empty));
}
```

Register them in `run_music_tests` (the function near the bottom of `tests/test_music.c`):

```c
    HU_RUN_TEST(music_match_exact_artist_and_title_passes);
    HU_RUN_TEST(music_match_wrong_artist_rejected);
    HU_RUN_TEST(music_match_wrong_title_rejected);
    HU_RUN_TEST(music_match_feat_and_parenthetical_normalized);
    HU_RUN_TEST(music_match_empty_inputs_rejected);
```

- [ ] **Step 2: Run to verify failure**

Run: `touch tests/test_music.c && cmake --build build --target human_tests -j8`
Expected: compile error — `hu_music_result_matches` undeclared.

- [ ] **Step 3: Add the prototype**

In `include/human/music.h`, after the `hu_music_parse_suggestion` declaration:

```c
/** True if an iTunes/Spotify result plausibly matches the LLM-suggested
 *  "Artist - Title" string. Normalizes both sides (lowercase; strip
 *  punctuation, "feat."/"ft." segments, trailing parentheticals) then
 *  requires BOTH a shared artist token AND a shared title token. Defends
 *  against iTunes fuzzy-matching the wrong song from a hallucinated
 *  suggestion. Returns false on NULL/empty inputs. */
bool hu_music_result_matches(const char *suggested, const hu_music_result_t *result);
```

- [ ] **Step 4: Implement in `src/music.c`** (place above the `#if !defined(HU_IS_TEST)` block so it is always compiled)

```c
/* Lowercase-copy `src` into `dst`, dropping anything from the first '('
 * and any "feat"/"ft" segment, mapping non-alnum to spaces. */
static void music_norm(const char *src, char *dst, size_t cap) {
    size_t j = 0;
    if (!src || cap == 0) { if (cap) dst[0] = '\0'; return; }
    for (size_t i = 0; src[i] && j + 1 < cap; i++) {
        char c = src[i];
        if (c == '(' || c == '[') break;                 /* drop "(Remastered)" etc. */
        if ((c == 'f' || c == 'F') &&
            (strncasecmp(src + i, "feat", 4) == 0 || strncasecmp(src + i, "ft ", 3) == 0 ||
             strncasecmp(src + i, "ft.", 3) == 0))
            break;                                        /* drop featured-artist tail */
        unsigned char u = (unsigned char)c;
        dst[j++] = (char)(isalnum(u) ? tolower(u) : ' ');
    }
    dst[j] = '\0';
}

/* True if any whitespace-delimited token of len>=2 in `a` also appears in `b`. */
static bool music_share_token(const char *a, const char *b) {
    char tmp[256];
    size_t n = strlen(a);
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, a, n);
    tmp[n] = '\0';
    char *save = NULL;
    for (char *t = strtok_r(tmp, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
        if (strlen(t) < 2) continue;
        /* word-boundary-safe contains: surround b lookups with spaces */
        char needle[128];
        int m = snprintf(needle, sizeof(needle), " %s ", t);
        if (m <= 0 || (size_t)m >= sizeof(needle)) continue;
        char padded[512];
        int p = snprintf(padded, sizeof(padded), " %s ", b);
        if (p <= 0 || (size_t)p >= sizeof(padded)) continue;
        if (strstr(padded, needle)) return true;
    }
    return false;
}

bool hu_music_result_matches(const char *suggested, const hu_music_result_t *result) {
    if (!suggested || !*suggested || !result || !result->artist_name || !result->track_name)
        return false;
    const char *dash = strstr(suggested, " - ");
    if (!dash) return false;                              /* need "Artist - Title" shape */

    char sug_artist[256], sug_title[256];
    size_t alen = (size_t)(dash - suggested);
    if (alen >= sizeof(sug_artist)) alen = sizeof(sug_artist) - 1;
    memcpy(sug_artist, suggested, alen);
    sug_artist[alen] = '\0';
    snprintf(sug_title, sizeof(sug_title), "%s", dash + 3);

    char na_sug[256], nt_sug[256], na_res[256], nt_res[256];
    music_norm(sug_artist, na_sug, sizeof(na_sug));
    music_norm(sug_title, nt_sug, sizeof(nt_sug));
    music_norm(result->artist_name, na_res, sizeof(na_res));
    music_norm(result->track_name, nt_res, sizeof(nt_res));

    return music_share_token(na_sug, na_res) && music_share_token(nt_sug, nt_res);
}
```

Add `#include <ctype.h>` and `#include <string.h>` at the top of `src/music.c` if not already present (string.h is; ctype.h: check and add).

- [ ] **Step 5: Run tests to verify pass**

Run: `touch src/music.c tests/test_music.c && cmake --build build --target human_tests -j8 && ./build/human_tests --filter=music_match`
Expected: all 5 `music_match_*` PASS, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add include/human/music.h src/music.c tests/test_music.c
git commit -m "feat(music): verify search result matches LLM suggestion before sending"
```

### Task 2: Gate the live music share on the predicate

**Files:**
- Modify: `src/daemon.c:13789` (the `if (search_err == HU_OK && (song.track_view_url || ...))` condition)

- [ ] **Step 1: Add the match gate**

At `src/daemon.c:13789`, change the guard so a verified match is required. Replace:

```c
                                    if (search_err == HU_OK &&
                                        (song.track_view_url ||
                                         (has_spotify && spotify_song.track_view_url))) {
```

with:

```c
                                    bool song_verified =
                                        hu_music_result_matches(search_query, &song) ||
                                        (has_spotify &&
                                         hu_music_result_matches(search_query, &spotify_song));
                                    if (search_err == HU_OK && song_verified &&
                                        (song.track_view_url ||
                                         (has_spotify && spotify_song.track_view_url))) {
```

And in the `else` branch at `src/daemon.c:13925` keep the existing silent-skip log; add the verify case:

```c
                                    } else {
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "music share skipped (no verified match) for: %s",
                                                    search_query);
                                    }
```

- [ ] **Step 2: Rebuild production binary + full suite**

Run: `touch src/daemon.c && cmake --build build --target human human_tests -j8`
Expected: `Linking C executable human` + `Signing human binary` appear (per `.claude/rules/cmake-build-stale-binary.md`).
Run: `./build/human_tests 2>/dev/null | grep -E 'Results:'`
Expected: `N/N passed`, 0 failures, 0 ASan errors.

- [ ] **Step 3: Commit**

```bash
git add src/daemon.c
git commit -m "fix(daemon): gate music share on verified result match (no wrong songs)"
```

---

# SLICE 2 — Two-bubble human framing (fixes the robotic feel)

### Task 3: Persona voice-hint helper (`src/inspiration.c` bootstrap)

**Files:**
- Create: `include/human/inspiration.h`
- Create: `src/inspiration.c`
- Test: `tests/test_inspiration.c`
- Modify: `CMakeLists.txt` (register `src/inspiration.c`, `tests/test_inspiration.c`)
- Modify: `tests/test_main.c` (declare + call `run_inspiration_tests`)

- [ ] **Step 1: Create the header**

`include/human/inspiration.h`:

```c
#ifndef HU_INSPIRATION_H
#define HU_INSPIRATION_H

#include <stdbool.h>
#include <stddef.h>

/* Which kind of media inspiration to share this turn. */
typedef enum hu_inspiration_medium {
    HU_INSPIRATION_NONE,
    HU_INSPIRATION_MUSIC,
    HU_INSPIRATION_YOUTUBE,
    HU_INSPIRATION_TIKTOK
} hu_inspiration_medium_t;

/* Pick the medium from conversation cues (word-boundary CI match).
 * youtube cue with youtube_available=false falls back to music.
 * Always returns a concrete medium (defaults to MUSIC). */
hu_inspiration_medium_t hu_inspiration_pick_medium(const char *incoming, size_t incoming_len,
                                                   bool youtube_available);

/* Static system-prompt instruction for the generation call, per medium.
 * Each asks for "<search intent> | <persona-voiced human line>". */
const char *hu_inspiration_system_prompt(hu_inspiration_medium_t medium);

/* Build a persona voice hint to append to the generation prompt.
 * Either arg may be NULL/empty; returns bytes written (0 if nothing to say). */
size_t hu_inspiration_build_voice_hint(const char *formality, const char *traits_csv,
                                        char *out, size_t cap);

/* Build a guaranteed-valid TikTok discovery URL: https://www.tiktok.com/tag/<kw>
 * Strips a leading '#', URL-encodes, collapses whitespace. Returns bytes
 * written or 0 on bad input/overflow. */
size_t hu_tiktok_tag_url(const char *keyword, size_t keyword_len, char *out, size_t cap);

#endif
```

- [ ] **Step 2: Write failing tests**

`tests/test_inspiration.c`:

```c
#include "human/inspiration.h"
#include "test_framework.h"
#include <string.h>

static void voice_hint_includes_formality_and_traits(void) {
    char buf[256];
    size_t n = hu_inspiration_build_voice_hint("casual", "dry humor,tech-nerd",
                                               buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "casual") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "dry humor") != NULL);
}

static void voice_hint_neutral_when_absent(void) {
    char buf[256];
    size_t n = hu_inspiration_build_voice_hint(NULL, NULL, buf, sizeof(buf));
    HU_ASSERT_EQ((int)n, 0);
}

static void system_prompt_differs_per_medium(void) {
    const char *m = hu_inspiration_system_prompt(HU_INSPIRATION_MUSIC);
    const char *y = hu_inspiration_system_prompt(HU_INSPIRATION_YOUTUBE);
    const char *t = hu_inspiration_system_prompt(HU_INSPIRATION_TIKTOK);
    HU_ASSERT_TRUE(m && y && t);
    HU_ASSERT_TRUE(strcmp(m, y) != 0 && strcmp(y, t) != 0);
}

void run_inspiration_tests(void) {
    HU_TEST_SUITE("inspiration");
    HU_RUN_TEST(voice_hint_includes_formality_and_traits);
    HU_RUN_TEST(voice_hint_neutral_when_absent);
    HU_RUN_TEST(system_prompt_differs_per_medium);
}
```

- [ ] **Step 3: Register in build + runner**

In `CMakeLists.txt`, find where `src/music.c` is listed in the core sources and `tests/test_music.c` in the test sources; add alongside:

```cmake
        src/inspiration.c
```
```cmake
        tests/test_inspiration.c
```

In `tests/test_main.c`, add a forward declaration near the others and a call in `main`:

```c
void run_inspiration_tests(void);
```
```c
    run_inspiration_tests();
```

- [ ] **Step 4: Run to verify failure**

Run: `cmake --build build --target human_tests -j8`
Expected: link error — `hu_inspiration_*` undefined (header exists, no impl yet).

- [ ] **Step 5: Implement `src/inspiration.c`**

```c
#include "human/inspiration.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

hu_inspiration_medium_t hu_inspiration_pick_medium(const char *incoming, size_t incoming_len,
                                                   bool youtube_available) {
    if (!incoming || incoming_len == 0) return HU_INSPIRATION_MUSIC;

    static const char *tiktok_cues[] = {"tiktok", "fyp", "for you", "trend", "trending", "reel"};
    for (size_t i = 0; i < sizeof(tiktok_cues) / sizeof(tiktok_cues[0]); i++)
        if (hu_str_contains_word_ci_n(incoming, incoming_len, tiktok_cues[i]))
            return HU_INSPIRATION_TIKTOK;

    static const char *yt_cues[] = {"video", "watch", "clip", "youtube", "tutorial", "trailer", "funny"};
    for (size_t i = 0; i < sizeof(yt_cues) / sizeof(yt_cues[0]); i++)
        if (hu_str_contains_word_ci_n(incoming, incoming_len, yt_cues[i]))
            return youtube_available ? HU_INSPIRATION_YOUTUBE : HU_INSPIRATION_MUSIC;

    return HU_INSPIRATION_MUSIC;  /* music is the default + catch-all */
}

const char *hu_inspiration_system_prompt(hu_inspiration_medium_t medium) {
    switch (medium) {
    case HU_INSPIRATION_YOUTUBE:
        return "Suggest ONE YouTube search that fits the conversation. Return exactly:\n"
               "SEARCH QUERY | your brief casual message\n"
               "The message is a natural text in the user's own voice — not a recommendation. "
               "Under 80 chars. No quotes, no URLs.";
    case HU_INSPIRATION_TIKTOK:
        return "Suggest ONE TikTok hashtag keyword that fits the conversation. Return exactly:\n"
               "HASHTAG KEYWORD | your brief casual message\n"
               "Keyword is 1-2 words, no '#'. Message is a natural text in the user's own "
               "voice. Under 80 chars. No quotes, no URLs.";
    case HU_INSPIRATION_MUSIC:
    default:
        return "Suggest ONE song that fits the conversation mood. Return exactly:\n"
               "ARTIST - TITLE | your brief casual message\n"
               "The message is a natural text in the user's own voice — not a recommendation. "
               "Under 80 chars. No quotes, no URLs.";
    }
}

size_t hu_inspiration_build_voice_hint(const char *formality, const char *traits_csv,
                                       char *out, size_t cap) {
    if (!out || cap < 16) return 0;
    bool has_f = formality && *formality;
    bool has_t = traits_csv && *traits_csv;
    if (!has_f && !has_t) { out[0] = '\0'; return 0; }
    int n = snprintf(out, cap, "Write the message in this voice: %s%s%s.",
                     has_f ? formality : "",
                     (has_f && has_t) ? ", " : "",
                     has_t ? traits_csv : "");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

size_t hu_tiktok_tag_url(const char *keyword, size_t keyword_len, char *out, size_t cap) {
    if (!keyword || keyword_len == 0 || !out || cap < 32) return 0;
    /* skip a leading '#' and leading spaces */
    size_t i = 0;
    while (i < keyword_len && (keyword[i] == '#' || keyword[i] == ' ')) i++;

    char enc[128];
    size_t e = 0;
    for (; i < keyword_len && e + 4 < sizeof(enc); i++) {
        unsigned char c = (unsigned char)keyword[i];
        if (isalnum(c)) {
            enc[e++] = (char)tolower(c);
        } else if (c == ' ' || c == '-' || c == '_') {
            continue; /* tags are single tokens; drop separators */
        } else {
            int m = snprintf(enc + e, sizeof(enc) - e, "%%%02X", c);
            if (m > 0) e += (size_t)m;
        }
    }
    enc[e] = '\0';
    if (e == 0) return 0;
    int n = snprintf(out, cap, "https://www.tiktok.com/tag/%s", enc);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}
```

- [ ] **Step 6: Add TikTok URL tests**

Append to `tests/test_inspiration.c` (and register each):

```c
static void tiktok_tag_url_basic(void) {
    char buf[128];
    size_t n = hu_tiktok_tag_url("#latteart", 9, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_EQ(buf, "https://www.tiktok.com/tag/latteart");
}

static void tiktok_tag_url_multiword_collapses(void) {
    char buf[128];
    size_t n = hu_tiktok_tag_url("latte art", 9, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_EQ(buf, "https://www.tiktok.com/tag/latteart");
}

static void pick_medium_routes_and_falls_back(void) {
    HU_ASSERT_EQ((int)hu_inspiration_pick_medium("send me a tiktok", 16, true),
                 (int)HU_INSPIRATION_TIKTOK);
    HU_ASSERT_EQ((int)hu_inspiration_pick_medium("got a funny video?", 18, true),
                 (int)HU_INSPIRATION_YOUTUBE);
    HU_ASSERT_EQ((int)hu_inspiration_pick_medium("got a funny video?", 18, false),
                 (int)HU_INSPIRATION_MUSIC);   /* no key → fall back */
    HU_ASSERT_EQ((int)hu_inspiration_pick_medium("what a day", 10, true),
                 (int)HU_INSPIRATION_MUSIC);   /* default */
}
```

(Use `HU_ASSERT_STR_EQ` if the framework provides it; otherwise `HU_ASSERT_TRUE(strcmp(...) == 0)`.)

- [ ] **Step 7: Run to verify pass**

Run: `cmake --build build --target human_tests -j8 && ./build/human_tests --filter=inspiration`
Expected: all inspiration tests PASS.

- [ ] **Step 8: Verify gate symmetry + commit**

Run: `bash scripts/check-test-source-gate-symmetry.sh`
Expected: exit 0 (source+test both unconditional).

```bash
git add include/human/inspiration.h src/inspiration.c tests/test_inspiration.c CMakeLists.txt tests/test_main.c
git commit -m "feat(inspiration): medium picker, per-medium prompts, voice hint, tiktok tag url"
```

### Task 4: Two-bubble send helper + wire into the music path

**Files:**
- Modify: `src/daemon.c` (rich-link branch at `:13806-13842`)

This task changes ONLY the rich-link (unfurling) branch. The legacy SMS branch (`:13843-13924`) is unchanged — inline caption is correct there.

- [ ] **Step 1: Build the persona voice hint and persona-shaped prompt**

At `src/daemon.c:13685` (where `music_prompt` is built), after appending the taste snippet, append the voice hint. First confirm the persona formality field path:

Run: `rg -n 'formality' include/human/persona.h | head`
Use the confirmed path (expected `agent->persona->humanization.formality`). Insert before the `chat_with_system` call:

```c
                        /* Persona voice hint → human line sounds like the user */
                        if (agent && agent->persona) {
                            char vh[256];
                            const char *form = agent->persona->humanization.formality;
                            const char *trait = (agent->persona->traits_count > 0)
                                                    ? agent->persona->traits[0] : NULL;
                            size_t vlen = hu_inspiration_build_voice_hint(form, trait, vh, sizeof(vh));
                            if (vlen > 0 && mp_len + vlen + 2 < sizeof(music_prompt)) {
                                music_prompt[mp_len++] = '\n';
                                memcpy(music_prompt + mp_len, vh, vlen);
                                mp_len += vlen;
                                music_prompt[mp_len] = '\0';
                            }
                        }
```

Add `#include "human/inspiration.h"` to the daemon includes.

- [ ] **Step 2: Replace the rich-link send (the bug) with a two-bubble send**

At `src/daemon.c:13806-13842`, the `if (rich_link) { ... }` block currently sends only `url`. Replace its body with: send the human line first (if present + non-empty), short gap, then the bare URL. Validate the URL first.

```c
                                        if (rich_link) {
                                            const char *url = link_song->track_view_url;
                                            size_t url_len = strlen(url);

                                            if (hu_tool_validate_url(url) != HU_OK) {
                                                hu_log_info("human", agent ? agent->observer : NULL,
                                                            "music rich-link rejected by url validation: %s",
                                                            url);
                                            } else {
                                                usleep(3000000 + (music_seed % 4000000));

                                                /* Bubble 1: the persona-voiced human line.
                                                 * No URL inline — that would kill the unfurl. */
                                                size_t casual_len = strlen(casual_msg);
                                                if (casual_len > 0) {
                                                    ch->channel->vtable->send(
                                                        ch->channel->ctx, batch_key, key_len,
                                                        casual_msg, casual_len, NULL, 0);
                                                    /* human-pacing gap between the two bubbles */
                                                    usleep(1500000 + (music_seed % 1500000));
                                                }

                                                /* Bubble 2: bare URL → platform renders the card.
                                                 * INVARIANT: body is exactly the URL bytes. */
                                                ch->channel->vtable->send(
                                                    ch->channel->ctx, batch_key, key_len,
                                                    url, url_len, NULL, 0);

                                                hu_log_info("human", agent ? agent->observer : NULL,
                                                            "sent music rich-link: %s - %s [%s]",
                                                            song.artist_name ? song.artist_name : "?",
                                                            song.track_name ? song.track_name : "?",
                                                            has_spotify ? "spotify" : "itunes");

                                                hu_music_taste_record_send(batch_key, key_len,
                                                                           song.artist_name,
                                                                           song.track_name);
                                                /* (keep the existing taste-save throttle block here) */
                                            }
                                        } else {
```

Preserve the existing taste-save throttle block (`static uint64_t last_taste_save_ms; ...`) inside the new `else` (validated) path.

- [ ] **Step 3: Rebuild production binary + full suite**

Run: `touch src/daemon.c && cmake --build build --target human human_tests -j8`
Expected: `Linking C executable human` + `Signing human binary`.
Run: `./build/human_tests 2>/dev/null | grep -E 'Results:'`
Expected: `N/N passed`, 0 failures.

- [ ] **Step 4: Commit**

```bash
git add src/daemon.c
git commit -m "feat(daemon): two-bubble music share (human line + bare link) with url validation"
```

### Task 5: Two-bubble contract test

**Files:**
- Modify: `tests/test_imessage_rich_link.c` (add a case asserting ordering: human line bubble THEN url bubble, url body == url bytes)

- [ ] **Step 1: Read the existing test to learn its mock-channel capture pattern**

Run: `sed -n '1,80p' tests/test_imessage_rich_link.c`
Identify how it captures sent messages (a mock `send` recording into an array). Reuse that harness.

- [ ] **Step 2: Add the ordering test** (adapt names to the file's existing mock)

```c
static void rich_link_sends_human_line_then_bare_url(void) {
    /* Arrange the mock channel + a verified song with track_view_url and a
     * non-empty casual_msg, drive the share path, then assert: */
    /* 1. exactly two sends were captured */
    /* 2. send[0].body is the human line (no "http") */
    /* 3. send[1].body equals the URL bytes exactly (no preamble/whitespace) */
    HU_ASSERT_EQ(mock_send_count, 2);
    HU_ASSERT_TRUE(strstr(mock_sends[0].body, "http") == NULL);
    HU_ASSERT_STR_EQ(mock_sends[1].body, expected_url);
}
```

If the share path isn't directly callable from this test (it's inline in the daemon turn loop), instead assert the **helper-level invariant** the daemon relies on: the URL bubble body equals the validated URL bytes, and `hu_tool_validate_url(expected_url) == HU_OK`. Register the test in the file's `run_*_tests`.

- [ ] **Step 3: Run + commit**

Run: `cmake --build build --target human_tests -j8 && ./build/human_tests --filter=rich_link`
Expected: PASS.

```bash
git add tests/test_imessage_rich_link.c
git commit -m "test(imessage): pin two-bubble ordering + bare-url invariant for music share"
```

---

# SLICE 3 — YouTube + TikTok resolvers and full dispatch

### Task 6: `hu_youtube_*` module

**Files:**
- Create: `include/human/youtube.h`
- Create: `src/youtube.c`
- Test: `tests/test_youtube.c`
- Modify: `CMakeLists.txt`, `tests/test_main.c`

- [ ] **Step 1: Create the header**

`include/human/youtube.h`:

```c
#ifndef HU_YOUTUBE_H
#define HU_YOUTUBE_H

#include "core/allocator.h"
#include "core/error.h"
#include <stddef.h>

typedef struct hu_youtube_result {
    char *video_id;       /* "dQw4w9WgXcQ" */
    char *title;          /* snippet.title */
    char *channel_title;  /* snippet.channelTitle */
    char *watch_url;      /* https://www.youtube.com/watch?v=<video_id> */
} hu_youtube_result_t;

/* YouTube Data API v3 search.list (type=video, maxResults=1).
 * Requires an API key. Network-guarded; returns the verified top result. */
hu_error_t hu_youtube_search(hu_allocator_t *alloc, const char *api_key, const char *query,
                             size_t query_len, hu_youtube_result_t *out);

/* Parse a search.list JSON response. Exposed for testing (no network). */
hu_error_t hu_youtube_parse_search_response(hu_allocator_t *alloc, const char *json,
                                            size_t json_len, hu_youtube_result_t *out);

void hu_youtube_result_free(hu_allocator_t *alloc, hu_youtube_result_t *out);

#endif
```

- [ ] **Step 2: Write failing parse tests**

`tests/test_youtube.c`:

```c
#include "human/youtube.h"
#include "test_framework.h"
#include <string.h>

static const char YT_OK[] =
    "{\"items\":[{\"id\":{\"videoId\":\"dQw4w9WgXcQ\"},"
    "\"snippet\":{\"title\":\"Latte Art Basics\",\"channelTitle\":\"CoffeeCo\"}}]}";

static const char YT_EMPTY[] = "{\"items\":[]}";

static void youtube_parse_builds_canonical_url(void) {
    hu_allocator_t alloc = hu_allocator_default();
    hu_youtube_result_t r = {0};
    HU_ASSERT_EQ((int)hu_youtube_parse_search_response(&alloc, YT_OK, sizeof(YT_OK) - 1, &r),
                 (int)HU_OK);
    HU_ASSERT_STR_EQ(r.video_id, "dQw4w9WgXcQ");
    HU_ASSERT_STR_EQ(r.watch_url, "https://www.youtube.com/watch?v=dQw4w9WgXcQ");
    hu_youtube_result_free(&alloc, &r);
}

static void youtube_parse_empty_items_errors(void) {
    hu_allocator_t alloc = hu_allocator_default();
    hu_youtube_result_t r = {0};
    HU_ASSERT_TRUE(hu_youtube_parse_search_response(&alloc, YT_EMPTY, sizeof(YT_EMPTY) - 1, &r)
                   != HU_OK);
    hu_youtube_result_free(&alloc, &r);
}

void run_youtube_tests(void) {
    HU_TEST_SUITE("youtube");
    HU_RUN_TEST(youtube_parse_builds_canonical_url);
    HU_RUN_TEST(youtube_parse_empty_items_errors);
}
```

(Match the project's allocator-acquisition idiom used in `tests/test_music.c` — replace `hu_allocator_default()` with whatever that file uses.)

- [ ] **Step 3: Register in build + runner**

`CMakeLists.txt`: add `src/youtube.c` and `tests/test_youtube.c` next to the inspiration entries.
`tests/test_main.c`: add `void run_youtube_tests(void);` and `run_youtube_tests();`.

- [ ] **Step 4: Run to verify failure**

Run: `cmake --build build --target human_tests -j8`
Expected: link error — `hu_youtube_*` undefined.

- [ ] **Step 5: Implement `src/youtube.c`** (mirror `src/music.c`'s parse + network split)

```c
#include "human/youtube.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

static char *yt_dup(hu_allocator_t *alloc, const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *d = alloc->alloc(alloc->ctx, n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

hu_error_t hu_youtube_parse_search_response(hu_allocator_t *alloc, const char *json,
                                            size_t json_len, hu_youtube_result_t *out) {
    if (!alloc || !json || json_len == 0 || !out) return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    hu_json_t *root = hu_json_parse(alloc, json, json_len);
    if (!root) return HU_ERR_PARSE;

    hu_json_t *items = hu_json_get(root, "items");
    hu_json_t *first = (items && hu_json_array_size(items) > 0) ? hu_json_array_get(items, 0) : NULL;
    if (!first) { hu_json_free(alloc, root); return HU_ERR_NOT_FOUND; }

    hu_json_t *id = hu_json_get(first, "id");
    hu_json_t *sn = hu_json_get(first, "snippet");
    const char *vid = id ? hu_json_get_string(id, "videoId") : NULL;
    if (!vid || !*vid) { hu_json_free(alloc, root); return HU_ERR_NOT_FOUND; }

    out->video_id = yt_dup(alloc, vid);
    out->title = yt_dup(alloc, sn ? hu_json_get_string(sn, "title") : NULL);
    out->channel_title = yt_dup(alloc, sn ? hu_json_get_string(sn, "channelTitle") : NULL);

    char url[128];
    int n = snprintf(url, sizeof(url), "https://www.youtube.com/watch?v=%s", vid);
    out->watch_url = (n > 0 && (size_t)n < sizeof(url)) ? yt_dup(alloc, url) : NULL;

    hu_json_free(alloc, root);
    return (out->video_id && out->watch_url) ? HU_OK : HU_ERR_PARSE;
}

void hu_youtube_result_free(hu_allocator_t *alloc, hu_youtube_result_t *out) {
    if (!alloc || !out) return;
    if (out->video_id) alloc->free(alloc->ctx, out->video_id, strlen(out->video_id) + 1);
    if (out->title) alloc->free(alloc->ctx, out->title, strlen(out->title) + 1);
    if (out->channel_title) alloc->free(alloc->ctx, out->channel_title, strlen(out->channel_title) + 1);
    if (out->watch_url) alloc->free(alloc->ctx, out->watch_url, strlen(out->watch_url) + 1);
    memset(out, 0, sizeof(*out));
}

#if !defined(HU_IS_TEST) && defined(HU_HTTP_CURL)
#include "human/core/http.h"
#include "human/music.h"  /* reuse hu_music_url_encode_query for the query */

hu_error_t hu_youtube_search(hu_allocator_t *alloc, const char *api_key, const char *query,
                             size_t query_len, hu_youtube_result_t *out) {
    if (!alloc || !api_key || !*api_key || !query || query_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;

    char enc[512];
    size_t e = hu_music_url_encode_query(query, query_len, enc, sizeof(enc));
    if (e == 0) return HU_ERR_INVALID_ARGUMENT;

    char url[1024];
    int n = snprintf(url, sizeof(url),
                     "https://www.googleapis.com/youtube/v3/search?part=snippet&type=video"
                     "&maxResults=1&q=%s&key=%s", enc, api_key);
    if (n < 0 || (size_t)n >= sizeof(url)) return HU_ERR_INVALID_ARGUMENT;

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url, NULL, &resp);
    if (err != HU_OK) { if (resp.owned && resp.body) hu_http_response_free(alloc, &resp); return err; }
    if (resp.status_code != 200 || !resp.body || resp.body_len == 0) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }
    err = hu_youtube_parse_search_response(alloc, resp.body, resp.body_len, out);
    hu_http_response_free(alloc, &resp);
    return err;
}
#else
hu_error_t hu_youtube_search(hu_allocator_t *alloc, const char *api_key, const char *query,
                             size_t query_len, hu_youtube_result_t *out) {
    (void)alloc; (void)api_key; (void)query; (void)query_len; (void)out;
    return HU_ERR_NOT_SUPPORTED;  /* tests use hu_youtube_parse_search_response */
}
#endif
```

Confirm exact `hu_json_*` names against `include/human/core/json.h` (e.g. `hu_json_parse`, `hu_json_get`, `hu_json_get_string`, `hu_json_array_size`, `hu_json_array_get`, `hu_json_free`) and adjust to the real API before compiling.

- [ ] **Step 6: Run + verify pass + gate symmetry**

Run: `cmake --build build --target human_tests -j8 && ./build/human_tests --filter=youtube`
Expected: PASS.
Run: `bash scripts/check-test-source-gate-symmetry.sh` → exit 0.

- [ ] **Step 7: Commit**

```bash
git add include/human/youtube.h src/youtube.c tests/test_youtube.c CMakeLists.txt tests/test_main.c
git commit -m "feat(youtube): Data API v3 search → verified canonical watch URL"
```

### Task 7: Dispatch all three media in the daemon

**Files:**
- Modify: `src/daemon.c` (the inspiration block; generalize the music-only flow)

- [ ] **Step 1: Compute youtube availability + pick the medium**

At the top of the share block (`src/daemon.c:13678`, inside the `should_send_music`/`should_share` branch), determine the medium once:

```c
                        const char *yt_key =
                            config ? hu_config_get_provider_key(config, "youtube") : NULL;
                        hu_inspiration_medium_t medium = hu_inspiration_pick_medium(
                            combined, combined_len, yt_key && *yt_key);
                        const char *insp_sys = hu_inspiration_system_prompt(medium);
```

Use `insp_sys` in place of the hardcoded `music_sys` in the `chat_with_system` call (keep `sizeof - 1` → `strlen(insp_sys)`).

- [ ] **Step 2: Branch on the medium after parsing the suggestion**

After `hu_music_parse_suggestion(...)` yields `search_query` + `casual_msg`, build a single `share_url[1024]` for the chosen medium, then funnel into the existing two-bubble sender:

```c
                                char share_url[1024] = {0};
                                bool have_url = false;

                                if (medium == HU_INSPIRATION_TIKTOK) {
                                    have_url = hu_tiktok_tag_url(search_query, strlen(search_query),
                                                                 share_url, sizeof(share_url)) > 0;
                                } else if (medium == HU_INSPIRATION_YOUTUBE) {
                                    hu_youtube_result_t yt = {0};
                                    if (hu_youtube_search(alloc, yt_key, search_query,
                                                          strlen(search_query), &yt) == HU_OK &&
                                        yt.watch_url) {
                                        snprintf(share_url, sizeof(share_url), "%s", yt.watch_url);
                                        have_url = true;
                                    }
                                    hu_youtube_result_free(alloc, &yt);
                                }
                                /* MUSIC keeps its existing iTunes/Spotify + verify path below. */
```

For TikTok/YouTube, reuse the **same two-bubble send** from Task 4 against `share_url` + `casual_msg`, guarded by `hu_tool_validate_url(share_url) == HU_OK`, on unfurling channels; on non-unfurling channels send `"<casual_msg> <share_url>"` as one message (no media). Factor the two-bubble send into a small `static` daemon helper `send_inspiration(ch, batch_key, key_len, casual_msg, share_url, music_seed)` and call it from both the music path (Task 4) and here, to stay DRY.

- [ ] **Step 3: Silent-skip on failure**

If `medium != MUSIC` and `!have_url`, log and send nothing:

```c
                                if (medium != HU_INSPIRATION_MUSIC && !have_url) {
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "inspiration skipped (no verified %s) for: %s",
                                                medium == HU_INSPIRATION_YOUTUBE ? "video" : "tiktok",
                                                search_query);
                                }
```

- [ ] **Step 4: Rebuild production binary + full suite**

Run: `touch src/daemon.c && cmake --build build --target human human_tests -j8`
Expected: `Linking C executable human` + `Signing human binary`.
Run: `./build/human_tests 2>/dev/null | grep -E 'Results:'`
Expected: `N/N passed`, 0 failures, 0 ASan errors.

- [ ] **Step 5: Commit**

```bash
git add src/daemon.c
git commit -m "feat(daemon): dispatch music/youtube/tiktok inspirations via one verified path"
```

### Task 8: Final verification

- [ ] **Step 1: Full suite + preflight**

Run: `./build/human_tests 2>/dev/null | grep -E 'Results:'` → `N/N passed`, 0 failures.
Run: `scripts/agent-preflight.sh` → green.
Run: `bash scripts/check-test-source-gate-symmetry.sh` → exit 0.

- [ ] **Step 2: Spawn /verify** (per project quality gate) to prove the predicate + dispatch behave on real inputs, capturing evidence.

- [ ] **Step 3: Document the YouTube key** — add a one-line note to the config docs that `"youtube"` provider key enables YouTube inspirations (graceful skip when absent).

```bash
git add docs/ && git commit -m "docs(config): document optional youtube provider key for inspirations"
```

---

## Self-review

- **Spec coverage:** resolve-and-verify (Tasks 1,2,6,7) ✓; two-bubble framing (Tasks 4,5) ✓; persona voice (Task 3 helper + Task 4 wiring) ✓; one-roll-pick-medium (Tasks 3,7) ✓; YouTube Data API + graceful skip (Tasks 6,7) ✓; TikTok tag URL (Task 3) ✓; silent skip (Tasks 2,7) ✓; testing (every task) ✓.
- **Placeholders:** code shown for all logic steps; the two spots that say "confirm exact API name against header" are lookups of already-existing symbols (`hu_json_*`, persona `formality` field), not unwritten logic — each task lists the grep to run first.
- **Type consistency:** `hu_inspiration_medium_t`, `hu_youtube_result_t`, `hu_music_result_matches(const char*, const hu_music_result_t*)`, `hu_tiktok_tag_url(kw,len,out,cap)`, `hu_inspiration_build_voice_hint(formality,traits,out,cap)`, `send_inspiration(...)` helper used identically in Tasks 4 and 7.
