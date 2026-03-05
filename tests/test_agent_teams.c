/* Tests for worktree integration, team config, mailbox, and task list. */

#include "seaclaw/agent.h"
#include "seaclaw/agent/mailbox.h"
#include "seaclaw/agent/spawn.h"
#include "seaclaw/agent/task_list.h"
#include "seaclaw/agent/team.h"
#include "seaclaw/agent/worktree.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/providers/factory.h"
#include "test_framework.h"
#include <string.h>

static void test_worktree_create_tracks_metadata(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_worktree_manager_t *mgr = sc_worktree_manager_create(&alloc, "/tmp/repo");
    SC_ASSERT_NOT_NULL(mgr);

    const char *path = NULL;
    sc_error_t err = sc_worktree_create(mgr, 42, "task-a", &path);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(path);
    SC_ASSERT_STR_EQ(path, "/tmp/repo/../.worktrees/task-a");
    sc_worktree_manager_destroy(mgr);
}

static void test_worktree_remove_marks_inactive(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_worktree_manager_t *mgr = sc_worktree_manager_create(&alloc, "/tmp/repo");
    SC_ASSERT_NOT_NULL(mgr);

    const char *path = NULL;
    SC_ASSERT_EQ(sc_worktree_create(mgr, 1, "label", &path), SC_OK);

    SC_ASSERT_EQ(sc_worktree_remove(mgr, 1), SC_OK);

    SC_ASSERT_NULL(sc_worktree_path_for_agent(mgr, 1));
    sc_worktree_manager_destroy(mgr);
}

static void test_worktree_list_shows_active_worktrees(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_worktree_manager_t *mgr = sc_worktree_manager_create(&alloc, "/tmp/repo");
    SC_ASSERT_NOT_NULL(mgr);

    const char *p1 = NULL, *p2 = NULL;
    SC_ASSERT_EQ(sc_worktree_create(mgr, 1, "a", &p1), SC_OK);
    SC_ASSERT_EQ(sc_worktree_create(mgr, 2, "b", &p2), SC_OK);

    SC_ASSERT_EQ(sc_worktree_remove(mgr, 1), SC_OK);

    sc_worktree_t *list = NULL;
    size_t count = 0;
    SC_ASSERT_EQ(sc_worktree_list(mgr, &list, &count), SC_OK);
    SC_ASSERT_EQ(count, 1u);
    SC_ASSERT_NOT_NULL(list);
    SC_ASSERT_STR_EQ(list[0].path, "/tmp/repo/../.worktrees/b");
    SC_ASSERT_EQ(list[0].agent_id, 2u);

    sc_worktree_list_free(&alloc, list, count);
    sc_worktree_manager_destroy(mgr);
}

static void test_worktree_path_for_agent_returns_correct_path(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_worktree_manager_t *mgr = sc_worktree_manager_create(&alloc, "/tmp/repo");
    SC_ASSERT_NOT_NULL(mgr);

    const char *path = NULL;
    SC_ASSERT_EQ(sc_worktree_create(mgr, 99, "task-99", &path), SC_OK);

    const char *got = sc_worktree_path_for_agent(mgr, 99);
    SC_ASSERT_NOT_NULL(got);
    SC_ASSERT_STR_EQ(got, "/tmp/repo/../.worktrees/task-99");

    SC_ASSERT_NULL(sc_worktree_path_for_agent(mgr, 999));
    sc_worktree_manager_destroy(mgr);
}

static void test_team_config_parse_with_3_members(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json =
        "{\"name\":\"checkout-feature\",\"base_branch\":\"main\",\"members\":["
        "{\"name\":\"backend\",\"role\":\"builder\",\"autonomy\":\"assisted\","
        "\"model\":\"anthropic/claude-sonnet\",\"tools\":[\"shell\",\"file_write\"]},"
        "{\"name\":\"frontend\",\"role\":\"builder\",\"autonomy\":\"assisted\"},"
        "{\"name\":\"reviewer\",\"role\":\"reviewer\",\"autonomy\":\"locked\","
        "\"tools\":[\"file_read\"]}]}";
    sc_team_config_t cfg = {0};
    SC_ASSERT_EQ(sc_team_config_parse(&alloc, json, strlen(json), &cfg), SC_OK);
    SC_ASSERT_STR_EQ(cfg.name, "checkout-feature");
    SC_ASSERT_STR_EQ(cfg.base_branch, "main");
    SC_ASSERT_EQ(cfg.members_count, 3u);

    SC_ASSERT_STR_EQ(cfg.members[0].name, "backend");
    SC_ASSERT_STR_EQ(cfg.members[0].role, "builder");
    SC_ASSERT_EQ(cfg.members[0].autonomy, SC_AUTONOMY_ASSISTED);
    SC_ASSERT_STR_EQ(cfg.members[0].model, "anthropic/claude-sonnet");
    SC_ASSERT_EQ(cfg.members[0].allowed_tools_count, 2u);
    SC_ASSERT_STR_EQ(cfg.members[0].allowed_tools[0], "shell");
    SC_ASSERT_STR_EQ(cfg.members[0].allowed_tools[1], "file_write");

    sc_team_config_free(&alloc, &cfg);
}

