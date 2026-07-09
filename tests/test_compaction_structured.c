#include "test_framework.h"
#include "human/agent/compaction.h"
#include "human/agent/compaction_structured.h"
#include "human/core/allocator.h"

/* ── Test helpers ────────────────────────────────────────────────────── */

static hu_owned_message_t make_msg(hu_role_t role, const char *text) {
    hu_owned_message_t m;
    memset(&m, 0, sizeof(m));
    m.role = role;
    if (text) {
        m.content_len = strlen(text);
        m.content = (char *)malloc(m.content_len + 1);
        memcpy(m.content, text, m.content_len + 1);
    }
    return m;
}

static hu_owned_message_t make_msg_with_tool(hu_role_t role, const char *text,
                                              const char *tool_name) {
    hu_owned_message_t m = make_msg(role, text);
    if (tool_name) {
        m.tool_calls = (hu_tool_call_t *)malloc(sizeof(hu_tool_call_t));
        memset(m.tool_calls, 0, sizeof(hu_tool_call_t));
        m.tool_calls_count = 1;
        m.tool_calls[0].name_len = strlen(tool_name);
        m.tool_calls[0].name = (char *)malloc(m.tool_calls[0].name_len + 1);
        memcpy((char *)m.tool_calls[0].name, tool_name, m.tool_calls[0].name_len + 1);
    }
    return m;
}

static void free_msg(hu_allocator_t *alloc, hu_owned_message_t *m) {
    if (m->content) { free(m->content); m->content = NULL; }
    if (m->tool_calls) {
        for (size_t i = 0; i < m->tool_calls_count; i++) {
            if (m->tool_calls[i].name) free((void *)m->tool_calls[i].name);
            if (m->tool_calls[i].id) free((void *)m->tool_calls[i].id);
            if (m->tool_calls[i].arguments) free((void *)m->tool_calls[i].arguments);
        }
        free(m->tool_calls);
        m->tool_calls = NULL;
    }
    (void)alloc;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

static void test_strip_analysis_basic(void) {
    char buf[] = "hello <analysis>secret stuff</analysis> world";
    size_t len = hu_compact_strip_analysis(buf, strlen(buf));
    HU_ASSERT_STR_EQ(buf, "hello  world");
    HU_ASSERT_EQ(len, strlen("hello  world"));
}

static void test_strip_analysis_multiple(void) {
    char buf[] = "a<analysis>1</analysis>b<analysis>2</analysis>c";
    size_t len = hu_compact_strip_analysis(buf, strlen(buf));
    HU_ASSERT_STR_EQ(buf, "abc");
    HU_ASSERT_EQ(len, 3);
}

static void test_strip_analysis_no_close(void) {
    char buf[] = "before <analysis>unclosed";
    size_t len = hu_compact_strip_analysis(buf, strlen(buf));
    HU_ASSERT_STR_EQ(buf, "before ");
    HU_ASSERT_EQ(len, strlen("before "));
}

static void test_strip_analysis_empty(void) {
    HU_ASSERT_EQ(hu_compact_strip_analysis(NULL, 0), 0);
    char buf[] = "";
    HU_ASSERT_EQ(hu_compact_strip_analysis(buf, 0), 0);
}

static void test_extract_metadata_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_owned_message_t msgs[4];
    msgs[0] = make_msg(HU_ROLE_SYSTEM, "You are helpful.");
    msgs[1] = make_msg(HU_ROLE_USER, "Hello world");
    msgs[2] = make_msg_with_tool(HU_ROLE_ASSISTANT, "Using file_read", "file_read");
    msgs[3] = make_msg(HU_ROLE_USER, "Thanks!");

    hu_compaction_summary_t meta;
    hu_error_t err = hu_compact_extract_metadata(&alloc, msgs, 4, 2, &meta);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(meta.total_messages, 4);
    HU_ASSERT_EQ(meta.preserved_count, 2);
    HU_ASSERT_EQ(meta.summarized_count, 1); /* 3 non-system - 2 kept = 1 */
    HU_ASSERT_EQ(meta.tool_mentions_count, 1);
    HU_ASSERT_STR_EQ(meta.tool_mentions[0], "file_read");
    HU_ASSERT_NOT_NULL(meta.recent_user_requests);
    HU_ASSERT_STR_CONTAINS(meta.recent_user_requests, "Thanks!");
    HU_ASSERT_STR_CONTAINS(meta.recent_user_requests, "Hello world");

    hu_compaction_summary_free(&alloc, &meta);
    for (int i = 0; i < 4; i++) free_msg(&alloc, &msgs[i]);
}

