#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/feeds/processor.h"
#include "test_framework.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#ifdef HU_ENABLE_SQLITE
#include "human/feeds/findings.h"
#include "human/feeds/trends.h"
#include "human/intelligence/cycle.h"
#include <sqlite3.h>
#include <time.h>
#endif

static void feeds_create_table_sql_valid(void) {
    char buf[1024];
    size_t len = 0;
    hu_error_t err = hu_feeds_create_table_sql(buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "CREATE TABLE"));
    HU_ASSERT_NOT_NULL(strstr(buf, "feed_items"));
    HU_ASSERT_NOT_NULL(strstr(buf, "content_type"));
    HU_ASSERT_NOT_NULL(strstr(buf, "source"));
    HU_ASSERT_NOT_NULL(strstr(buf, "content"));
    HU_ASSERT_NOT_NULL(strstr(buf, "ingested_at"));
    HU_ASSERT_NOT_NULL(strstr(buf, "referenced"));
    HU_ASSERT_NOT_NULL(strstr(buf, "cluster_id"));
}

static void feeds_insert_sql_valid(void) {
    hu_feed_item_t item = {
        .id = 0,
        .type = HU_FEED_SOCIAL_INSTAGRAM,
        .source = "instagram:@friend",
        .source_len = 17,
        .content = "posted about hiking",
        .content_len = 19,
        .topic = "outdoors",
        .topic_len = 8,
        .relevance = 0.85,
        .fetched_at = 1700000000000ULL,
        .processed = false,
    };
    char buf[1024];
    size_t len = 0;
    hu_error_t err = hu_feeds_insert_sql(&item, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "INSERT INTO"));
    HU_ASSERT_NOT_NULL(strstr(buf, "social_instagram"));
    HU_ASSERT_NOT_NULL(strstr(buf, "instagram:@friend"));
    HU_ASSERT_NOT_NULL(strstr(buf, "posted about hiking"));
}

static void feeds_query_unprocessed_sql_valid(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_feeds_query_unprocessed_sql(HU_FEED_NEWS_RSS, 10, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "SELECT"));
    HU_ASSERT_NOT_NULL(strstr(buf, "news_rss"));
    HU_ASSERT_NOT_NULL(strstr(buf, "ingested_at"));
    HU_ASSERT_NOT_NULL(strstr(buf, "LIMIT 10"));
}

static void feeds_mark_processed_sql_valid(void) {
    char buf[256];
    size_t len = 0;
    hu_error_t err = hu_feeds_mark_processed_sql(42, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "UPDATE"));
    HU_ASSERT_NOT_NULL(strstr(buf, "referenced = 1"));
    HU_ASSERT_NOT_NULL(strstr(buf, "id = 42"));
}

static void feeds_query_by_topic_sql_valid(void) {
    char buf[512];
    size_t len = 0;
    const char *topic = "hiking";
    hu_error_t err = hu_feeds_query_by_topic_sql(topic, 6, 5, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "SELECT"));
    HU_ASSERT_NOT_NULL(strstr(buf, "content LIKE"));
    HU_ASSERT_NOT_NULL(strstr(buf, "hiking"));
    HU_ASSERT_NOT_NULL(strstr(buf, "LIMIT 5"));
}

static void feeds_should_poll_enabled_interval_elapsed(void) {
    hu_feed_config_t config = {0};
    config.enabled[HU_FEED_MUSIC] = true;
    config.poll_interval_minutes[HU_FEED_MUSIC] = 60;
    uint64_t last = 1000000;
    uint64_t now = 1000000 + (60ULL * 60 * 1000);
    HU_ASSERT_TRUE(hu_feeds_should_poll(HU_FEED_MUSIC, &config, last, now));
}

static void feeds_should_poll_disabled_returns_false(void) {
    hu_feed_config_t config = {0};
    config.enabled[HU_FEED_EMAIL] = false;
    config.poll_interval_minutes[HU_FEED_EMAIL] = 30;
    uint64_t last = 1000000;
    uint64_t now = 1000000 + (60ULL * 60 * 1000);
    HU_ASSERT_FALSE(hu_feeds_should_poll(HU_FEED_EMAIL, &config, last, now));
}

static void feeds_should_poll_interval_not_elapsed_returns_false(void) {
    hu_feed_config_t config = {0};
    config.enabled[HU_FEED_NEWS_RSS] = true;
    config.poll_interval_minutes[HU_FEED_NEWS_RSS] = 60;
    uint64_t last = 1000000;
    uint64_t now = 1000000 + (30ULL * 60 * 1000);
    HU_ASSERT_FALSE(hu_feeds_should_poll(HU_FEED_NEWS_RSS, &config, last, now));
}

static void feeds_should_poll_interval_zero_returns_true_when_enabled(void) {
    hu_feed_config_t config = {0};
    config.enabled[HU_FEED_APPLE_REMINDERS] = true;
    config.poll_interval_minutes[HU_FEED_APPLE_REMINDERS] = 0;
    uint64_t last = 1000000;
    uint64_t now = 1000000;
    HU_ASSERT_TRUE(hu_feeds_should_poll(HU_FEED_APPLE_REMINDERS, &config, last, now));
}

static void feeds_feed_type_str_all_types(void) {
    HU_ASSERT_NOT_NULL(hu_feed_type_str(HU_FEED_SOCIAL_FACEBOOK));
    HU_ASSERT_STR_EQ(hu_feed_type_str(HU_FEED_SOCIAL_FACEBOOK), "social_facebook");
    HU_ASSERT_STR_EQ(hu_feed_type_str(HU_FEED_SOCIAL_INSTAGRAM), "social_instagram");
    HU_ASSERT_STR_EQ(hu_feed_type_str(HU_FEED_MUSIC), "music");
    HU_ASSERT_STR_EQ(hu_feed_type_str(HU_FEED_NEWS_RSS), "news_rss");
}