static void test_team_config_get_member_by_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json =
        "{\"name\":\"team\",\"members\":["
        "{\"name\":\"backend\",\"role\":\"builder\"},"
        "{\"name\":\"reviewer\",\"role\":\"reviewer\"}]}";
    sc_team_config_t cfg = {0};
    SC_ASSERT_EQ(sc_team_config_parse(&alloc, json, strlen(json), &cfg), SC_OK);

    const sc_team_config_member_t *m = sc_team_config_get_member(&cfg, "reviewer");
    SC_ASSERT_NOT_NULL(m);
    SC_ASSERT_STR_EQ(m->name, "reviewer");
    SC_ASSERT_STR_EQ(m->role, "reviewer");

    SC_ASSERT_NULL(sc_team_config_get_member(&cfg, "nonexistent"));

    sc_team_config_free(&alloc, &cfg);
}

static void test_team_config_get_by_role_finds_first_match(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json =
        "{\"name\":\"team\",\"members\":["
        "{\"name\":\"backend\",\"role\":\"builder\"},"
        "{\"name\":\"frontend\",\"role\":\"builder\"},"
        "{\"name\":\"reviewer\",\"role\":\"reviewer\"}]}";
    sc_team_config_t cfg = {0};
    SC_ASSERT_EQ(sc_team_config_parse(&alloc, json, strlen(json), &cfg), SC_OK);

    const sc_team_config_member_t *m = sc_team_config_get_by_role(&cfg, "builder");
    SC_ASSERT_NOT_NULL(m);
    SC_ASSERT_STR_EQ(m->name, "backend");

    sc_team_config_free(&alloc, &cfg);
}

static void test_team_config_parse_autonomy_levels_maps_correctly(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json =
        "{\"name\":\"team\",\"members\":["
        "{\"name\":\"a\",\"autonomy\":\"locked\"},"
        "{\"name\":\"b\",\"autonomy\":\"supervised\"},"
        "{\"name\":\"c\",\"autonomy\":\"assisted\"},"
        "{\"name\":\"d\",\"autonomy\":\"autonomous\"}]}";
    sc_team_config_t cfg = {0};
    SC_ASSERT_EQ(sc_team_config_parse(&alloc, json, strlen(json), &cfg), SC_OK);

    SC_ASSERT_EQ(cfg.members[0].autonomy, SC_AUTONOMY_LOCKED);
    SC_ASSERT_EQ(cfg.members[1].autonomy, SC_AUTONOMY_SUPERVISED);
    SC_ASSERT_EQ(cfg.members[2].autonomy, SC_AUTONOMY_ASSISTED);
    SC_ASSERT_EQ(cfg.members[3].autonomy, SC_AUTONOMY_AUTONOMOUS);

    sc_team_config_free(&alloc, &cfg);
}

static void test_team_config_parse_missing_fields_uses_defaults(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"members\":[{\"name\":\"x\"}]}";
    sc_team_config_t cfg = {0};
    SC_ASSERT_EQ(sc_team_config_parse(&alloc, json, strlen(json), &cfg), SC_OK);

    SC_ASSERT_NULL(cfg.name);
    SC_ASSERT_NULL(cfg.base_branch);
    SC_ASSERT_EQ(cfg.members_count, 1u);
    SC_ASSERT_STR_EQ(cfg.members[0].name, "x");
    SC_ASSERT_NULL(cfg.members[0].role);
    SC_ASSERT_EQ(cfg.members[0].autonomy, SC_AUTONOMY_ASSISTED);
    SC_ASSERT_NULL(cfg.members[0].allowed_tools);
    SC_ASSERT_EQ(cfg.members[0].allowed_tools_count, 0u);
    SC_ASSERT_NULL(cfg.members[0].model);

    sc_team_config_free(&alloc, &cfg);
}

