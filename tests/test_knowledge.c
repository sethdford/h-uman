#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/memory/knowledge.h"
#include "test_framework.h"
#include <string.h>

static void test_knowledge_create_table_sql_valid(void) {
    char buf[1024];
    size_t len = 0;
    hu_error_t err = hu_knowledge_create_table_sql(buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "CREATE TABLE") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "contact_knowledge") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "UNIQUE") != NULL);
}

static void test_knowledge_insert_sql_escapes_quotes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_knowledge_entry_t entry = {
        .id = 1,
        .topic = "O'Brien's party",
        .topic_len = 16,
        .detail = NULL,
        .detail_len = 0,
        .contact_id = "alice",
        .contact_id_len = 5,
        .shared_at = 1000,
        .source = HU_KNOWLEDGE_DIRECT,
        .confidence = 1.0,
    };
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_knowledge_insert_sql(&entry, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "''") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "O'Brien") != NULL || strstr(buf, "O''Brien") != NULL);
}

static void test_knowledge_insert_sql_maps_source_enum(void) {
    hu_knowledge_entry_t entry = {
        .id = 2,
        .topic = "job_interview",
        .topic_len = 13,
        .detail = NULL,
        .detail_len = 0,
        .contact_id = "bob",
        .contact_id_len = 3,
        .shared_at = 2000,
        .source = HU_KNOWLEDGE_GROUP_CHAT,
        .confidence = 0.8,
    };
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_knowledge_insert_sql(&entry, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "group_chat") != NULL);
}

static void test_knowledge_query_sql_with_contact(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err =
        hu_knowledge_query_sql("user_123", 8, 0.5, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "user_123") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "confidence") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "WHERE") != NULL);
}

static void test_knowledge_contact_knows_true_when_told(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_knowledge_entry_t entries[1];
    memset(entries, 0, sizeof(entries));
    entries[0].id = 1;
    entries[0].topic = hu_strndup(&alloc, "job_interview", 13);
    entries[0].topic_len = 13;
    entries[0].detail = NULL;
    entries[0].detail_len = 0;
    entries[0].contact_id = hu_strndup(&alloc, "alice", 5);
    entries[0].contact_id_len = 5;
    entries[0].shared_at = 1000;
    entries[0].source = HU_KNOWLEDGE_DIRECT;
    entries[0].confidence = 1.0;

    bool knows = hu_knowledge_contact_knows(entries, 1, "alice", 5, "job_interview", 13, 0.7);
    HU_ASSERT_TRUE(knows);

    hu_knowledge_entry_deinit(&alloc, &entries[0]);
}

static void test_knowledge_contact_knows_false_when_not_told(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_knowledge_entry_t entries[1];
    memset(entries, 0, sizeof(entries));
    entries[0].id = 1;
    entries[0].topic = hu_strndup(&alloc, "job_interview", 13);
    entries[0].topic_len = 13;
    entries[0].detail = NULL;
    entries[0].detail_len = 0;
    entries[0].contact_id = hu_strndup(&alloc, "alice", 5);
    entries[0].contact_id_len = 5;
    entries[0].shared_at = 1000;
    entries[0].source = HU_KNOWLEDGE_DIRECT;
    entries[0].confidence = 1.0;

    bool knows = hu_knowledge_contact_knows(entries, 1, "bob", 3, "job_interview", 13, 0.7);
    HU_ASSERT_FALSE(knows);

    hu_knowledge_entry_deinit(&alloc, &entries[0]);
}

