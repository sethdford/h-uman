/*
 * Real iMessage integration tests — runs against ~/Library/Messages/chat.db.
 *
 * Links against human_core (NOT human_core_test), so HU_IS_TEST is NOT defined.
 * This exercises the production code paths: real SQLite polling, sanitization,
 * conversation history, tapback context, read receipts, attachment paths,
 * user_responded_recently, and the full format pipeline.
 *
 * Requires: macOS, Full Disk Access, Messages.app signed in.
 * Build:    cmake -DHU_ENABLE_INTEGRATION_TESTS=ON -DHU_HAS_IMESSAGE=ON \
 *                 -DHU_ENABLE_SQLITE=ON -DHU_ENABLE_CURL=ON ..
 * Run:      ./build/human_integration_tests --suite="iMessage Real"
 */
#if defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)

#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/channels/format.h"
#include "human/channels/imessage.h"
#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define S(lit) (lit), (sizeof(lit) - 1)

static const char *get_test_target(void) {
    const char *t = getenv("HU_IMESSAGE_TEST_TARGET");
    return (t && t[0]) ? t : "test@example.com";
}

static bool chatdb_accessible(void) {
    const char *home = getenv("HOME");
    if (!home)
        return false;
    char path[512];
    snprintf(path, sizeof(path), "%s/Library/Messages/chat.db", home);
    return access(path, R_OK) == 0;
}

/* ── 1. Health check: can we read chat.db? ────────────────────────────── */