/* ── Runtime team (sc_team_t) ─────────────────────────────────────────── */
static void test_team_add_remove_members(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_team_t *team = sc_team_create(&alloc, "squad");
    SC_ASSERT_NOT_NULL(team);

    SC_ASSERT_EQ(sc_team_add_member(team, 1, "alice", SC_ROLE_LEAD, 2), SC_OK);
    SC_ASSERT_EQ(sc_team_add_member(team, 2, "bob", SC_ROLE_BUILDER, 2), SC_OK);
    SC_ASSERT_EQ(sc_team_member_count(team), 2u);

    SC_ASSERT_EQ(sc_team_remove_member(team, 1), SC_OK);
    SC_ASSERT_EQ(sc_team_member_count(team), 1u);
    SC_ASSERT_NULL(sc_team_get_member(team, 1));
    SC_ASSERT_NOT_NULL(sc_team_get_member(team, 2));

    sc_team_destroy(team);
}

static void test_team_member_count_correct_after_add_remove(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_team_t *team = sc_team_create(&alloc, "t");
    SC_ASSERT_NOT_NULL(team);

    SC_ASSERT_EQ(sc_team_member_count(team), 0u);
    SC_ASSERT_EQ(sc_team_add_member(team, 1, "a", SC_ROLE_REVIEWER, 1), SC_OK);
    SC_ASSERT_EQ(sc_team_member_count(team), 1u);
    SC_ASSERT_EQ(sc_team_add_member(team, 2, "b", SC_ROLE_TESTER, 2), SC_OK);
    SC_ASSERT_EQ(sc_team_member_count(team), 2u);
    SC_ASSERT_EQ(sc_team_remove_member(team, 1), SC_OK);
    SC_ASSERT_EQ(sc_team_member_count(team), 1u);
    SC_ASSERT_EQ(sc_team_remove_member(team, 2), SC_OK);
    SC_ASSERT_EQ(sc_team_member_count(team), 0u);

    sc_team_destroy(team);
}

static void test_team_role_reviewer_denies_file_write(void) {
    SC_ASSERT_FALSE(sc_team_role_allows_tool(SC_ROLE_REVIEWER, "file_write"));
    SC_ASSERT_TRUE(sc_team_role_allows_tool(SC_ROLE_REVIEWER, "file_read"));
    SC_ASSERT_TRUE(sc_team_role_allows_tool(SC_ROLE_REVIEWER, "shell"));
    SC_ASSERT_TRUE(sc_team_role_allows_tool(SC_ROLE_REVIEWER, "memory_recall"));
}

static void test_team_role_lead_allows_all_tools(void) {
    SC_ASSERT_TRUE(sc_team_role_allows_tool(SC_ROLE_LEAD, "file_write"));
    SC_ASSERT_TRUE(sc_team_role_allows_tool(SC_ROLE_LEAD, "file_read"));
    SC_ASSERT_TRUE(sc_team_role_allows_tool(SC_ROLE_LEAD, "agent_spawn"));
    SC_ASSERT_TRUE(sc_team_role_allows_tool(SC_ROLE_LEAD, "shell"));
}

/* ── Mailbox + agent integration ──────────────────────────────────────── */
static void test_mailbox_recv_in_agent_context_works(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_mailbox_t *mb = sc_mailbox_create(&a, 8);
    SC_ASSERT_NOT_NULL(mb);

    sc_provider_t prov = {0};
    SC_ASSERT_EQ(sc_provider_create(&a, "openai", 6, "test-key", 8, "", 0, &prov), SC_OK);

    sc_agent_t agent = {0};
    SC_ASSERT_EQ(sc_agent_from_config(&agent, &a, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      "gpt-4o-mini", 10, "openai", 6, 0.7, ".", 1, 5, 20, false,
                                      2, NULL, 0, NULL),
                 SC_OK);
    sc_agent_set_mailbox(&agent, mb);

    uint64_t agent_id = (uint64_t)(uintptr_t)&agent;
    SC_ASSERT_EQ(sc_mailbox_send(mb, 999, agent_id, SC_MSG_TASK, "API ready at /api/checkout",
                                 24, 0),
                 SC_OK);

    sc_message_t msg = {0};
    SC_ASSERT_EQ(sc_mailbox_recv(mb, agent_id, &msg), SC_OK);
    SC_ASSERT_EQ(msg.type, SC_MSG_TASK);
    SC_ASSERT_EQ(msg.from_agent, 999u);
    SC_ASSERT_NOT_NULL(msg.payload);
    SC_ASSERT_EQ(msg.payload_len, 24u);
    sc_message_free(&a, &msg);

    sc_agent_deinit(&agent);
    sc_mailbox_destroy(mb);
}

