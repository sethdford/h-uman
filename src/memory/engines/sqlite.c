#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/platform.h"
#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "human/memory/encrypted_store.h"
#include "human/memory/entropy_gate.h"
#include "human/memory/graph_index.h"
#include "human/memory/sql_common.h"

#define HU_SQLITE_BUSY_TIMEOUT_MS 5000

typedef struct hu_sqlite_memory {
    sqlite3 *db;
    hu_allocator_t *alloc;
    hu_graph_index_t graph_index;
    hu_graph_hierarchy_t graph_hierarchy;
    bool graph_initialized;
    bool graph_hierarchy_ready;
    /* W15 envelope encryption opt-in. When `encrypt_at_rest` is true
     * and `keystore` is non-NULL+unlocked, impl_store wraps `content`
     * via hu_encrypted_store_wrap before INSERT and read_entry_from_row
     * unwraps it on SELECT. Legacy plaintext rows (no magic header)
     * are passed through unchanged so an upgrade can flip the flag
     * without re-writing the database. */
    hu_keystore_t *keystore;
    bool encrypt_at_rest;
} hu_sqlite_memory_t;

static const char *const schema_parts[] = {
    "CREATE TABLE IF NOT EXISTS memories("
    "id TEXT PRIMARY KEY,key TEXT NOT NULL UNIQUE,"
    "content TEXT NOT NULL,category TEXT NOT NULL DEFAULT'core',"
    "session_id TEXT,source TEXT,created_at TEXT NOT NULL,updated_at TEXT NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_memories_category ON memories(category)",
    "CREATE INDEX IF NOT EXISTS idx_memories_key ON memories(key)",
    "CREATE INDEX IF NOT EXISTS idx_memories_session ON memories(session_id)",
    "CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts USING fts5("
    "key,content,content=memories,content_rowid=rowid)",
    "CREATE TRIGGER IF NOT EXISTS memories_ai AFTER INSERT ON memories BEGIN "
    "INSERT INTO memories_fts(rowid,key,content)VALUES(new.rowid,new.key,new.content);END",
    "CREATE TRIGGER IF NOT EXISTS memories_ad AFTER DELETE ON memories BEGIN "
    "INSERT INTO memories_fts(memories_fts,rowid,key,content)"
    "VALUES('delete',old.rowid,old.key,old.content);END",
    "CREATE TRIGGER IF NOT EXISTS memories_au AFTER UPDATE ON memories BEGIN "
    "INSERT INTO memories_fts(memories_fts,rowid,key,content)"
    "VALUES('delete',old.rowid,old.key,old.content);"
    "INSERT INTO memories_fts(rowid,key,content)"
    "VALUES(new.rowid,new.key,new.content);END",
    "CREATE TABLE IF NOT EXISTS messages("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "session_id TEXT NOT NULL,role TEXT NOT NULL,"
    "content TEXT NOT NULL,created_at TEXT DEFAULT(datetime('now')))",
    "CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id)",
    "CREATE TABLE IF NOT EXISTS kv(key TEXT PRIMARY KEY,value TEXT NOT NULL)",
    "CREATE TABLE IF NOT EXISTS emotional_moments("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id TEXT NOT NULL,"
    "topic TEXT NOT NULL,"
    "emotion TEXT NOT NULL,"
    "intensity REAL,"
    "created_at INTEGER NOT NULL,"
    "follow_up_date INTEGER,"
    "followed_up INTEGER DEFAULT 0)",
    "CREATE INDEX IF NOT EXISTS idx_emotional_moments_contact ON emotional_moments(contact_id)",
    "CREATE INDEX IF NOT EXISTS idx_emotional_moments_follow_up ON "
    "emotional_moments(follow_up_date)",
    "CREATE TABLE IF NOT EXISTS comfort_patterns("
    "contact_id TEXT NOT NULL,"
    "emotion TEXT NOT NULL,"
    "response_type TEXT NOT NULL,"
    "engagement_score REAL,"
    "sample_count INTEGER DEFAULT 0,"
    "PRIMARY KEY (contact_id, emotion, response_type))",
    "CREATE TABLE IF NOT EXISTS inside_jokes("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id TEXT NOT NULL,"
    "context TEXT NOT NULL,"
    "punchline TEXT,"
    "created_at INTEGER NOT NULL,"
    "last_referenced INTEGER,"
    "reference_count INTEGER DEFAULT 0)",
    "CREATE INDEX IF NOT EXISTS idx_inside_jokes_contact ON inside_jokes(contact_id)",
    "CREATE TABLE IF NOT EXISTS commitments("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id TEXT NOT NULL,"
    "description TEXT NOT NULL,"
    "who TEXT NOT NULL,"
    "deadline INTEGER,"
    "status TEXT DEFAULT 'pending',"
    "created_at INTEGER NOT NULL,"
    "followed_up_at INTEGER)",
    "CREATE INDEX IF NOT EXISTS idx_commitments_contact ON commitments(contact_id)",
    "CREATE INDEX IF NOT EXISTS idx_commitments_deadline ON commitments(deadline) WHERE "
    "status='pending'",
    "CREATE TABLE IF NOT EXISTS temporal_patterns("
    "contact_id TEXT NOT NULL,"
    "day_of_week INTEGER NOT NULL,"
    "hour INTEGER NOT NULL,"
    "message_count INTEGER DEFAULT 0,"
    "avg_response_time_ms INTEGER,"
    "PRIMARY KEY (contact_id, day_of_week, hour))",
    "CREATE TABLE IF NOT EXISTS delayed_followups("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id TEXT NOT NULL,"
    "topic TEXT NOT NULL,"
    "scheduled_at INTEGER NOT NULL,"
    "sent INTEGER DEFAULT 0)",
    "CREATE INDEX IF NOT EXISTS idx_delayed_followups_contact ON delayed_followups(contact_id)",
    "CREATE INDEX IF NOT EXISTS idx_delayed_followups_scheduled ON delayed_followups(scheduled_at) "
    "WHERE sent=0",
    "CREATE TABLE IF NOT EXISTS avoidance_patterns("
    "contact_id TEXT NOT NULL,"
    "topic TEXT NOT NULL,"
    "mention_count INTEGER DEFAULT 0,"
    "change_count INTEGER DEFAULT 0,"
    "last_mentioned INTEGER,"
    "PRIMARY KEY (contact_id, topic))",
    "CREATE TABLE IF NOT EXISTS topic_baselines("
    "contact_id TEXT NOT NULL,"
    "topic TEXT NOT NULL,"
    "mention_count INTEGER DEFAULT 0,"
    "last_mentioned INTEGER,"
    "PRIMARY KEY (contact_id, topic))",
    "CREATE TABLE IF NOT EXISTS micro_moments("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id TEXT NOT NULL,"
    "fact TEXT NOT NULL,"
    "significance TEXT,"
    "created_at INTEGER NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_micro_moments_contact ON micro_moments(contact_id)",
    "CREATE TABLE IF NOT EXISTS growth_milestones("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id TEXT NOT NULL,"
    "topic TEXT NOT NULL,"
    "before_state TEXT,"
    "after_state TEXT,"
    "created_at INTEGER NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_growth_milestones_contact ON growth_milestones(contact_id)",
    "CREATE TABLE IF NOT EXISTS pattern_observations("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id TEXT NOT NULL,"
    "topic TEXT NOT NULL,"
    "tone TEXT NOT NULL,"
    "day_of_week INTEGER,"
    "hour INTEGER,"
    "observed_at INTEGER NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_pattern_observations_contact ON "
    "pattern_observations(contact_id)",
    "CREATE TABLE IF NOT EXISTS style_fingerprints("
    "contact_id TEXT NOT NULL PRIMARY KEY,"
    "uses_lowercase INTEGER DEFAULT 0,"
    "uses_periods INTEGER DEFAULT 0,"
    "laugh_style TEXT,"
    "avg_message_length INTEGER,"
    "common_phrases TEXT,"
    "distinctive_words TEXT,"
    "updated_at INTEGER)",
    "CREATE TABLE IF NOT EXISTS contact_baselines("
    "contact_id TEXT PRIMARY KEY,"
    "avg_message_length REAL,"
    "avg_response_time_ms REAL,"
    "emoji_frequency REAL,"
    "topic_diversity REAL,"
    "sentiment_baseline REAL,"
    "messages_sampled INTEGER,"
    "updated_at INTEGER)",
    "CREATE TABLE IF NOT EXISTS contact_style_evolution("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id TEXT NOT NULL,"
    "response_length INTEGER,"
    "formality REAL,"
    "used_emoji INTEGER,"
    "asked_question INTEGER,"
    "recorded_at INTEGER NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_style_evo_contact ON contact_style_evolution(contact_id)",
    "CREATE TABLE IF NOT EXISTS mood_log("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "mood TEXT,"
    "intensity REAL,"
    "cause TEXT,"
    "set_at INTEGER,"
    "decayed_at INTEGER)",
    "CREATE INDEX IF NOT EXISTS idx_mood_log_set_at ON mood_log(set_at)",
    "CREATE TABLE IF NOT EXISTS self_awareness_stats("
    "contact_id TEXT PRIMARY KEY,"
    "messages_sent_week INTEGER DEFAULT 0,"
    "initiations_week INTEGER DEFAULT 0,"
    "last_topic TEXT,"
    "topic_repeat_count INTEGER DEFAULT 0,"
    "updated_at INTEGER)",
    "CREATE TABLE IF NOT EXISTS reciprocity_scores("
    "contact_id TEXT NOT NULL,"
    "metric TEXT NOT NULL,"
    "value REAL,"
    "updated_at INTEGER,"
    "PRIMARY KEY (contact_id, metric))",
    "CREATE TABLE IF NOT EXISTS opinions("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "topic TEXT,"
    "position TEXT,"
    "confidence REAL,"
    "first_expressed INTEGER,"
    "last_expressed INTEGER,"
    "superseded_by INTEGER)",
    "CREATE INDEX IF NOT EXISTS idx_opinions_topic ON opinions(topic)",
    "CREATE TABLE IF NOT EXISTS life_chapters("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "theme TEXT,"
    "mood TEXT,"
    "started_at INTEGER,"
    "ended_at INTEGER,"
    "key_threads TEXT,"
    "active INTEGER)",
    "CREATE INDEX IF NOT EXISTS idx_life_chapters_active ON life_chapters(active)",
    "CREATE TABLE IF NOT EXISTS emotional_predictions("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id TEXT,"
    "predicted_emotion TEXT,"
    "confidence REAL,"
    "basis TEXT,"
    "target_date INTEGER,"
    "verified INTEGER)",
    "CREATE INDEX IF NOT EXISTS idx_emotional_predictions_contact ON "
    "emotional_predictions(contact_id)",
    "CREATE TABLE IF NOT EXISTS boundaries("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id TEXT,"
    "topic TEXT,"
    "type TEXT DEFAULT 'avoid',"
    "set_at INTEGER,"
    "source TEXT)",
    "CREATE INDEX IF NOT EXISTS idx_boundaries_contact ON boundaries(contact_id)",
    "CREATE TABLE IF NOT EXISTS contact_relationships("
    "contact_id TEXT NOT NULL,"
    "person_name TEXT NOT NULL,"
    "role TEXT NOT NULL,"
    "last_mentioned INTEGER,"
    "notes TEXT,"
    "PRIMARY KEY (contact_id, person_name))",
    "CREATE TABLE IF NOT EXISTS episodes("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id TEXT NOT NULL,"
    "summary TEXT NOT NULL,"
    "emotional_arc TEXT,"
    "key_moments TEXT,"
    "impact_score REAL DEFAULT 0.5,"
    "salience_score REAL DEFAULT 0.5,"
    "last_reinforced_at INTEGER,"
    "source TEXT DEFAULT 'conversation',"
    "created_at INTEGER NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_episodes_contact ON episodes(contact_id)",
    "CREATE INDEX IF NOT EXISTS idx_episodes_created ON episodes(created_at)",
    "CREATE TABLE IF NOT EXISTS prospective_memories("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "trigger_type TEXT NOT NULL,"
    "trigger_value TEXT NOT NULL,"
    "action TEXT NOT NULL,"
    "contact_id TEXT,"
    "expires_at INTEGER,"
    "fired INTEGER DEFAULT 0,"
    "created_at INTEGER NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_prospective_trigger ON prospective_memories(trigger_type, "
    "trigger_value)",
    "CREATE INDEX IF NOT EXISTS idx_prospective_expires ON prospective_memories(expires_at)",
    "CREATE TABLE IF NOT EXISTS prospective_tasks("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "description TEXT NOT NULL,"
    "trigger_type TEXT NOT NULL,"
    "trigger_value TEXT NOT NULL,"
    "priority REAL DEFAULT 0.5,"
    "fired INTEGER DEFAULT 0,"
    "created_at INTEGER NOT NULL,"
    "fired_at INTEGER)",
    "CREATE INDEX IF NOT EXISTS idx_prospective_tasks_trigger ON prospective_tasks(trigger_type, "
    "trigger_value) WHERE fired=0",
    "CREATE TABLE IF NOT EXISTS emotional_residue("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "episode_id INTEGER,"
    "contact_id TEXT NOT NULL,"
    "valence REAL NOT NULL,"
    "intensity REAL NOT NULL,"
    "decay_rate REAL DEFAULT 0.1,"
    "created_at INTEGER NOT NULL,"
    "FOREIGN KEY (episode_id) REFERENCES episodes(id))",
    "CREATE INDEX IF NOT EXISTS idx_emotional_residue_contact ON emotional_residue(contact_id)",
    "CREATE TABLE IF NOT EXISTS feed_items("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "source TEXT NOT NULL,"
    "contact_id TEXT,"
    "content_type TEXT NOT NULL,"
    "content TEXT NOT NULL,"
    "url TEXT,"
    "ingested_at INTEGER NOT NULL,"
    "referenced INTEGER DEFAULT 0,"
    "cluster_id INTEGER DEFAULT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_feed_items_source ON feed_items(source)",
    "CREATE INDEX IF NOT EXISTS idx_feed_items_contact ON feed_items(contact_id)",
    "CREATE INDEX IF NOT EXISTS idx_feed_items_ingested ON feed_items(ingested_at)",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_feed_items_dedup ON feed_items(source, substr(content, "
    "1, 200))",
    "CREATE VIRTUAL TABLE IF NOT EXISTS feed_items_fts USING fts5(content, source, content_type, "
    "content=feed_items, content_rowid=id)",
    "CREATE TRIGGER IF NOT EXISTS feed_items_ai AFTER INSERT ON feed_items BEGIN INSERT INTO "
    "feed_items_fts(rowid, content, source, content_type) VALUES (new.id, new.content, new.source, "
    "new.content_type); END",
    "CREATE TRIGGER IF NOT EXISTS feed_items_ad AFTER DELETE ON feed_items BEGIN INSERT INTO "
    "feed_items_fts(feed_items_fts, rowid, content, source, content_type) VALUES ('delete', "
    "old.id, old.content, old.source, old.content_type); END",
    "CREATE TABLE IF NOT EXISTS research_findings("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "source TEXT,"
    "finding TEXT NOT NULL,"
    "relevance TEXT,"
    "priority TEXT DEFAULT 'MEDIUM',"
    "suggested_action TEXT,"
    "status TEXT DEFAULT 'pending',"
    "created_at INTEGER NOT NULL,"
    "acted_at INTEGER)",
    "CREATE INDEX IF NOT EXISTS idx_findings_status ON research_findings(status)",
    "CREATE INDEX IF NOT EXISTS idx_findings_priority ON research_findings(priority)",
    "CREATE TABLE IF NOT EXISTS oauth_tokens("
    "provider TEXT PRIMARY KEY,"
    "access_token TEXT NOT NULL,"
    "refresh_token TEXT,"
    "expires_at INTEGER,"
    "scope TEXT)",
    "CREATE TABLE IF NOT EXISTS skills("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "name TEXT NOT NULL,"
    "type TEXT NOT NULL,"
    "contact_id TEXT,"
    "trigger_conditions TEXT,"
    "strategy TEXT NOT NULL,"
    "success_rate REAL DEFAULT 0.5,"
    "attempts INTEGER DEFAULT 0,"
    "successes INTEGER DEFAULT 0,"
    "version INTEGER DEFAULT 1,"
    "origin TEXT NOT NULL,"
    "parent_skill_id INTEGER,"
    "created_at INTEGER NOT NULL,"
    "updated_at INTEGER,"
    "retired INTEGER DEFAULT 0)",
    "CREATE INDEX IF NOT EXISTS idx_skills_contact ON skills(contact_id)",
    "CREATE TABLE IF NOT EXISTS skill_attempts("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "skill_id INTEGER NOT NULL,"
    "contact_id TEXT NOT NULL,"
    "applied_at INTEGER NOT NULL,"
    "outcome_signal TEXT,"
    "outcome_evidence TEXT,"
    "context TEXT)",
    "CREATE INDEX IF NOT EXISTS idx_skill_attempts_skill ON skill_attempts(skill_id)",
    "CREATE TABLE IF NOT EXISTS skill_evolution("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "skill_id INTEGER NOT NULL,"
    "version INTEGER NOT NULL,"
    "strategy TEXT NOT NULL,"
    "success_rate REAL,"
    "evolved_at INTEGER NOT NULL,"
    "reason TEXT)",
    "CREATE INDEX IF NOT EXISTS idx_skill_evolution_skill ON skill_evolution(skill_id)",
    "CREATE TABLE IF NOT EXISTS behavioral_feedback("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "behavior_type TEXT NOT NULL,"
    "contact_id TEXT NOT NULL,"
    "signal TEXT NOT NULL,"
    "context TEXT,"
    "timestamp INTEGER NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_behavioral_feedback_contact ON behavioral_feedback(contact_id)",
    "CREATE INDEX IF NOT EXISTS idx_behavioral_feedback_timestamp ON "
    "behavioral_feedback(timestamp)",
    "CREATE TABLE IF NOT EXISTS self_evaluations("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "contact_id TEXT NOT NULL,"
    "week INTEGER NOT NULL,"
    "metrics TEXT NOT NULL,"
    "recommendations TEXT,"
    "created_at INTEGER NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_self_evaluations_contact ON self_evaluations(contact_id)",
    "CREATE TABLE IF NOT EXISTS general_lessons("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "lesson TEXT NOT NULL,"
    "confidence REAL DEFAULT 0.5,"
    "source_count INTEGER DEFAULT 1,"
    "first_learned INTEGER NOT NULL,"
    "last_confirmed INTEGER)",
    "CREATE TABLE IF NOT EXISTS cognitive_load_log("
    "id INTEGER PRIMARY KEY,"
    "capacity REAL NOT NULL,"
    "conversation_depth INTEGER DEFAULT 0,"
    "hour_of_day INTEGER NOT NULL,"
    "day_of_week INTEGER NOT NULL,"
    "physical_state TEXT,"
    "recorded_at INTEGER NOT NULL)",
    "CREATE TABLE IF NOT EXISTS active_threads("
    "id INTEGER PRIMARY KEY,"
    "contact_id TEXT NOT NULL,"
    "topic TEXT NOT NULL,"
    "status TEXT DEFAULT 'open',"
    "last_update_at INTEGER NOT NULL,"
    "created_at INTEGER NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_active_threads_contact_status ON active_threads(contact_id, "
    "status)",
    "CREATE TABLE IF NOT EXISTS interaction_quality("
    "id INTEGER PRIMARY KEY,"
    "contact_id TEXT NOT NULL,"
    "quality_score REAL NOT NULL,"
    "cognitive_load REAL,"
    "mood_state TEXT,"
    "recovery_sent INTEGER DEFAULT 0,"
    "recovery_at INTEGER,"
    "timestamp INTEGER NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_interaction_quality_contact_recovery ON "
    "interaction_quality(contact_id, recovery_sent)",
    "CREATE TABLE IF NOT EXISTS life_narration_events("
    "id INTEGER PRIMARY KEY,"
    "event_type TEXT NOT NULL,"
    "description TEXT NOT NULL,"
    "shareability_score REAL NOT NULL,"
    "shared_with TEXT,"
    "generated_at INTEGER NOT NULL,"
    "shared_at INTEGER)",
    "CREATE TABLE IF NOT EXISTS held_contradictions("
    "id INTEGER PRIMARY KEY,"
    "topic TEXT NOT NULL,"
    "position_a TEXT NOT NULL,"
    "position_b TEXT NOT NULL,"
    "expressed_a_count INTEGER DEFAULT 0,"
    "expressed_b_count INTEGER DEFAULT 0,"
    "created_at INTEGER NOT NULL)",
    "CREATE TABLE IF NOT EXISTS visual_content("
    "id INTEGER PRIMARY KEY,"
    "source TEXT NOT NULL,"
    "path TEXT NOT NULL,"
    "description TEXT,"
    "tags TEXT,"
    "location TEXT,"
    "captured_at INTEGER NOT NULL,"
    "indexed_at INTEGER NOT NULL,"
    "shared_with TEXT,"
    "share_count INTEGER DEFAULT 0)",
    "CREATE TABLE IF NOT EXISTS shareable_content("
    "id INTEGER PRIMARY KEY,"
    "content TEXT NOT NULL,"
    "source TEXT NOT NULL,"
    "topic TEXT,"
    "received_at INTEGER NOT NULL,"
    "share_score REAL DEFAULT 0,"
    "shared INTEGER DEFAULT 0)",
    "CREATE TABLE IF NOT EXISTS current_events("
    "id INTEGER PRIMARY KEY,"
    "topic TEXT NOT NULL,"
    "summary TEXT NOT NULL,"
    "source TEXT,"
    "published_at INTEGER NOT NULL,"
    "relevance REAL DEFAULT 0.5)",
    NULL};

static void get_timestamp(char *buf, size_t buf_size) {
    time_t t = time(NULL);
    struct tm tm_buf;
    struct tm *tm = hu_platform_gmtime_r(&t, &tm_buf);
    if (tm) {
        strftime(buf, buf_size, "%Y-%m-%dT%H:%M:%SZ", tm);
    } else {
        snprintf(buf, buf_size, "%ld", (long)t);
    }
}

static char *generate_id(hu_allocator_t *alloc) {
    static unsigned long counter = 0;
    char ts[32];
    get_timestamp(ts, sizeof(ts));
    return hu_sprintf(alloc, "mem_%ld_%lu_%s", (long)time(NULL), ++counter, ts);
}

static const char *category_to_string(const hu_memory_category_t *cat) {
    if (!cat)
        return "core";
    switch (cat->tag) {
    case HU_MEMORY_CATEGORY_CORE:
        return "core";
    case HU_MEMORY_CATEGORY_DAILY:
        return "daily";
    case HU_MEMORY_CATEGORY_CONVERSATION:
        return "conversation";
    case HU_MEMORY_CATEGORY_INSIGHT:
        return "insight";
    case HU_MEMORY_CATEGORY_CUSTOM:
        if (cat->data.custom.name && cat->data.custom.name_len > 0)
            return cat->data.custom.name;
        return "custom";
    default:
        return "core";
    }
}

/* Decrypt a row's content column when it carries the W15 envelope
 * magic and `self` has an attached keystore. Legacy plaintext rows
 * (no magic header) and rows read while encryption is OFF are passed
 * through unchanged.
 *
 * Returns true on a successful decrypt; `*out_content` then points
 * to a heap buffer (NUL-terminated by the keystore) and `*out_len`
 * is the plaintext length — the caller frees with
 * `alloc->free(alloc->ctx, *out_content, *out_len + 1)`.
 *
 * Returns false when the caller should fall through to the legacy
 * `hu_strndup` path. We never lose data: a decrypt failure on a
 * wrapped row falls through too so the envelope bytes are surfaced
 * verbatim instead of dropping the row silently. */
static bool maybe_decrypt_content(hu_sqlite_memory_t *self, hu_allocator_t *alloc,
                                  const void *raw, size_t raw_len,
                                  char **out_content, size_t *out_len) {
    if (!self || !self->keystore || !self->encrypt_at_rest)
        return false;
    if (!hu_encrypted_store_is_encrypted(raw, raw_len))
        return false;

    void *pt = NULL;
    size_t pt_len = 0;
    hu_error_t err = hu_encrypted_store_unwrap(self->keystore, raw, raw_len, &pt, &pt_len);
    if (err != HU_OK || !pt) {
        if (pt)
            alloc->free(alloc->ctx, pt, pt_len + 1);
        return false;
    }
    *out_content = (char *)pt;
    *out_len = pt_len;
    return true;
}

static hu_error_t read_entry_from_row(sqlite3_stmt *stmt, hu_allocator_t *alloc,
                                      hu_sqlite_memory_t *self,
                                      hu_memory_entry_t *out) {
    const char *id_p = (const char *)sqlite3_column_text(stmt, 0);
    const char *key_p = (const char *)sqlite3_column_text(stmt, 1);
    /* Use _blob accessors for `content` so the W15 envelope (which
     * may contain NUL bytes inside the keystore payload) round-trips
     * cleanly even when written into a TEXT column. */
    const void *content_blob = sqlite3_column_blob(stmt, 2);
    const char *category_p = (const char *)sqlite3_column_text(stmt, 3);
    const char *timestamp_p = (const char *)sqlite3_column_text(stmt, 4);
    const char *session_id_p = (const char *)sqlite3_column_text(stmt, 5);
    const char *source_p = (const char *)sqlite3_column_text(stmt, 6);

    size_t id_len = id_p ? (size_t)sqlite3_column_bytes(stmt, 0) : 0;
    size_t key_len = key_p ? (size_t)sqlite3_column_bytes(stmt, 1) : 0;
    size_t content_len = content_blob ? (size_t)sqlite3_column_bytes(stmt, 2) : 0;
    size_t timestamp_len = timestamp_p ? (size_t)sqlite3_column_bytes(stmt, 4) : 0;
    size_t session_id_len = session_id_p ? (size_t)sqlite3_column_bytes(stmt, 5) : 0;
    size_t source_len = source_p ? (size_t)sqlite3_column_bytes(stmt, 6) : 0;

    out->id = id_p ? hu_strndup(alloc, id_p, id_len) : NULL;
    out->id_len = id_len;
    out->key = key_p ? hu_strndup(alloc, key_p, key_len) : NULL;
    out->key_len = key_len;

    char *decrypted = NULL;
    size_t decrypted_len = 0;
    if (maybe_decrypt_content(self, alloc, content_blob, content_len,
                              &decrypted, &decrypted_len)) {
        out->content = decrypted;
        out->content_len = decrypted_len;
    } else {
        out->content = content_blob ? hu_strndup(alloc, (const char *)content_blob,
                                                 content_len)
                                    : NULL;
        out->content_len = content_len;
    }
    out->category.tag = HU_MEMORY_CATEGORY_CUSTOM;
    out->category.data.custom.name =
        category_p
            ? hu_strndup(alloc, category_p, category_p ? (size_t)sqlite3_column_bytes(stmt, 3) : 0)
            : NULL;
    out->category.data.custom.name_len = category_p ? (size_t)sqlite3_column_bytes(stmt, 3) : 0;
    out->timestamp = timestamp_p ? hu_strndup(alloc, timestamp_p, timestamp_len) : NULL;
    out->timestamp_len = timestamp_len;
    out->session_id = session_id_p ? hu_strndup(alloc, session_id_p, session_id_len) : NULL;
    out->session_id_len = session_id_len;
    out->source = source_p ? hu_strndup(alloc, source_p, source_len) : NULL;
    out->source_len = source_len;

    /* SOTA-2026 init-09: trust_tier + provenance columns read when present.
     * Existing SELECT lists (id, key, content, category, created_at,
     * session_id, source[, score]) DO NOT include these columns yet, so
     * keep the defaults aligned with the M5 migration's safe baseline. */
    out->trust_tier = (int)HU_TRUST_THIRD_PARTY;
    out->provenance = NULL;
    out->provenance_len = 0;

    if (sqlite3_column_count(stmt) > 7) {
        out->score = sqlite3_column_double(stmt, 7);
    } else {
        out->score = NAN;
    }
    return HU_OK;
}

static void free_entry(hu_allocator_t *alloc, hu_memory_entry_t *e) {
    hu_memory_entry_free_fields(alloc, e);
}

static const char *impl_name(void *ctx) {
    (void)ctx;
    return "sqlite";
}

static hu_error_t impl_store(void *ctx, const char *key, size_t key_len, const char *content,
                             size_t content_len, const hu_memory_category_t *category,
                             const char *session_id, size_t session_id_len) {
    hu_sqlite_memory_t *self = (hu_sqlite_memory_t *)ctx;
    char ts[64];
    get_timestamp(ts, sizeof(ts));

    char *id = generate_id(self->alloc);
    if (!id)
        return HU_ERR_OUT_OF_MEMORY;

    /* W15 envelope encryption (opt-in). Wrap `content` before bind
     * so the on-disk row is opaque without the per-user master key.
     * On wrap failure (locked keystore, OOM) we fail the store rather
     * than silently writing plaintext — better to surface the error
     * than to violate the user's encrypt-at-rest expectation. */
    void *wrapped = NULL;
    size_t wrapped_len = 0;
    const char *bind_content = content;
    int bind_content_len = (int)content_len;
    if (self->encrypt_at_rest && self->keystore && content) {
        hu_error_t enc_err = hu_encrypted_store_wrap(
            self->keystore, self->alloc, content, content_len, &wrapped, &wrapped_len);
        if (enc_err != HU_OK) {
            hu_str_free(self->alloc, id);
            return enc_err;
        }
        bind_content = (const char *)wrapped;
        bind_content_len = (int)wrapped_len;
    }

    const char *cat_str = category_to_string(category);
    const char *sql =
        "INSERT INTO memories (id, key, content, category, session_id, created_at, updated_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) "
        "ON CONFLICT(key) DO UPDATE SET "
        "content = excluded.content, category = excluded.category, "
        "session_id = excluded.session_id, updated_at = excluded.updated_at";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        hu_str_free(self->alloc, id);
        if (wrapped)
            self->alloc->free(self->alloc->ctx, wrapped, wrapped_len);
        return HU_ERR_MEMORY_STORE;
    }

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, key, (int)key_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, bind_content, bind_content_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, cat_str, -1, SQLITE_STATIC);
    if (session_id && session_id_len > 0)
        sqlite3_bind_text(stmt, 5, session_id, (int)session_id_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 5);
    sqlite3_bind_text(stmt, 6, ts, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, ts, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    hu_str_free(self->alloc, id);
    if (wrapped)
        self->alloc->free(self->alloc->ctx, wrapped, wrapped_len);

    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;

    /* Feed into MAGMA graph index for multi-dimensional reranking */
    if (self->graph_initialized && content && content_len > 0) {
        (void)hu_graph_index_add(&self->graph_index, key, key_len, content, content_len,
                                 (int64_t)time(NULL));
        if (self->graph_hierarchy_ready)
            (void)hu_graph_hierarchy_build(&self->graph_hierarchy, &self->graph_index);
    }

    return HU_OK;
}

static hu_error_t impl_store_ex(void *ctx, const char *key, size_t key_len, const char *content,
                                size_t content_len, const hu_memory_category_t *category,
                                const char *session_id, size_t session_id_len,
                                const hu_memory_store_opts_t *opts) {
    hu_sqlite_memory_t *self = (hu_sqlite_memory_t *)ctx;
    char ts[64];
    get_timestamp(ts, sizeof(ts));
    char *id = generate_id(self->alloc);
    if (!id)
        return HU_ERR_OUT_OF_MEMORY;
    const char *cat_str = category_to_string(category);
    const char *sql = "INSERT INTO memories (id, key, content, category, session_id, source, "
                      "created_at, updated_at) "
                      "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
                      "ON CONFLICT(key) DO UPDATE SET "
                      "content = excluded.content, category = excluded.category, "
                      "session_id = excluded.session_id, source = excluded.source, updated_at = "
                      "excluded.updated_at";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        hu_str_free(self->alloc, id);
        return HU_ERR_MEMORY_STORE;
    }
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, key, (int)key_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, content, (int)content_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, cat_str, -1, SQLITE_STATIC);
    if (session_id && session_id_len > 0)
        sqlite3_bind_text(stmt, 5, session_id, (int)session_id_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 5);
    if (opts && opts->source && opts->source_len > 0)
        sqlite3_bind_text(stmt, 6, opts->source, (int)opts->source_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 6);
    sqlite3_bind_text(stmt, 7, ts, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, ts, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    hu_str_free(self->alloc, id);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;

    if (self->graph_initialized && content && content_len > 0) {
        (void)hu_graph_index_add(&self->graph_index, key, key_len, content, content_len,
                                 (int64_t)time(NULL));
        if (self->graph_hierarchy_ready)
            (void)hu_graph_hierarchy_build(&self->graph_hierarchy, &self->graph_index);
    }
    return HU_OK;
}