static void imessage_real_health_check_passes(void) {
    HU_SKIP_IF(!chatdb_accessible(), "chat.db not accessible (Full Disk Access?)");
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_imessage_create(&alloc, NULL, 0, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_imessage_destroy(&ch);
}

/* ── 2. Conversation history loading from real chat.db ────────────────── */

static void imessage_real_load_history_returns_entries(void) {
    HU_SKIP_IF(!chatdb_accessible(), "chat.db not accessible");
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, NULL, 0, NULL, 0, &ch);

    /* Find a handle with messages */
    const char *home = getenv("HOME");
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                     "SELECT h.id FROM handle h "
                                     "JOIN message m ON m.handle_id = h.ROWID "
                                     "WHERE m.text IS NOT NULL "
                                     "GROUP BY h.id HAVING COUNT(*) > 5 "
                                     "ORDER BY COUNT(*) DESC LIMIT 1",
                                     -1, &stmt, NULL),
                 SQLITE_OK);
    char contact[128] = {0};
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *cid = (const char *)sqlite3_column_text(stmt, 0);
        if (cid) {
            size_t len = strlen(cid);
            if (len >= sizeof(contact))
                len = sizeof(contact) - 1;
            memcpy(contact, cid, len);
            contact[len] = '\0';
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    HU_SKIP_IF(contact[0] == '\0', "no contacts with 5+ messages found");

    hu_channel_history_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = ch.vtable->load_conversation_history(ch.ctx, &alloc, contact,
                                                           strlen(contact), 10, &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(entries);
    HU_ASSERT_TRUE(count > 0 && count <= 10);

    /* Verify entries have text and timestamps */
    for (size_t i = 0; i < count; i++) {
        HU_ASSERT_TRUE(entries[i].text[0] != '\0');
    }

    /* Verify chronological order (oldest first) */
    if (count >= 2) {
        /* timestamps are strings like "2024-01-01 10:00:00" — lexicographic compare works */
        HU_ASSERT_TRUE(strcmp(entries[0].timestamp, entries[count - 1].timestamp) <= 0);
    }

    alloc.free(alloc.ctx, entries, count * sizeof(*entries));
    hu_imessage_destroy(&ch);
}

/* ── 3. Tapback context from real reactions ───────────────────────────── */

static void imessage_real_tapback_context_runs_without_error(void) {
    HU_SKIP_IF(!chatdb_accessible(), "chat.db not accessible");
    hu_allocator_t alloc = hu_system_allocator();

    /* Find a contact we've sent messages to (so tapback context is meaningful) */
    const char *home = getenv("HOME");
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    sqlite3 *db = NULL;
    sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
                        "SELECT h.id FROM message m JOIN handle h ON m.handle_id = h.ROWID "
                        "WHERE m.is_from_me = 1 AND m.text IS NOT NULL "
                        "GROUP BY h.id ORDER BY COUNT(*) DESC LIMIT 1",
                        -1, &stmt, NULL);
    char contact[128] = {0};
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *cid = (const char *)sqlite3_column_text(stmt, 0);
        if (cid) {
            size_t len = strlen(cid);
            if (len >= sizeof(contact))
                len = sizeof(contact) - 1;
            memcpy(contact, cid, len);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    HU_SKIP_IF(contact[0] == '\0', "no outbound messages found");

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_imessage_build_tapback_context(&alloc, contact, strlen(contact), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    /* out may be NULL (no recent tapbacks) or a formatted string — both are valid */
    if (out) {
        HU_ASSERT_TRUE(out_len > 0);
        alloc.free(alloc.ctx, out, out_len + 1);
    }
}

/* ── 4. Read receipt context from real data ───────────────────────────── */

static void imessage_real_read_receipt_context_runs_without_error(void) {
    HU_SKIP_IF(!chatdb_accessible(), "chat.db not accessible");
    hu_allocator_t alloc = hu_system_allocator();

    const char *home = getenv("HOME");
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    sqlite3 *db = NULL;
    sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
                        "SELECT h.id FROM message m JOIN handle h ON m.handle_id = h.ROWID "
                        "WHERE m.is_from_me = 1 "
                        "GROUP BY h.id ORDER BY MAX(m.ROWID) DESC LIMIT 1",
                        -1, &stmt, NULL);
    char contact[128] = {0};
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *cid = (const char *)sqlite3_column_text(stmt, 0);
        if (cid) {
            size_t len = strlen(cid);
            if (len >= sizeof(contact))
                len = sizeof(contact) - 1;
            memcpy(contact, cid, len);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    HU_SKIP_IF(contact[0] == '\0', "no contacts found");

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_imessage_build_read_receipt_context(&alloc, contact, strlen(contact), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    if (out) {
        HU_ASSERT_TRUE(out_len > 0);
        alloc.free(alloc.ctx, out, out_len + 1);
    }
}

/* ── 5. GIF tapback count from real data ──────────────────────────────── */

static void imessage_real_gif_tapback_count_returns_int(void) {
    HU_SKIP_IF(!chatdb_accessible(), "chat.db not accessible");
    int count = hu_imessage_count_recent_gif_tapbacks(S("+15551234567"));
    HU_ASSERT_TRUE(count >= 0);
}

/* ── 6. user_responded_recently on real chat.db ───────────────────────── */

static void imessage_real_user_responded_recently_returns_bool(void) {
    HU_SKIP_IF(!chatdb_accessible(), "chat.db not accessible");
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, NULL, 0, NULL, 0, &ch);

    bool recent = ch.vtable->human_active_recently(ch.ctx, S("+15551234567"), 3600);
    (void)recent;
    hu_imessage_destroy(&ch);
}

/* ── 7. GUID lookup on real chat.db ───────────────────────────────────── */

static void imessage_real_guid_lookup_on_known_guid(void) {
    HU_SKIP_IF(!chatdb_accessible(), "chat.db not accessible");
    hu_allocator_t alloc = hu_system_allocator();

    /* Grab a real GUID from the database */
    const char *home = getenv("HOME");
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    sqlite3 *db = NULL;
    sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
                        "SELECT guid FROM message WHERE text IS NOT NULL AND LENGTH(text) > 0 "
                        "ORDER BY ROWID DESC LIMIT 1",
                        -1, &stmt, NULL);
    char guid[96] = {0};
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *g = (const char *)sqlite3_column_text(stmt, 0);
        if (g) {
            size_t len = strlen(g);
            if (len >= sizeof(guid))
                len = sizeof(guid) - 1;
            memcpy(guid, g, len);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    HU_SKIP_IF(guid[0] == '\0', "no messages with GUID found");

    char out_text[512];
    size_t out_len = 0;
    hu_error_t err = hu_imessage_lookup_message_by_guid(&alloc, guid, strlen(guid), out_text,
                                                         sizeof(out_text), &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(out_text[0] != '\0');
}

/* ── 8. Attachment path lookup ────────────────────────────────────────── */

static void imessage_real_attachment_path_for_known_message(void) {
    HU_SKIP_IF(!chatdb_accessible(), "chat.db not accessible");
    hu_allocator_t alloc = hu_system_allocator();

    const char *home = getenv("HOME");
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    sqlite3 *db = NULL;
    sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
                        "SELECT maj.message_id FROM message_attachment_join maj "
                        "JOIN attachment a ON maj.attachment_id = a.ROWID "
                        "WHERE a.filename IS NOT NULL LIMIT 1",
                        -1, &stmt, NULL);
    int64_t msg_id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        msg_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    HU_SKIP_IF(msg_id < 0, "no attachments found");

    hu_channel_t ch;
    hu_imessage_create(&alloc, NULL, 0, NULL, 0, &ch);
    char *path = ch.vtable->get_attachment_path(ch.ctx, &alloc, msg_id);
    /* path may be NULL if the attachment is outside ~/Library/Messages/Attachments/ */
    if (path) {
        HU_ASSERT_TRUE(strlen(path) > 0);
        alloc.free(alloc.ctx, path, strlen(path) + 1);
    }
    hu_imessage_destroy(&ch);
}

/* ── 9. Poll seeds to MAX(ROWID) and returns 0 new messages ──────────── */

static void imessage_real_poll_seeds_and_returns_zero(void) {
    HU_SKIP_IF(!chatdb_accessible(), "chat.db not accessible");
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, NULL, 0, NULL, 0, &ch);

    hu_channel_loop_msg_t msgs[4];
    size_t count = 99;
    hu_error_t err = hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    /* First poll seeds last_rowid to MAX(ROWID), returns 0 */
    HU_ASSERT_EQ(count, 0u);

    /* Second poll also returns 0 (no new messages since seed) */
    count = 99;
    err = hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);

    hu_imessage_destroy(&ch);
}