static void feeds_score_relevance_high_overlap(void) {
    size_t content_len = strlen("hiking mountains trail");
    size_t interest_len = strlen("hiking trail");
    double score = hu_feeds_score_relevance("hiking mountains trail", content_len, "hiking trail",
                                            interest_len);
    HU_ASSERT_TRUE(score >= 0.99);
}

static void feeds_score_relevance_no_overlap(void) {
    size_t content_len = strlen("cooking pizza");
    size_t interest_len = strlen("hiking trail");
    double score =
        hu_feeds_score_relevance("cooking pizza", content_len, "hiking trail", interest_len);
    HU_ASSERT_FLOAT_EQ(score, 0.0, 0.001);
}

static void feeds_score_relevance_partial(void) {
    size_t content_len = strlen("friend posted about hiking");
    size_t interest_len = strlen("hiking outdoors");
    double score = hu_feeds_score_relevance("friend posted about hiking", content_len,
                                            "hiking outdoors", interest_len);
    HU_ASSERT_TRUE(score > 0.0 && score < 1.0);
}

static void feeds_score_relevance_null_returns_zero(void) {
    double score = hu_feeds_score_relevance(NULL, 0, "hiking", 5);
    HU_ASSERT_FLOAT_EQ(score, 0.0, 0.001);
}

static void feeds_build_prompt_empty_returns_none(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_feeds_build_prompt(&alloc, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_NOT_NULL(strstr(out, "[EXTERNAL AWARENESS]"));
    HU_ASSERT_NOT_NULL(strstr(out, "(none)"));
    hu_str_free(&alloc, out);
}

static void feeds_build_prompt_single_item(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_item_t item = {
        .type = HU_FEED_SOCIAL_INSTAGRAM,
        .content = "friend posted about hiking",
        .content_len = 26,
    };
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_feeds_build_prompt(&alloc, &item, 1, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_NOT_NULL(strstr(out, "[EXTERNAL AWARENESS]"));
    HU_ASSERT_NOT_NULL(strstr(out, "Social"));
    HU_ASSERT_NOT_NULL(strstr(out, "friend posted about hiking"));
    hu_str_free(&alloc, out);
}

static void feeds_build_prompt_multiple_items(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_item_t items[3] = {
        {
            .type = HU_FEED_SOCIAL_INSTAGRAM,
            .content = "friend posted about hiking",
            .content_len = 26,
        },
        {
            .type = HU_FEED_NEWS_RSS,
            .content = "new AI chip announced",
            .content_len = 21,
        },
        {
            .type = HU_FEED_MUSIC,
            .content = "listening to artist",
            .content_len = 19,
        },
    };
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_feeds_build_prompt(&alloc, items, 3, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_NOT_NULL(strstr(out, "[EXTERNAL AWARENESS]"));
    HU_ASSERT_NOT_NULL(strstr(out, "Social"));
    HU_ASSERT_NOT_NULL(strstr(out, "News"));
    HU_ASSERT_NOT_NULL(strstr(out, "Music"));
    HU_ASSERT_NOT_NULL(strstr(out, "friend posted about hiking"));
    HU_ASSERT_NOT_NULL(strstr(out, "new AI chip announced"));
    HU_ASSERT_NOT_NULL(strstr(out, "listening to artist"));
    hu_str_free(&alloc, out);
}

static void feeds_feed_item_deinit_frees_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_item_t item;
    memset(&item, 0, sizeof(item));
    item.source = hu_strndup(&alloc, "instagram:@friend", 17);
    item.source_len = 17;
    item.content = hu_strndup(&alloc, "test content", 11);
    item.content_len = 11;
    item.topic = hu_strndup(&alloc, "outdoors", 8);
    item.topic_len = 8;
    HU_ASSERT_NOT_NULL(item.source);
    HU_ASSERT_NOT_NULL(item.content);
    HU_ASSERT_NOT_NULL(item.topic);
    hu_feed_item_deinit(&alloc, &item);
    HU_ASSERT_NULL(item.source);
    HU_ASSERT_NULL(item.content);
    HU_ASSERT_NULL(item.topic);
    HU_ASSERT_EQ(item.source_len, 0);
    HU_ASSERT_EQ(item.content_len, 0);
    HU_ASSERT_EQ(item.topic_len, 0);
}

#ifdef HU_ENABLE_SQLITE

static void feed_search_fts_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    const char *schema = "CREATE TABLE IF NOT EXISTS feed_items("
                         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "source TEXT NOT NULL,"
                         "contact_id TEXT,"
                         "content_type TEXT NOT NULL,"
                         "content TEXT NOT NULL,"
                         "url TEXT,"
                         "ingested_at INTEGER NOT NULL,"
                         "referenced INTEGER DEFAULT 0,"
                         "cluster_id INTEGER DEFAULT NULL)";
    HU_ASSERT_EQ(sqlite3_exec(db, schema, NULL, NULL, NULL), SQLITE_OK);

    const char *fts = "CREATE VIRTUAL TABLE IF NOT EXISTS feed_items_fts USING fts5("
                      "content, source, content_type, content=feed_items, content_rowid=id)";
    HU_ASSERT_EQ(sqlite3_exec(db, fts, NULL, NULL, NULL), SQLITE_OK);

    const char *trig_ai = "CREATE TRIGGER IF NOT EXISTS feed_items_ai AFTER INSERT ON feed_items "
                          "BEGIN INSERT INTO feed_items_fts(rowid, content, source, content_type) "
                          "VALUES (new.id, new.content, new.source, new.content_type); END";
    HU_ASSERT_EQ(sqlite3_exec(db, trig_ai, NULL, NULL, NULL), SQLITE_OK);

    const char *trig_ad =
        "CREATE TRIGGER IF NOT EXISTS feed_items_ad AFTER DELETE ON feed_items "
        "BEGIN INSERT INTO feed_items_fts(feed_items_fts, rowid, content, source, content_type) "
        "VALUES ('delete', old.id, old.content, old.source, old.content_type); END";
    HU_ASSERT_EQ(sqlite3_exec(db, trig_ad, NULL, NULL, NULL), SQLITE_OK);

    int64_t now = (int64_t)time(NULL);
    sqlite3_stmt *ins = NULL;
    const char *ins_sql =
        "INSERT INTO feed_items(source, content_type, content, ingested_at) VALUES (?,?,?,?)";
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, ins_sql, -1, &ins, NULL), SQLITE_OK);

    const char *src = "rss";
    const char *ct = "article";
    sqlite3_bind_text(ins, 1, src, -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, ct, -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, "Advances in machine learning", -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 4, now);
    HU_ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
    sqlite3_reset(ins);

    sqlite3_bind_text(ins, 1, src, -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, ct, -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, "Deep learning and machine learning", -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 4, now);
    HU_ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
    sqlite3_reset(ins);

    sqlite3_bind_text(ins, 1, "twitter", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, "tweet", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, "Cooking pasta recipes", -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 4, now);
    HU_ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
    sqlite3_finalize(ins);

    hu_feed_item_stored_t *out = NULL;
    size_t count = 0;
    hu_error_t err = hu_feed_search(&alloc, db, "machine learning", 16, 10, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    hu_feed_items_free(&alloc, out, count);
    sqlite3_close(db);
}