static void test_mailbox_register_send_recv_with_agent_id(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_mailbox_t *mb = sc_mailbox_create(&a, 8);
    SC_ASSERT_NOT_NULL(mb);

    uint64_t agent_id = 0x1234;
    SC_ASSERT_EQ(sc_mailbox_register(mb, agent_id), SC_OK);
    SC_ASSERT_EQ(sc_mailbox_send(mb, 999, agent_id, SC_MSG_TASK, "API ready", 9, 0), SC_OK);

    sc_message_t msg = {0};
    SC_ASSERT_EQ(sc_mailbox_recv(mb, agent_id, &msg), SC_OK);
    SC_ASSERT_EQ(msg.type, SC_MSG_TASK);
    SC_ASSERT_EQ(msg.from_agent, 999u);
    SC_ASSERT_NOT_NULL(msg.payload);
    SC_ASSERT_EQ(msg.payload_len, 9u);
    sc_message_free(&a, &msg);

    sc_mailbox_unregister(mb, agent_id);
    sc_mailbox_destroy(mb);
}

static void test_cancel_message_sets_cancel_requested(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_mailbox_t *mb = sc_mailbox_create(&a, 8);
    SC_ASSERT_NOT_NULL(mb);

    sc_provider_t prov = {0};
    SC_ASSERT_EQ(sc_provider_create(&a, "openai", 6, "test-key", 8, "", 0, &prov), SC_OK);

    sc_agent_t agent = {0};
    SC_ASSERT_EQ(sc_agent_from_config(&agent, &a, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      "gpt-4o-mini", 10, "openai", 6, 0.7, ".", 1, 5, 20, false,
                                      2, NULL, 0, NULL),
                 SC_OK);
    sc_agent_set_mailbox(&agent, mb);
    SC_ASSERT_EQ(agent.cancel_requested, 0);

    uint64_t agent_id = (uint64_t)(uintptr_t)&agent;
    SC_ASSERT_EQ(sc_mailbox_send(mb, 1, agent_id, SC_MSG_CANCEL, "stop", 4, 0), SC_OK);

    sc_message_t msg = {0};
    SC_ASSERT_EQ(sc_mailbox_recv(mb, agent_id, &msg), SC_OK);
    SC_ASSERT_EQ(msg.type, SC_MSG_CANCEL);
    agent.cancel_requested = 1;
    sc_message_free(&a, &msg);

    SC_ASSERT_EQ(agent.cancel_requested, 1);

    sc_agent_deinit(&agent);
    sc_mailbox_destroy(mb);
}

/* ── Task list ────────────────────────────────────────────────────────── */
static void test_task_list_add_claim_complete_flow(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_task_list_t *list = sc_task_list_create(&a, NULL, 16);
    SC_ASSERT_NOT_NULL(list);

    uint64_t id = 0;
    SC_ASSERT_EQ(sc_task_list_add(list, "Deploy API", "Deploy the checkout API", NULL, 0, &id),
                 SC_OK);
    SC_ASSERT_EQ(id, 1u);

    SC_ASSERT_EQ(sc_task_list_claim(list, 1, 100), SC_OK);
    sc_task_t t = {0};
    SC_ASSERT_EQ(sc_task_list_get(list, 1, &t), SC_OK);
    SC_ASSERT_EQ(t.status, SC_TASK_LIST_CLAIMED);
    SC_ASSERT_EQ(t.owner_agent_id, 100u);
    sc_task_free(&a, &t);

    SC_ASSERT_EQ(sc_task_list_update_status(list, 1, SC_TASK_LIST_COMPLETED), SC_OK);
    SC_ASSERT_EQ(sc_task_list_count_by_status(list, SC_TASK_LIST_COMPLETED), 1u);

    sc_task_list_destroy(list);
}

static void test_task_blocked_by_incomplete_dependency_stays_blocked(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_task_list_t *list = sc_task_list_create(&a, NULL, 16);
    SC_ASSERT_NOT_NULL(list);

    uint64_t id1 = 0, id2 = 0;
    SC_ASSERT_EQ(sc_task_list_add(list, "Task A", "First", NULL, 0, &id1), SC_OK);
    SC_ASSERT_EQ(id1, 1u);

    uint64_t deps[] = {1};
    SC_ASSERT_EQ(sc_task_list_add(list, "Task B", "Depends on A", deps, 1, &id2), SC_OK);
    SC_ASSERT_EQ(id2, 2u);

    SC_ASSERT_TRUE(sc_task_list_is_blocked(list, 2));

    sc_task_t next = {0};
    SC_ASSERT_EQ(sc_task_list_next_available(list, &next), SC_OK);
    SC_ASSERT_EQ(next.id, 1u);
    sc_task_free(&a, &next);

    sc_task_list_destroy(list);
}