static void test_extract_metadata_dedup_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_owned_message_t msgs[3];
    msgs[0] = make_msg_with_tool(HU_ROLE_ASSISTANT, "a", "file_read");
    msgs[1] = make_msg_with_tool(HU_ROLE_ASSISTANT, "b", "file_read");
    msgs[2] = make_msg_with_tool(HU_ROLE_ASSISTANT, "c", "shell");

    hu_compaction_summary_t meta;
    hu_error_t err = hu_compact_extract_metadata(&alloc, msgs, 3, 1, &meta);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(meta.tool_mentions_count, 2); /* file_read + shell, deduplicated */

    hu_compaction_summary_free(&alloc, &meta);
    for (int i = 0; i < 3; i++) free_msg(&alloc, &msgs[i]);
}

static void test_extract_metadata_pending_work(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_owned_message_t msgs[2];
    msgs[0] = make_msg(HU_ROLE_USER, "Fix the bug");
    msgs[1] = make_msg(HU_ROLE_ASSISTANT, "I found the issue. The next step is to update the test.");

    hu_compaction_summary_t meta;
    hu_error_t err = hu_compact_extract_metadata(&alloc, msgs, 2, 1, &meta);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(meta.pending_work_inference);
    HU_ASSERT_STR_CONTAINS(meta.pending_work_inference, "next step");

    hu_compaction_summary_free(&alloc, &meta);
    for (int i = 0; i < 2; i++) free_msg(&alloc, &msgs[i]);
}

static void test_build_structured_summary(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_owned_message_t msgs[2];
    msgs[0] = make_msg(HU_ROLE_USER, "Please help me <analysis>internal thought</analysis> ok");
    msgs[1] = make_msg(HU_ROLE_ASSISTANT, "Sure, I'll help.");

    hu_compaction_summary_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.total_messages = 2;
    meta.summarized_count = 1;
    meta.preserved_count = 1;

    char *xml = NULL;
    size_t xml_len = 0;
    hu_error_t err = hu_compact_build_structured_summary(&alloc, msgs, 2, &meta, &xml, &xml_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(xml);
    HU_ASSERT_GT(xml_len, 0);

    /* Should contain XML structure */
    HU_ASSERT_STR_CONTAINS(xml, "<summary>");
    HU_ASSERT_STR_CONTAINS(xml, "</summary>");
    HU_ASSERT_STR_CONTAINS(xml, "<stats");
    HU_ASSERT_STR_CONTAINS(xml, "total=\"2\"");

    /* Should NOT contain analysis block content */
    HU_ASSERT_STR_NOT_CONTAINS(xml, "internal thought");
    HU_ASSERT_STR_NOT_CONTAINS(xml, "<analysis>");

    alloc.free(alloc.ctx, xml, xml_len + 1);
    for (int i = 0; i < 2; i++) free_msg(&alloc, &msgs[i]);
}

static void test_build_summary_with_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_compaction_summary_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.total_messages = 5;
    meta.summarized_count = 3;
    meta.preserved_count = 2;

    char *tool1 = strdup("file_read");
    char *tool2 = strdup("shell");
    char *tools[] = { tool1, tool2 };
    meta.tool_mentions = tools;
    meta.tool_mentions_count = 2;

    char *xml = NULL;
    size_t xml_len = 0;
    hu_error_t err = hu_compact_build_structured_summary(&alloc, NULL, 0, &meta, &xml, &xml_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_CONTAINS(xml, "<tools_used>");
    HU_ASSERT_STR_CONTAINS(xml, "<tool>file_read</tool>");
    HU_ASSERT_STR_CONTAINS(xml, "<tool>shell</tool>");

    alloc.free(alloc.ctx, xml, xml_len + 1);
    /* Don't free tools via summary_free since they're stack/strdup'd */
    free(tool1);
    free(tool2);
}

static void test_detect_artifacts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_owned_message_t msgs[2];
    msgs[0] = make_msg(HU_ROLE_ASSISTANT, "I modified /workspace/src/main.c and /workspace/README.md");
    msgs[1] = make_msg(HU_ROLE_USER, "Thanks, also check /workspace/src/main.c again");

    hu_artifact_pin_t *pins = NULL;
    size_t pin_count = 0;
    hu_error_t err = hu_compact_detect_artifacts(&alloc, msgs, 2,
        "/workspace/", 11, &pins, &pin_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(pin_count, 2); /* main.c and README.md, deduplicated */

    hu_artifact_pins_free(&alloc, pins, pin_count);
    for (int i = 0; i < 2; i++) free_msg(&alloc, &msgs[i]);
}

static void test_is_pinned(void) {
    hu_artifact_pin_t pin;
    pin.file_path = (char *)"/workspace/src/main.c";
    pin.file_path_len = strlen(pin.file_path);
    pin.last_modified_ts = 0;

    hu_owned_message_t m1 = make_msg(HU_ROLE_ASSISTANT, "Edited /workspace/src/main.c");
    HU_ASSERT_TRUE(hu_compact_is_pinned(&m1, &pin, 1));

    hu_owned_message_t m2 = make_msg(HU_ROLE_USER, "Hello world");
    HU_ASSERT_FALSE(hu_compact_is_pinned(&m2, &pin, 1));

    free_msg(NULL, &m1);
    free_msg(NULL, &m2);
}

static void test_continuation_preamble(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_owned_message_t history[8];
    memset(history, 0, sizeof(history));
    size_t count = 2;
    size_t cap = 8;

    history[0] = make_msg(HU_ROLE_SYSTEM, "System prompt");
    history[1] = make_msg(HU_ROLE_USER, "Hello");

    hu_compaction_summary_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.total_messages = 10;
    meta.summarized_count = 8;
    meta.preserved_count = 2;

    hu_owned_message_t *hist = history;
    hu_error_t err = hu_compact_inject_continuation_preamble(&alloc, &meta, &hist, &count, &cap);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 3);

    /* System prompt stays at 0 */
    HU_ASSERT_EQ(history[0].role, HU_ROLE_SYSTEM);
    /* Preamble inserted at 1 */
    HU_ASSERT_EQ(history[1].role, HU_ROLE_USER);
    HU_ASSERT_STR_CONTAINS(history[1].content, "continued from a previous conversation");
    HU_ASSERT_STR_CONTAINS(history[1].content, "10 messages");
    /* Original user message shifted to 2 */
    HU_ASSERT_EQ(history[2].role, HU_ROLE_USER);
    HU_ASSERT_STR_EQ(history[2].content, "Hello");

    /* Clean up */
    free(history[0].content);
    alloc.free(alloc.ctx, history[1].content, history[1].content_len + 1);
    free(history[2].content);
}