static hu_error_t impl_recall(void *ctx, hu_allocator_t *alloc, const char *query, size_t query_len,
                              size_t limit, const char *session_id, size_t session_id_len,
                              hu_memory_entry_t **out, size_t *out_count) {
    hu_sqlite_memory_t *self = (hu_sqlite_memory_t *)ctx;
    *out = NULL;
    *out_count = 0;

    if (!query || query_len == 0) {
        *out = NULL;
        return HU_OK;
    }

    /* FTS5 BM25 search - build query from words */
    char fts_buf[512];
    size_t fts_len = 0;
    const char *p = query;
    const char *end = query + query_len;
    bool first = true;
    while (p < end && fts_len < sizeof(fts_buf) - 10) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
        if (p >= end)
            break;
        const char *word_start = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
            p++;
        if (p > word_start) {
            /* Escape double quotes in words for FTS5 MATCH query */
            char escaped_word[256];
            size_t ew_len = 0;
            for (const char *c = word_start; c < p && ew_len < sizeof(escaped_word) - 2; c++) {
                if (*c == '"') {
                    if (ew_len < sizeof(escaped_word) - 3) {
                        escaped_word[ew_len++] = '"';
                        escaped_word[ew_len++] = '"';
                    }
                } else {
                    escaped_word[ew_len++] = *c;
                }
            }
            escaped_word[ew_len] = '\0';
            if (!first)
                fts_len = hu_buf_appendf(fts_buf, sizeof(fts_buf), fts_len, " OR ");
            fts_len = hu_buf_appendf(fts_buf, sizeof(fts_buf), fts_len, "\"%s\"", escaped_word);
            first = false;
        }
    }

    /* Try FTS5 first; fall back to LIKE when FTS returns no rows */
    if (fts_len > 0) {
        const char *sql =
            "SELECT m.id, m.key, m.content, m.category, m.created_at, m.session_id, m.source, "
            "bm25(memories_fts) as score FROM memories_fts f "
            "JOIN memories m ON m.rowid = f.rowid "
            "WHERE memories_fts MATCH ?1 "
            "ORDER BY score LIMIT ?2";
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, fts_buf, (int)fts_len, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 2, (sqlite3_int64)limit);

            hu_memory_entry_t *entries =
                (hu_memory_entry_t *)alloc->alloc(alloc->ctx, limit * sizeof(hu_memory_entry_t));
            if (!entries) {
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            size_t count = 0;

            while (sqlite3_step(stmt) == SQLITE_ROW && count < limit) {
                hu_memory_entry_t *e = &entries[count];
                read_entry_from_row(stmt, alloc, self, e);
                if (session_id && session_id_len > 0 && e->session_id &&
                    (e->session_id_len != session_id_len ||
                     memcmp(e->session_id, session_id, session_id_len) != 0)) {
                    free_entry(alloc, e);
                    continue;
                }
                count++;
            }
            sqlite3_finalize(stmt);
            if (count > 0) {
                /* MAGMA graph reranking: boost results connected by entity/temporal edges */
                if (self->graph_initialized && count > 1) {
                    const char **rkeys =
                        (const char **)alloc->alloc(alloc->ctx, count * sizeof(const char *));
                    size_t *rkl = (size_t *)alloc->alloc(alloc->ctx, count * sizeof(size_t));
                    double *scores = (double *)alloc->alloc(alloc->ctx, count * sizeof(double));
                    if (rkeys && rkl && scores) {
                        for (size_t ri = 0; ri < count; ri++) {
                            rkeys[ri] = entries[ri].key;
                            rkl[ri] = entries[ri].key_len;
                            scores[ri] = isnan(entries[ri].score) ? 0.5 : entries[ri].score;
                        }
                        (void)hu_graph_index_rerank(&self->graph_index, query, query_len, rkeys,
                                                    rkl, scores, count);
                        for (size_t ri = 0; ri < count; ri++)
                            entries[ri].score = scores[ri];
                    }
                    if (rkeys)
                        alloc->free(alloc->ctx, (void *)rkeys, count * sizeof(const char *));
                    if (rkl)
                        alloc->free(alloc->ctx, rkl, count * sizeof(size_t));
                    if (scores)
                        alloc->free(alloc->ctx, scores, count * sizeof(double));
                }

                /* Spreading activation: discover entity-connected memories beyond FTS.
                 * Requires >=2 seed results to avoid noise from single weak matches
                 * (single FTS hits often come from cross-context content that will be
                 * filtered by contact/session isolation). */
                if (self->graph_initialized && count >= 2 && count < limit) {
                    const size_t seed_alloc_count = count;
                    uint32_t *seeds =
                        (uint32_t *)alloc->alloc(alloc->ctx, seed_alloc_count * sizeof(uint32_t));
                    size_t seed_count = 0;
                    if (seeds) {
                        for (size_t si = 0; si < count && si < self->graph_index.node_count; si++) {
                            for (uint32_t ni = 0; ni < self->graph_index.node_count; ni++) {
                                if (self->graph_index.nodes[ni].memory_key_len ==
                                        entries[si].key_len &&
                                    memcmp(self->graph_index.nodes[ni].memory_key, entries[si].key,
                                           entries[si].key_len) == 0) {
                                    seeds[seed_count++] = ni;
                                    break;
                                }
                            }
                        }
                        if (seed_count > 0) {
                            hu_spread_activation_config_t sa_cfg;
                            hu_spread_activation_config_default(&sa_cfg);
                            size_t max_act = limit - count;
                            if (max_act > sa_cfg.max_activated)
                                max_act = sa_cfg.max_activated;
                            hu_activated_node_t *activated = (hu_activated_node_t *)alloc->alloc(
                                alloc->ctx, max_act * sizeof(hu_activated_node_t));
                            if (activated) {
                                size_t act_count = 0;
                                sa_cfg.max_activated = max_act;
                                if (hu_graph_index_spread_activation(&self->graph_index, &sa_cfg,
                                                                     seeds, seed_count, activated,
                                                                     &act_count) == HU_OK &&
                                    act_count > 0) {
                                    for (size_t ai = 0; ai < act_count && count < limit; ai++) {
                                        uint32_t ni = activated[ai].node_idx;
                                        if (ni >= self->graph_index.node_count)
                                            continue;
                                        const hu_graph_node_t *gn = &self->graph_index.nodes[ni];
                                        bool dup = false;
                                        for (size_t di = 0; di < count; di++) {
                                            if (entries[di].key_len == gn->memory_key_len &&
                                                memcmp(entries[di].key, gn->memory_key,
                                                       gn->memory_key_len) == 0) {
                                                dup = true;
                                                break;
                                            }
                                        }
                                        if (dup)
                                            continue;
                                        /* Fetch the activated memory from SQLite */
                                        sqlite3_stmt *sa_stmt = NULL;
                                        if (sqlite3_prepare_v2(
                                                self->db,
                                                "SELECT id, key, content, category, "
                                                "created_at, session_id, source "
                                                "FROM memories WHERE key = ?1 LIMIT 1",
                                                -1, &sa_stmt, NULL) == SQLITE_OK) {
                                            sqlite3_bind_text(sa_stmt, 1, gn->memory_key,
                                                              (int)gn->memory_key_len, NULL);
                                            if (sqlite3_step(sa_stmt) == SQLITE_ROW) {
                                                hu_memory_entry_t *e = &entries[count];
                                                read_entry_from_row(sa_stmt, alloc, self, e);
                                                bool session_ok = true;
                                                if (session_id && session_id_len > 0 &&
                                                    e->session_id &&
                                                    (e->session_id_len != session_id_len ||
                                                     memcmp(e->session_id, session_id,
                                                            session_id_len) != 0)) {
                                                    session_ok = false;
                                                }
                                                if (session_ok) {
                                                    e->score = activated[ai].energy * 0.5;
                                                    count++;
                                                } else {
                                                    free_entry(alloc, e);
                                                }
                                            }
                                            sqlite3_finalize(sa_stmt);
                                        }
                                    }
                                }
                                alloc->free(alloc->ctx, activated,
                                            max_act * sizeof(hu_activated_node_t));
                            }
                        }
                        alloc->free(alloc->ctx, seeds, seed_alloc_count * sizeof(uint32_t));
                    }
                }

                /* System-2 hierarchy: add cluster members when recall is still sparse.
                 * Require >=2 seed hits (same as spread activation) so single weak FTS rows
                 * cannot pull unrelated cluster members (e.g. misc bucket). */
                if (self->graph_initialized && self->graph_hierarchy_ready && count >= 2 &&
                    count < limit) {
                    uint32_t h_indices[64];
                    size_t hcnt = 0;
                    size_t want = limit - count;
                    if (want > 64)
                        want = 64;
                    if (hu_graph_hierarchy_traverse(&self->graph_hierarchy, &self->graph_index,
                                                    query, query_len, h_indices, &hcnt,
                                                    want) == HU_OK &&
                        hcnt > 0) {
                        for (size_t hi = 0; hi < hcnt && count < limit; hi++) {
                            uint32_t ni = h_indices[hi];
                            if (ni >= self->graph_index.node_count)
                                continue;
                            const hu_graph_node_t *gn = &self->graph_index.nodes[ni];
                            bool dup = false;
                            for (size_t di = 0; di < count; di++) {
                                if (entries[di].key_len == gn->memory_key_len &&
                                    memcmp(entries[di].key, gn->memory_key, gn->memory_key_len) ==
                                        0) {
                                    dup = true;
                                    break;
                                }
                            }
                            if (dup)
                                continue;
                            sqlite3_stmt *hi_stmt = NULL;
                            if (sqlite3_prepare_v2(self->db,
                                                   "SELECT id, key, content, category, "
                                                   "created_at, session_id, source "
                                                   "FROM memories WHERE key = ?1 LIMIT 1",
                                                   -1, &hi_stmt, NULL) == SQLITE_OK) {
                                sqlite3_bind_text(hi_stmt, 1, gn->memory_key,
                                                  (int)gn->memory_key_len, NULL);
                                if (sqlite3_step(hi_stmt) == SQLITE_ROW) {
                                    hu_memory_entry_t *e = &entries[count];
                                    read_entry_from_row(hi_stmt, alloc, self, e);
                                    bool session_ok = true;
                                    if (session_id && session_id_len > 0 && e->session_id &&
                                        (e->session_id_len != session_id_len ||
                                         memcmp(e->session_id, session_id, session_id_len) != 0)) {
                                        session_ok = false;
                                    }
                                    if (session_ok) {
                                        e->score = 0.35;
                                        count++;
                                    } else {
                                        free_entry(alloc, e);
                                    }
                                }
                                sqlite3_finalize(hi_stmt);
                            }
                        }
                    }
                }

                /* Entropy gate: drop low-information results */
                if (count > 2) {
                    hu_entropy_gate_config_t eg_cfg = hu_entropy_gate_config_default();
                    eg_cfg.threshold = 0.15;
                    size_t echunks_count = count;
                    hu_memory_chunk_t *echunks = (hu_memory_chunk_t *)alloc->alloc(
                        alloc->ctx, echunks_count * sizeof(hu_memory_chunk_t));
                    if (echunks) {
                        for (size_t ei = 0; ei < echunks_count; ei++) {
                            echunks[ei].text = entries[ei].content;
                            echunks[ei].text_len = entries[ei].content_len;
                            echunks[ei].entropy = 0.0;
                            echunks[ei].passed = true;
                        }
                        size_t passed = 0;
                        if (hu_entropy_gate_filter(&eg_cfg, echunks, echunks_count, &passed) ==
                                HU_OK &&
                            passed > 0 && passed < echunks_count) {
                            size_t wp = 0;
                            for (size_t ei = 0; ei < echunks_count; ei++) {
                                if (echunks[ei].passed) {
                                    if (wp != ei)
                                        entries[wp] = entries[ei];
                                    wp++;
                                } else {
                                    free_entry(alloc, &entries[ei]);
                                }
                            }
                            count = wp;
                        }
                        alloc->free(alloc->ctx, echunks,
                                    echunks_count * sizeof(hu_memory_chunk_t));
                    }
                }

                *out = entries;
                *out_count = count;
                return HU_OK;
            }
            alloc->free(alloc->ctx, entries, limit * sizeof(hu_memory_entry_t));
        } else {
            if (stmt)
                sqlite3_finalize(stmt);
        }
    }

    /* Fallback: LIKE search */
    char *like_pattern = (char *)alloc->alloc(alloc->ctx, query_len + 3);
    if (!like_pattern)
        return HU_ERR_OUT_OF_MEMORY;
    like_pattern[0] = '%';
    memcpy(like_pattern + 1, query, query_len);
    like_pattern[query_len + 1] = '%';
    like_pattern[query_len + 2] = '\0';

    const char *sql =
        "SELECT id, key, content, category, created_at, session_id, source "
        "FROM memories WHERE content LIKE ?1 OR key LIKE ?1 ORDER BY updated_at DESC LIMIT ?2";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        alloc->free(alloc->ctx, like_pattern, query_len + 3);
        return HU_ERR_MEMORY_RECALL;
    }

    sqlite3_bind_text(stmt, 1, like_pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)limit);

    hu_memory_entry_t *entries =
        (hu_memory_entry_t *)alloc->alloc(alloc->ctx, limit * sizeof(hu_memory_entry_t));
    if (!entries) {
        sqlite3_finalize(stmt);
        alloc->free(alloc->ctx, like_pattern, query_len + 3);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < limit) {
        read_entry_from_row(stmt, alloc, self, &entries[count]);
        if (session_id && session_id_len > 0 && entries[count].session_id &&
            (entries[count].session_id_len != session_id_len ||
             memcmp(entries[count].session_id, session_id, session_id_len) != 0)) {
            free_entry(alloc, &entries[count]);
            continue;
        }
        count++;
    }
    sqlite3_finalize(stmt);
    alloc->free(alloc->ctx, like_pattern, query_len + 3);
    if (count == 0) {
        /* No matches. Free the pre-allocated entries buffer so callers
         * can rely on count==0 meaning nothing to free. */
        alloc->free(alloc->ctx, entries, limit * sizeof(hu_memory_entry_t));
        *out = NULL;
        *out_count = 0;
        return HU_OK;
    }
    *out = entries;
    *out_count = count;
    return HU_OK;
}

