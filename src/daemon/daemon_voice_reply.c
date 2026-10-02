/* src/daemon/daemon_voice_reply.c
 *
 * Carved out of hu_service_run (daemon.c) 2026-09-12 — behavior-preserving move.
 * hu_service_run was a single 10,087-line function; each carve is one
 * self-contained block with zero loop escapes, moved verbatim behind a
 * named entry point so the service loop reads as a sequence of steps. */

#include "human/agent.h"
#include "human/config.h"
#include "human/context/voice_decision.h"
#include "human/context/voice_intent.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/local_only_guard.h"
#include "human/core/log.h"
#include "human/daemon.h"
#include "human/daemon/voice_facade.h"
#include "human/daemon/voice_first.h"
#include "human/daemon_outbound_bus.h"
#include "human/persona.h"
#include "human/platform.h"
#include "human/security/moderation.h"
#include "human/tts/opener_gate.h"
#include "human/tts/speech_direction.h"
#include "human/tts/speech_perform.h"
#include "human/tts/speech_rewrite.h"
#include "human/tts/speech_text.h"
#if defined(HU_ENABLE_CARTESIA)
#include "human/tts/voice_reply.h"
#endif
#if defined(HU_ENABLE_SQLITE)
#include "human/memory.h"
#include "human/memory/engines.h"
#include "human/memory/proactive_decisions_repo.h"
#endif

#if defined(HU_ENABLE_CARTESIA)
/* Log the voice/text decision (trigger='voice_reply') into proactive_decisions
 * so voice timing gets the same When2Speak measurement as proactive sends
 * (scripts/eval_when_to_speak.py). Best-effort: never affects the reply. */
static void daemon_voice_record_decision(hu_agent_t *agent, const char *batch_key, size_t key_len,
                                         bool chose_voice, const char *reason, bool sent) {
#if defined(HU_ENABLE_SQLITE)
    if (!agent || !agent->memory)
        return;
    sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
    if (!db)
        return;
    char contact[128];
    size_t n = key_len < sizeof(contact) - 1 ? key_len : sizeof(contact) - 1;
    if (batch_key && n > 0)
        memcpy(contact, batch_key, n);
    contact[batch_key ? n : 0] = '\0';
    if (hu_proactive_decisions_repo_ensure_schema(db) != HU_OK)
        return;
    hu_error_t err = hu_proactive_decisions_repo_record(
        db, (int64_t)time(NULL), contact[0] ? contact : NULL, "voice_reply",
        chose_voice ? HU_PROACTIVE_DECISION_SEND : HU_PROACTIVE_DECISION_DECLINE,
        reason ? reason : "unknown", sent ? 1 : 0, NULL);
    if (err != HU_OK)
        hu_log_warn("voice_reply", NULL, "proactive_decisions_repo_record failed: err=%d",
                    (int)err);
#else
    (void)agent;
    (void)batch_key;
    (void)key_len;
    (void)chose_voice;
    (void)reason;
    (void)sent;
#endif
}
#endif /* HU_ENABLE_CARTESIA */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

bool hu_voice_reply_gates_clear(hu_allocator_t *alloc, const char *text, size_t text_len,
                                const char *inbound, size_t inbound_len, const char **reason_out) {
    const char *why = "invalid";
    bool clear = false;
    if (text && text_len > 0) {
        hu_moderation_result_t in_mod;
        memset(&in_mod, 0, sizeof(in_mod));
        /* Same criterion as the daemon's SHIELD-005 inbound_crisis flag. */
        if (inbound && inbound_len > 0 &&
            (hu_moderation_check(alloc, inbound, inbound_len, &in_mod) != HU_OK ||
             in_mod.self_harm))
            why = "inbound_crisis";
        else /* the reply itself: the one definition of the outbound gates */
            clear = hu_daemon_outbound_final_gates_clear(alloc, text, text_len, &why);
    }
    if (reason_out)
        *reason_out = why;
    return clear;
}

