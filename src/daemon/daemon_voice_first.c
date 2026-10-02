#include "human/agent.h"
#include "human/channels/imessage_voice_record.h"
#include "human/context/voice_intent.h"
#include "human/context/voice_triggers.h"
#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/daemon/reactive_turn.h"
#include "human/daemon/share_queue.h"
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
        hu_proactive_decisions_repo_last_sent_ts_except(db, contact, "voice_reply",
                                                        HU_VOICE_SELF_TEST_REASON, &ts) != HU_OK ||
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

/* The cap ledger is per mode: SHADOW counts its own would-voices
 * ("voice_v2_shadow") so a shadow week simulates the cap without using up the
 * LIVE one ("voice_v2"), which counts only memos LIVE actually had the turn
 * write (review of #576). */
#if defined(HU_ENABLE_SQLITE)
static const char *v2_ledger(hu_gate_mode_t mode) {
    return mode == HU_GATE_LIVE ? "voice_v2" : "voice_v2_shadow";
}
#endif

/* v2 memos to this contact in the last week. Fails CLOSED: with no decision
 * log, or a read error, the cap counts as reached and v2 never chooses voice. */
static uint32_t v2_memos_this_week(struct hu_agent *agent, hu_gate_mode_t mode,
                                   const char *contact) {
#if defined(HU_ENABLE_SQLITE)
    sqlite3 *db = memory_db(agent);
    int64_t n = 0;
    if (!db || !contact[0] ||
        hu_proactive_decisions_repo_count_since(
            db, contact, v2_ledger(mode), HU_PROACTIVE_DECISION_SEND,
            (int64_t)time(NULL) - HU_VOICE_V2_WEEK_SEC, &n) != HU_OK)
        return UINT32_MAX;
    return n > 0 ? (uint32_t)n : 0;
#else
    (void)agent;
    (void)mode;
    (void)contact;
    return UINT32_MAX;
#endif
}

static void record_v2(struct hu_agent *agent, hu_gate_mode_t mode, const char *contact, bool send,
                      const char *why) {
#if defined(HU_ENABLE_SQLITE)
    sqlite3 *db = memory_db(agent);
    if (db && contact[0])
        (void)hu_proactive_decisions_repo_record(
            db, (int64_t)time(NULL), contact, v2_ledger(mode),
            send ? HU_PROACTIVE_DECISION_SEND : HU_PROACTIVE_DECISION_DECLINE, why, 0, NULL);
#else
    (void)agent;
    (void)mode;
    (void)contact;
    (void)send;
    (void)why;
#endif
}

/* A 16-bit tag for the shadow log, enough to count per contact. It is
 * pseudonymous, not anonymous: anyone holding the contact list can hash it and
 * match every tag. It only keeps the handle itself out of the log. */
static unsigned contact_tag(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return (unsigned)((h ^ (h >> 16)) & 0xffffu);
}

/* The owner's local time: the clock of the machine the daemon runs on. */
static int local_minute_now(void) {
    time_t t = time(NULL);
    struct tm tmv;
    if (!localtime_r(&t, &tmv))
        return -1;
    return tmv.tm_hour * 60 + tmv.tm_min;
}

typedef struct {
    bool evaluated; /* the base decision was no_trigger */
    bool voice;     /* v2 chose voice (after the cap) */
    const char *why;
} v2_outcome_t;

/* Voice triggers v2: only where the base decision found no trigger, so audio,
 * logistics and spacing still win. SHADOW logs the would-voice decision; LIVE
 * lets it choose voice. HU_VOICE_TRIGGERS_V2 LIVE is gated on the shadow
 * would-voice rate for close, non-owner contacts landing in 5-12% of replies
 * and Seth reviewing the first shadow-picked moments; see
 * docs/guides/voice-triggers.md. */
