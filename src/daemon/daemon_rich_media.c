/* src/daemon/daemon_rich_media.c
 *
 * Carved out of hu_service_run (daemon.c) 2026-09-12 — behavior-preserving move.
 * hu_service_run was a single 10,087-line function; each carve is one
 * self-contained block with zero loop escapes, moved verbatim behind a
 * named entry point so the service loop reads as a sequence of steps. */

#include "human/agent.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/paths.h"
#include "human/daemon.h"
#include "human/daemon_routing.h"
#include "human/inspiration.h"
#include "human/music.h"
#include "human/security/moderation.h"
#include "human/youtube.h"

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void hu_daemon_rich_media_tick(hu_allocator_t *alloc, hu_agent_t *agent, const hu_config_t *config,
                               hu_service_channel_t *ch, const char *batch_key, size_t key_len,
                               const char *combined, size_t combined_len,
                               hu_channel_history_entry_t *history_entries, size_t history_count,
                               bool gif_sent_this_turn) {
    /* Music teaser: share a song with 30s preview + artwork */
    if (combined_len > 0 && ch->channel->vtable->send && !gif_sent_this_turn) {
        float music_prob = 0.05f;
        if (agent->persona) {
            music_prob = agent->persona->humanization.gif_probability > 0.0f
                             ? agent->persona->humanization.gif_probability * 0.3f
                             : 0.05f;
        }
        /* Boost probability if taste hit rate is high for this contact */
        float taste_rate = hu_music_taste_hit_rate(batch_key, key_len);
        if (taste_rate > 0.5f && music_prob < 0.15f)
            music_prob = 0.15f;

        uint32_t music_seed = (uint32_t)time(NULL) * 16807u + (uint32_t)(uintptr_t)combined;
        if (hu_conversation_should_send_music(combined, combined_len, history_entries,
                                              history_count, music_seed, music_prob)) {
            const char *yt_key = config ? hu_config_get_provider_key(config, "youtube") : NULL;
            hu_inspiration_medium_t medium =
                hu_inspiration_pick_medium(combined, combined_len, yt_key && *yt_key);

            /* Build taste-enriched prompt */
            char taste_snippet[256] = {0};
            size_t taste_len = hu_music_taste_build_prompt(batch_key, key_len, taste_snippet,
                                                           sizeof(taste_snippet));

            char music_prompt[768];
            size_t mp_len;
            if (medium == HU_INSPIRATION_MUSIC) {
                mp_len = hu_conversation_build_music_prompt(combined, combined_len, music_prompt,
                                                            sizeof(music_prompt));
            } else {
                size_t clip = combined_len > 200 ? 200 : combined_len;
                int pn = snprintf(music_prompt, sizeof(music_prompt),
                                  "Recent message context: \"%.*s\"", (int)clip, combined);
                mp_len = (pn > 0 && (size_t)pn < sizeof(music_prompt)) ? (size_t)pn : 0;
            }

            /* Append taste context to prompt if available */
            if (taste_len > 0 && mp_len > 0 && mp_len + taste_len + 2 < sizeof(music_prompt)) {
                music_prompt[mp_len++] = '\n';
                memcpy(music_prompt + mp_len, taste_snippet, taste_len);
                mp_len += taste_len;
                music_prompt[mp_len] = '\0';
            }

            /* Persona voice hint → the human line sounds like the user */
            if (agent && agent->persona) {
                char vh[256];
                const char *form = agent->persona->overlays && agent->persona->overlays_count > 0
                                       ? agent->persona->overlays[0].formality
                                       : NULL;
                const char *trait =
                    (agent->persona->traits_count > 0) ? agent->persona->traits[0] : NULL;
                size_t vlen = hu_inspiration_build_voice_hint(form, trait, vh, sizeof(vh));
                if (vlen > 0 && mp_len + vlen + 2 < sizeof(music_prompt)) {
                    music_prompt[mp_len++] = '\n';
                    memcpy(music_prompt + mp_len, vh, vlen);
                    mp_len += vlen;
                    music_prompt[mp_len] = '\0';
                }
            }

            if (mp_len > 0 && agent->provider.vtable && agent->provider.vtable->chat_with_system) {
                char *music_suggestion = NULL;
                size_t music_suggestion_len = 0;
                const char *insp_sys = hu_inspiration_system_prompt(medium);
                size_t music_fb_len = 0;
                const char *music_fb = hu_daemon_fallback_model(config, &music_fb_len);
                const char *music_model = agent->model_name ? agent->model_name : music_fb;
                size_t music_model_len = agent->model_name ? agent->model_name_len : music_fb_len;
                (void)agent->provider.vtable->chat_with_system(
                    agent->provider.ctx, alloc, insp_sys, strlen(insp_sys), music_prompt, mp_len,
                    music_model, music_model_len, 0.9, &music_suggestion, &music_suggestion_len);

                hu_moderation_result_t music_mod = {0};
                if (hu_moderation_check_local(alloc, music_suggestion, music_suggestion_len,
                                              &music_mod) == HU_OK &&
                    music_mod.flagged) {
                    hu_log_warn("human", agent ? agent->observer : NULL,
                                "music teaser blocked by moderation");
                } else if (music_suggestion && music_suggestion_len > 0 &&
                           music_suggestion_len < 300) {
                    char search_query[256];
                    char casual_msg[256];
                    bool parsed = hu_music_parse_suggestion(music_suggestion, music_suggestion_len,
                                                            search_query, sizeof(search_query),
                                                            casual_msg, sizeof(casual_msg));

                    if (parsed && search_query[0] != '\0' && medium == HU_INSPIRATION_MUSIC) {
                        /* Detect user's streaming preference from history */
                        hu_music_source_t pref = HU_MUSIC_SOURCE_ITUNES;
                        if (history_entries && history_count > 0) {
                            enum { MUSIC_HIST_CAP = 20 };
                            char music_texts[MUSIC_HIST_CAP][512];
                            size_t music_lens[MUSIC_HIST_CAP];
                            const char *music_pref_ptrs[MUSIC_HIST_CAP];
                            size_t music_n = history_count < (size_t)MUSIC_HIST_CAP
                                                 ? history_count
                                                 : (size_t)MUSIC_HIST_CAP;
                            for (size_t mi = 0; mi < music_n; mi++) {
                                size_t tlen = strnlen(history_entries[mi].text, 511);
                                memcpy(music_texts[mi], history_entries[mi].text, tlen);
                                music_texts[mi][tlen] = '\0';
                                music_lens[mi] = tlen;
                                music_pref_ptrs[mi] = music_texts[mi];
                            }
                            pref = hu_music_detect_preference(music_pref_ptrs, music_lens, music_n);
                        }

                        /* Always search iTunes (for the .m4a preview) */
                        hu_music_result_t song = {0};
                        size_t sq_len = strlen(search_query);
                        hu_error_t search_err = hu_music_search(alloc, search_query, sq_len, &song);

                        /* If user prefers Spotify, try to get the Spotify share link */
                        hu_music_result_t spotify_song = {0};
                        bool has_spotify = false;
                        if (pref == HU_MUSIC_SOURCE_SPOTIFY) {
                            const char *sp_cred =
                                config ? hu_config_get_provider_key(config, "spotify") : NULL;
                            if (sp_cred) {
                                /* Expect "client_id:client_secret" format */
                                const char *colon = strchr(sp_cred, ':');
                                if (colon) {
                                    char sp_id[128] = {0};
                                    size_t id_len = (size_t)(colon - sp_cred);
                                    if (id_len < sizeof(sp_id)) {
                                        memcpy(sp_id, sp_cred, id_len);
                                        has_spotify = hu_music_search_spotify(
                                                          alloc, sp_id, colon + 1, search_query,
                                                          sq_len, &spotify_song) == HU_OK;
                                    }
                                }
                            }
                        }

                        /* Use Spotify URL if available, iTunes preview for audio */
                        hu_music_result_t *link_song = has_spotify ? &spotify_song : &song;

                        bool song_verified =
                            hu_music_result_matches(search_query, &song) ||
                            (has_spotify && hu_music_result_matches(search_query, &spotify_song));
                        if (search_err == HU_OK && song_verified &&
                            (song.track_view_url || (has_spotify && spotify_song.track_view_url))) {
                            /* Rich-link mode: when the channel auto-unfurls bare URLs
                             * (iMessage, Telegram, Discord, Slack, WhatsApp, Signal),
                             * send the URL alone in its own bubble. The platform
                             * renders the full rich card — album art, title, artist,
                             * play button — from the URL. No .m4a download, no JPG
                             * download, no caption (caption inline kills the unfurl).
                             *
                             * INVARIANT: the URL bubble body must be exactly the URL
                             * bytes — no preamble, no trailing whitespace, no caption.
                             * Pinned by tests/test_imessage_rich_link.c. */
                            bool rich_link = hu_channel_supports_link_unfurl(ch->channel) &&
                                             link_song->track_view_url != NULL;

                            if (rich_link) {
                                const char *url = link_song->track_view_url;

                                /* Same human-pacing delay as the legacy path so the
                                 * share lands in a natural conversational rhythm. */
                                usleep(3000000 + (music_seed % 4000000));

                                if (hu_inspiration_send_two_bubble(
                                        ch->channel, batch_key, key_len, casual_msg, url,
                                        1500000u + (music_seed % 1500000u))) {
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "sent music rich-link: %s - %s [%s]",
                                                song.artist_name ? song.artist_name : "?",
                                                song.track_name ? song.track_name : "?",
                                                has_spotify ? "spotify" : "itunes");

                                    hu_music_taste_record_send(batch_key, key_len, song.artist_name,
                                                               song.track_name);
                                    {
                                        static uint64_t last_taste_save_ms;
                                        uint64_t tnow = (uint64_t)time(NULL) * 1000ULL;
                                        if (tnow - last_taste_save_ms > 30000) {
                                            last_taste_save_ms = tnow;
                                            char tp[512];
                                            int tn2 =
                                                hu_paths_state(tp, sizeof(tp), "music_taste.json");
                                            if (tn2 > 0 && (size_t)tn2 < sizeof(tp))
                                                hu_music_taste_save(tp, (size_t)tn2);
                                        }
                                    }
                                } else {
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "music rich-link rejected by url "
                                                "validation: %s",
                                                url ? url : "(null)");
                                }
                            } else {
                                /* Legacy: channel doesn't unfurl URLs (SMS etc.).
                                 * Build a text caption and attach the .m4a preview
                                 * + JPG artwork so the recipient still gets media. */
                                char share_text[512];
                                size_t casual_len = strlen(casual_msg);
                                size_t st_len = hu_music_build_share_text(
                                    link_song, casual_len > 0 ? casual_msg : NULL, casual_len,
                                    share_text, sizeof(share_text));

                                char preview_path[256] = {0};
                                bool has_preview = false;
                                if (song.preview_url) {
                                    has_preview = hu_music_download_preview(
                                                      alloc, song.preview_url, preview_path,
                                                      sizeof(preview_path)) == HU_OK;
                                }

                                char artwork_path[256] = {0};
                                bool has_artwork = false;
                                const char *art_url = link_song->artwork_url
                                                          ? link_song->artwork_url
                                                          : song.artwork_url;
                                if (art_url) {
                                    has_artwork =
                                        hu_music_download_artwork(alloc, art_url, artwork_path,
                                                                  sizeof(artwork_path)) == HU_OK;
                                }

                                usleep(3000000 + (music_seed % 4000000));

                                if (st_len > 0) {
                                    int media_count = 0;
                                    const char *media[2];
                                    if (has_preview)
                                        media[media_count++] = preview_path;
                                    if (has_artwork)
                                        media[media_count++] = artwork_path;

                                    hu_error_t mserr = ch->channel->vtable->send(
                                        ch->channel->ctx, batch_key, key_len, share_text, st_len,
                                        media_count > 0 ? media : NULL, (size_t)media_count);
                                    if (mserr != HU_OK)
                                        hu_log_warn("human", NULL, "music send failed: %d",
                                                    (int)mserr);
                                    else
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "sent music %s: %s - %s [%s%s]",
                                                    has_preview ? "preview" : "link",
                                                    song.artist_name ? song.artist_name : "?",
                                                    song.track_name ? song.track_name : "?",
                                                    has_spotify ? "spotify" : "itunes",
                                                    has_artwork ? "+art" : "");

                                    hu_music_taste_record_send(batch_key, key_len, song.artist_name,
                                                               song.track_name);
                                    {
                                        static uint64_t last_taste_save_ms;
                                        uint64_t tnow = (uint64_t)time(NULL) * 1000ULL;
                                        if (tnow - last_taste_save_ms > 30000) {
                                            last_taste_save_ms = tnow;
                                            char tp[512];
                                            int tn2 =
                                                hu_paths_state(tp, sizeof(tp), "music_taste.json");
                                            if (tn2 > 0 && (size_t)tn2 < sizeof(tp))
                                                hu_music_taste_save(tp, (size_t)tn2);
                                        }
                                    }
                                }

                                if (has_preview)
                                    (void)unlink(preview_path);
                                if (has_artwork)
                                    (void)unlink(artwork_path);
                            }
                        } else {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "music share skipped (no verified match) for: %s",
                                        search_query);
                        }
                        hu_music_result_free(alloc, &song);
                        if (has_spotify)
                            hu_music_result_free(alloc, &spotify_song);
                    } else if (parsed && search_query[0] != '\0') {
                        /* YouTube / TikTok: resolve a VERIFIED url, then share it the
                         * same human two-bubble way (or a caption on non-unfurl
                         * channels). No verified url → silent skip. */
                        char share_url[1024] = {0};
                        bool have_url = false;
                        if (medium == HU_INSPIRATION_TIKTOK) {
                            have_url = hu_tiktok_tag_url(search_query, strlen(search_query),
                                                         share_url, sizeof(share_url)) > 0;
                        } else if (medium == HU_INSPIRATION_YOUTUBE) {
                            hu_youtube_result_t yt = {0};
                            if (hu_youtube_search(alloc, yt_key, search_query, strlen(search_query),
                                                  &yt) == HU_OK &&
                                yt.watch_url) {
                                int un = snprintf(share_url, sizeof(share_url), "%s", yt.watch_url);
                                have_url = (un > 0 && (size_t)un < sizeof(share_url));
                            }
                            hu_youtube_result_free(alloc, &yt);
                        }

                        if (have_url) {
                            if (hu_channel_supports_link_unfurl(ch->channel)) {
                                usleep(3000000 + (music_seed % 4000000));
                                hu_inspiration_send_two_bubble(ch->channel, batch_key, key_len,
                                                               casual_msg, share_url,
                                                               1500000u + (music_seed % 1500000u));
                            } else if (hu_tool_validate_url(share_url) == HU_OK) {
                                char cap[1280];
                                int cn =
                                    (casual_msg[0] != '\0')
                                        ? snprintf(cap, sizeof(cap), "%s %s", casual_msg, share_url)
                                        : snprintf(cap, sizeof(cap), "%s", share_url);
                                if (cn > 0 && (size_t)cn < sizeof(cap)) {
                                    hu_error_t send_err = ch->channel->vtable->send(
                                        ch->channel->ctx, batch_key, key_len, cap, (size_t)cn, NULL,
                                        0);
                                    if (send_err != HU_OK)
                                        hu_log_warn("human", agent ? agent->observer : NULL,
                                                    "inspiration send failed (err=%d)",
                                                    (int)send_err);
                                }
                            }
                            hu_log_info(
                                "human", agent ? agent->observer : NULL, "sent %s inspiration: %s",
                                medium == HU_INSPIRATION_YOUTUBE ? "youtube" : "tiktok", share_url);
                        } else {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "inspiration skipped (no verified %s) for: %s",
                                        medium == HU_INSPIRATION_YOUTUBE ? "video" : "tiktok",
                                        search_query);
                        }
                    }
                }
                if (music_suggestion)
                    alloc->free(alloc->ctx, music_suggestion, music_suggestion_len + 1);
            }
        }
    }
}