static bool voice_gates_pass(hu_allocator_t *alloc, const char *text, size_t len,
                             const char *inbound, size_t inbound_len, const char *what) {
    const char *why = NULL;
    if (hu_voice_reply_gates_clear(alloc, text, len, inbound, inbound_len, &why))
        return true;
    hu_log_info("voice_reply", NULL, "voice declined by safety gate (%s): %s", what, why);
    return false;
}

char *hu_daemon_voice_capture_unshaped(hu_allocator_t *alloc, const hu_config_t *config,
                                       hu_service_channel_t *ch, const char *response,
                                       size_t response_len, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!alloc || !config || !ch || !ch->channel || !response || response_len == 0 || !out_len)
        return NULL;
    const char *chn = ch->channel->vtable && ch->channel->vtable->name
                          ? ch->channel->vtable->name(ch->channel->ctx)
                          : NULL;
    const hu_channel_daemon_config_t *dc = hu_daemon_active_daemon_config(config, chn);
    if (!dc || !dc->voice_enabled)
        return NULL;
    char *copy = alloc->alloc(alloc->ctx, response_len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, response, response_len);
    copy[response_len] = '\0';
    *out_len = response_len;
    return copy;
}

/* One memo's spoken form (F1) and its direction (F2-voice, spec 2026-09-27). */
typedef struct {
    int state;     /* 0 not yet run, 1 speak, -1 declined */
    bool directed; /* LIVE direction passed: speak `rendered` */
    char rendered[HU_DIRECTION_RENDER_CAP];
    size_t rendered_len;
    char words[HU_DIRECTION_WORDS_CAP];
    size_t words_len;
    char emotion[24];
    size_t sentences;
} voice_final_t;

/* D1 + D2 + S3 (inside hu_speech_perform), then S4 on the words and D3. SHADOW
 * runs and logs only. HU_SPEECH_DIRECTION as a default is gated on Seth's ear
 * test (>= 8/10 directed) and the W5 real-or-clone test rated by Mindy — do
 * not flip without both. */
static void voice_direct(hu_allocator_t *alloc, hu_agent_t *agent, const char *batch_key,
                         size_t key_len, const char *combined, size_t combined_len,
                         const hu_speech_result_t *sp, hu_speech_rewrite_mode_t mode,
                         voice_final_t *vf) {
    hu_perform_result_t *r = alloc->alloc(alloc->ctx, sizeof(*r));
    if (!r)
        return;
    const hu_contact_profile_t *cp =
        agent && agent->persona && batch_key
            ? hu_persona_find_contact(agent->persona, batch_key, key_len)
            : NULL;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    hu_perform_scene_t scene = {
        .speaker = agent && agent->persona ? agent->persona->name : NULL,
        .listener = cp ? cp->name : NULL,
        .relationship = cp ? cp->relationship : NULL,
        .hour_local = tmv.tm_hour,
        .weekday = tmv.tm_wday,
        .inbound = combined,
        .inbound_len = combined_len,
    };
    (void)hu_speech_perform(alloc, agent ? &agent->provider : NULL,
                            agent ? agent->model_name : NULL, agent ? agent->model_name_len : 0,
                            &scene, sp->spoken, sp->spoken_len, r);
    char summary[192];
    if (hu_direction_summary(&r->dir, summary, sizeof(summary)) == 0)
        summary[0] = '\0';
    hu_log_info("voice_reply", NULL, "direction %s: ok=%d reason=%s segments=%zu %s",
                mode == HU_SPEECH_REWRITE_LIVE ? "live" : "shadow", r->ok ? 1 : 0, r->reason,
                r->dir.count, summary);
    if (mode == HU_SPEECH_REWRITE_LIVE && r->ok &&
        voice_gates_pass(alloc, r->dir.words, r->dir.words_len, combined, combined_len,
                         "directed")) {
        vf->rendered_len =
            hu_direction_render(&r->dir, hu_laugh_style_parse(getenv("HU_VOICE_LAUGH")),
                                vf->rendered, sizeof(vf->rendered));
        if (vf->rendered_len > 0) {
            memcpy(vf->words, r->dir.words, r->dir.words_len + 1);
            vf->words_len = r->dir.words_len;
            const char *fe = hu_direction_first_emotion(&r->dir);
            snprintf(vf->emotion, sizeof(vf->emotion), "%s", fe ? fe : "");
            vf->sentences = r->dir.sentences;
            vf->directed = true;
        }
    }
    alloc->free(alloc->ctx, r, sizeof(*r));
}

