/* src/daemon_imessage_observer.c
 *
 * Phase 3 completion: poll chat.db for the enriched-event columns
 * (payload_data, message_summary_info, group_action_type, balloon_bundle_id)
 * that the existing iMessage channel poll does NOT read, and route them
 * into hu_personal_model_t.
 *
 * Architectural note: this lives next to daemon_reaction_poll.c rather
 * than inside src/channels/imessage.c because it shares the daemon-tick
 * + watermark + personal-model-setter pattern from reaction_poll.
 * Keeping them adjacent makes the wiring symmetry obvious. */

#include "human/daemon_imessage_observer.h"

#include "human/channels/imessage_ingest.h"
#include "human/memory/personal_model.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#endif

/* Daemon-owned personal_model handle. NULL until wire_personal_model is
 * called. */
static hu_personal_model_t *s_observer_pm = NULL;

void hu_daemon_imessage_observer_wire_personal_model(struct hu_personal_model *model) {
    s_observer_pm = (hu_personal_model_t *)model;
}

#if HU_IS_TEST || !defined(__APPLE__) || !defined(__MACH__) || !defined(HU_ENABLE_SQLITE)

/* Stub: same convention as the reaction_poll stub — returns OK and does
 * nothing on test/non-Apple/no-SQLite builds. Real path is exercised
 * only in production binaries. */

hu_error_t hu_daemon_imessage_observer_tick(const hu_config_t *cfg, int64_t since_unix,
                                            int64_t *watermark_inout, size_t *out_ingested) {
    (void)cfg;
    (void)since_unix;
    (void)watermark_inout;
    if (out_ingested)
        *out_ingested = 0;
    return HU_OK;
}

hu_error_t hu_daemon_tick_imessage_observer(const hu_config_t *cfg, int64_t now_unix,
                                            int64_t *last_poll_unix_inout,
                                            int64_t *watermark_inout) {
    (void)cfg;
    (void)now_unix;
    (void)last_poll_unix_inout;
    (void)watermark_inout;
    return HU_OK;
}

#else

/* Apple mac_time epoch offset (2001-01-01 UTC → Unix). */
#define HU_MAC_EPOCH_OFFSET 978307200LL

/* Cached schema probe. We need to know which columns are present per
 * macOS version so the SELECT doesn't fail on Catalina/Big Sur builds
 * lacking message_summary_info or group_action_type. */
typedef struct {
    bool probed;
    bool has_payload_data;
    bool has_message_summary_info;
    bool has_group_action_type;
    bool has_group_title;
    bool has_balloon_bundle_id;
    bool has_date_edited;
} schema_caps_t;

static schema_caps_t s_caps = {0};

static void probe_schema_once(sqlite3 *db) {
    if (s_caps.probed)
        return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(message)", -1, &st, NULL) != SQLITE_OK) {
        s_caps.probed = true; /* avoid retrying every tick */
        return;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 1);
        if (!name)
            continue;
        const char *n = (const char *)name;
        if (strcmp(n, "payload_data") == 0)
            s_caps.has_payload_data = true;
        else if (strcmp(n, "message_summary_info") == 0)
            s_caps.has_message_summary_info = true;
        else if (strcmp(n, "group_action_type") == 0)
            s_caps.has_group_action_type = true;
        else if (strcmp(n, "group_title") == 0)
            s_caps.has_group_title = true;
        else if (strcmp(n, "balloon_bundle_id") == 0)
            s_caps.has_balloon_bundle_id = true;
        else if (strcmp(n, "date_edited") == 0)
            s_caps.has_date_edited = true;
    }
    sqlite3_finalize(st);
    s_caps.probed = true;
}

/* Build a SELECT that includes only the columns the schema actually has.
 * Output positions are stable: text(0) handle(1) chat_guid(2) date(3) is_from_me(4)
 * balloon(5) payload(6) summary(7) group_action(8) group_title(9).
 * Missing columns return NULL via "NULL AS column" placeholders. */
