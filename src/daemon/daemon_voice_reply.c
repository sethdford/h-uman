/* src/daemon/daemon_voice_reply.c
 *
 * Carved out of hu_service_run (daemon.c) 2026-09-12 — behavior-preserving move.
 * hu_service_run was a single 10,087-line function; each carve is one
 * self-contained block with zero loop escapes, moved verbatim behind a
 * named entry point so the service loop reads as a sequence of steps. */

#include "human/agent.h"
#include "human/config.h"
#include "human/context/voice_decision.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/daemon.h"
#include "human/daemon/voice_facade.h"
#include "human/platform.h"
#if defined(HU_ENABLE_CARTESIA)
#include "human/tts/voice_reply.h"
#endif

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

bool hu_daemon_voice_reply(hu_allocator_t *alloc, hu_agent_t *agent, const hu_config_t *config,
                           hu_service_channel_t *ch, const char *batch_key, size_t key_len,
                           const char *combined, size_t combined_len, const char *response,
                           size_t response_len, int bth_hour) {
    /* Only the Cartesia arm below reads these; without HU_ENABLE_CARTESIA the
     * legacy path ignores them. Stated here so -Werror builds of every preset
     * agree on the signature. */
    (void)agent;
    (void)combined;
    (void)combined_len;
    (void)bth_hour;
    bool sent_voice = false;
    {
        const char *chn_voice =
            ch->channel->vtable->name ? ch->channel->vtable->name(ch->channel->ctx) : NULL;
        const hu_channel_daemon_config_t *dcfg_voice =
            hu_daemon_active_daemon_config(config, chn_voice);
        bool voice_channel_ok = dcfg_voice && dcfg_voice->voice_enabled;

        /* Unified duplex + Realtime (`voice.mode`: "realtime" or legacy
         * `voice.tts_provider`: "realtime"). */
        hu_voice_session_t unified_voice = {0};
        bool unified_voice_active = false;
        bool cfg_realtime =
            config &&
            ((config->voice.mode && strcmp(config->voice.mode, "realtime") == 0) ||
             (config->voice.tts_provider && strcmp(config->voice.tts_provider, "realtime") == 0));
        if (voice_channel_ok && config && chn_voice && cfg_realtime) {
            size_t chn_len = strlen(chn_voice);
            if (hu_voice_session_start(alloc, &unified_voice, chn_voice, chn_len, config) == HU_OK)
                unified_voice_active = true;
        }

#if defined(HU_ENABLE_CARTESIA)
        if (voice_channel_ok && agent->persona && agent->persona->voice.voice_id[0] &&
            agent->persona->voice_messages.enabled) {
            hu_voice_decision_t vdec = hu_voice_decision_classify(
                response, response_len, combined, combined_len, &agent->persona->voice_messages,
                true, bth_hour, (uint32_t)(time(NULL) ^ (uintptr_t)combined));
            if (vdec == HU_VOICE_SEND_VOICE) {
                const char *cartesia_key = hu_config_get_provider_key(config, "cartesia");
                if (cartesia_key && cartesia_key[0]) {
                    hu_voice_reply_request_t req;
                    hu_error_t prep_err = hu_voice_reply_build_request(
                        &agent->persona->voice, response, response_len, combined, combined_len,
                        bth_hour, (uint32_t)time(NULL), &req);
                    unsigned char *audio_bytes = NULL;
                    size_t audio_len = 0;
                    hu_error_t tts_err = prep_err;
                    if (prep_err == HU_OK)
                        tts_err = hu_cartesia_tts_synthesize(
                            alloc, cartesia_key, strlen(cartesia_key), req.transcript,
                            req.transcript_len, &req.tts, hu_tts_format_for_channel(chn_voice),
                            &audio_bytes, &audio_len);
                    if (tts_err == HU_OK && audio_bytes && audio_len > 0) {
                        char audio_path[512];
                        hu_error_t pipe_err =
                            hu_voice_reply_audio_to_temp(alloc, chn_voice, audio_bytes, audio_len,
                                                         audio_path, sizeof(audio_path));
                        hu_cartesia_tts_free_bytes(alloc, audio_bytes, audio_len);
                        if (pipe_err == HU_OK) {
                            const char *media_paths[] = {audio_path};
                            hu_error_t send_err = ch->channel->vtable->send(
                                ch->channel->ctx, batch_key, key_len, "", 0, media_paths, 1);
                            hu_audio_cleanup_temp(audio_path);
                            if (send_err == HU_OK)
                                sent_voice = true;
                        }
                    } else if (audio_bytes) {
                        hu_cartesia_tts_free_bytes(alloc, audio_bytes, audio_len);
                    }
                }
            }
        }
#endif
        /* Fallback: unified voice pipeline when persona Cartesia path did not send.
         */
        if (!sent_voice && voice_channel_ok && !unified_voice_active && config) {
            hu_voice_config_t voice_cfg = {0};
            if (hu_voice_config_from_settings(config, &voice_cfg) == HU_OK &&
                voice_cfg.tts_provider && voice_cfg.tts_provider[0]) {
                void *audio = NULL;
                size_t audio_len = 0;
                hu_error_t tts_err =
                    hu_voice_tts(alloc, &voice_cfg, response, response_len, &audio, &audio_len);
                if (tts_err == HU_OK && audio && audio_len > 0) {
                    unsigned char *audio_bytes = (unsigned char *)audio;
                    char audio_path[512];
                    hu_error_t pipe_err = HU_ERR_IO;
#if defined(HU_ENABLE_CARTESIA)
                    pipe_err = hu_voice_reply_audio_to_temp(
                        alloc, chn_voice, audio_bytes, audio_len, audio_path, sizeof(audio_path));
#else
                    {
                        char *tmp_dir = hu_platform_get_temp_dir(alloc);
                        if (tmp_dir) {
                            int np =
                                snprintf(audio_path, sizeof(audio_path), "%s/human_dtts_%lld.mp3",
                                         tmp_dir, (long long)time(NULL));
                            size_t tdl = strlen(tmp_dir);
                            alloc->free(alloc->ctx, tmp_dir, tdl + 1);
                            if (np > 0 && (size_t)np < sizeof(audio_path)) {
                                FILE *tf = fopen(audio_path, "wb");
                                if (tf) {
                                    if (fwrite(audio_bytes, 1, audio_len, tf) == audio_len)
                                        pipe_err = HU_OK;
                                    fclose(tf);
                                    if (pipe_err != HU_OK)
                                        (void)unlink(audio_path);
                                }
                            }
                        }
                    }
#endif
                    alloc->free(alloc->ctx, audio, audio_len);
                    if (pipe_err == HU_OK) {
                        const char *media_paths[] = {audio_path};
                        hu_error_t send_err = ch->channel->vtable->send(
                            ch->channel->ctx, batch_key, key_len, "", 0, media_paths, 1);
#if defined(HU_ENABLE_CARTESIA)
                        hu_audio_cleanup_temp(audio_path);
#else
                        (void)unlink(audio_path);
#endif
                        if (send_err == HU_OK)
                            sent_voice = true;
                    }
                } else if (audio) {
                    alloc->free(alloc->ctx, audio, audio_len);
                }
            }
        }
        if (unified_voice_active) {
            hu_voice_session_warn_first_byte_latency_if_needed(&unified_voice);
            (void)hu_voice_session_stop(&unified_voice);
        }
    }
    return sent_voice;
}