/* voiceai opener gate, per recipient, for the whole daemon's lifetime. */
static hu_opener_gate_t g_opener_gate;
static bool g_opener_gate_ready;
static pthread_mutex_t g_opener_gate_mu = PTHREAD_MUTEX_INITIALIZER;

/* Strip a leading reaction word from buf in place; false when there is none. */
static bool strip_opener_in_place(hu_allocator_t *alloc, char *buf, size_t *len, size_t cap) {
    char *tmp = alloc->alloc(alloc->ctx, cap);
    if (!tmp)
        return false;
    size_t n = hu_opener_strip(buf, *len, tmp, cap);
    if (n > 0) {
        memcpy(buf, tmp, n + 1);
        *len = n;
    }
    alloc->free(alloc->ctx, tmp, cap);
    return n > 0;
}

/* Keep a reaction-word opener ("Oh,", "Ha,", "Yeah,") at most once every
 * HU_OPENER_EVERY memos to the same person. HU_VOICE_OPENER_GATE: off
 * (default) | shadow (log the decision) | live (strip). LIVE as a default is
 * gated on Seth's ear test preferring gated memos — do not flip without it. */
static void voice_gate_opener(hu_allocator_t *alloc, const char *batch_key, size_t key_len,
                              hu_speech_result_t *sp, voice_final_t *vf) {
    hu_speech_rewrite_mode_t m = hu_speech_rewrite_mode_parse(getenv("HU_VOICE_OPENER_GATE"));
    if (m == HU_SPEECH_REWRITE_OFF)
        return;
    const char *said = vf->directed ? vf->words : sp->spoken;
    size_t said_len = vf->directed ? vf->words_len : sp->spoken_len;
    char probe[HU_DIRECTION_WORDS_CAP];
    if (hu_opener_strip(said, said_len, probe, sizeof(probe)) == 0)
        return;
    pthread_mutex_lock(&g_opener_gate_mu);
    if (!g_opener_gate_ready) {
        hu_opener_gate_init(&g_opener_gate, HU_OPENER_EVERY);
        g_opener_gate_ready = true;
    }
    bool keep = hu_opener_gate_keep(&g_opener_gate, batch_key, key_len);
    pthread_mutex_unlock(&g_opener_gate_mu);
    hu_log_info("voice_reply", NULL, "opener gate %s: %s",
                m == HU_SPEECH_REWRITE_LIVE ? "live" : "shadow", keep ? "kept" : "stripped");
    if (keep || m != HU_SPEECH_REWRITE_LIVE)
        return;
    (void)strip_opener_in_place(alloc, sp->spoken, &sp->spoken_len, sizeof(sp->spoken));
    if (vf->directed) {
        (void)strip_opener_in_place(alloc, vf->words, &vf->words_len, sizeof(vf->words));
        (void)strip_opener_in_place(alloc, vf->rendered, &vf->rendered_len, sizeof(vf->rendered));
    }
}

/* F1 S1 + S4 (and F2-voice direction) for a memo that is about to be
 * synthesized. The rewrite and the performance are LLM calls, so they run
 * here — once — rather than for every reply. */