static void feed_build_daily_digest_groups_by_source(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    const char *schema = "CREATE TABLE IF NOT EXISTS feed_items("
                         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "source TEXT NOT NULL,"
                         "contact_id TEXT,"
                         "content_type TEXT NOT NULL,"
                         "content TEXT NOT NULL,"
                         "url TEXT,"
                         "ingested_at INTEGER NOT NULL,"
                         "referenced INTEGER DEFAULT 0,"
                         "cluster_id INTEGER DEFAULT NULL)";
    HU_ASSERT_EQ(sqlite3_exec(db, schema, NULL, NULL, NULL), SQLITE_OK);

    int64_t now = (int64_t)time(NULL);
    sqlite3_stmt *ins = NULL;
    const char *ins_sql =
        "INSERT INTO feed_items(source, content_type, content, ingested_at) VALUES (?,?,?,?)";
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, ins_sql, -1, &ins, NULL), SQLITE_OK);

    const char *sources[2] = {"rss", "twitter"};
    for (int i = 0; i < 5; i++) {
        sqlite3_bind_text(ins, 1, sources[i % 2], -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 2, "post", -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, "Item content", -1, SQLITE_STATIC);
        sqlite3_bind_int64(ins, 4, now);
        HU_ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);

    char *out = NULL;
    size_t out_len = 0;
    int64_t since_ts = now - 86400;
    hu_error_t err = hu_feed_build_daily_digest(&alloc, db, since_ts, 4000, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_NOT_NULL(strstr(out, "rss"));
    HU_ASSERT_NOT_NULL(strstr(out, "twitter"));
    hu_str_free(&alloc, out);
    sqlite3_close(db);
}

static void feed_processor_cleanup_removes_old(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    const char *schema = "CREATE TABLE IF NOT EXISTS feed_items("
                         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "source TEXT NOT NULL,"
                         "contact_id TEXT,"
                         "content_type TEXT NOT NULL,"
                         "content TEXT NOT NULL,"
                         "url TEXT,"
                         "ingested_at INTEGER NOT NULL,"
                         "referenced INTEGER DEFAULT 0,"
                         "cluster_id INTEGER DEFAULT NULL)";
    HU_ASSERT_EQ(sqlite3_exec(db, schema, NULL, NULL, NULL), SQLITE_OK);

    int64_t now = (int64_t)time(NULL);
    int64_t old_ts = now - (60 * 86400);

    sqlite3_stmt *ins = NULL;
    const char *ins_sql =
        "INSERT INTO feed_items(source, content_type, content, ingested_at) VALUES (?,?,?,?)";
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, ins_sql, -1, &ins, NULL), SQLITE_OK);

    sqlite3_bind_text(ins, 1, "rss", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, "article", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, "Recent item", -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 4, now);
    HU_ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
    sqlite3_reset(ins);

    sqlite3_bind_text(ins, 1, "twitter", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, "tweet", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, "Old item", -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 4, old_ts);
    HU_ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
    sqlite3_finalize(ins);

    hu_feed_processor_t proc = {.alloc = &alloc, .db = db};
    hu_error_t err = hu_feed_processor_cleanup(&proc, 30);
    HU_ASSERT_EQ(err, HU_OK);

    sqlite3_stmt *sel = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM feed_items", -1, &sel, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(sel), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(sel, 0), 1);
    sqlite3_finalize(sel);
    sqlite3_close(db);
}

static void feed_get_all_recent_returns_items(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    const char *schema = "CREATE TABLE IF NOT EXISTS feed_items("
                         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "source TEXT NOT NULL,"
                         "contact_id TEXT,"
                         "content_type TEXT NOT NULL,"
                         "content TEXT NOT NULL,"
                         "url TEXT,"
                         "ingested_at INTEGER NOT NULL,"
                         "referenced INTEGER DEFAULT 0,"
                         "cluster_id INTEGER DEFAULT NULL)";
    HU_ASSERT_EQ(sqlite3_exec(db, schema, NULL, NULL, NULL), SQLITE_OK);

    int64_t now = (int64_t)time(NULL);
    int64_t since = now - 86400;
    int64_t old_ts = now - (2 * 86400);

    sqlite3_stmt *ins = NULL;
    const char *ins_sql =
        "INSERT INTO feed_items(source, content_type, content, ingested_at) VALUES (?,?,?,?)";
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, ins_sql, -1, &ins, NULL), SQLITE_OK);

    sqlite3_bind_text(ins, 1, "rss", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, "article", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, "Recent 1", -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 4, now);
    HU_ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
    sqlite3_reset(ins);

    sqlite3_bind_text(ins, 1, "twitter", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, "tweet", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, "Recent 2", -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 4, now - 3600);
    HU_ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
    sqlite3_reset(ins);

    sqlite3_bind_text(ins, 1, "rss", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, "article", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, "Old item", -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 4, old_ts);
    HU_ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
    sqlite3_finalize(ins);

    hu_feed_item_stored_t *out = NULL;
    size_t count = 0;
    hu_error_t err = hu_feed_processor_get_all_recent(&alloc, db, since, 10, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    hu_feed_items_free(&alloc, out, count);
    sqlite3_close(db);
}