static hu_error_t impl_get(void *ctx, hu_allocator_t *alloc, const char *key, size_t key_len,
                           hu_memory_entry_t *out, bool *found) {
    hu_sqlite_memory_t *self = (hu_sqlite_memory_t *)ctx;
    *found = false;

    const char *sql = "SELECT id, key, content, category, created_at, session_id, source "
                      "FROM memories WHERE key = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, key, (int)key_len, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        read_entry_from_row(stmt, alloc, self, out);
        *found = true;
    }
    sqlite3_finalize(stmt);
    return HU_OK;
}

static hu_error_t impl_list(void *ctx, hu_allocator_t *alloc, const hu_memory_category_t *category,
                            const char *session_id, size_t session_id_len, hu_memory_entry_t **out,
                            size_t *out_count) {
    hu_sqlite_memory_t *self = (hu_sqlite_memory_t *)ctx;
    const char *sql;
    if (category) {
        sql = "SELECT id, key, content, category, created_at, session_id, source "
              "FROM memories WHERE category = ?1 ORDER BY updated_at DESC";
    } else {
        sql = "SELECT id, key, content, category, created_at, session_id, source "
              "FROM memories ORDER BY updated_at DESC";
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    if (category)
        sqlite3_bind_text(stmt, 1, category_to_string(category), -1, SQLITE_STATIC);

    size_t cap = 64;
    hu_memory_entry_t *entries =
        (hu_memory_entry_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_memory_entry_t));
    if (!entries) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            hu_memory_entry_t *n = (hu_memory_entry_t *)alloc->realloc(
                alloc->ctx, entries, cap * sizeof(hu_memory_entry_t),
                (cap * 2) * sizeof(hu_memory_entry_t));
            if (!n)
                break;
            entries = n;
            cap *= 2;
        }
        read_entry_from_row(stmt, alloc, self, &entries[count]);
        if (session_id && session_id_len > 0 && entries[count].session_id &&
            (entries[count].session_id_len != session_id_len ||
             memcmp(entries[count].session_id, session_id, session_id_len) != 0)) {
            free_entry(alloc, &entries[count]);
            continue;
        }
        count++;
    }
    sqlite3_finalize(stmt);
    if (count == 0) {
        /* All rows filtered out (e.g. by session_id). Free the empty
         * entries buffer so callers can rely on the contract that
         * count==0 means there is nothing to free. */
        alloc->free(alloc->ctx, entries, cap * sizeof(hu_memory_entry_t));
        *out = NULL;
        *out_count = 0;
        return HU_OK;
    }
    if (count < cap) {
        hu_memory_entry_t *shrunk = (hu_memory_entry_t *)alloc->realloc(
            alloc->ctx, entries, cap * sizeof(hu_memory_entry_t),
            count * sizeof(hu_memory_entry_t));
        if (shrunk)
            entries = shrunk;
    }
    *out = entries;
    *out_count = count;
    return HU_OK;
}

