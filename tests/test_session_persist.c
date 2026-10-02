#include "human/agent.h"
#include "human/agent/session_persist.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Test Helpers ───────────────────────────────────────────────────────── */

static hu_tracking_allocator_t *g_ta;
static hu_allocator_t g_alloc;
static char g_tmpdir[256];

/* Minimal mock provider vtable */
static const char *mock_get_name(void *ctx) {
    (void)ctx;
    return "mock";
}
static hu_error_t mock_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                            const char *model, size_t model_len, double temperature,
                            hu_chat_response_t *out) {
    (void)ctx;
    (void)alloc;
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    (void)out;
    return HU_OK;
}
static hu_provider_vtable_t g_mock_vtable = {
    .get_name = mock_get_name,
    .chat = mock_chat,
};

static void make_tmpdir(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/hu_session_test_XXXXXX");
    HU_ASSERT_NOT_NULL(mkdtemp(g_tmpdir));
}

static void rm_rf(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0) {
        /* best-effort cleanup; a (void) cast does not silence glibc warn_unused_result */
    }
}

static void setup(void) {
    g_ta = hu_tracking_allocator_create();
    g_alloc = hu_tracking_allocator_allocator(g_ta);
    make_tmpdir();
}

static void teardown(void) {
    rm_rf(g_tmpdir);
    hu_tracking_allocator_destroy(g_ta);
}

static void init_agent(hu_agent_t *agent) {
    memset(agent, 0, sizeof(*agent));
    agent->alloc = &g_alloc;
    hu_provider_t prov = {.vtable = &g_mock_vtable, .ctx = NULL};
    agent->provider = prov;
    agent->model_name = hu_strdup(&g_alloc, "test-model");
    agent->model_name_len = 10;
    agent->workspace_dir = hu_strdup(&g_alloc, "/tmp/test-workspace");
    agent->workspace_dir_len = 19;
    agent->history = NULL;
    agent->history_count = 0;
    agent->history_cap = 0;
}

static void add_message(hu_agent_t *agent, hu_role_t role, const char *content) {
    size_t new_count = agent->history_count + 1;
    hu_owned_message_t *new_hist = (hu_owned_message_t *)agent->alloc->realloc(
        agent->alloc->ctx, agent->history, agent->history_cap * sizeof(hu_owned_message_t),
        new_count * sizeof(hu_owned_message_t));
    HU_ASSERT_NOT_NULL(new_hist);
    agent->history = new_hist;
    agent->history_cap = new_count;

    hu_owned_message_t *msg = &agent->history[agent->history_count];
    memset(msg, 0, sizeof(*msg));
    msg->role = role;
    msg->content = hu_strdup(agent->alloc, content);
    msg->content_len = strlen(content);
    agent->history_count = new_count;
}

static void free_agent(hu_agent_t *agent) {
    for (size_t i = 0; i < agent->history_count; i++) {
        hu_owned_message_t *m = &agent->history[i];
        if (m->content)
            agent->alloc->free(agent->alloc->ctx, m->content, m->content_len + 1);
        if (m->name)
            agent->alloc->free(agent->alloc->ctx, m->name, m->name_len + 1);
        if (m->tool_call_id)
            agent->alloc->free(agent->alloc->ctx, m->tool_call_id, m->tool_call_id_len + 1);
        if (m->tool_calls) {
            for (size_t j = 0; j < m->tool_calls_count; j++) {
                hu_tool_call_t *tc = &m->tool_calls[j];
                if (tc->id)
                    agent->alloc->free(agent->alloc->ctx, (void *)tc->id, tc->id_len + 1);
                if (tc->name)
                    agent->alloc->free(agent->alloc->ctx, (void *)tc->name, tc->name_len + 1);
                if (tc->arguments)
                    agent->alloc->free(agent->alloc->ctx, (void *)tc->arguments,
                                       tc->arguments_len + 1);
            }
            agent->alloc->free(agent->alloc->ctx, m->tool_calls,
                               m->tool_calls_count * sizeof(hu_tool_call_t));
        }
    }
    if (agent->history)
        agent->alloc->free(agent->alloc->ctx, agent->history,
                           agent->history_cap * sizeof(hu_owned_message_t));
    if (agent->model_name)
        agent->alloc->free(agent->alloc->ctx, agent->model_name, agent->model_name_len + 1);
    if (agent->workspace_dir)
        agent->alloc->free(agent->alloc->ctx, agent->workspace_dir, agent->workspace_dir_len + 1);
}