static void feed_correlate_groups_similar(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    const char *schema = "CREATE TABLE IF NOT EXISTS feed_items("
                         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "source TEXT NOT NULL,"
                         "contact_id TEXT,"
                         "content_type TEXT NOT NULL,"
                         "content TEXT NOT NULL,"
                         "url TEXT,"
                         "ingested_at INTEGER NOT NULL,"
                         "referenced INTEGER DEFAULT 0,"
                         "cluster_id INTEGER DEFAULT NULL)";
    HU_ASSERT_EQ(sqlite3_exec(db, schema, NULL, NULL, NULL), SQLITE_OK);

    int64_t now = (int64_t)time(NULL);
    int64_t since = now - 86400;

    sqlite3_stmt *ins = NULL;
    const char *ins_sql =
        "INSERT INTO feed_items(source, content_type, content, ingested_at) VALUES (?,?,?,?)";
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, ins_sql, -1, &ins, NULL), SQLITE_OK);

    sqlite3_bind_text(ins, 1, "rss", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, "article", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, "AI agent developments in tech", -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 4, now);
    HU_ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
    sqlite3_reset(ins);

    sqlite3_bind_text(ins, 1, "twitter", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, "tweet", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, "AI agent news and updates", -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 4, now);
    HU_ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
    sqlite3_reset(ins);

    sqlite3_bind_text(ins, 1, "instagram", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, "post", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, "Cooking recipes for dinner", -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 4, now);
    HU_ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
    sqlite3_finalize(ins);

    hu_error_t err = hu_feed_correlate_recent(&alloc, db, since, 0.3);
    HU_ASSERT_EQ(err, HU_OK);

    sqlite3_stmt *sel = NULL;
    HU_ASSERT_EQ(
        sqlite3_prepare_v2(db, "SELECT cluster_id FROM feed_items WHERE cluster_id IS NOT NULL", -1,
                           &sel, NULL),
        SQLITE_OK);
    int has_cluster = 0;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        has_cluster = 1;
        break;
    }
    sqlite3_finalize(sel);
    HU_ASSERT_TRUE(has_cluster);
    sqlite3_close(db);
}

static void findings_store_and_retrieve(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    const char *schema = "CREATE TABLE IF NOT EXISTS research_findings("
                         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "source TEXT,"
                         "finding TEXT NOT NULL,"
                         "relevance TEXT,"
                         "priority TEXT DEFAULT 'MEDIUM',"
                         "suggested_action TEXT,"
                         "status TEXT DEFAULT 'pending',"
                         "created_at INTEGER NOT NULL,"
                         "acted_at INTEGER)";
    HU_ASSERT_EQ(sqlite3_exec(db, schema, NULL, NULL, NULL), SQLITE_OK);

    hu_error_t err =
        hu_findings_store(&alloc, db, "test", "A finding", "HIGH", "HIGH", "Do something");
    HU_ASSERT_EQ(err, HU_OK);

    hu_research_finding_t *items = NULL;
    size_t count = 0;
    err = hu_findings_get_pending(&alloc, db, 10, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_NOT_NULL(strstr(items[0].finding, "A finding"));
    hu_findings_free(&alloc, items, count);
    sqlite3_close(db);
}

static void findings_mark_status(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    const char *schema = "CREATE TABLE IF NOT EXISTS research_findings("
                         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "source TEXT,"
                         "finding TEXT NOT NULL,"
                         "relevance TEXT,"
                         "priority TEXT DEFAULT 'MEDIUM',"
                         "suggested_action TEXT,"
                         "status TEXT DEFAULT 'pending',"
                         "created_at INTEGER NOT NULL,"
                         "acted_at INTEGER)";
    HU_ASSERT_EQ(sqlite3_exec(db, schema, NULL, NULL, NULL), SQLITE_OK);

    hu_error_t err =
        hu_findings_store(&alloc, db, "test", "A finding", "HIGH", "HIGH", "Do something");
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_findings_mark_status(db, 1, "completed");
    HU_ASSERT_EQ(err, HU_OK);

    hu_research_finding_t *items = NULL;
    size_t count = 0;
    err = hu_findings_get_pending(&alloc, db, 10, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    sqlite3_close(db);
}

static void feed_detect_trends_no_spikes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    const char *schema = "CREATE TABLE IF NOT EXISTS feed_items("
                         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "source TEXT NOT NULL,"
                         "contact_id TEXT,"
                         "content_type TEXT NOT NULL,"
                         "content TEXT NOT NULL,"
                         "url TEXT,"
                         "ingested_at INTEGER NOT NULL,"
                         "referenced INTEGER DEFAULT 0,"
                         "cluster_id INTEGER DEFAULT NULL)";
    HU_ASSERT_EQ(sqlite3_exec(db, schema, NULL, NULL, NULL), SQLITE_OK);

    int64_t now = (int64_t)time(NULL);
    sqlite3_stmt *ins = NULL;
    const char *ins_sql =
        "INSERT INTO feed_items(source, content_type, content, ingested_at) VALUES (?,?,?,?)";
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, ins_sql, -1, &ins, NULL), SQLITE_OK);
    sqlite3_bind_text(ins, 1, "rss", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, "article", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, "Single item", -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 4, now);
    HU_ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
    sqlite3_finalize(ins);

    hu_feed_trend_t *trends = NULL;
    size_t count = 0;
    const char *keywords = "AI blockchain";
    hu_error_t err = hu_feed_detect_trends(&alloc, db, keywords, strlen(keywords), &trends, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    hu_feed_trends_free(&alloc, trends, count);
    sqlite3_close(db);
}