static void test_task_unblocks_when_dependency_completes(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_task_list_t *list = sc_task_list_create(&a, NULL, 16);
    SC_ASSERT_NOT_NULL(list);

    uint64_t id1 = 0, id2 = 0;
    SC_ASSERT_EQ(sc_task_list_add(list, "Task A", "First", NULL, 0, &id1), SC_OK);
    uint64_t deps[] = {1};
    SC_ASSERT_EQ(sc_task_list_add(list, "Task B", "Depends on A", deps, 1, &id2), SC_OK);

    SC_ASSERT_TRUE(sc_task_list_is_blocked(list, 2));
    SC_ASSERT_EQ(sc_task_list_update_status(list, 1, SC_TASK_LIST_COMPLETED), SC_OK);
    SC_ASSERT_FALSE(sc_task_list_is_blocked(list, 2));

    sc_task_t next = {0};
    SC_ASSERT_EQ(sc_task_list_next_available(list, &next), SC_OK);
    SC_ASSERT_EQ(next.id, 2u);
    sc_task_free(&a, &next);

    sc_task_list_destroy(list);
}

static void test_next_available_skips_blocked_tasks(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_task_list_t *list = sc_task_list_create(&a, NULL, 16);
    SC_ASSERT_NOT_NULL(list);

    uint64_t id1 = 0, id2 = 0;
    SC_ASSERT_EQ(sc_task_list_add(list, "Unblocked", "No deps", NULL, 0, &id1), SC_OK);
    uint64_t deps[] = {1};
    SC_ASSERT_EQ(sc_task_list_add(list, "Blocked", "Depends on 1", deps, 1, &id2), SC_OK);

    sc_task_t next = {0};
    SC_ASSERT_EQ(sc_task_list_next_available(list, &next), SC_OK);
    SC_ASSERT_EQ(next.id, 1u);
    sc_task_free(&a, &next);

    sc_task_list_destroy(list);
}

static void test_claim_fails_on_already_claimed_task(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_task_list_t *list = sc_task_list_create(&a, NULL, 16);
    SC_ASSERT_NOT_NULL(list);

    uint64_t id = 0;
    SC_ASSERT_EQ(sc_task_list_add(list, "Task", "Desc", NULL, 0, &id), SC_OK);
    SC_ASSERT_EQ(sc_task_list_claim(list, 1, 100), SC_OK);
    SC_ASSERT_EQ(sc_task_list_claim(list, 1, 200), SC_ERR_ALREADY_EXISTS);

    sc_task_list_destroy(list);
}

/* ── Feature 1: Mailbox in agent loop ───────────────────────────────────── */
static void test_agent_turn_processes_mailbox_messages(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_mailbox_t *mb = sc_mailbox_create(&a, 8);
    SC_ASSERT_NOT_NULL(mb);

    sc_provider_t prov = {0};
    SC_ASSERT_EQ(sc_provider_create(&a, "openai", 6, "test-key", 8, "", 0, &prov), SC_OK);

    sc_agent_t agent = {0};
    SC_ASSERT_EQ(sc_agent_from_config(&agent, &a, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      "gpt-4o-mini", 10, "openai", 6, 0.7, ".", 1, 5, 20, false,
                                      2, NULL, 0, NULL),
                 SC_OK);
    sc_agent_set_mailbox(&agent, mb);

    uint64_t agent_id = (uint64_t)(uintptr_t)&agent;
    SC_ASSERT_EQ(sc_mailbox_send(mb, 999, agent_id, SC_MSG_TASK, "Task from agent 999", 19, 0),
                 SC_OK);

    char *resp = NULL;
    size_t rlen = 0;
    SC_ASSERT_EQ(sc_agent_turn(&agent, "/status", 7, &resp, &rlen), SC_OK);
    SC_ASSERT_NOT_NULL(resp);
    a.free(a.ctx, resp, rlen + 1);

    SC_ASSERT_EQ(agent.history_count, 1u);
    SC_ASSERT_NOT_NULL(agent.history[0].content);
    SC_ASSERT_TRUE(strstr(agent.history[0].content, "[Message from agent 999]") != NULL);
    SC_ASSERT_TRUE(strstr(agent.history[0].content, "Task from agent 999") != NULL);

    sc_agent_deinit(&agent);
    sc_mailbox_destroy(mb);
}