static void test_knowledge_contact_knows_respects_min_confidence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_knowledge_entry_t entries[1];
    memset(entries, 0, sizeof(entries));
    entries[0].id = 1;
    entries[0].topic = hu_strndup(&alloc, "job_interview", 13);
    entries[0].topic_len = 13;
    entries[0].detail = NULL;
    entries[0].detail_len = 0;
    entries[0].contact_id = hu_strndup(&alloc, "alice", 5);
    entries[0].contact_id_len = 5;
    entries[0].shared_at = 1000;
    entries[0].source = HU_KNOWLEDGE_INFERRED;
    entries[0].confidence = 0.5;

    bool knows = hu_knowledge_contact_knows(entries, 1, "alice", 5, "job_interview", 13, 0.7);
    HU_ASSERT_FALSE(knows);

    hu_knowledge_entry_deinit(&alloc, &entries[0]);
}

static void test_knowledge_would_leak_detects_private_info(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_knowledge_entry_t entries[1];
    memset(entries, 0, sizeof(entries));
    entries[0].id = 1;
    entries[0].topic = hu_strndup(&alloc, "health_issue", 12);
    entries[0].topic_len = 12;
    entries[0].detail = NULL;
    entries[0].detail_len = 0;
    entries[0].contact_id = hu_strndup(&alloc, "alice", 5);
    entries[0].contact_id_len = 5;
    entries[0].shared_at = 1000;
    entries[0].source = HU_KNOWLEDGE_DIRECT;
    entries[0].confidence = 1.0;

    bool leak = hu_knowledge_would_leak(entries, 1, "health_issue", 12, "alice", 5, "bob", 3);
    HU_ASSERT_TRUE(leak);

    hu_knowledge_entry_deinit(&alloc, &entries[0]);
}

static void test_knowledge_would_leak_false_for_public_info(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_knowledge_entry_t entries[1];
    memset(entries, 0, sizeof(entries));
    entries[0].id = 1;
    entries[0].topic = hu_strndup(&alloc, "new_job", 7);
    entries[0].topic_len = 7;
    entries[0].detail = NULL;
    entries[0].detail_len = 0;
    entries[0].contact_id = hu_strndup(&alloc, "alice", 5);
    entries[0].contact_id_len = 5;
    entries[0].shared_at = 1000;
    entries[0].source = HU_KNOWLEDGE_PUBLIC;
    entries[0].confidence = 1.0;

    bool leak = hu_knowledge_would_leak(entries, 1, "new_job", 7, "alice", 5, "bob", 3);
    HU_ASSERT_FALSE(leak);

    hu_knowledge_entry_deinit(&alloc, &entries[0]);
}

static void test_knowledge_would_leak_false_when_both_know(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_knowledge_entry_t entries[2];
    memset(entries, 0, sizeof(entries));
    entries[0].id = 1;
    entries[0].topic = hu_strndup(&alloc, "new_apartment", 13);
    entries[0].topic_len = 13;
    entries[0].detail = NULL;
    entries[0].detail_len = 0;
    entries[0].contact_id = hu_strndup(&alloc, "alice", 5);
    entries[0].contact_id_len = 5;
    entries[0].shared_at = 1000;
    entries[0].source = HU_KNOWLEDGE_DIRECT;
    entries[0].confidence = 1.0;

    entries[1].id = 2;
    entries[1].topic = hu_strndup(&alloc, "new_apartment", 13);
    entries[1].topic_len = 13;
    entries[1].detail = NULL;
    entries[1].detail_len = 0;
    entries[1].contact_id = hu_strndup(&alloc, "bob", 3);
    entries[1].contact_id_len = 3;
    entries[1].shared_at = 1100;
    entries[1].source = HU_KNOWLEDGE_DIRECT;
    entries[1].confidence = 1.0;

    bool leak = hu_knowledge_would_leak(entries, 2, "new_apartment", 13, "alice", 5, "bob", 3);
    HU_ASSERT_FALSE(leak);

    hu_knowledge_entry_deinit(&alloc, &entries[0]);
    hu_knowledge_entry_deinit(&alloc, &entries[1]);
}