static hu_error_t impl_forget(void *ctx, const char *key, size_t key_len, bool *deleted) {
    hu_sqlite_memory_t *self = (hu_sqlite_memory_t *)ctx;
    const char *sql = "DELETE FROM memories WHERE key = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, key, (int)key_len, SQLITE_STATIC);
    int step_rc = sqlite3_step(stmt);
    if (step_rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return HU_ERR_MEMORY_BACKEND;
    }
    *deleted = sqlite3_changes(self->db) > 0;
    sqlite3_finalize(stmt);
    return HU_OK;
}

static hu_error_t impl_count(void *ctx, size_t *out) {
    hu_sqlite_memory_t *self = (hu_sqlite_memory_t *)ctx;
    const char *sql = "SELECT COUNT(*) FROM memories";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        *out = (size_t)sqlite3_column_int64(stmt, 0);
    else
        *out = 0;
    sqlite3_finalize(stmt);
    return HU_OK;
}

static bool impl_health_check(void *ctx) {
    hu_sqlite_memory_t *self = (hu_sqlite_memory_t *)ctx;
    char *err = NULL;
    int rc = sqlite3_exec(self->db, "SELECT 1", NULL, NULL, &err);
    if (err)
        sqlite3_free(err);
    return rc == SQLITE_OK;
}