static void findings_parse_and_store_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    const char *schema = "CREATE TABLE IF NOT EXISTS research_findings("
                         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "source TEXT,"
                         "finding TEXT NOT NULL,"
                         "relevance TEXT,"
                         "priority TEXT DEFAULT 'MEDIUM',"
                         "suggested_action TEXT,"
                         "status TEXT DEFAULT 'pending',"
                         "created_at INTEGER NOT NULL,"
                         "acted_at INTEGER)";
    HU_ASSERT_EQ(sqlite3_exec(db, schema, NULL, NULL, NULL), SQLITE_OK);

    const char *agent_output =
        "Here are research findings:\n"
        "- **Source**: arxiv.org\n"
        "- **Finding**: New transformer architecture reduces compute by 40%\n"
        "- **Relevance**: Directly applicable to our training pipeline\n"
        "- **Priority**: HIGH\n"
        "- **Action**: Implement scaled dot-product variant\n"
        "\n"
        "- **Source**: github.com/openai\n"
        "- **Finding**: Tiktoken v2 released with faster BPE\n"
        "- **Relevance**: Could improve our tokenizer performance\n"
        "- **Priority**: MEDIUM\n"
        "- **Action**: Benchmark against our BPE implementation\n";

    hu_error_t err = hu_findings_parse_and_store(&alloc, db, agent_output, strlen(agent_output));
    HU_ASSERT_EQ(err, HU_OK);

    hu_research_finding_t *items = NULL;
    size_t count = 0;
    err = hu_findings_get_pending(&alloc, db, 10, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);

    /* Results are DESC by created_at, so order may vary — check both exist */
    int found_arxiv = 0, found_github = 0;
    for (size_t i = 0; i < count; i++) {
        if (strstr(items[i].source, "arxiv"))
            found_arxiv = 1;
        if (strstr(items[i].source, "github"))
            found_github = 1;
    }
    HU_ASSERT_TRUE(found_arxiv);
    HU_ASSERT_TRUE(found_github);

    hu_findings_free(&alloc, items, count);
    sqlite3_close(db);
}

static void findings_parse_and_store_malformed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    const char *schema = "CREATE TABLE IF NOT EXISTS research_findings("
                         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "source TEXT,"
                         "finding TEXT NOT NULL,"
                         "relevance TEXT,"
                         "priority TEXT DEFAULT 'MEDIUM',"
                         "suggested_action TEXT,"
                         "status TEXT DEFAULT 'pending',"
                         "created_at INTEGER NOT NULL,"
                         "acted_at INTEGER)";
    HU_ASSERT_EQ(sqlite3_exec(db, schema, NULL, NULL, NULL), SQLITE_OK);

    /* No markers at all — should store nothing */
    const char *garbage = "This is just random text with no structure";
    hu_error_t err = hu_findings_parse_and_store(&alloc, db, garbage, strlen(garbage));
    HU_ASSERT_EQ(err, HU_OK);

    hu_research_finding_t *items = NULL;
    size_t count = 0;
    err = hu_findings_get_pending(&alloc, db, 10, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);

    /* Marker present but finding line is empty — should skip */
    const char *partial = "- **Source**: test\n- **Finding**:\n";
    err = hu_findings_parse_and_store(&alloc, db, partial, strlen(partial));
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_findings_get_pending(&alloc, db, 10, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);

    sqlite3_close(db);
}

static void findings_parse_and_store_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    HU_ASSERT_EQ(hu_findings_parse_and_store(NULL, db, "x", 1), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_findings_parse_and_store(&alloc, NULL, "x", 1), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_findings_parse_and_store(&alloc, db, NULL, 1), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_findings_parse_and_store(&alloc, db, "x", 0), HU_ERR_INVALID_ARGUMENT);

    sqlite3_close(db);
}

static void findings_get_all_with_limit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    const char *schema = "CREATE TABLE IF NOT EXISTS research_findings("
                         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "source TEXT,"
                         "finding TEXT NOT NULL,"
                         "relevance TEXT,"
                         "priority TEXT DEFAULT 'MEDIUM',"
                         "suggested_action TEXT,"
                         "status TEXT DEFAULT 'pending',"
                         "created_at INTEGER NOT NULL,"
                         "acted_at INTEGER)";
    HU_ASSERT_EQ(sqlite3_exec(db, schema, NULL, NULL, NULL), SQLITE_OK);

    /* Store 5 findings */
    for (int i = 0; i < 5; i++) {
        char finding[64];
        snprintf(finding, sizeof(finding), "Finding number %d", i);
        hu_error_t err = hu_findings_store(&alloc, db, "test", finding, "MED", "MEDIUM", "none");
        HU_ASSERT_EQ(err, HU_OK);
    }

    /* Mark one as completed so it's not "pending" */
    HU_ASSERT_EQ(hu_findings_mark_status(db, 3, "completed"), HU_OK);

    /* get_all should return all 5 regardless of status */
    hu_research_finding_t *items = NULL;
    size_t count = 0;
    hu_error_t err = hu_findings_get_all(&alloc, db, 100, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 5u);
    hu_findings_free(&alloc, items, count);

    /* get_pending should return only 4 (one was completed) */
    err = hu_findings_get_pending(&alloc, db, 100, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 4u);
    hu_findings_free(&alloc, items, count);

    /* Limit should be respected */
    err = hu_findings_get_all(&alloc, db, 2, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    hu_findings_free(&alloc, items, count);

    /* get_all null args */
    HU_ASSERT_EQ(hu_findings_get_all(NULL, db, 10, &items, &count), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_findings_get_all(&alloc, NULL, 10, &items, &count), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_findings_get_all(&alloc, db, 10, NULL, &count), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_findings_get_all(&alloc, db, 10, &items, NULL), HU_ERR_INVALID_ARGUMENT);

    sqlite3_close(db);
}

static void findings_store_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    HU_ASSERT_EQ(hu_findings_store(&alloc, NULL, "s", "f", "r", "p", "a"), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_findings_store(&alloc, db, "s", NULL, "r", "p", "a"), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_findings_mark_status(NULL, 1, "done"), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_findings_mark_status(db, 1, NULL), HU_ERR_INVALID_ARGUMENT);

    sqlite3_close(db);
}