static void test_knowledge_build_summary_categorizes_correctly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_knowledge_entry_t entries[3];
    memset(entries, 0, sizeof(entries));
    entries[0].id = 1;
    entries[0].topic = hu_strndup(&alloc, "known_topic", 11);
    entries[0].topic_len = 11;
    entries[0].detail = NULL;
    entries[0].detail_len = 0;
    entries[0].contact_id = hu_strndup(&alloc, "alice", 5);
    entries[0].contact_id_len = 5;
    entries[0].shared_at = 1000;
    entries[0].source = HU_KNOWLEDGE_DIRECT;
    entries[0].confidence = 0.9;

    entries[1].id = 2;
    entries[1].topic = hu_strndup(&alloc, "uncertain_topic", 15);
    entries[1].topic_len = 15;
    entries[1].detail = NULL;
    entries[1].detail_len = 0;
    entries[1].contact_id = hu_strndup(&alloc, "alice", 5);
    entries[1].contact_id_len = 5;
    entries[1].shared_at = 1100;
    entries[1].source = HU_KNOWLEDGE_GROUP_CHAT;
    entries[1].confidence = 0.5;

    entries[2].id = 3;
    entries[2].topic = hu_strndup(&alloc, "other_known", 11);
    entries[2].topic_len = 11;
    entries[2].detail = NULL;
    entries[2].detail_len = 0;
    entries[2].contact_id = hu_strndup(&alloc, "alice", 5);
    entries[2].contact_id_len = 5;
    entries[2].shared_at = 1200;
    entries[2].source = HU_KNOWLEDGE_DIRECT;
    entries[2].confidence = 0.8;

    const char *all_topics[] = {"known_topic", "uncertain_topic", "unknown_topic"};
    hu_knowledge_summary_t summary = {0};
    hu_error_t err = hu_knowledge_build_summary(&alloc, entries, 3, "alice", 5, all_topics, 3,
                                                 &summary);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(summary.known_count, 1);
    HU_ASSERT_EQ(summary.uncertain_count, 1);
    HU_ASSERT_EQ(summary.unknown_count, 1);

    hu_knowledge_summary_deinit(&alloc, &summary);
    hu_knowledge_entry_deinit(&alloc, &entries[0]);
    hu_knowledge_entry_deinit(&alloc, &entries[1]);
    hu_knowledge_entry_deinit(&alloc, &entries[2]);
}

#define HU_KNOWLEDGE_SUMMARY_MAX_TOPICS 64

static void test_knowledge_summary_to_prompt_contains_sections(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_knowledge_summary_t summary = {0};
    summary.contact_id = hu_strndup(&alloc, "alice", 5);
    summary.contact_id_len = 5;
    summary.known_topics =
        (char **)alloc.alloc(alloc.ctx, HU_KNOWLEDGE_SUMMARY_MAX_TOPICS * sizeof(char *));
    summary.known_topics[0] = hu_strndup(&alloc, "topic1", 6);
    summary.known_topics[1] = hu_strndup(&alloc, "topic2", 6);
    summary.known_count = 2;
    summary.unknown_topics =
        (char **)alloc.alloc(alloc.ctx, HU_KNOWLEDGE_SUMMARY_MAX_TOPICS * sizeof(char *));
    summary.unknown_topics[0] = hu_strndup(&alloc, "topicA", 6);
    summary.unknown_count = 1;
    summary.uncertain_topics =
        (char **)alloc.alloc(alloc.ctx, HU_KNOWLEDGE_SUMMARY_MAX_TOPICS * sizeof(char *));
    summary.uncertain_topics[0] = hu_strndup(&alloc, "topicX", 6);
    summary.uncertain_count = 1;

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_knowledge_summary_to_prompt(&alloc, &summary, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "know about") != NULL);
    HU_ASSERT_TRUE(strstr(out, "DON'T know") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Uncertain") != NULL);

    hu_str_free(&alloc, out);
    hu_knowledge_summary_deinit(&alloc, &summary);
}