static void impl_deinit(void *ctx) {
    hu_sqlite_memory_t *self = (hu_sqlite_memory_t *)ctx;
    if (self->graph_hierarchy_ready)
        hu_graph_hierarchy_deinit(&self->graph_hierarchy);
    if (self->graph_initialized)
        hu_graph_index_deinit(&self->graph_index);
    if (self->db)
        sqlite3_close(self->db);
    self->alloc->free(self->alloc->ctx, self, sizeof(hu_sqlite_memory_t));
}

static const hu_memory_vtable_t sqlite_vtable = {
    .name = impl_name,
    .store = impl_store,
    .store_ex = impl_store_ex,
    .recall = impl_recall,
    .get = impl_get,
    .list = impl_list,
    .forget = impl_forget,
    .count = impl_count,
    .health_check = impl_health_check,
    .deinit = impl_deinit,
};

/* Session store implementation */
static hu_error_t impl_session_save_message(void *ctx, const char *session_id,
                                            size_t session_id_len, const char *role,
                                            size_t role_len, const char *content,
                                            size_t content_len) {
    hu_sqlite_memory_t *self = (hu_sqlite_memory_t *)ctx;
    const char *sql = "INSERT INTO messages (session_id, role, content) VALUES (?1, ?2, ?3)";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, session_id, (int)session_id_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, role, (int)role_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, content, (int)content_len, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_STORE;
}