static void findings_get_pending_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    hu_research_finding_t *items = NULL;
    size_t count = 0;

    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_findings_get_pending(NULL, db, 10, &items, &count), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_findings_get_pending(&alloc, NULL, 10, &items, &count),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_findings_get_pending(&alloc, db, 10, NULL, &count), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_findings_get_pending(&alloc, db, 10, &items, NULL), HU_ERR_INVALID_ARGUMENT);
    sqlite3_close(db);
}

static void feed_trends_build_section_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t len = 0;
    hu_error_t err = hu_feed_trends_build_section(&alloc, NULL, 0, &out, &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_NOT_NULL(strstr(out, "No significant trends"));
    hu_str_free(&alloc, out);
}

static sqlite3 *setup_cycle_db(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS feed_items("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "source TEXT NOT NULL, contact_id TEXT,"
                 "content_type TEXT NOT NULL, content TEXT NOT NULL,"
                 "url TEXT, ingested_at INTEGER NOT NULL,"
                 "referenced INTEGER DEFAULT 0, cluster_id INTEGER DEFAULT NULL);"
                 "CREATE TABLE IF NOT EXISTS research_findings("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "source TEXT, finding TEXT NOT NULL, relevance TEXT,"
                 "priority TEXT DEFAULT 'MEDIUM', suggested_action TEXT,"
                 "status TEXT DEFAULT 'pending', created_at INTEGER NOT NULL,"
                 "acted_at INTEGER);"
                 "CREATE TABLE IF NOT EXISTS current_events("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "topic TEXT, summary TEXT, source TEXT,"
                 "published_at INTEGER, relevance REAL);"
                 "CREATE UNIQUE INDEX IF NOT EXISTS idx_ce_dedup ON current_events(summary);"
                 "CREATE TABLE IF NOT EXISTS general_lessons("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "lesson TEXT UNIQUE, confidence REAL,"
                 "source_count INTEGER, first_learned INTEGER, last_confirmed INTEGER);"
                 "CREATE TABLE IF NOT EXISTS inferred_values("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "name TEXT UNIQUE, description TEXT,"
                 "importance REAL, evidence_count INTEGER,"
                 "created_at INTEGER, updated_at INTEGER);"
                 "CREATE TABLE IF NOT EXISTS behavioral_feedback("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "behavior_type TEXT, contact_id TEXT, signal TEXT,"
                 "context TEXT, timestamp INTEGER);"
                 "CREATE TABLE IF NOT EXISTS self_evaluations("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "contact_id TEXT, week TEXT, metrics TEXT,"
                 "recommendations TEXT, created_at INTEGER);"
                 "CREATE TABLE IF NOT EXISTS growth_milestones("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "contact_id TEXT, topic TEXT, before_state TEXT,"
                 "after_state TEXT, created_at INTEGER);"
                 "CREATE TABLE IF NOT EXISTS opinions("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "topic TEXT, position TEXT, confidence REAL,"
                 "first_expressed INTEGER, last_expressed INTEGER,"
                 "superseded_by TEXT);"
                 "CREATE TABLE IF NOT EXISTS cognitive_load_log("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "capacity REAL, conversation_depth REAL,"
                 "hour_of_day INTEGER, day_of_week INTEGER,"
                 "physical_state TEXT, recorded_at INTEGER);"
                 "CREATE TABLE IF NOT EXISTS causal_observations("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "action TEXT, outcome TEXT, context TEXT,"
                 "confidence REAL, observed_at INTEGER);"
                 "CREATE TABLE IF NOT EXISTS learning_signals("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "type INTEGER, context TEXT, tool_name TEXT,"
                 "magnitude REAL, timestamp INTEGER);"
                 "CREATE TABLE IF NOT EXISTS strategy_weights("
                 "strategy TEXT PRIMARY KEY, weight REAL, updated_at INTEGER);"
                 "CREATE TABLE IF NOT EXISTS kv(key TEXT PRIMARY KEY, value TEXT);"
                 "CREATE TABLE IF NOT EXISTS skills("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "name TEXT, type TEXT, contact_id TEXT,"
                 "trigger_conditions TEXT, strategy TEXT,"
                 "success_rate REAL DEFAULT 0, attempts INTEGER DEFAULT 0,"
                 "successes INTEGER DEFAULT 0, version INTEGER DEFAULT 1,"
                 "origin TEXT, parent_skill_id INTEGER DEFAULT 0,"
                 "created_at INTEGER, updated_at INTEGER, retired INTEGER DEFAULT 0);"
                 "CREATE TABLE IF NOT EXISTS skill_attempts("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "skill_id INTEGER, contact_id TEXT, applied_at INTEGER,"
                 "outcome_signal TEXT, outcome_evidence TEXT, context TEXT);",
                 NULL, NULL, NULL);
    return db;
}

static void cycle_actions_high_findings(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = setup_cycle_db();
    HU_ASSERT_NOT_NULL(db);
    int64_t now = (int64_t)time(NULL);

    sqlite3_exec(
        db,
        "INSERT INTO research_findings(source, finding, relevance, priority, "
        "suggested_action, status, created_at) VALUES "
        "('test', 'Important finding', 'high', 'HIGH', 'Investigate this', 'pending', 1000),"
        "('test', 'Low finding', 'low', 'LOW', 'Monitor later', 'pending', 1000);",
        NULL, NULL, NULL);

    sqlite3_exec(db,
                 "INSERT INTO feed_items(source, content_type, content, ingested_at) "
                 "VALUES ('rss', 'article', 'Test article about AI', ?);",
                 NULL, NULL, NULL);
    sqlite3_stmt *fi = NULL;
    sqlite3_prepare_v2(db,
                       "INSERT INTO feed_items(source, content_type, content, ingested_at) VALUES "
                       "('rss', 'article', 'Test article about AI', ?)",
                       -1, &fi, NULL);
    sqlite3_bind_int64(fi, 1, now);
    sqlite3_step(fi);
    sqlite3_finalize(fi);

    hu_intelligence_cycle_result_t result;
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.findings_actioned, 1u);
    HU_ASSERT_TRUE(result.causal_recorded >= 1);

    sqlite3_stmt *chk = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM research_findings WHERE status = 'actioned'", -1,
                       &chk, NULL);
    HU_ASSERT_EQ(sqlite3_step(chk), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(chk, 0), 1);
    sqlite3_finalize(chk);

    sqlite3_close(db);
}