/* ── Tests ──────────────────────────────────────────────────────────────── */

static void test_generate_id_format(void) {
    char id[HU_SESSION_ID_MAX];
    hu_session_generate_id(id, sizeof(id));
    HU_ASSERT(strlen(id) > 0);
    HU_ASSERT(strncmp(id, "session_", 8) == 0);
    HU_ASSERT(strlen(id) == 23); /* session_YYYYMMDD_HHMMSS */
}

static void test_save_creates_file(void) {
    setup();
    hu_agent_t agent;
    init_agent(&agent);
    add_message(&agent, HU_ROLE_USER, "Hello");
    add_message(&agent, HU_ROLE_ASSISTANT, "Hi there!");

    char sid[HU_SESSION_ID_MAX];
    hu_error_t err = hu_session_persist_save(&g_alloc, &agent, g_tmpdir, sid);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(strlen(sid) > 0);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.json", g_tmpdir, sid);
    struct stat st;
    HU_ASSERT_EQ(stat(path, &st), 0);
    HU_ASSERT_GT(st.st_size, 0);

    free_agent(&agent);
    teardown();
}

static void test_round_trip_save_load(void) {
    setup();
    hu_agent_t agent;
    init_agent(&agent);
    add_message(&agent, HU_ROLE_USER, "What is 2+2?");
    add_message(&agent, HU_ROLE_ASSISTANT, "4");
    add_message(&agent, HU_ROLE_USER, "Thanks!");

    char sid[HU_SESSION_ID_MAX];
    hu_error_t err = hu_session_persist_save(&g_alloc, &agent, g_tmpdir, sid);
    HU_ASSERT_EQ(err, HU_OK);

    /* Load into a fresh agent */
    hu_agent_t agent2;
    init_agent(&agent2);
    err = hu_session_persist_load(&g_alloc, &agent2, g_tmpdir, sid);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(agent2.history_count, 3);
    HU_ASSERT_EQ(agent2.history[0].role, HU_ROLE_USER);
    HU_ASSERT_STR_EQ(agent2.history[0].content, "What is 2+2?");
    HU_ASSERT_EQ(agent2.history[1].role, HU_ROLE_ASSISTANT);
    HU_ASSERT_STR_EQ(agent2.history[1].content, "4");
    HU_ASSERT_EQ(agent2.history[2].role, HU_ROLE_USER);
    HU_ASSERT_STR_EQ(agent2.history[2].content, "Thanks!");
    HU_ASSERT_STR_EQ(agent2.session_id, sid);

    free_agent(&agent);
    free_agent(&agent2);
    teardown();
}

static void test_round_trip_with_tool_calls(void) {
    setup();
    hu_agent_t agent;
    init_agent(&agent);
    add_message(&agent, HU_ROLE_USER, "Search for cats");

    /* Add assistant message with tool_calls */
    add_message(&agent, HU_ROLE_ASSISTANT, "");
    hu_owned_message_t *last = &agent.history[agent.history_count - 1];
    last->tool_calls = (hu_tool_call_t *)g_alloc.alloc(g_alloc.ctx, sizeof(hu_tool_call_t));
    HU_ASSERT_NOT_NULL(last->tool_calls);
    memset(last->tool_calls, 0, sizeof(hu_tool_call_t));
    last->tool_calls[0].id = hu_strdup(&g_alloc, "tc_001");
    last->tool_calls[0].id_len = 6;
    last->tool_calls[0].name = hu_strdup(&g_alloc, "web_search");
    last->tool_calls[0].name_len = 10;
    last->tool_calls[0].arguments = hu_strdup(&g_alloc, "{\"q\":\"cats\"}");
    last->tool_calls[0].arguments_len = 12;
    last->tool_calls_count = 1;

    /* Add tool result */
    add_message(&agent, HU_ROLE_TOOL, "Results for cats...");
    agent.history[agent.history_count - 1].tool_call_id = hu_strdup(&g_alloc, "tc_001");
    agent.history[agent.history_count - 1].tool_call_id_len = 6;

    char sid[HU_SESSION_ID_MAX];
    hu_error_t err = hu_session_persist_save(&g_alloc, &agent, g_tmpdir, sid);
    HU_ASSERT_EQ(err, HU_OK);

    hu_agent_t agent2;
    init_agent(&agent2);
    err = hu_session_persist_load(&g_alloc, &agent2, g_tmpdir, sid);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(agent2.history_count, 3);

    /* Verify tool calls preserved */
    HU_ASSERT_EQ(agent2.history[1].tool_calls_count, 1);
    HU_ASSERT_STR_EQ(agent2.history[1].tool_calls[0].id, "tc_001");
    HU_ASSERT_STR_EQ(agent2.history[1].tool_calls[0].name, "web_search");
    HU_ASSERT_STR_EQ(agent2.history[1].tool_calls[0].arguments, "{\"q\":\"cats\"}");

    /* Verify tool result */
    HU_ASSERT_STR_EQ(agent2.history[2].tool_call_id, "tc_001");

    free_agent(&agent);
    free_agent(&agent2);
    teardown();
}