static void test_agent_registers_unregisters_with_mailbox(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_mailbox_t *mb = sc_mailbox_create(&a, 8);
    SC_ASSERT_NOT_NULL(mb);

    sc_provider_t prov = {0};
    SC_ASSERT_EQ(sc_provider_create(&a, "openai", 6, "test-key", 8, "", 0, &prov), SC_OK);

    sc_agent_t agent = {0};
    SC_ASSERT_EQ(sc_agent_from_config(&agent, &a, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      "gpt-4o-mini", 10, "openai", 6, 0.7, ".", 1, 5, 20, false,
                                      2, NULL, 0, NULL),
                 SC_OK);
    agent.agent_id = 1;
    sc_agent_set_mailbox(&agent, mb);

    SC_ASSERT_EQ(sc_mailbox_send(mb, 999, 1, SC_MSG_TASK, "hello", 5, 0), SC_OK);
    sc_message_t msg = {0};
    SC_ASSERT_EQ(sc_mailbox_recv(mb, 1, &msg), SC_OK);
    sc_message_free(&a, &msg);

    sc_agent_deinit(&agent);
    SC_ASSERT_EQ(sc_mailbox_send(mb, 999, 1, SC_MSG_TASK, "after deinit", 12, 0),
                 SC_ERR_NOT_FOUND);

    sc_mailbox_destroy(mb);
}

static void test_send_slash_command_sends_message(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_mailbox_t *mb = sc_mailbox_create(&a, 8);
    SC_ASSERT_NOT_NULL(mb);
    SC_ASSERT_EQ(sc_mailbox_register(mb, 42), SC_OK);

    sc_provider_t prov = {0};
    SC_ASSERT_EQ(sc_provider_create(&a, "openai", 6, "test-key", 8, "", 0, &prov), SC_OK);

    sc_agent_t agent = {0};
    SC_ASSERT_EQ(sc_agent_from_config(&agent, &a, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      "gpt-4o-mini", 10, "openai", 6, 0.7, ".", 1, 5, 20, false,
                                      2, NULL, 0, NULL),
                 SC_OK);
    sc_agent_set_mailbox(&agent, mb);

    char *resp = sc_agent_handle_slash_command(&agent, "/send 42 hello from slash", 25);
    SC_ASSERT_NOT_NULL(resp);
    SC_ASSERT_TRUE(strstr(resp, "Sent to agent") != NULL);
    a.free(a.ctx, resp, strlen(resp) + 1);

    sc_message_t msg = {0};
    SC_ASSERT_EQ(sc_mailbox_recv(mb, 42, &msg), SC_OK);
    SC_ASSERT_NOT_NULL(msg.payload);
    SC_ASSERT_TRUE(strstr(msg.payload, "hello from slash") != NULL);
    sc_message_free(&a, &msg);

    sc_mailbox_unregister(mb, 42);
    sc_agent_deinit(&agent);
    sc_mailbox_destroy(mb);
}

/* ── Feature 2: Task list is_ready and query ────────────────────────────── */
static void test_task_is_ready_when_deps_complete(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_task_list_t *list = sc_task_list_create(&a, NULL, 16);
    SC_ASSERT_NOT_NULL(list);

    uint64_t id1 = 0, id2 = 0;
    SC_ASSERT_EQ(sc_task_list_add(list, "Task A", "First", NULL, 0, &id1), SC_OK);
    uint64_t deps[] = {1};
    SC_ASSERT_EQ(sc_task_list_add(list, "Task B", "Depends on A", deps, 1, &id2), SC_OK);

    SC_ASSERT_TRUE(sc_task_list_is_ready(list, 1));
    SC_ASSERT_FALSE(sc_task_list_is_ready(list, 2));

    SC_ASSERT_EQ(sc_task_list_update_status(list, 1, SC_TASK_LIST_COMPLETED), SC_OK);
    SC_ASSERT_TRUE(sc_task_list_is_ready(list, 2));

    sc_task_list_destroy(list);
}

