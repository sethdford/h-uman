#include "human/agent.h"
#include "human/channels/imessage_voice_record.h"
#include "human/context/voice_intent.h"
#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/daemon/voice_first.h"
#include "human/persona.h"
#if defined(HU_ENABLE_SQLITE)
#include "human/memory.h"
#include "human/memory/proactive_decisions_repo.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* What the turn is told when it is writing a memo. Seth's own memos run
 * 20-31 s / 47-58 words (MV7 calibration 2026-09-27); "cheesy" was greeting-card
 * lines and announcements (Betty memo, 2026-09-28); "you told me" trips the
 * claim-language gate and sends the memo back to text. */
static const char k_memo_directive[] =
    "VOICE MEMO: this reply goes out as a voice memo in your own voice, so say it the way "
    "you'd talk: about 45-80 words, a few connected thoughts, answering what they actually "
    "said. You can ask about something from their life if it appears above; bring it up "
    "naturally (\"how's the new job going?\"), never as \"you told me\" or \"you said\". "
    "Share news from your own life only if it is stated above; never invent plans, events, "
    "places, people or numbers. Keep it low-key and understated: no greeting-card lines "
    "(\"hope you have a wonderful day\"), no announcements, no hype, at most one "
    "exclamation. This overrides the texting length rules and any short length cue in the scene "
    "direction.\n";

#if defined(HU_ENABLE_SQLITE)
static sqlite3 *memory_db(struct hu_agent *agent) {
    return agent && agent->memory ? hu_sqlite_memory_get_db(agent->memory) : NULL;
}
#endif

static int64_t secs_since_last_memo(struct hu_agent *agent, const char *contact) {
#if defined(HU_ENABLE_SQLITE)
    sqlite3 *db = memory_db(agent);
    int64_t ts = -1;
    if (!db || !contact[0] ||
        hu_proactive_decisions_repo_last_sent_ts(db, contact, "voice_reply", &ts) != HU_OK ||
        ts < 0)
        return -1;
    int64_t d = (int64_t)time(NULL) - ts;
    return d < 0 ? 0 : d;
#else
    (void)agent;
    (void)contact;
    return -1;
#endif
}

static void record_decision(struct hu_agent *agent, const char *contact,
                            const hu_daemon_voice_first_t *vf) {
#if defined(HU_ENABLE_SQLITE)
    sqlite3 *db = memory_db(agent);
    if (db)
        (void)hu_proactive_decisions_repo_record(
            db, (int64_t)time(NULL), contact[0] ? contact : NULL, "voice_first",
            vf->decision == HU_VOICE_SEND_VOICE ? HU_PROACTIVE_DECISION_SEND
                                                : HU_PROACTIVE_DECISION_DECLINE,
            vf->reason, 0, NULL);
#else
    (void)agent;
    (void)contact;
    (void)vf;
#endif
}

/* Prepend the directive; on allocation failure the turn simply stays a text. */
static bool prepend_directive(hu_allocator_t *alloc, char **ctx, size_t *ctx_len) {
    size_t dl = sizeof(k_memo_directive) - 1;
    size_t old = (*ctx && *ctx_len) ? *ctx_len : 0;
    char *merged = alloc->alloc(alloc->ctx, dl + old + 1);
    if (!merged)
        return false;
    memcpy(merged, k_memo_directive, dl);
    if (old)
        memcpy(merged + dl, *ctx, old);
    merged[dl + old] = '\0';
    if (*ctx)
        alloc->free(alloc->ctx, *ctx, *ctx_len + 1);
    *ctx = merged;
    *ctx_len = dl + old;
    return true;
}

/* LIVE writes memos only for the family list; an unset list means nobody,
 * never everybody. */
static bool contact_listed(const char *key, size_t key_len) {
    const char *allow = getenv("HU_VOICE_DELIVERY_ONLY");
    return key && allow && allow[0] && hu_voice_record_handle_allowed(allow, key, key_len);
}

void hu_daemon_voice_first_prepare(hu_allocator_t *alloc, struct hu_agent *agent,
                                   const char *batch_key, size_t key_len, bool is_group, bool force,
                                   const char *inbound, size_t inbound_len, char **convo_ctx,
                                   size_t *convo_ctx_len, uint32_t *max_chars,
                                   hu_daemon_voice_first_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->decision = HU_VOICE_SEND_TEXT;
    out->reason = "off";
    /* HU_VOICE_FIRST LIVE as a default is gated on the spec's measurement:
     * memos drawing replies at least as often as texts over >= 10 memos, plus
     * Mindy's W5 real-or-clone rating. Do not flip without both. */
    hu_gate_mode_t mode = hu_gate_mode_from_env("HU_VOICE_FIRST", HU_GATE_OFF);
    if (mode == HU_GATE_OFF || !alloc || !agent || !agent->persona)
        return;
    /* A memo goes to the sender's handle, not the group (review C1). */
    if (is_group) {
        out->reason = "group";
        return;
    }
    char contact[128];
    size_t n = batch_key ? (key_len < sizeof(contact) - 1 ? key_len : sizeof(contact) - 1) : 0;
    memcpy(contact, batch_key ? batch_key : "", n);
    contact[n] = '\0';

    hu_voice_intent_facts_t facts = {
        .inbound = inbound,
        .inbound_len = inbound ? inbound_len : 0,
        .cfg = &agent->persona->voice_messages,
        .has_voice_id = agent->persona->voice.voice_id[0] != '\0',
        .secs_since_last_memo = secs_since_last_memo(agent, contact),
        .min_gap_sec = hu_voice_intent_parse_gap(getenv("HU_VOICE_MIN_GAP_SEC")),
    };
    if (force && facts.has_voice_id && facts.cfg && facts.cfg->enabled) {
        out->decision = HU_VOICE_SEND_VOICE; /* #voice from Seth's own number */
        out->reason = "self_test";
    } else {
        out->decision = hu_voice_intent_decide(&facts, &out->reason);
    }

    bool listed = contact_listed(contact, n);
    if (mode == HU_GATE_LIVE && out->decision == HU_VOICE_SEND_VOICE && listed && convo_ctx &&
        convo_ctx_len && max_chars && prepend_directive(alloc, convo_ctx, convo_ctx_len)) {
        *max_chars = HU_VOICE_FIRST_MEMO_MAX_CHARS;
        out->memo = true;
    }
    hu_log_info("voice_first", NULL, "voice_first %s: decision=%s reason=%s memo=%d",
                mode == HU_GATE_LIVE ? "live" : "shadow",
                out->decision == HU_VOICE_SEND_VOICE ? "voice" : "text", out->reason,
                out->memo ? 1 : 0);
    record_decision(agent, contact, out);
}

bool hu_daemon_voice_first_available(struct hu_agent *agent, const char *batch_key, size_t key_len,
                                     bool is_group) {
    if (is_group || !agent || !agent->persona || !agent->persona->voice_messages.enabled ||
        !agent->persona->voice.voice_id[0])
        return false;
    if (hu_gate_mode_from_env("HU_VOICE_FIRST", HU_GATE_OFF) != HU_GATE_LIVE)
        return false;
    return contact_listed(batch_key, key_len);
}