static void cycle_populates_current_events(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = setup_cycle_db();
    int64_t now = (int64_t)time(NULL);

    sqlite3_stmt *ins = NULL;
    sqlite3_prepare_v2(
        db, "INSERT INTO feed_items(source, content_type, content, ingested_at) VALUES (?,?,?,?)",
        -1, &ins, NULL);
    for (int i = 0; i < 5; i++) {
        char content[64];
        snprintf(content, sizeof(content), "Feed item %d about AI", i);
        sqlite3_bind_text(ins, 1, "rss", -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 2, "article", -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, content, -1, SQLITE_STATIC);
        sqlite3_bind_int64(ins, 4, now);
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);

    hu_intelligence_cycle_result_t result;
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.events_recorded, 5u);

    sqlite3_stmt *chk = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM current_events", -1, &chk, NULL);
    HU_ASSERT_EQ(sqlite3_step(chk), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(chk, 0), 5);
    sqlite3_finalize(chk);

    sqlite3_close(db);
}

static void cycle_populates_opinions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = setup_cycle_db();

    /* Seed as 'pending' and let the cycle action it, exactly as
     * cycle_actions_high_findings does. Pre-seeding 'actioned' with acted_at NULL
     * is a state production never creates: Step 1 sets status and acted_at in the
     * SAME update (cycle.c "UPDATE research_findings SET status = ?, acted_at = ?"),
     * so an actioned row always carries a timestamp. The old fixture only passed
     * because Step 8 was unscoped -- which is the bug that grew `opinions` to
     * 9.5M rows by re-inserting every historical finding on every cycle. */
    sqlite3_exec(db,
                 "INSERT INTO research_findings(source, finding, relevance, priority, "
                 "suggested_action, status, created_at) VALUES "
                 "('test', 'AI safety critical', 'high', 'HIGH', 'Review safety protocols', "
                 "'pending', 1000);",
                 NULL, NULL, NULL);

    hu_intelligence_cycle_result_t result;
    hu_error_t err = hu_intelligence_run_cycle(&alloc, db, &result);
    HU_ASSERT_EQ(err, HU_OK);

    sqlite3_stmt *chk = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM opinions", -1, &chk, NULL);
    HU_ASSERT_EQ(sqlite3_step(chk), SQLITE_ROW);
    HU_ASSERT_TRUE(sqlite3_column_int(chk, 0) >= 1);
    sqlite3_finalize(chk);

    sqlite3_close(db);
}

static void cycle_records_cognitive_load(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = setup_cycle_db();
    int64_t now = (int64_t)time(NULL);

    sqlite3_stmt *ins = NULL;
    sqlite3_prepare_v2(db,
                       "INSERT INTO feed_items(source, content_type, content, ingested_at) VALUES "
                       "('rss', 'article', 'AI news', ?)",
                       -1, &ins, NULL);
    sqlite3_bind_int64(ins, 1, now);
    sqlite3_step(ins);
    sqlite3_finalize(ins);

    hu_intelligence_cycle_result_t result;
    hu_intelligence_run_cycle(&alloc, db, &result);

    sqlite3_stmt *chk = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM cognitive_load_log", -1, &chk, NULL);
    HU_ASSERT_EQ(sqlite3_step(chk), SQLITE_ROW);
    HU_ASSERT_TRUE(sqlite3_column_int(chk, 0) >= 1);
    sqlite3_finalize(chk);

    sqlite3_close(db);
}