static void test_load_not_found(void) {
    setup();
    hu_agent_t agent;
    init_agent(&agent);
    hu_error_t err = hu_session_persist_load(&g_alloc, &agent, g_tmpdir, "nonexistent_session");
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
    free_agent(&agent);
    teardown();
}

static void test_load_malformed_json(void) {
    setup();
    /* Write invalid JSON to a session file */
    char path[512];
    snprintf(path, sizeof(path), "%s/bad_session.json", g_tmpdir);
    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    fprintf(f, "{this is not valid json!!!}");
    fclose(f);

    hu_agent_t agent;
    init_agent(&agent);
    hu_error_t err = hu_session_persist_load(&g_alloc, &agent, g_tmpdir, "bad_session");
    HU_ASSERT_EQ(err, HU_ERR_PARSE);
    free_agent(&agent);
    teardown();
}

static void test_load_wrong_schema_version(void) {
    setup();
    char path[512];
    snprintf(path, sizeof(path), "%s/wrong_ver.json", g_tmpdir);
    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    fprintf(f, "{\"schema_version\":99,\"metadata\":{},\"messages\":[]}");
    fclose(f);

    hu_agent_t agent;
    init_agent(&agent);
    hu_error_t err = hu_session_persist_load(&g_alloc, &agent, g_tmpdir, "wrong_ver");
    HU_ASSERT_EQ(err, HU_ERR_PARSE);
    free_agent(&agent);
    teardown();
}

static void test_load_missing_messages_field(void) {
    setup();
    char path[512];
    snprintf(path, sizeof(path), "%s/no_msgs.json", g_tmpdir);
    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    fprintf(f, "{\"schema_version\":1,\"metadata\":{}}");
    fclose(f);

    hu_agent_t agent;
    init_agent(&agent);
    hu_error_t err = hu_session_persist_load(&g_alloc, &agent, g_tmpdir, "no_msgs");
    HU_ASSERT_EQ(err, HU_ERR_PARSE);
    free_agent(&agent);
    teardown();
}

static void test_list_sessions(void) {
    setup();
    hu_agent_t agent;
    init_agent(&agent);
    add_message(&agent, HU_ROLE_USER, "msg1");

    /* Save two sessions */
    char sid1[HU_SESSION_ID_MAX];
    hu_error_t err = hu_session_persist_save(&g_alloc, &agent, g_tmpdir, sid1);
    HU_ASSERT_EQ(err, HU_OK);

    /* Ensure different timestamps by slightly modifying the second save */
    add_message(&agent, HU_ROLE_ASSISTANT, "reply");

    /* Manually create a second session file */
    char path2[512];
    snprintf(path2, sizeof(path2), "%s/session_other.json", g_tmpdir);
    FILE *f = fopen(path2, "w");
    HU_ASSERT_NOT_NULL(f);
    fprintf(f, "{\"schema_version\":1,\"metadata\":{\"id\":\"session_other\",\"created_at\":100,"
               "\"model_name\":\"gpt-4\",\"workspace_dir\":\"/x\",\"message_count\":5},"
               "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}");
    fclose(f);

    hu_session_metadata_t *sessions = NULL;
    size_t count = 0;
    err = hu_session_persist_list(&g_alloc, g_tmpdir, &sessions, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2);

    /* Verify at least one has the expected metadata */
    bool found_other = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(sessions[i].id, "session_other") == 0) {
            found_other = true;
            HU_ASSERT_EQ(sessions[i].message_count, 5);
            HU_ASSERT_STR_EQ(sessions[i].model_name, "gpt-4");
        }
    }
    HU_ASSERT(found_other);

    hu_session_metadata_free(&g_alloc, sessions, count);
    free_agent(&agent);
    teardown();
}