static const char *build_observer_sql(void) {
    static char sql[2048];
    static bool built = false;
    if (built)
        return sql;
    snprintf(sql, sizeof(sql),
             "SELECT m.text, h.id, c.guid, m.date, m.is_from_me, "
             "       %s, %s, %s, %s, %s, "
             "       (SELECT COUNT(*) FROM chat_handle_join WHERE chat_id = c.ROWID) "
             "FROM message m "
             "JOIN chat_message_join cmj ON cmj.message_id = m.ROWID "
             "JOIN chat c ON c.ROWID = cmj.chat_id "
             "LEFT JOIN handle h ON h.ROWID = m.handle_id "
             "WHERE m.date > ((? - %lld) * 1000000000) "
             "  AND m.associated_message_type = 0 "
             "ORDER BY m.date ASC LIMIT ?",
             s_caps.has_balloon_bundle_id ? "m.balloon_bundle_id" : "NULL",
             s_caps.has_payload_data ? "m.payload_data" : "NULL",
             s_caps.has_message_summary_info ? "m.message_summary_info" : "NULL",
             s_caps.has_group_action_type ? "m.group_action_type" : "NULL",
             s_caps.has_group_title ? "m.group_title" : "NULL", HU_MAC_EPOCH_OFFSET);
    built = true;
    return sql;
}

/* Process one chat.db row through the appropriate extractor + ingest.
 * Returns 1 if anything was ingested, 0 otherwise (no signal in this row). */
static int process_row(sqlite3_stmt *st, hu_personal_model_t *pm) {
    const unsigned char *handle = sqlite3_column_text(st, 1);
    const unsigned char *chat_guid = sqlite3_column_text(st, 2);
    int64_t mac_ns = sqlite3_column_int64(st, 3);
    int is_from_me = sqlite3_column_int(st, 4);
    const unsigned char *balloon = sqlite3_column_text(st, 5);
    const unsigned char *payload = sqlite3_column_blob(st, 6);
    int payload_len = sqlite3_column_bytes(st, 6);
    const unsigned char *summary = sqlite3_column_blob(st, 7);
    int summary_len = sqlite3_column_bytes(st, 7);
    int group_action = sqlite3_column_int(st, 8);
    const unsigned char *group_title = sqlite3_column_text(st, 9);
    int participant_count = sqlite3_column_int(st, 10);

    int64_t ts_unix = (mac_ns / 1000000000) + HU_MAC_EPOCH_OFFSET;
    bool in_group = participant_count > 2;
    const char *sender = handle ? (const char *)handle : NULL;
    int signaled = 0;
    (void)chat_guid;

    /* 1. Group event — high-signal social-graph data ("Alice renamed
     *    the group to 'Hiking Trip 2026'"). associated_message_type=0
     *    plus group_action_type > 0 marks a structural event row.
     *    Action types per Apple's docs: 0=NoChange, 1=NameChange,
     *    2=ParticipantAdded, 3=ParticipantLeft, 4=AvatarChange. */
    if (group_action > 0) {
        char buf[256];
        const char *actor = is_from_me ? "I" : (sender ? sender : "someone");
        const char *title = group_title ? (const char *)group_title : "";
        const char *verb = "modified the group";
        switch (group_action) {
        case 1:
            verb = "renamed the group to";
            break;
        case 2:
            verb = "added a member to the group";
            break;
        case 3:
            verb = "left the group";
            break;
        case 4:
            verb = "changed the group photo";
            break;
        }
        if (title[0])
            snprintf(buf, sizeof(buf), "%s %s \"%s\".", actor, verb, title);
        else
            snprintf(buf, sizeof(buf), "%s %s.", actor, verb);
        /* Reuse the edit-shaped ingest since it carries actor + text +
         * provenance — group events are conceptually similar narrations. */
        (void)hu_imessage_ingest_edit(pm, sender, is_from_me != 0, NULL, buf, ts_unix, in_group);
        signaled = 1;
    }

    /* 2. Edit chain — Apple keeps up to 5 edits per part; we extract
     *    each delta into its own ingest call so the personal model
     *    sees the full "softened wording → final" progression. */
    if (summary && summary_len > 0) {
        char chain[8 * 256];
        size_t n_edits =
            hu_imessage_extract_edit_chain(summary, (size_t)summary_len, chain, 8, 256);
        for (size_t i = 0; i < n_edits; i++) {
            const char *delta = chain + (i * 256);
            if (!delta[0])
                continue;
            const char *old_text = i > 0 ? (chain + ((i - 1) * 256)) : NULL;
            (void)hu_imessage_ingest_edit(pm, sender, is_from_me != 0, old_text, delta, ts_unix,
                                          in_group);
            signaled = 1;
        }
    }

    /* 3. Balloon (app-message) payloads — audio transcripts get the
     *    highest priority because their detail string is real linguistic
     *    signal, not metadata. Other balloon kinds get a stub detail
     *    pending Phase 5 per-balloon decoders. */
    if (balloon && balloon[0]) {
        hu_imessage_balloon_kind_t kind =
            hu_imessage_balloon_kind_from_bundle_id((const char *)balloon);
        if (kind == HU_IMESSAGE_BALLOON_AUDIO_TRANSCRIPT && payload && payload_len > 0) {
            char transcript[1024];
            size_t tlen = hu_imessage_extract_audio_transcript(payload, (size_t)payload_len,
                                                               transcript, sizeof(transcript));
            if (tlen > 0) {
                (void)hu_imessage_ingest_balloon(pm, sender, is_from_me != 0, kind, transcript,
                                                 ts_unix, in_group);
                signaled = 1;
            }
        } else if (kind != HU_IMESSAGE_BALLOON_UNKNOWN) {
            /* Non-audio balloon: emit a generic narration. Phase 5
             * decoders will plug in detail strings. */
            (void)hu_imessage_ingest_balloon(pm, sender, is_from_me != 0, kind, NULL, ts_unix,
                                             in_group);
            signaled = 1;
        }
    }

    return signaled;
}