static bool voice_spoken_final(hu_allocator_t *alloc, hu_agent_t *agent, const char *batch_key,
                               size_t key_len, const char *response, size_t response_len,
                               const char *combined, size_t combined_len, hu_speech_result_t *sp,
                               voice_final_t *vf) {
    if (vf->state == 0) {
        hu_speech_rewrite_mode_t dm = hu_speech_rewrite_mode_parse(getenv("HU_SPEECH_DIRECTION"));
        /* LIVE as a default is gated on the voice A/B drip preferring the
         * rewrite over cleanup-only, then the W5 real-or-clone test — do not
         * flip without them. LIVE direction supersedes the rewrite; SHADOW
         * direction only observes, so the rewrite keeps running. */
        hu_speech_rewrite_mode_t m = hu_speech_rewrite_mode_parse(getenv("HU_SPEECH_REWRITE"));
        if (m != HU_SPEECH_REWRITE_OFF && dm != HU_SPEECH_REWRITE_LIVE) {
            (void)hu_speech_prepare(
                alloc, agent ? &agent->provider : NULL, agent ? agent->model_name : NULL,
                agent ? agent->model_name_len : 0, agent ? agent->persona : NULL, m, response,
                response_len, combined, combined_len, sp);
            if (m == HU_SPEECH_REWRITE_SHADOW)
                hu_speech_shadow_record(alloc, sp);
        }
        /* F1 S4: the gates also judge what is actually spoken. */
        vf->state = sp->spoken_len > 0 && voice_gates_pass(alloc, sp->spoken, sp->spoken_len,
                                                           combined, combined_len, "spoken")
                        ? 1
                        : -1;
        if (vf->state == 1 && dm != HU_SPEECH_REWRITE_OFF)
            voice_direct(alloc, agent, batch_key, key_len, combined, combined_len, sp, dm, vf);
        if (vf->state == 1)
            voice_gate_opener(alloc, batch_key, key_len, sp, vf);
    }
    return vf->state == 1;
}