/* ── 10. Sanitize output strips AI phrases (production path) ──────────── */

static void imessage_real_send_sanitizes_ai_phrases(void) {
    HU_SKIP_IF(!chatdb_accessible(), "chat.db not accessible");
    hu_allocator_t alloc = hu_system_allocator();

    /* The format pipeline is the closest we can test without actually calling
     * osascript. It runs the same strip_markdown + strip_ai_phrases + cap logic. */
    char *out = NULL;
    size_t out_len = 0;
    const char *ai_text = "I'd be happy to help! Great question! As an AI, "
                          "I think **this is bold** and `code` here. "
                          "Feel free to ask more. Don't hesitate to reach out. "
                          "I hope this helps! Let me know if you need anything else.";
    hu_error_t err =
        hu_channel_format_outbound(&alloc, S("imessage"), ai_text, strlen(ai_text), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(out_len <= 301);

    /* All AI phrases should be gone */
    HU_ASSERT_TRUE(strstr(out, "happy to") == NULL);
    HU_ASSERT_TRUE(strstr(out, "Great question") == NULL);
    HU_ASSERT_TRUE(strstr(out, "As an AI") == NULL);
    HU_ASSERT_TRUE(strstr(out, "Feel free") == NULL);
    HU_ASSERT_TRUE(strstr(out, "hesitate") == NULL);
    HU_ASSERT_TRUE(strstr(out, "hope this helps") == NULL);
    /* Markdown should be gone */
    HU_ASSERT_TRUE(strstr(out, "**") == NULL);
    HU_ASSERT_TRUE(strstr(out, "`") == NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

/* ── 11. Conversation history + awareness pipeline end-to-end ─────────── */

static void imessage_real_history_feeds_awareness_pipeline(void) {
    HU_SKIP_IF(!chatdb_accessible(), "chat.db not accessible");
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, NULL, 0, NULL, 0, &ch);

    /* Find top contact */
    const char *home = getenv("HOME");
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    sqlite3 *db = NULL;
    sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
                        "SELECT h.id FROM handle h "
                        "JOIN message m ON m.handle_id = h.ROWID "
                        "WHERE m.text IS NOT NULL "
                        "GROUP BY h.id ORDER BY COUNT(*) DESC LIMIT 1",
                        -1, &stmt, NULL);
    char contact[128] = {0};
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *cid = (const char *)sqlite3_column_text(stmt, 0);
        if (cid) {
            size_t len = strlen(cid);
            if (len >= sizeof(contact))
                len = sizeof(contact) - 1;
            memcpy(contact, cid, len);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    HU_SKIP_IF(contact[0] == '\0', "no contacts found");

    /* Load real history */
    hu_channel_history_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = ch.vtable->load_conversation_history(ch.ctx, &alloc, contact,
                                                           strlen(contact), 25, &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count > 0);

    /* Feed into awareness builder — production path */
    size_t ctx_len = 0;
    char *ctx = hu_conversation_build_awareness(&alloc, entries, count, NULL, &ctx_len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 100);

    /* Should contain anti-AI style rules */
    HU_ASSERT_TRUE(strstr(ctx, "STYLE") != NULL || strstr(ctx, "happy to") != NULL ||
                    strstr(ctx, "naturally") != NULL || strstr(ctx, "AI") != NULL);

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    alloc.free(alloc.ctx, entries, count * sizeof(*entries));
    hu_imessage_destroy(&ch);
}

/* ── 12. Latest attachment path for a real contact ────────────────────── */

static void imessage_real_latest_attachment_for_top_contact(void) {
    HU_SKIP_IF(!chatdb_accessible(), "chat.db not accessible");
    hu_allocator_t alloc = hu_system_allocator();

    const char *home = getenv("HOME");
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    sqlite3 *db = NULL;
    sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
                        "SELECT h.id FROM message_attachment_join maj "
                        "JOIN message m ON maj.message_id = m.ROWID "
                        "JOIN handle h ON m.handle_id = h.ROWID "
                        "JOIN attachment a ON maj.attachment_id = a.ROWID "
                        "WHERE a.filename IS NOT NULL "
                        "GROUP BY h.id ORDER BY COUNT(*) DESC LIMIT 1",
                        -1, &stmt, NULL);
    char contact[128] = {0};
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *cid = (const char *)sqlite3_column_text(stmt, 0);
        if (cid) {
            size_t len = strlen(cid);
            if (len >= sizeof(contact))
                len = sizeof(contact) - 1;
            memcpy(contact, cid, len);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    HU_SKIP_IF(contact[0] == '\0', "no contacts with attachments");

    hu_channel_t ch;
    hu_imessage_create(&alloc, NULL, 0, NULL, 0, &ch);
    char *path =
        ch.vtable->get_latest_attachment_path(ch.ctx, &alloc, contact, strlen(contact));
    if (path) {
        HU_ASSERT_TRUE(strlen(path) > 0);
        alloc.free(alloc.ctx, path, strlen(path) + 1);
    }
    hu_imessage_destroy(&ch);
}

/* ── 13. attributedBody extraction recovers text-less messages ─────────── */

static void imessage_real_attributed_body_extraction_works(void) {
    HU_SKIP_IF(!chatdb_accessible(), "chat.db not accessible");
    hu_allocator_t alloc = hu_system_allocator();

    /* Find a message where text IS NULL but attributedBody IS NOT NULL */
    const char *home = getenv("HOME");
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    HU_ASSERT_EQ(
        sqlite3_prepare_v2(db,
                            "SELECT COUNT(*) FROM message "
                            "WHERE is_from_me = 0 "
                            "AND (text IS NULL OR LENGTH(text) = 0) "
                            "AND attributedBody IS NOT NULL AND LENGTH(attributedBody) > 0",
                            -1, &stmt, NULL),
        SQLITE_OK);
    int text_less_count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        text_less_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    HU_SKIP_IF(text_less_count == 0, "no text-less messages with attributedBody");

    /* Load history for a contact that has text-less messages */
    hu_channel_t ch;
    hu_imessage_create(&alloc, NULL, 0, NULL, 0, &ch);

    sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    sqlite3_prepare_v2(db,
                        "SELECT h.id FROM message m JOIN handle h ON m.handle_id = h.ROWID "
                        "WHERE m.is_from_me = 0 "
                        "AND (m.text IS NULL OR LENGTH(m.text) = 0) "
                        "AND m.attributedBody IS NOT NULL "
                        "GROUP BY h.id ORDER BY COUNT(*) DESC LIMIT 1",
                        -1, &stmt, NULL);
    char contact[128] = {0};
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *cid = (const char *)sqlite3_column_text(stmt, 0);
        if (cid) {
            size_t len = strlen(cid);
            if (len >= sizeof(contact))
                len = sizeof(contact) - 1;
            memcpy(contact, cid, len);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    HU_SKIP_IF(contact[0] == '\0', "no contact with text-less messages");

    hu_channel_history_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = ch.vtable->load_conversation_history(ch.ctx, &alloc, contact,
                                                           strlen(contact), 25, &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count > 0);

    /* At least some entries should have non-placeholder text (not [Photo], [Video], etc.)
     * from attributedBody extraction */
    int real_text_count = 0;
    for (size_t i = 0; i < count; i++) {
        const char *t = entries[i].text;
        if (t[0] != '\0' && t[0] != '[' && strcmp(t, "[you replied]") != 0)
            real_text_count++;
    }
    HU_ASSERT_TRUE(real_text_count > 0);

    alloc.free(alloc.ctx, entries, count * sizeof(*entries));
    hu_imessage_destroy(&ch);
}

/* ── 14. AppleScript send roundtrip (self-send, verify in chat.db) ────── */

static void imessage_real_applescript_send_roundtrip(void) {
    HU_SKIP_IF(!chatdb_accessible(), "chat.db not accessible");
    hu_allocator_t alloc = hu_system_allocator();

    /* Record current MAX(ROWID) */
    const char *home = getenv("HOME");
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    sqlite3 *db = NULL;
    sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db, "SELECT MAX(ROWID) FROM message", -1, &stmt, NULL);
    int64_t before_rowid = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        before_rowid = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    /* Send a message to ourselves via the production channel */
    const char *target = get_test_target();
    size_t target_len = strlen(target);
    hu_channel_t ch;
    hu_imessage_create(&alloc, target, target_len, NULL, 0, &ch);
    const char *probe = "h-uman integ roundtrip [probe-002]";
    hu_error_t err = ch.vtable->send(ch.ctx, target, target_len, probe, strlen(probe), NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    /* Wait for chat.db to sync */
    usleep(2000000);

    /* Verify a new message appeared */
    sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    sqlite3_prepare_v2(db, "SELECT MAX(ROWID) FROM message", -1, &stmt, NULL);
    int64_t after_rowid = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        after_rowid = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    HU_ASSERT_TRUE(after_rowid > before_rowid);
    hu_imessage_destroy(&ch);
}

/* ── 15. Full pipeline: history → awareness → format → send ───────────── */

static void imessage_real_full_pipeline_end_to_end(void) {
    HU_SKIP_IF(!chatdb_accessible(), "chat.db not accessible");
    hu_allocator_t alloc = hu_system_allocator();

    /* 1. Create the iMessage channel (production, no HU_IS_TEST) */
    const char *target = get_test_target();
    size_t target_len = strlen(target);
    hu_channel_t ch;
    hu_error_t err = hu_imessage_create(&alloc, target, target_len, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);

    /* 2. Health check — proves chat.db is readable */
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));

    /* 3. Poll — seeds last_rowid, returns 0 (no new since seed) */
    hu_channel_loop_msg_t msgs[4];
    size_t poll_count = 0;
    err = hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &poll_count);
    HU_ASSERT_EQ(err, HU_OK);

    /* 4. Load real conversation history */
    hu_channel_history_entry_t *entries = NULL;
    size_t hist_count = 0;
    err = ch.vtable->load_conversation_history(ch.ctx, &alloc, target, target_len, 10, &entries,
                                                &hist_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(hist_count > 0);

    /* 5. Build conversation awareness (what the agent uses as context) */
    size_t ctx_len = 0;
    char *ctx = hu_conversation_build_awareness(&alloc, entries, hist_count, NULL, &ctx_len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 50);

    /* 6. Simulate an AI response, then format it through the pipeline */
    const char *raw_reply = "I'd be happy to help! **Sure**, let me know when works. "
                            "Feel free to reach out anytime!";
    char *formatted = NULL;
    size_t fmt_len = 0;
    err = hu_channel_format_outbound(&alloc, S("imessage"), raw_reply, strlen(raw_reply),
                                      &formatted, &fmt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(formatted);
    HU_ASSERT_TRUE(fmt_len <= 300);
    HU_ASSERT_TRUE(strstr(formatted, "happy to") == NULL);
    HU_ASSERT_TRUE(strstr(formatted, "**") == NULL);

    /* 7. Check response constraints (300 char cap for iMessage) */
    hu_channel_response_constraints_t constraints = {0};
    err = ch.vtable->get_response_constraints(ch.ctx, &constraints);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(constraints.max_chars, 300u);
    HU_ASSERT_TRUE(fmt_len <= constraints.max_chars);

    /* 8. Send the formatted reply (self-send) */
    int64_t before_rowid = 0;
    {
        const char *home = getenv("HOME");
        char db_path[512];
        snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
        sqlite3 *db = NULL;
        sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
        sqlite3_stmt *stmt = NULL;
        sqlite3_prepare_v2(db, "SELECT MAX(ROWID) FROM message", -1, &stmt, NULL);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            before_rowid = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }

    err = ch.vtable->send(ch.ctx, target, target_len, formatted, fmt_len, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    /* 9. Verify the message landed in chat.db */
    usleep(2000000);
    {
        const char *home = getenv("HOME");
        char db_path[512];
        snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
        sqlite3 *db = NULL;
        sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
        sqlite3_stmt *stmt = NULL;
        sqlite3_prepare_v2(db, "SELECT MAX(ROWID) FROM message", -1, &stmt, NULL);
        int64_t after_rowid = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            after_rowid = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        HU_ASSERT_TRUE(after_rowid > before_rowid);
    }

    /* 10. Check bot suppression — human_active_recently should be true now */
    bool active = ch.vtable->human_active_recently(ch.ctx, target, target_len, 60);
    HU_ASSERT_TRUE(active);

    /* Cleanup */
    alloc.free(alloc.ctx, formatted, fmt_len + 1);
    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    alloc.free(alloc.ctx, entries, hist_count * sizeof(*entries));
    hu_imessage_destroy(&ch);
}

/* ── Registration ─────────────────────────────────────────────────────── */

void run_integration_imessage_tests(void) {
    HU_RUN_TEST(imessage_real_health_check_passes);
    HU_RUN_TEST(imessage_real_load_history_returns_entries);
    HU_RUN_TEST(imessage_real_tapback_context_runs_without_error);
    HU_RUN_TEST(imessage_real_read_receipt_context_runs_without_error);
    HU_RUN_TEST(imessage_real_gif_tapback_count_returns_int);
    HU_RUN_TEST(imessage_real_user_responded_recently_returns_bool);
    HU_RUN_TEST(imessage_real_guid_lookup_on_known_guid);
    HU_RUN_TEST(imessage_real_attachment_path_for_known_message);
    HU_RUN_TEST(imessage_real_poll_seeds_and_returns_zero);
    HU_RUN_TEST(imessage_real_send_sanitizes_ai_phrases);
    HU_RUN_TEST(imessage_real_history_feeds_awareness_pipeline);
    HU_RUN_TEST(imessage_real_latest_attachment_for_top_contact);
    HU_RUN_TEST(imessage_real_attributed_body_extraction_works);
    HU_RUN_TEST(imessage_real_applescript_send_roundtrip);
    HU_RUN_TEST(imessage_real_full_pipeline_end_to_end);
}

#else /* Not macOS or no SQLite */

void run_integration_imessage_tests(void) {
    /* no-op on non-macOS */
}

#endif