static const char *resolve_chatdb_path(const hu_config_t *cfg) {
    if (cfg && cfg->reaction_collection.chatdb_path[0])
        return cfg->reaction_collection.chatdb_path;
    const char *env = getenv("HU_CHATDB");
    if (env && env[0])
        return env;
    static char home_path[512];
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return NULL;
    snprintf(home_path, sizeof(home_path), "%s/Library/Messages/chat.db", home);
    return home_path;
}

hu_error_t hu_daemon_imessage_observer_tick(const hu_config_t *cfg, int64_t since_unix,
                                            int64_t *watermark_inout, size_t *out_ingested) {
    if (out_ingested)
        *out_ingested = 0;
    if (!s_observer_pm)
        return HU_OK; /* nothing wired; no-op */

    const char *db_path = resolve_chatdb_path(cfg);
    if (!db_path || !db_path[0])
        return HU_OK;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }
    probe_schema_once(db);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, build_observer_sql(), -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }
    sqlite3_bind_int64(st, 1, since_unix);
    sqlite3_bind_int(st, 2, 64); /* hard cap per tick */

    size_t ingested = 0;
    int64_t max_ts = since_unix;
    while (sqlite3_step(st) == SQLITE_ROW) {
        int64_t mac_ns = sqlite3_column_int64(st, 3);
        int64_t ts_unix = (mac_ns / 1000000000) + HU_MAC_EPOCH_OFFSET;
        if (ts_unix > max_ts)
            max_ts = ts_unix;
        if (process_row(st, s_observer_pm))
            ingested++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    if (watermark_inout && max_ts > *watermark_inout)
        *watermark_inout = max_ts;
    if (out_ingested)
        *out_ingested = ingested;
    return HU_OK;
}

hu_error_t hu_daemon_tick_imessage_observer(const hu_config_t *cfg, int64_t now_unix,
                                            int64_t *last_poll_unix_inout,
                                            int64_t *watermark_inout) {
    if (!cfg || !last_poll_unix_inout || !watermark_inout)
        return HU_ERR_INVALID_ARGUMENT;
    /* Same 30-second cadence as reaction_poll by default. The
     * observer's work is read-only on chat.db and bounded by the
     * 64-row LIMIT in build_observer_sql. */
    int interval = cfg->reaction_collection.poll_interval_seconds > 0
                       ? cfg->reaction_collection.poll_interval_seconds
                       : 30;
    if (*last_poll_unix_inout > 0 && now_unix - *last_poll_unix_inout < interval)
        return HU_OK;
    *last_poll_unix_inout = now_unix;
    return hu_daemon_imessage_observer_tick(cfg, *watermark_inout, watermark_inout, NULL);
}

#endif /* !HU_IS_TEST && defined(__APPLE__) ... */