static v2_outcome_t voice_v2_apply(struct hu_agent *agent, hu_gate_mode_t mode, const char *contact,
                                   size_t contact_len, const char *inbound, size_t inbound_len,
                                   const hu_reactive_turn_ctx_t *rt, hu_daemon_voice_first_t *out) {
    /* One line per decision, so the line count is the rate's denominator. */
    v2_outcome_t o = {.evaluated = strcmp(out->reason, "no_trigger") == 0, .why = "base"};
    const hu_contact_profile_t *cp = hu_persona_find_contact(agent->persona, contact, contact_len);
    hu_voice_v2_facts_t f = {
        .inbound = inbound,
        .inbound_len = inbound ? inbound_len : 0,
        .local_minute = local_minute_now(),
        .close_contact = hu_voice_v2_close_contact(cp),
        .secs_since_owner_reply = rt ? hu_daemon_voice_first_secs_since_owner_reply(
                                           rt->history_entries, rt->history_count, time(NULL))
                                     : -1,
        .weekly_cap = hu_voice_v2_parse_weekly_cap(getenv("HU_VOICE_V2_WEEKLY_CAP")),
    };
    const char *pre = "-";
    if (o.evaluated) {
        pre = hu_voice_v2_trigger(&f);
        if (pre)
            f.v2_memos_this_week = v2_memos_this_week(agent, mode, contact);
        o.voice = hu_voice_v2_decide(&f, &o.why);
        if (!pre)
            pre = "none";
    } else {
        o.voice = out->decision == HU_VOICE_SEND_VOICE;
    }
    char week[16] = "-";
    if (o.evaluated && strcmp(pre, "none") != 0) {
        if (f.v2_memos_this_week == UINT32_MAX)
            snprintf(week, sizeof(week), "unknown");
        else
            snprintf(week, sizeof(week), "%u", f.v2_memos_this_week);
    }
    hu_log_info("voice_first", NULL,
                "[HU_VOICE_TRIGGERS_V2 %s] base=%s reason=%s pre=%s would_voice=%d contact=%04x "
                "owner=%d close=%d week=%s",
                mode == HU_GATE_LIVE ? "live" : "shadow", out->reason, o.why, pre, o.voice ? 1 : 0,
                contact_tag(contact, contact_len),
                hu_share_is_owner(agent->persona, contact, contact_len) ? 1 : 0,
                f.close_contact ? 1 : 0, week);
    if (o.evaluated && mode == HU_GATE_LIVE && o.voice) {
        out->decision = HU_VOICE_SEND_VOICE;
        out->reason = o.why;
    }
    return o;
}

void hu_daemon_voice_first_prepare(hu_allocator_t *alloc, struct hu_agent *agent,
                                   const char *batch_key, size_t key_len, bool is_group, bool force,
                                   const char *inbound, size_t inbound_len, char **convo_ctx,
                                   size_t *convo_ctx_len, uint32_t *max_chars,
                                   const struct hu_reactive_turn_ctx *rt,
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
    hu_gate_mode_t v2 = HU_GATE_OFF;
    v2_outcome_t v2o = {0};
    if (force && facts.has_voice_id && facts.cfg && facts.cfg->enabled) {
        out->decision = HU_VOICE_SEND_VOICE; /* #voice from Seth's own number */
        out->reason = "self_test";
    } else {
        out->decision = hu_voice_intent_decide(&facts, &out->reason);
        v2 = hu_gate_mode_from_env("HU_VOICE_TRIGGERS_V2", HU_GATE_OFF);
        if (v2 != HU_GATE_OFF)
            v2o = voice_v2_apply(agent, v2, contact, n, inbound, inbound_len, rt, out);
    }

    bool listed = contact_listed(contact, n);
    if (mode == HU_GATE_LIVE && out->decision == HU_VOICE_SEND_VOICE && listed && convo_ctx &&
        convo_ctx_len && max_chars && prepend_directive(alloc, convo_ctx, convo_ctx_len)) {
        *max_chars = HU_VOICE_FIRST_MEMO_MAX_CHARS;
        out->memo = true;
    }
    /* SHADOW books every would-voice in its own ledger; LIVE books only a memo
     * this turn will actually write (on the family list, voice-first LIVE). */
    if (v2o.evaluated)
        record_v2(agent, v2, contact, v2 == HU_GATE_LIVE ? (v2o.voice && out->memo) : v2o.voice,
                  v2o.why);
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

void hu_daemon_voice_first_direction(char *direction, size_t cap) {
    static const char k_memo[] =
        "Voice memo, a few connected thoughts (45-80 words), not a one-liner";
    if (!direction || cap == 0)
        return;
    char objective[512];
    snprintf(objective, sizeof(objective), "%s", direction);
    if (objective[0])
        snprintf(direction, cap, "%s: %s", k_memo, objective);
    else
        snprintf(direction, cap, "%s", k_memo);
}

const char *hu_daemon_voice_first_reply_reason(int voice_first) {
    return voice_first == HU_VOICE_FIRST_FORCED ? HU_VOICE_SELF_TEST_REASON : "voice_first";
}

/* chat.db history stamps are "YYYY-MM-DD HH:MM:SS" in local time. */
static int64_t parse_local_stamp(const char *ts) {
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    char tail = 0;
    if (sscanf(ts, "%4d-%2d-%2d %2d:%2d:%2d%c", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday,
               &tmv.tm_hour, &tmv.tm_min, &tmv.tm_sec, &tail) != 6)
        return -1;
    tmv.tm_year -= 1900;
    tmv.tm_mon -= 1;
    tmv.tm_isdst = -1;
    time_t t = mktime(&tmv);
    return t == (time_t)-1 ? -1 : (int64_t)t;
}

int64_t hu_daemon_voice_first_secs_since_owner_reply(const hu_channel_history_entry_t *history,
                                                     size_t count, int64_t now) {
    for (size_t i = count; history && i > 0; i--) {
        if (!history[i - 1].from_me)
            continue;
        int64_t t = parse_local_stamp(history[i - 1].timestamp);
        if (t >= 0)
            return now > t ? now - t : 0;
    }
    return -1;
}