static void test_extract_metadata_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_compaction_summary_t meta;

    HU_ASSERT_EQ(hu_compact_extract_metadata(NULL, NULL, 0, 0, &meta), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_compact_extract_metadata(&alloc, NULL, 0, 0, &meta), HU_ERR_INVALID_ARGUMENT);
}

static void test_build_summary_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *xml = NULL;
    size_t len = 0;

    HU_ASSERT_EQ(hu_compact_build_structured_summary(NULL, NULL, 0, NULL, &xml, &len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_compact_build_structured_summary(&alloc, NULL, 0, NULL, &xml, &len),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_detect_artifacts_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_artifact_pin_t *pins = NULL;
    size_t count = 0;

    hu_error_t err = hu_compact_detect_artifacts(&alloc, NULL, 0, "/ws/", 4, &pins, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
    HU_ASSERT_NULL(pins);
}

static void test_is_pinned_empty(void) {
    hu_owned_message_t m = make_msg(HU_ROLE_USER, "hello");
    HU_ASSERT_FALSE(hu_compact_is_pinned(&m, NULL, 0));
    HU_ASSERT_FALSE(hu_compact_is_pinned(NULL, NULL, 0));
    free_msg(NULL, &m);
}

static void test_build_summary_xml_escaping(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_owned_message_t msgs[1];
    msgs[0] = make_msg(HU_ROLE_USER, "Use <b>bold</b> & \"quotes\"");

    hu_compaction_summary_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.total_messages = 1;

    char *xml = NULL;
    size_t xml_len = 0;
    hu_error_t err = hu_compact_build_structured_summary(&alloc, msgs, 1, &meta, &xml, &xml_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_CONTAINS(xml, "&lt;b&gt;bold&lt;/b&gt;");
    HU_ASSERT_STR_CONTAINS(xml, "&amp;");
    HU_ASSERT_STR_CONTAINS(xml, "&quot;quotes&quot;");

    alloc.free(alloc.ctx, xml, xml_len + 1);
    free_msg(&alloc, &msgs[0]);
}

static void test_compaction_with_structured_flag_enabled(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Build a history that exceeds compact threshold */
    hu_owned_message_t *history = (hu_owned_message_t *)malloc(55 * sizeof(hu_owned_message_t));
    memset(history, 0, 55 * sizeof(hu_owned_message_t));

    /* System prompt */
    history[0] = make_msg(HU_ROLE_SYSTEM, "You are helpful.");

    /* Add 54 user/assistant alternating messages to trigger compaction */
    for (int i = 1; i < 55; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Message %d", i);
        history[i] = make_msg(i % 2 == 0 ? HU_ROLE_USER : HU_ROLE_ASSISTANT, buf);
    }

    size_t history_count = 55;
    size_t history_cap = 55;

    /* Create compaction config with structured flag enabled */
    hu_compaction_config_t cfg;
    hu_compaction_config_default(&cfg);
    cfg.use_structured_summary = true;  /* This is the key flag being tested */
    cfg.keep_recent = 5;
    cfg.max_history_messages = 50;
    cfg.token_limit = 100000;

    /* Perform compaction */
    hu_error_t err = hu_compact_history(&alloc, &history, &history_count, &history_cap, &cfg);
    HU_ASSERT_EQ(err, HU_OK);

    /* Verify that compaction happened */
    HU_ASSERT_TRUE(history_count < 55);

    /* Key assertion: the compacted summary should contain XML structure,
     * not plain text. The structured summary contains <summary> tags. */
    bool has_summary_tags = false;
    for (size_t i = 0; i < history_count; i++) {
        if (history[i].content && strstr(history[i].content, "<summary>")) {
            has_summary_tags = true;
            break;
        }
    }
    HU_ASSERT_TRUE(has_summary_tags);

    /* Clean up */
    for (size_t i = 0; i < history_count; i++) {
        if (history[i].content)
            free(history[i].content);
    }
    free(history);
}

static void test_compaction_with_structured_flag_disabled(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Build a history that exceeds compact threshold */
    hu_owned_message_t *history = (hu_owned_message_t *)malloc(55 * sizeof(hu_owned_message_t));
    memset(history, 0, 55 * sizeof(hu_owned_message_t));

    /* System prompt */
    history[0] = make_msg(HU_ROLE_SYSTEM, "You are helpful.");

    /* Add 54 user/assistant alternating messages to trigger compaction */
    for (int i = 1; i < 55; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Message %d", i);
        history[i] = make_msg(i % 2 == 0 ? HU_ROLE_USER : HU_ROLE_ASSISTANT, buf);
    }

    size_t history_count = 55;
    size_t history_cap = 55;

    /* Create compaction config with structured flag DISABLED (false) */
    hu_compaction_config_t cfg;
    hu_compaction_config_default(&cfg);
    cfg.use_structured_summary = false;  /* Disabled, should use plain text */
    cfg.keep_recent = 5;
    cfg.max_history_messages = 50;
    cfg.token_limit = 100000;

    /* Perform compaction */
    hu_error_t err = hu_compact_history(&alloc, &history, &history_count, &history_cap, &cfg);
    HU_ASSERT_EQ(err, HU_OK);

    /* Verify that compaction happened */
    HU_ASSERT_TRUE(history_count < 55);

    /* With structured flag disabled, summary should NOT contain <summary> tags */
    bool has_summary_tags = false;
    for (size_t i = 0; i < history_count; i++) {
        if (history[i].content && strstr(history[i].content, "<summary>")) {
            has_summary_tags = true;
            break;
        }
    }
    HU_ASSERT_FALSE(has_summary_tags);

    /* Clean up */
    for (size_t i = 0; i < history_count; i++) {
        if (history[i].content)
            free(history[i].content);
    }
    free(history);
}

/* The default conversation-history window was widened 50 -> 120 (2026-06-06)
 * so long iMessage threads keep earlier context. Pin the generous default so a
 * future edit can't silently shrink it back. */
static void test_compaction_default_history_window_is_generous(void) {
    hu_compaction_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_compaction_config_default(&cfg);
    HU_ASSERT_EQ((int)cfg.max_history_messages, (int)HU_COMPACTION_DEFAULT_MAX_HISTORY);
    HU_ASSERT_TRUE(cfg.max_history_messages >= 120); /* not the old 50 cap */
}

/* ── Test Runner ──────────────────────────────────────────────────────── */

void run_compaction_structured_tests(void) {
    HU_TEST_SUITE("compaction_structured");

    HU_RUN_TEST(test_compaction_default_history_window_is_generous);
    HU_RUN_TEST(test_strip_analysis_basic);
    HU_RUN_TEST(test_strip_analysis_multiple);
    HU_RUN_TEST(test_strip_analysis_no_close);
    HU_RUN_TEST(test_strip_analysis_empty);
    HU_RUN_TEST(test_extract_metadata_basic);
    HU_RUN_TEST(test_extract_metadata_dedup_tools);
    HU_RUN_TEST(test_extract_metadata_pending_work);
    HU_RUN_TEST(test_build_structured_summary);
    HU_RUN_TEST(test_build_summary_with_tools);
    HU_RUN_TEST(test_build_summary_xml_escaping);
    HU_RUN_TEST(test_detect_artifacts);
    HU_RUN_TEST(test_is_pinned);
    HU_RUN_TEST(test_is_pinned_empty);
    HU_RUN_TEST(test_continuation_preamble);
    HU_RUN_TEST(test_extract_metadata_null_args);
    HU_RUN_TEST(test_build_summary_null_args);
    HU_RUN_TEST(test_detect_artifacts_empty);
    HU_RUN_TEST(test_compaction_with_structured_flag_enabled);
    HU_RUN_TEST(test_compaction_with_structured_flag_disabled);
}