static hu_error_t impl_session_load_messages(void *ctx, hu_allocator_t *alloc,
                                             const char *session_id, size_t session_id_len,
                                             hu_message_entry_t **out, size_t *out_count) {
    hu_sqlite_memory_t *self = (hu_sqlite_memory_t *)ctx;
    const char *sql = "SELECT role, content FROM messages WHERE session_id = ?1 ORDER BY id ASC";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, session_id, (int)session_id_len, SQLITE_STATIC);

    size_t cap = 32;
    hu_message_entry_t *entries =
        (hu_message_entry_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_message_entry_t));
    if (!entries) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            hu_message_entry_t *n = (hu_message_entry_t *)alloc->realloc(
                alloc->ctx, entries, cap * sizeof(hu_message_entry_t),
                (cap * 2) * sizeof(hu_message_entry_t));
            if (!n)
                break;
            entries = n;
            cap *= 2;
        }
        const char *role_p = (const char *)sqlite3_column_text(stmt, 0);
        const char *content_p = (const char *)sqlite3_column_text(stmt, 1);
        size_t rl = role_p ? (size_t)sqlite3_column_bytes(stmt, 0) : 0;
        size_t cl = content_p ? (size_t)sqlite3_column_bytes(stmt, 1) : 0;
        entries[count].role = role_p ? hu_strndup(alloc, role_p, rl) : NULL;
        entries[count].role_len = rl;
        entries[count].content = content_p ? hu_strndup(alloc, content_p, cl) : NULL;
        entries[count].content_len = cl;
        count++;
    }
    sqlite3_finalize(stmt);
    if (count == 0) {
        alloc->free(alloc->ctx, entries, cap * sizeof(hu_message_entry_t));
        *out = NULL;
        *out_count = 0;
        return HU_OK;
    }
    if (count < cap) {
        hu_message_entry_t *shrunk = (hu_message_entry_t *)alloc->realloc(
            alloc->ctx, entries, cap * sizeof(hu_message_entry_t),
            count * sizeof(hu_message_entry_t));
        if (shrunk)
            entries = shrunk;
    }
    *out = entries;
    *out_count = count;
    return HU_OK;
}