static void cycle_null_args_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_intelligence_cycle_result_t result;
    HU_ASSERT_EQ(hu_intelligence_run_cycle(NULL, NULL, &result), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_intelligence_run_cycle(&alloc, NULL, &result), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_intelligence_run_cycle(&alloc, (sqlite3 *)1, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void e2e_ingest_findings_cycle_learns(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = setup_cycle_db();
    HU_ASSERT_NOT_NULL(db);
    int64_t now = (int64_t)time(NULL);

    sqlite3_stmt *ins = NULL;
    sqlite3_prepare_v2(
        db, "INSERT INTO feed_items(source, content_type, content, ingested_at) VALUES (?,?,?,?)",
        -1, &ins, NULL);
    const char *articles[] = {
        "New transformer architecture achieves SOTA on reasoning benchmarks",
        "Anthropic releases Claude 4 with improved autonomous coding",
        "OpenAI introduces GPT-5 with native tool use",
        "Google DeepMind publishes Gemini 3 technical report",
        "Meta open-sources Llama 4 with 405B parameters",
    };
    for (int i = 0; i < 5; i++) {
        sqlite3_bind_text(ins, 1, "rss", -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 2, "article", -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, articles[i], -1, SQLITE_STATIC);
        sqlite3_bind_int64(ins, 4, now - (i * 60));
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);

    const char *agent_output =
        "- **Source**: rss\n"
        "- **Finding**: New transformer beats SOTA on reasoning\n"
        "- **Relevance**: Could improve h-uman's provider layer\n"
        "- **Priority**: HIGH\n"
        "- **Suggested Action**: Investigate new architecture for integration\n\n"
        "- **Source**: rss\n"
        "- **Finding**: Claude 4 improves autonomous coding\n"
        "- **Relevance**: Direct competitor analysis needed\n"
        "- **Priority**: MEDIUM\n"
        "- **Suggested Action**: Investigate competitive features and benchmark\n\n"
        "- **Source**: rss\n"
        "- **Finding**: Llama 4 open-sourced with 405B params\n"
        "- **Relevance**: Could serve as local provider option\n"
        "- **Priority**: HIGH\n"
        "- **Suggested Action**: Investigate adding Llama 4 as provider\n";

    hu_error_t err = hu_findings_parse_and_store(&alloc, db, agent_output, strlen(agent_output));
    HU_ASSERT_EQ(err, HU_OK);

    sqlite3_stmt *fc = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM research_findings", -1, &fc, NULL);
    sqlite3_step(fc);
    int findings_count = sqlite3_column_int(fc, 0);
    sqlite3_finalize(fc);
    HU_ASSERT_TRUE(findings_count >= 3);

    hu_intelligence_cycle_result_t result;
    err = hu_intelligence_run_cycle(&alloc, db, &result);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT_TRUE(result.findings_actioned >= 3);
    HU_ASSERT_TRUE(result.events_recorded >= 1);
    HU_ASSERT_TRUE(result.causal_recorded >= 1);

    sqlite3_stmt *chk = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM general_lessons", -1, &chk, NULL);
    HU_ASSERT_EQ(sqlite3_step(chk), SQLITE_ROW);
    int lessons = sqlite3_column_int(chk, 0);
    sqlite3_finalize(chk);
    HU_ASSERT_TRUE(lessons >= 0);

    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM current_events", -1, &chk, NULL);
    HU_ASSERT_EQ(sqlite3_step(chk), SQLITE_ROW);
    int events = sqlite3_column_int(chk, 0);
    sqlite3_finalize(chk);
    HU_ASSERT_TRUE(events >= 1);

    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM inferred_values", -1, &chk, NULL);
    HU_ASSERT_EQ(sqlite3_step(chk), SQLITE_ROW);
    int values = sqlite3_column_int(chk, 0);
    sqlite3_finalize(chk);
    HU_ASSERT_TRUE(values >= 0);

    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM opinions", -1, &chk, NULL);
    HU_ASSERT_EQ(sqlite3_step(chk), SQLITE_ROW);
    int opinions = sqlite3_column_int(chk, 0);
    sqlite3_finalize(chk);
    HU_ASSERT_TRUE(opinions >= 0);

    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM behavioral_feedback", -1, &chk, NULL);
    HU_ASSERT_EQ(sqlite3_step(chk), SQLITE_ROW);
    int feedback = sqlite3_column_int(chk, 0);
    sqlite3_finalize(chk);
    HU_ASSERT_TRUE(feedback >= 0);

    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM cognitive_load_log", -1, &chk, NULL);
    HU_ASSERT_EQ(sqlite3_step(chk), SQLITE_ROW);
    int cog = sqlite3_column_int(chk, 0);
    sqlite3_finalize(chk);
    HU_ASSERT_TRUE(cog >= 1);

    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM growth_milestones", -1, &chk, NULL);
    HU_ASSERT_EQ(sqlite3_step(chk), SQLITE_ROW);
    int milestones = sqlite3_column_int(chk, 0);
    sqlite3_finalize(chk);
    HU_ASSERT_TRUE(milestones >= 1);

    sqlite3_close(db);
}

#endif /* HU_ENABLE_SQLITE */

void run_feeds_tests(void) {
    HU_TEST_SUITE("feeds");
    HU_RUN_TEST(feeds_create_table_sql_valid);
    HU_RUN_TEST(feeds_insert_sql_valid);
    HU_RUN_TEST(feeds_query_unprocessed_sql_valid);
    HU_RUN_TEST(feeds_mark_processed_sql_valid);
    HU_RUN_TEST(feeds_query_by_topic_sql_valid);
    HU_RUN_TEST(feeds_should_poll_enabled_interval_elapsed);
    HU_RUN_TEST(feeds_should_poll_disabled_returns_false);
    HU_RUN_TEST(feeds_should_poll_interval_not_elapsed_returns_false);
    HU_RUN_TEST(feeds_should_poll_interval_zero_returns_true_when_enabled);
    HU_RUN_TEST(feeds_feed_type_str_all_types);
    HU_RUN_TEST(feeds_score_relevance_high_overlap);
    HU_RUN_TEST(feeds_score_relevance_no_overlap);
    HU_RUN_TEST(feeds_score_relevance_partial);
    HU_RUN_TEST(feeds_score_relevance_null_returns_zero);
    HU_RUN_TEST(feeds_build_prompt_empty_returns_none);
    HU_RUN_TEST(feeds_build_prompt_single_item);
    HU_RUN_TEST(feeds_build_prompt_multiple_items);
    HU_RUN_TEST(feeds_feed_item_deinit_frees_memory);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(feed_search_fts_match);
    HU_RUN_TEST(feed_build_daily_digest_groups_by_source);
    HU_RUN_TEST(feed_processor_cleanup_removes_old);
    HU_RUN_TEST(feed_get_all_recent_returns_items);
    HU_RUN_TEST(feed_correlate_groups_similar);
    HU_RUN_TEST(findings_store_and_retrieve);
    HU_RUN_TEST(findings_mark_status);
    HU_RUN_TEST(findings_parse_and_store_valid);
    HU_RUN_TEST(findings_parse_and_store_malformed);
    HU_RUN_TEST(findings_parse_and_store_null_args);
    HU_RUN_TEST(findings_get_all_with_limit);
    HU_RUN_TEST(findings_store_null_args);
    HU_RUN_TEST(findings_get_pending_null_args);
    HU_RUN_TEST(feed_detect_trends_no_spikes);
    HU_RUN_TEST(feed_trends_build_section_empty);
    HU_RUN_TEST(cycle_actions_high_findings);
    HU_RUN_TEST(cycle_populates_current_events);
    HU_RUN_TEST(cycle_populates_opinions);
    HU_RUN_TEST(cycle_records_cognitive_load);
    HU_RUN_TEST(cycle_null_args_returns_error);
    HU_RUN_TEST(e2e_ingest_findings_cycle_learns);
#endif
}