static void test_task_query_by_status_returns_correct_tasks(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_task_list_t *list = sc_task_list_create(&a, NULL, 16);
    SC_ASSERT_NOT_NULL(list);

    uint64_t id1 = 0, id2 = 0, id3 = 0;
    SC_ASSERT_EQ(sc_task_list_add(list, "P1", NULL, NULL, 0, &id1), SC_OK);
    SC_ASSERT_EQ(sc_task_list_add(list, "P2", NULL, NULL, 0, &id2), SC_OK);
    SC_ASSERT_EQ(sc_task_list_add(list, "P3", NULL, NULL, 0, &id3), SC_OK);
    SC_ASSERT_EQ(sc_task_list_claim(list, 3, 100), SC_OK);
    SC_ASSERT_EQ(sc_task_list_update_status(list, 3, SC_TASK_LIST_COMPLETED), SC_OK);

    sc_task_t *pending = NULL;
    size_t pending_count = 0;
    SC_ASSERT_EQ(sc_task_list_query(list, SC_TASK_LIST_PENDING, &pending, &pending_count),
                 SC_OK);
    SC_ASSERT_EQ(pending_count, 2u);
    SC_ASSERT_NOT_NULL(pending);
    sc_task_array_free(&a, pending, pending_count);

    sc_task_t *completed = NULL;
    size_t completed_count = 0;
    SC_ASSERT_EQ(sc_task_list_query(list, SC_TASK_LIST_COMPLETED, &completed, &completed_count),
                 SC_OK);
    SC_ASSERT_EQ(completed_count, 1u);
    SC_ASSERT_NOT_NULL(completed);
    SC_ASSERT_EQ(completed[0].id, 3u);
    sc_task_array_free(&a, completed, completed_count);

    sc_task_list_destroy(list);
}

/* ── Task list persistence (serialize/deserialize without file I/O) ───────── */
static void test_task_list_serialize_deserialize_round_trip_preserves_all_fields(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_task_list_t *list = sc_task_list_create(&a, NULL, 16);
    SC_ASSERT_NOT_NULL(list);

    uint64_t id = 0;
    SC_ASSERT_EQ(sc_task_list_add(list, "Build checkout API", "REST endpoints for cart + payment",
                                 NULL, 0, &id), SC_OK);
    SC_ASSERT_EQ(id, 1u);
    SC_ASSERT_EQ(sc_task_list_claim(list, 1, 42), SC_OK);
    SC_ASSERT_EQ(sc_task_list_update_status(list, 1, SC_TASK_LIST_IN_PROGRESS), SC_OK);

    char *json = NULL;
    size_t json_len = 0;
    SC_ASSERT_EQ(sc_task_list_serialize(list, &json, &json_len), SC_OK);
    SC_ASSERT_NOT_NULL(json);
    SC_ASSERT_TRUE(json_len > 0);
    SC_ASSERT_TRUE(strstr(json, "Build checkout API") != NULL);
    SC_ASSERT_TRUE(strstr(json, "in_progress") != NULL);
    SC_ASSERT_TRUE(strstr(json, "\"owner\":42") != NULL);

    sc_task_list_t *list2 = sc_task_list_create(&a, NULL, 16);
    SC_ASSERT_NOT_NULL(list2);
    SC_ASSERT_EQ(sc_task_list_deserialize(list2, json, json_len), SC_OK);
    a.free(a.ctx, json, json_len + 1);

    sc_task_t t = {0};
    SC_ASSERT_EQ(sc_task_list_get(list2, 1, &t), SC_OK);
    SC_ASSERT_EQ(t.id, 1u);
    SC_ASSERT_EQ(t.status, SC_TASK_LIST_IN_PROGRESS);
    SC_ASSERT_EQ(t.owner_agent_id, 42u);
    SC_ASSERT_NOT_NULL(t.subject);
    SC_ASSERT_TRUE(strcmp(t.subject, "Build checkout API") == 0);
    SC_ASSERT_NOT_NULL(t.description);
    SC_ASSERT_TRUE(strcmp(t.description, "REST endpoints for cart + payment") == 0);
    sc_task_free(&a, &t);

    sc_task_list_destroy(list2);
    sc_task_list_destroy(list);
}

static void test_task_list_status_survives_save_load_cycle(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_task_list_t *list = sc_task_list_create(&a, NULL, 16);
    SC_ASSERT_NOT_NULL(list);

    uint64_t id = 0;
    SC_ASSERT_EQ(sc_task_list_add(list, "Task", "Desc", NULL, 0, &id), SC_OK);
    SC_ASSERT_EQ(sc_task_list_claim(list, 1, 100), SC_OK);
    SC_ASSERT_EQ(sc_task_list_update_status(list, 1, SC_TASK_LIST_COMPLETED), SC_OK);

    char *json = NULL;
    size_t json_len = 0;
    SC_ASSERT_EQ(sc_task_list_serialize(list, &json, &json_len), SC_OK);
    SC_ASSERT_NOT_NULL(json);

    sc_task_list_t *list2 = sc_task_list_create(&a, NULL, 16);
    SC_ASSERT_EQ(sc_task_list_deserialize(list2, json, json_len), SC_OK);
    a.free(a.ctx, json, json_len + 1);

    SC_ASSERT_EQ(sc_task_list_count_by_status(list2, SC_TASK_LIST_COMPLETED), 1u);
    sc_task_t t = {0};
    SC_ASSERT_EQ(sc_task_list_get(list2, 1, &t), SC_OK);
    SC_ASSERT_EQ(t.status, SC_TASK_LIST_COMPLETED);
    sc_task_free(&a, &t);

    sc_task_list_destroy(list2);
    sc_task_list_destroy(list);
}