static void test_list_empty_dir(void) {
    setup();
    hu_session_metadata_t *sessions = NULL;
    size_t count = 0;
    hu_error_t err = hu_session_persist_list(&g_alloc, g_tmpdir, &sessions, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
    teardown();
}

static void test_list_nonexistent_dir(void) {
    setup();
    hu_session_metadata_t *sessions = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_session_persist_list(&g_alloc, "/tmp/hu_no_such_dir_xyz", &sessions, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
    teardown();
}

static void test_delete_session(void) {
    setup();
    hu_agent_t agent;
    init_agent(&agent);
    add_message(&agent, HU_ROLE_USER, "to delete");

    char sid[HU_SESSION_ID_MAX];
    hu_error_t err = hu_session_persist_save(&g_alloc, &agent, g_tmpdir, sid);
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_session_persist_delete(g_tmpdir, sid);
    HU_ASSERT_EQ(err, HU_OK);

    /* Verify file is gone */
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.json", g_tmpdir, sid);
    struct stat st;
    HU_ASSERT_NEQ(stat(path, &st), 0);

    free_agent(&agent);
    teardown();
}

static void test_delete_nonexistent(void) {
    setup();
    hu_error_t err = hu_session_persist_delete(g_tmpdir, "nonexistent_id");
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
    teardown();
}

static void test_path_traversal_rejected(void) {
    setup();
    hu_agent_t agent;
    init_agent(&agent);

    /* Attempt to load with path traversal */
    hu_error_t err = hu_session_persist_load(&g_alloc, &agent, g_tmpdir, "../../../etc/passwd");
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    /* Attempt to delete with path traversal */
    err = hu_session_persist_delete(g_tmpdir, "../../../etc/passwd");
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    free_agent(&agent);
    teardown();
}

static void test_save_empty_history(void) {
    setup();
    hu_agent_t agent;
    init_agent(&agent);
    /* No messages added */

    char sid[HU_SESSION_ID_MAX];
    hu_error_t err = hu_session_persist_save(&g_alloc, &agent, g_tmpdir, sid);
    HU_ASSERT_EQ(err, HU_OK);

    /* Load it back */
    hu_agent_t agent2;
    init_agent(&agent2);
    err = hu_session_persist_load(&g_alloc, &agent2, g_tmpdir, sid);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(agent2.history_count, 0);

    free_agent(&agent);
    free_agent(&agent2);
    teardown();
}

static void test_null_args_rejected(void) {
    hu_error_t err;
    err = hu_session_persist_save(NULL, NULL, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    err = hu_session_persist_load(NULL, NULL, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    err = hu_session_persist_list(NULL, NULL, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    err = hu_session_persist_delete(NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_save_preserves_all_roles(void) {
    setup();
    hu_agent_t agent;
    init_agent(&agent);
    add_message(&agent, HU_ROLE_SYSTEM, "You are helpful.");
    add_message(&agent, HU_ROLE_USER, "Hi");
    add_message(&agent, HU_ROLE_ASSISTANT, "Hello!");
    add_message(&agent, HU_ROLE_TOOL, "tool output");

    char sid[HU_SESSION_ID_MAX];
    hu_error_t err = hu_session_persist_save(&g_alloc, &agent, g_tmpdir, sid);
    HU_ASSERT_EQ(err, HU_OK);

    hu_agent_t agent2;
    init_agent(&agent2);
    err = hu_session_persist_load(&g_alloc, &agent2, g_tmpdir, sid);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(agent2.history_count, 4);
    HU_ASSERT_EQ(agent2.history[0].role, HU_ROLE_SYSTEM);
    HU_ASSERT_EQ(agent2.history[1].role, HU_ROLE_USER);
    HU_ASSERT_EQ(agent2.history[2].role, HU_ROLE_ASSISTANT);
    HU_ASSERT_EQ(agent2.history[3].role, HU_ROLE_TOOL);

    free_agent(&agent);
    free_agent(&agent2);
    teardown();
}

static void test_save_reuses_agent_session_id(void) {
    setup();
    hu_agent_t agent;
    init_agent(&agent);
    add_message(&agent, HU_ROLE_USER, "Hello");
    add_message(&agent, HU_ROLE_ASSISTANT, "Hi there!");

    /* Set a stable session ID on the agent */
    const char *stable_id = "my-stable-session";
    size_t sid_len = strlen(stable_id);
    memcpy(agent.session_id, stable_id, sid_len);
    agent.session_id[sid_len] = '\0';

    char sid_out[HU_SESSION_ID_MAX];
    hu_error_t err = hu_session_persist_save(&g_alloc, &agent, g_tmpdir, sid_out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(sid_out, "my-stable-session");

    /* Verify file is named with the stable ID, not a generated timestamp */
    char path[512];
    snprintf(path, sizeof(path), "%s/my-stable-session.json", g_tmpdir);
    struct stat st;
    HU_ASSERT_EQ(stat(path, &st), 0);

    /* Round-trip: load with same ID, verify history */
    hu_agent_t agent2;
    init_agent(&agent2);
    err = hu_session_persist_load(&g_alloc, &agent2, g_tmpdir, "my-stable-session");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(agent2.history_count, 2);
    HU_ASSERT_STR_EQ(agent2.history[0].content, "Hello");
    HU_ASSERT_STR_EQ(agent2.history[1].content, "Hi there!");
    HU_ASSERT_STR_EQ(agent2.session_id, "my-stable-session");

    free_agent(&agent);
    free_agent(&agent2);
    teardown();
}

static void test_save_overwrites_same_session_id(void) {
    setup();
    hu_agent_t agent;
    init_agent(&agent);
    add_message(&agent, HU_ROLE_USER, "Turn 1");
    add_message(&agent, HU_ROLE_ASSISTANT, "Reply 1");

    const char *stable_id = "accumulate-test";
    size_t sid_len = strlen(stable_id);
    memcpy(agent.session_id, stable_id, sid_len);
    agent.session_id[sid_len] = '\0';

    hu_error_t err = hu_session_persist_save(&g_alloc, &agent, g_tmpdir, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    /* Simulate second invocation: load, add turn, save again */
    hu_agent_t agent2;
    init_agent(&agent2);
    err = hu_session_persist_load(&g_alloc, &agent2, g_tmpdir, stable_id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(agent2.history_count, 2);

    add_message(&agent2, HU_ROLE_USER, "Turn 2");
    add_message(&agent2, HU_ROLE_ASSISTANT, "Reply 2");
    err = hu_session_persist_save(&g_alloc, &agent2, g_tmpdir, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    /* Third invocation: load and verify all 4 messages accumulated */
    hu_agent_t agent3;
    init_agent(&agent3);
    err = hu_session_persist_load(&g_alloc, &agent3, g_tmpdir, stable_id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(agent3.history_count, 4);
    HU_ASSERT_STR_EQ(agent3.history[0].content, "Turn 1");
    HU_ASSERT_STR_EQ(agent3.history[1].content, "Reply 1");
    HU_ASSERT_STR_EQ(agent3.history[2].content, "Turn 2");
    HU_ASSERT_STR_EQ(agent3.history[3].content, "Reply 2");

    free_agent(&agent);
    free_agent(&agent2);
    free_agent(&agent3);
    teardown();
}

static void test_default_dir(void) {
    setup();
    char *dir = hu_session_default_dir(&g_alloc);
    HU_ASSERT_NOT_NULL(dir);
    HU_ASSERT(strstr(dir, "/.human/sessions") != NULL);
    g_alloc.free(g_alloc.ctx, dir, strlen(dir) + 1);
    teardown();
}

/* ── Test Runner ────────────────────────────────────────────────────────── */

void test_session_persist(void) {
    HU_TEST_SUITE("session_persist");
    HU_RUN_TEST(test_generate_id_format);
    HU_RUN_TEST(test_save_creates_file);
    HU_RUN_TEST(test_round_trip_save_load);
    HU_RUN_TEST(test_round_trip_with_tool_calls);
    HU_RUN_TEST(test_load_not_found);
    HU_RUN_TEST(test_load_malformed_json);
    HU_RUN_TEST(test_load_wrong_schema_version);
    HU_RUN_TEST(test_load_missing_messages_field);
    HU_RUN_TEST(test_list_sessions);
    HU_RUN_TEST(test_list_empty_dir);
    HU_RUN_TEST(test_list_nonexistent_dir);
    HU_RUN_TEST(test_delete_session);
    HU_RUN_TEST(test_delete_nonexistent);
    HU_RUN_TEST(test_path_traversal_rejected);
    HU_RUN_TEST(test_save_empty_history);
    HU_RUN_TEST(test_null_args_rejected);
    HU_RUN_TEST(test_save_preserves_all_roles);
    HU_RUN_TEST(test_save_reuses_agent_session_id);
    HU_RUN_TEST(test_save_overwrites_same_session_id);
    HU_RUN_TEST(test_default_dir);
}