static hu_error_t impl_session_clear_messages(void *ctx, const char *session_id,
                                              size_t session_id_len) {
    hu_sqlite_memory_t *self = (hu_sqlite_memory_t *)ctx;
    const char *sql = "DELETE FROM messages WHERE session_id = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(stmt, 1, session_id, (int)session_id_len, SQLITE_STATIC);
    int step_rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (step_rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

static hu_error_t impl_session_clear_auto_saved(void *ctx, const char *session_id,
                                                size_t session_id_len) {
    hu_sqlite_memory_t *self = (hu_sqlite_memory_t *)ctx;
    const char *sql;
    if (session_id && session_id_len > 0)
        sql = "DELETE FROM memories WHERE key LIKE 'autosave_%' AND session_id = ?1";
    else
        sql = "DELETE FROM memories WHERE key LIKE 'autosave_%'";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    if (session_id && session_id_len > 0)
        sqlite3_bind_text(stmt, 1, session_id, (int)session_id_len, SQLITE_STATIC);
    int step_rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (step_rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

static const hu_session_store_vtable_t sqlite_session_vtable = {
    .save_message = impl_session_save_message,
    .load_messages = impl_session_load_messages,
    .clear_messages = impl_session_clear_messages,
    .clear_auto_saved = impl_session_clear_auto_saved,
};

hu_memory_t hu_sqlite_memory_create(hu_allocator_t *alloc, const char *db_path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(db_path ? db_path : ":memory:", &db);
    if (rc != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return (hu_memory_t){.ctx = NULL, .vtable = NULL};
    }
    sqlite3_busy_timeout(db, HU_SQLITE_BUSY_TIMEOUT_MS);
    sqlite3_exec(db, HU_SQL_PRAGMA_INIT, NULL, NULL, NULL);

    for (const char *const *part = schema_parts; *part; part++) {
        char *err = NULL;
        rc = sqlite3_exec(db, *part, NULL, NULL, &err);
        if (rc != SQLITE_OK) {
            if (err)
                sqlite3_free(err);
            sqlite3_close(db);
            return (hu_memory_t){.ctx = NULL, .vtable = NULL};
        }
    }

    {
        char *err = NULL;
        sqlite3_exec(db, "ALTER TABLE memories ADD COLUMN source TEXT", NULL, NULL, &err);
        if (err)
            sqlite3_free(err);
    }

    /* SOTA-2026 init-09 sec 2.8: Migration M5 - trust_tier + provenance.
     *
     * The DEFAULT 1 (HU_TRUST_THIRD_PARTY) is the safe, conservative
     * default for pre-migration rows. The initial design used DEFAULT 2
     * (FIRST_PARTY) and was flipped to THIRD_PARTY per adversarial
     * review finding B2: defaulting unclassified rows to FIRST_PARTY
     * would permanently stamp any already-ingested MINJA-poisoned fact
     * as high-trust. The sec 2.11 audit promotes rows whose source
     * unambiguously identifies a 1:1 channel after this default. */
    {
        char *e = NULL;
        sqlite3_exec(db,
                     "ALTER TABLE memories ADD COLUMN trust_tier "
                     "INTEGER NOT NULL DEFAULT 1",
                     NULL, NULL, &e);
        if (e)
            sqlite3_free(e);
    }
    {
        char *e = NULL;
        sqlite3_exec(db, "ALTER TABLE memories ADD COLUMN provenance TEXT",
                     NULL, NULL, &e);
        if (e)
            sqlite3_free(e);
    }
    /* sec 2.11 upgrade-on-positive-identification audit. Idempotent. */
    {
        const char *audit_q1 =
            "UPDATE memories SET trust_tier = 4 "
            "WHERE source IN ('cli','stdin','telegram_dm','discord_dm',"
            "'slack_dm','imessage_dm') AND trust_tier = 1";
        const char *audit_q2 =
            "UPDATE memories SET trust_tier = 2 "
            "WHERE source IN ('human','tool:human','self_email','calendar_self') "
            "AND trust_tier = 1";
        char *e = NULL;
        sqlite3_exec(db, audit_q1, NULL, NULL, &e);
        if (e) { sqlite3_free(e); e = NULL; }
        sqlite3_exec(db, audit_q2, NULL, NULL, &e);
        if (e) sqlite3_free(e);
    }

    hu_sqlite_memory_t *self =
        (hu_sqlite_memory_t *)alloc->alloc(alloc->ctx, sizeof(hu_sqlite_memory_t));
    if (!self) {
        sqlite3_close(db);
        return (hu_memory_t){.ctx = NULL, .vtable = NULL};
    }
    self->db = db;
    self->alloc = alloc;
    self->keystore = NULL;
    self->encrypt_at_rest = false;
    self->graph_initialized = (hu_graph_index_init(&self->graph_index, alloc) == HU_OK);
    self->graph_hierarchy_ready = false;
    if (self->graph_initialized)
        self->graph_hierarchy_ready =
            (hu_graph_hierarchy_init(&self->graph_hierarchy, alloc) == HU_OK);
    return (hu_memory_t){
        .ctx = self,
        .vtable = &sqlite_vtable,
    };
}

hu_error_t hu_sqlite_memory_attach_keystore(hu_memory_t *mem, hu_keystore_t *ks,
                                             bool encrypt_at_rest) {
    if (!mem || !mem->ctx || !mem->vtable)
        return HU_ERR_INVALID_ARGUMENT;
    const char *n = mem->vtable->name(mem->ctx);
    if (!n || strcmp(n, "sqlite") != 0)
        return HU_ERR_NOT_SUPPORTED;
    hu_sqlite_memory_t *self = (hu_sqlite_memory_t *)mem->ctx;
    self->keystore = ks;
    /* Encryption only takes effect when both flag and keystore are
     * set; allowing one without the other would either leave us
     * unable to wrap (no key) or unable to decrypt prior writes
     * (key but flag off). We refuse to enable the flag without a
     * keystore so the failure is loud at attach time. */
    if (encrypt_at_rest && !ks)
        return HU_ERR_INVALID_ARGUMENT;
    self->encrypt_at_rest = encrypt_at_rest;
    return HU_OK;
}

hu_session_store_t hu_sqlite_memory_get_session_store(hu_memory_t *mem) {
    if (!mem || !mem->ctx || !mem->vtable)
        return (hu_session_store_t){.ctx = NULL, .vtable = NULL};
    const char *n = mem->vtable->name(mem->ctx);
    if (!n || strcmp(n, "sqlite") != 0)
        return (hu_session_store_t){.ctx = NULL, .vtable = NULL};
    return (hu_session_store_t){
        .ctx = mem->ctx,
        .vtable = &sqlite_session_vtable,
    };
}

sqlite3 *hu_sqlite_memory_get_db(hu_memory_t *mem) {
    if (!mem || !mem->ctx || !mem->vtable)
        return NULL;
    const char *n = mem->vtable->name(mem->ctx);
    if (!n || strcmp(n, "sqlite") != 0)
        return NULL;
    return ((hu_sqlite_memory_t *)mem->ctx)->db;
}

#else /* !HU_ENABLE_SQLITE */

#include "human/core/allocator.h"
#include "human/memory.h"

hu_memory_t hu_sqlite_memory_create(hu_allocator_t *alloc, const char *db_path) {
    (void)db_path;
    return hu_none_memory_create(alloc);
}

hu_session_store_t hu_sqlite_memory_get_session_store(hu_memory_t *mem) {
    (void)mem;
    return (hu_session_store_t){.ctx = NULL, .vtable = NULL};
}

#endif