static void test_knowledge_entry_deinit_frees_all(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_knowledge_entry_t entry = {0};
    entry.topic = hu_strndup(&alloc, "topic", 5);
    entry.topic_len = 5;
    entry.detail = hu_strndup(&alloc, "detail", 6);
    entry.detail_len = 6;
    entry.contact_id = hu_strndup(&alloc, "contact", 7);
    entry.contact_id_len = 7;

    hu_knowledge_entry_deinit(&alloc, &entry);

    HU_ASSERT_NULL(entry.topic);
    HU_ASSERT_NULL(entry.detail);
    HU_ASSERT_NULL(entry.contact_id);
}

static void test_knowledge_summary_deinit_frees_all(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_knowledge_entry_t entries[1];
    memset(entries, 0, sizeof(entries));
    entries[0].id = 1;
    entries[0].topic = hu_strndup(&alloc, "t", 1);
    entries[0].topic_len = 1;
    entries[0].detail = NULL;
    entries[0].detail_len = 0;
    entries[0].contact_id = hu_strndup(&alloc, "c", 1);
    entries[0].contact_id_len = 1;
    entries[0].shared_at = 0;
    entries[0].source = HU_KNOWLEDGE_DIRECT;
    entries[0].confidence = 0.9;

    const char *topics[] = {"t"};
    hu_knowledge_summary_t summary = {0};
    hu_knowledge_build_summary(&alloc, entries, 1, "c", 1, topics, 1, &summary);
    hu_knowledge_summary_deinit(&alloc, &summary);

    HU_ASSERT_NULL(summary.contact_id);
    HU_ASSERT_NULL(summary.known_topics);
    hu_knowledge_entry_deinit(&alloc, &entries[0]);
}

static void test_knowledge_contact_knows_case_insensitive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_knowledge_entry_t entries[1];
    memset(entries, 0, sizeof(entries));
    entries[0].id = 1;
    entries[0].topic = hu_strndup(&alloc, "job_interview", 13);
    entries[0].topic_len = 13;
    entries[0].detail = NULL;
    entries[0].detail_len = 0;
    entries[0].contact_id = hu_strndup(&alloc, "alice", 5);
    entries[0].contact_id_len = 5;
    entries[0].shared_at = 1000;
    entries[0].source = HU_KNOWLEDGE_DIRECT;
    entries[0].confidence = 1.0;

    bool knows = hu_knowledge_contact_knows(entries, 1, "alice", 5, "Job Interview", 13, 0.7);
    HU_ASSERT_TRUE(knows);

    hu_knowledge_entry_deinit(&alloc, &entries[0]);
}

void run_knowledge_tests(void) {
    HU_TEST_SUITE("knowledge");
    HU_RUN_TEST(test_knowledge_create_table_sql_valid);
    HU_RUN_TEST(test_knowledge_insert_sql_escapes_quotes);
    HU_RUN_TEST(test_knowledge_insert_sql_maps_source_enum);
    HU_RUN_TEST(test_knowledge_query_sql_with_contact);
    HU_RUN_TEST(test_knowledge_contact_knows_true_when_told);
    HU_RUN_TEST(test_knowledge_contact_knows_false_when_not_told);
    HU_RUN_TEST(test_knowledge_contact_knows_respects_min_confidence);
    HU_RUN_TEST(test_knowledge_would_leak_detects_private_info);
    HU_RUN_TEST(test_knowledge_would_leak_false_for_public_info);
    HU_RUN_TEST(test_knowledge_would_leak_false_when_both_know);
    HU_RUN_TEST(test_knowledge_build_summary_categorizes_correctly);
    HU_RUN_TEST(test_knowledge_summary_to_prompt_contains_sections);
    HU_RUN_TEST(test_knowledge_entry_deinit_frees_all);
    HU_RUN_TEST(test_knowledge_summary_deinit_frees_all);
    HU_RUN_TEST(test_knowledge_contact_knows_case_insensitive);
}