bool hu_daemon_voice_reply(hu_allocator_t *alloc, hu_agent_t *agent, const hu_config_t *config,
                           hu_service_channel_t *ch, const char *batch_key, size_t key_len,
                           const char *combined, size_t combined_len, const char *response,
                           size_t response_len, const char *unshaped, size_t unshaped_len,
                           int bth_hour, int voice_first) {
    /* F1: a memo says the reply as written, not the copy text shaping styled
     * for iMessage (typos, lowercase quirks, "haha " fillers). */
    if (unshaped && unshaped_len > 0) {
        response = unshaped;
        response_len = unshaped_len;
    }
    /* Only the Cartesia arm below reads these; without HU_ENABLE_CARTESIA the
     * legacy path ignores them. Stated here so -Werror builds of every preset
     * agree on the signature. */
    (void)agent;
    (void)combined;
    (void)combined_len;
    (void)bth_hour;
    (void)voice_first;
    /* SHIELD parity: the text path runs moderation/crisis, companion safety and
     * claim hedging inside `if (!sent_voice …)` in daemon.c, so a voice memo would
     * skip all three. Decline voice unless every gate is clear; the caller then
     * delivers the reply through the text path, which applies them. */
    if (!voice_gates_pass(alloc, response, response_len, combined, combined_len, "reply"))
        return false;
    /* A memo cannot carry a link: speaking "I'll send you the link" would
     * promise a send that never happens. Text goes, link intact. */
    if (hu_speech_has_url(response, response_len))
        return false;
    /* F1 S2: speak the spoken form of the reply — texting shorthand expanded,
     * narrated actions and emoji removed. Nothing speakable: text goes. The
     * S1 rewrite (HU_SPEECH_REWRITE) waits for voice_spoken_final. */
    hu_speech_result_t sp;
    (void)hu_speech_prepare(alloc, NULL, NULL, 0, NULL, HU_SPEECH_REWRITE_OFF, response,
                            response_len, combined, combined_len, &sp);
    if (sp.spoken_len == 0)
        return false;
    /* ~6 KB of memo state: heap, not the service loop's stack. */
    voice_final_t *vf = alloc->alloc(alloc->ctx, sizeof(*vf));
    if (!vf)
        return false; /* text goes */
    memset(vf, 0, sizeof(*vf));
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
            /* A #voice self-test logs "self_test" so it never starts the spacing gap. */
            const char *vreason = hu_daemon_voice_first_reply_reason(voice_first);
            /* Voice-first LIVE already decided from what arrived, and the turn
             * wrote a memo; the post-hoc classifier would judge it as a text.
             * The safety gates above still ran. */
            hu_voice_decision_t vdec =
                (voice_first == HU_VOICE_FIRST_FORCED ||
                 (voice_first && hu_voice_intent_memo_shaped(response, response_len)))
                    ? HU_VOICE_SEND_VOICE
                    : hu_voice_decision_classify_ex(response, response_len, combined, combined_len,
                                                    &agent->persona->voice_messages, true, bth_hour,
                                                    (uint32_t)(time(NULL) ^ (uintptr_t)combined),
                                                    &vreason);
            if (vdec == HU_VOICE_SEND_VOICE) {
                const char *cartesia_key = hu_config_get_provider_key(config, "cartesia");
                if (cartesia_key && cartesia_key[0] &&
                    voice_spoken_final(alloc, agent, batch_key, key_len, response, response_len,
                                       combined, combined_len, &sp, vf)) {
                    hu_voice_reply_request_t req;
                    hu_error_t prep_err =
                        vf->directed
                            ? hu_voice_reply_build_request_directed(
                                  &agent->persona->voice, vf->rendered, vf->rendered_len,
                                  vf->emotion[0] ? vf->emotion : NULL, vf->sentences, &req)
                            : hu_voice_reply_build_request_ex(&agent->persona->voice, sp.spoken,
                                                              sp.spoken_len, combined, combined_len,
                                                              bth_hour, (uint32_t)time(NULL),
                                                              sp.laughter_cue, &req);
                    unsigned char *audio_bytes = NULL;
                    size_t audio_len = 0;
                    hu_error_t tts_err = prep_err;
                    const char *lo_prev = hu_local_only_set_caller("voice");
                    if (prep_err == HU_OK)
                        tts_err = hu_cartesia_tts_synthesize(
                            alloc, cartesia_key, strlen(cartesia_key), req.transcript,
                            req.transcript_len, &req.tts, hu_tts_format_for_channel(chn_voice),
                            &audio_bytes, &audio_len);
                    (void)hu_local_only_set_caller(lo_prev);
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
            daemon_voice_record_decision(agent, batch_key, key_len, vdec == HU_VOICE_SEND_VOICE,
                                         vreason, sent_voice);
        }
#endif
        /* Fallback: unified voice pipeline when persona Cartesia path did not send.
         */
        if (!sent_voice && voice_channel_ok && !unified_voice_active && config) {
            hu_voice_config_t voice_cfg = {0};
            if (hu_voice_config_from_settings(config, &voice_cfg) == HU_OK &&
                voice_cfg.tts_provider && voice_cfg.tts_provider[0] &&
                voice_spoken_final(alloc, agent, batch_key, key_len, response, response_len,
                                   combined, combined_len, &sp, vf)) {
                void *audio = NULL;
                size_t audio_len = 0;
                /* Tags mean something only to Cartesia; others get the words. */
                bool tags_ok = strcmp(voice_cfg.tts_provider, "cartesia") == 0 &&
                               (!voice_cfg.tts_model || !voice_cfg.tts_model[0] ||
                                strncmp(voice_cfg.tts_model, "sonic-3", 7) == 0);
                const char *say = vf->directed ? (tags_ok ? vf->rendered : vf->words) : sp.spoken;
                size_t say_len =
                    vf->directed ? (tags_ok ? vf->rendered_len : vf->words_len) : sp.spoken_len;
                const char *lo_prev = hu_local_only_set_caller("voice");
                hu_error_t tts_err =
                    hu_voice_tts(alloc, &voice_cfg, say, say_len, &audio, &audio_len);
                (void)hu_local_only_set_caller(lo_prev);
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
    alloc->free(alloc->ctx, vf, sizeof(*vf));
    return sent_voice;
}