static void test_task_list_dependencies_preserved_through_serialization(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_task_list_t *list = sc_task_list_create(&a, NULL, 16);
    SC_ASSERT_NOT_NULL(list);

    uint64_t id1 = 0, id2 = 0;
    SC_ASSERT_EQ(sc_task_list_add(list, "Task A", "First", NULL, 0, &id1), SC_OK);
    uint64_t deps[] = {1};
    SC_ASSERT_EQ(sc_task_list_add(list, "Task B", "Depends on A", deps, 1, &id2), SC_OK);

    char *json = NULL;
    size_t json_len = 0;
    SC_ASSERT_EQ(sc_task_list_serialize(list, &json, &json_len), SC_OK);
    SC_ASSERT_NOT_NULL(json);
    SC_ASSERT_TRUE(strstr(json, "blocked_by") != NULL);
    SC_ASSERT_TRUE(strstr(json, "1") != NULL);

    sc_task_list_t *list2 = sc_task_list_create(&a, NULL, 16);
    SC_ASSERT_EQ(sc_task_list_deserialize(list2, json, json_len), SC_OK);
    a.free(a.ctx, json, json_len + 1);

    SC_ASSERT_TRUE(sc_task_list_is_blocked(list2, 2));
    sc_task_t t = {0};
    SC_ASSERT_EQ(sc_task_list_get(list2, 2, &t), SC_OK);
    SC_ASSERT_EQ(t.blocked_by_count, 1u);
    SC_ASSERT_EQ(t.blocked_by[0], 1u);
    sc_task_free(&a, &t);

    sc_task_list_destroy(list2);
    sc_task_list_destroy(list);
}

void run_agent_teams_tests(void) {
    SC_TEST_SUITE("agent teams (worktree + team config + mailbox + task list)");
    SC_RUN_TEST(test_mailbox_register_send_recv_with_agent_id);
    SC_RUN_TEST(test_mailbox_recv_in_agent_context_works);
    SC_RUN_TEST(test_agent_turn_processes_mailbox_messages);
    SC_RUN_TEST(test_agent_registers_unregisters_with_mailbox);
    SC_RUN_TEST(test_send_slash_command_sends_message);
    SC_RUN_TEST(test_cancel_message_sets_cancel_requested);
    SC_RUN_TEST(test_task_list_add_claim_complete_flow);
    SC_RUN_TEST(test_task_is_ready_when_deps_complete);
    SC_RUN_TEST(test_task_query_by_status_returns_correct_tasks);
    SC_RUN_TEST(test_task_blocked_by_incomplete_dependency_stays_blocked);
    SC_RUN_TEST(test_task_unblocks_when_dependency_completes);
    SC_RUN_TEST(test_next_available_skips_blocked_tasks);
    SC_RUN_TEST(test_claim_fails_on_already_claimed_task);
    SC_RUN_TEST(test_task_list_serialize_deserialize_round_trip_preserves_all_fields);
    SC_RUN_TEST(test_task_list_status_survives_save_load_cycle);
    SC_RUN_TEST(test_task_list_dependencies_preserved_through_serialization);
    SC_RUN_TEST(test_worktree_create_tracks_metadata);
    SC_RUN_TEST(test_worktree_remove_marks_inactive);
    SC_RUN_TEST(test_worktree_list_shows_active_worktrees);
    SC_RUN_TEST(test_worktree_path_for_agent_returns_correct_path);
    SC_RUN_TEST(test_team_config_parse_with_3_members);
    SC_RUN_TEST(test_team_config_get_member_by_name);
    SC_RUN_TEST(test_team_config_get_by_role_finds_first_match);
    SC_RUN_TEST(test_team_config_parse_autonomy_levels_maps_correctly);
    SC_RUN_TEST(test_team_config_parse_missing_fields_uses_defaults);
    SC_RUN_TEST(test_team_add_remove_members);
    SC_RUN_TEST(test_team_member_count_correct_after_add_remove);
    SC_RUN_TEST(test_team_role_reviewer_denies_file_write);
    SC_RUN_TEST(test_team_role_lead_allows_all_tools);
}
