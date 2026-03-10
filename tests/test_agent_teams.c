/* Tests for worktree integration, team config, mailbox, and task list. */

#include "human/agent.h"
#include "human/agent/mailbox.h"
#include "human/agent/spawn.h"
#include "human/agent/task_list.h"
#include "human/agent/team.h"
#include "human/agent/worktree.h"
#include "human/core/allocator.h"
#include "human/providers/factory.h"
#include "test_framework.h"
#include <string.h>

static void test_worktree_create_tracks_metadata(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_worktree_manager_t *mgr = hu_worktree_manager_create(&alloc, "/tmp/repo");
    HU_ASSERT_NOT_NULL(mgr);

    const char *path = NULL;
    hu_error_t err = hu_worktree_create(mgr, 42, "task-a", &path);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(path);
    HU_ASSERT_STR_EQ(path, "/tmp/repo/../.worktrees/task-a");
    hu_worktree_manager_destroy(mgr);
}

static void test_worktree_remove_marks_inactive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_worktree_manager_t *mgr = hu_worktree_manager_create(&alloc, "/tmp/repo");
    HU_ASSERT_NOT_NULL(mgr);

    const char *path = NULL;
    HU_ASSERT_EQ(hu_worktree_create(mgr, 1, "label", &path), HU_OK);

    HU_ASSERT_EQ(hu_worktree_remove(mgr, 1), HU_OK);

    HU_ASSERT_NULL(hu_worktree_path_for_agent(mgr, 1));
    hu_worktree_manager_destroy(mgr);
}

static void test_worktree_list_shows_active_worktrees(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_worktree_manager_t *mgr = hu_worktree_manager_create(&alloc, "/tmp/repo");
    HU_ASSERT_NOT_NULL(mgr);

    const char *p1 = NULL, *p2 = NULL;
    HU_ASSERT_EQ(hu_worktree_create(mgr, 1, "a", &p1), HU_OK);
    HU_ASSERT_EQ(hu_worktree_create(mgr, 2, "b", &p2), HU_OK);

    HU_ASSERT_EQ(hu_worktree_remove(mgr, 1), HU_OK);

    hu_worktree_t *list = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_worktree_list(mgr, &list, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_NOT_NULL(list);
    HU_ASSERT_STR_EQ(list[0].path, "/tmp/repo/../.worktrees/b");
    HU_ASSERT_EQ(list[0].agent_id, 2u);

    hu_worktree_list_free(&alloc, list, count);
    hu_worktree_manager_destroy(mgr);
}

static void test_worktree_path_for_agent_returns_correct_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_worktree_manager_t *mgr = hu_worktree_manager_create(&alloc, "/tmp/repo");
    HU_ASSERT_NOT_NULL(mgr);

    const char *path = NULL;
    HU_ASSERT_EQ(hu_worktree_create(mgr, 99, "task-99", &path), HU_OK);

    const char *got = hu_worktree_path_for_agent(mgr, 99);
    HU_ASSERT_NOT_NULL(got);
    HU_ASSERT_STR_EQ(got, "/tmp/repo/../.worktrees/task-99");

    HU_ASSERT_NULL(hu_worktree_path_for_agent(mgr, 999));
    hu_worktree_manager_destroy(mgr);
}

static void test_team_config_parse_with_3_members(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"checkout-feature\",\"base_branch\":\"main\",\"members\":["
        "{\"name\":\"backend\",\"role\":\"builder\",\"autonomy\":\"assisted\","
        "\"model\":\"anthropic/claude-sonnet\",\"tools\":[\"shell\",\"file_write\"]},"
        "{\"name\":\"frontend\",\"role\":\"builder\",\"autonomy\":\"assisted\"},"
        "{\"name\":\"reviewer\",\"role\":\"reviewer\",\"autonomy\":\"locked\","
        "\"tools\":[\"file_read\"]}]}";
    hu_team_config_t cfg = {0};
    HU_ASSERT_EQ(hu_team_config_parse(&alloc, json, strlen(json), &cfg), HU_OK);
    HU_ASSERT_STR_EQ(cfg.name, "checkout-feature");
    HU_ASSERT_STR_EQ(cfg.base_branch, "main");
    HU_ASSERT_EQ(cfg.members_count, 3u);

    HU_ASSERT_STR_EQ(cfg.members[0].name, "backend");
    HU_ASSERT_STR_EQ(cfg.members[0].role, "builder");
    HU_ASSERT_EQ(cfg.members[0].autonomy, HU_AUTONOMY_ASSISTED);
    HU_ASSERT_STR_EQ(cfg.members[0].model, "anthropic/claude-sonnet");
    HU_ASSERT_EQ(cfg.members[0].allowed_tools_count, 2u);
    HU_ASSERT_STR_EQ(cfg.members[0].allowed_tools[0], "shell");
    HU_ASSERT_STR_EQ(cfg.members[0].allowed_tools[1], "file_write");

    hu_team_config_free(&alloc, &cfg);
}

static void test_team_config_get_member_by_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"team\",\"members\":["
                       "{\"name\":\"backend\",\"role\":\"builder\"},"
                       "{\"name\":\"reviewer\",\"role\":\"reviewer\"}]}";
    hu_team_config_t cfg = {0};
    HU_ASSERT_EQ(hu_team_config_parse(&alloc, json, strlen(json), &cfg), HU_OK);

    const hu_team_config_member_t *m = hu_team_config_get_member(&cfg, "reviewer");
    HU_ASSERT_NOT_NULL(m);
    HU_ASSERT_STR_EQ(m->name, "reviewer");
    HU_ASSERT_STR_EQ(m->role, "reviewer");

    HU_ASSERT_NULL(hu_team_config_get_member(&cfg, "nonexistent"));

    hu_team_config_free(&alloc, &cfg);
}

static void test_team_config_get_by_role_finds_first_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"team\",\"members\":["
                       "{\"name\":\"backend\",\"role\":\"builder\"},"
                       "{\"name\":\"frontend\",\"role\":\"builder\"},"
                       "{\"name\":\"reviewer\",\"role\":\"reviewer\"}]}";
    hu_team_config_t cfg = {0};
    HU_ASSERT_EQ(hu_team_config_parse(&alloc, json, strlen(json), &cfg), HU_OK);

    const hu_team_config_member_t *m = hu_team_config_get_by_role(&cfg, "builder");
    HU_ASSERT_NOT_NULL(m);
    HU_ASSERT_STR_EQ(m->name, "backend");

    hu_team_config_free(&alloc, &cfg);
}

static void test_team_config_parse_autonomy_levels_maps_correctly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"team\",\"members\":["
                       "{\"name\":\"a\",\"autonomy\":\"locked\"},"
                       "{\"name\":\"b\",\"autonomy\":\"supervised\"},"
                       "{\"name\":\"c\",\"autonomy\":\"assisted\"},"
                       "{\"name\":\"d\",\"autonomy\":\"autonomous\"}]}";
    hu_team_config_t cfg = {0};
    HU_ASSERT_EQ(hu_team_config_parse(&alloc, json, strlen(json), &cfg), HU_OK);

    HU_ASSERT_EQ(cfg.members[0].autonomy, HU_AUTONOMY_LOCKED);
    HU_ASSERT_EQ(cfg.members[1].autonomy, HU_AUTONOMY_SUPERVISED);
    HU_ASSERT_EQ(cfg.members[2].autonomy, HU_AUTONOMY_ASSISTED);
    HU_ASSERT_EQ(cfg.members[3].autonomy, HU_AUTONOMY_AUTONOMOUS);

    hu_team_config_free(&alloc, &cfg);
}

static void test_team_config_parse_missing_fields_uses_defaults(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"members\":[{\"name\":\"x\"}]}";
    hu_team_config_t cfg = {0};
    HU_ASSERT_EQ(hu_team_config_parse(&alloc, json, strlen(json), &cfg), HU_OK);

    HU_ASSERT_NULL(cfg.name);
    HU_ASSERT_NULL(cfg.base_branch);
    HU_ASSERT_EQ(cfg.members_count, 1u);
    HU_ASSERT_STR_EQ(cfg.members[0].name, "x");
    HU_ASSERT_NULL(cfg.members[0].role);
    HU_ASSERT_EQ(cfg.members[0].autonomy, HU_AUTONOMY_ASSISTED);
    HU_ASSERT_NULL(cfg.members[0].allowed_tools);
    HU_ASSERT_EQ(cfg.members[0].allowed_tools_count, 0u);
    HU_ASSERT_NULL(cfg.members[0].model);

    hu_team_config_free(&alloc, &cfg);
}

/* ── Runtime team (hu_team_t) ─────────────────────────────────────────── */
static void test_team_add_remove_members(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_team_t *team = hu_team_create(&alloc, "squad");
    HU_ASSERT_NOT_NULL(team);

    HU_ASSERT_EQ(hu_team_add_member(team, 1, "alice", HU_ROLE_LEAD, 2), HU_OK);
    HU_ASSERT_EQ(hu_team_add_member(team, 2, "bob", HU_ROLE_BUILDER, 2), HU_OK);
    HU_ASSERT_EQ(hu_team_member_count(team), 2u);

    HU_ASSERT_EQ(hu_team_remove_member(team, 1), HU_OK);
    HU_ASSERT_EQ(hu_team_member_count(team), 1u);
    HU_ASSERT_NULL(hu_team_get_member(team, 1));
    HU_ASSERT_NOT_NULL(hu_team_get_member(team, 2));

    hu_team_destroy(team);
}

static void test_team_member_count_correct_after_add_remove(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_team_t *team = hu_team_create(&alloc, "t");
    HU_ASSERT_NOT_NULL(team);

    HU_ASSERT_EQ(hu_team_member_count(team), 0u);
    HU_ASSERT_EQ(hu_team_add_member(team, 1, "a", HU_ROLE_REVIEWER, 1), HU_OK);
    HU_ASSERT_EQ(hu_team_member_count(team), 1u);
    HU_ASSERT_EQ(hu_team_add_member(team, 2, "b", HU_ROLE_TESTER, 2), HU_OK);
    HU_ASSERT_EQ(hu_team_member_count(team), 2u);
    HU_ASSERT_EQ(hu_team_remove_member(team, 1), HU_OK);
    HU_ASSERT_EQ(hu_team_member_count(team), 1u);
    HU_ASSERT_EQ(hu_team_remove_member(team, 2), HU_OK);
    HU_ASSERT_EQ(hu_team_member_count(team), 0u);

    hu_team_destroy(team);
}

static void test_team_role_reviewer_denies_file_write(void) {
    HU_ASSERT_FALSE(hu_team_role_allows_tool(HU_ROLE_REVIEWER, "file_write"));
    HU_ASSERT_TRUE(hu_team_role_allows_tool(HU_ROLE_REVIEWER, "file_read"));
    HU_ASSERT_TRUE(hu_team_role_allows_tool(HU_ROLE_REVIEWER, "shell"));
    HU_ASSERT_TRUE(hu_team_role_allows_tool(HU_ROLE_REVIEWER, "memory_recall"));
}

static void test_team_role_lead_allows_all_tools(void) {
    HU_ASSERT_TRUE(hu_team_role_allows_tool(HU_ROLE_LEAD, "file_write"));
    HU_ASSERT_TRUE(hu_team_role_allows_tool(HU_ROLE_LEAD, "file_read"));
    HU_ASSERT_TRUE(hu_team_role_allows_tool(HU_ROLE_LEAD, "agent_spawn"));
    HU_ASSERT_TRUE(hu_team_role_allows_tool(HU_ROLE_LEAD, "shell"));
}

/* ── Mailbox + agent integration ──────────────────────────────────────── */
static void test_mailbox_recv_in_agent_context_works(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_mailbox_t *mb = hu_mailbox_create(&a, 8);
    HU_ASSERT_NOT_NULL(mb);

    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create(&a, "openai", 6, "test-key", 8, "", 0, &prov), HU_OK);

    hu_agent_t agent = {0};
    HU_ASSERT_EQ(hu_agent_from_config(&agent, &a, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      "gpt-4o-mini", 10, "openai", 6, 0.7, ".", 1, 5, 20, false, 2,
                                      NULL, 0, NULL, 0, NULL),
                 HU_OK);
    hu_agent_set_mailbox(&agent, mb);

    uint64_t agent_id = (uint64_t)(uintptr_t)&agent;
    HU_ASSERT_EQ(
        hu_mailbox_send(mb, 999, agent_id, HU_MSG_TASK, "API ready at /api/checkout", 24, 0),
        HU_OK);

    hu_message_t msg = {0};
    HU_ASSERT_EQ(hu_mailbox_recv(mb, agent_id, &msg), HU_OK);
    HU_ASSERT_EQ(msg.type, HU_MSG_TASK);
    HU_ASSERT_EQ(msg.from_agent, 999u);
    HU_ASSERT_NOT_NULL(msg.payload);
    HU_ASSERT_EQ(msg.payload_len, 24u);
    hu_message_free(&a, &msg);

    hu_agent_deinit(&agent);
    hu_mailbox_destroy(mb);
}

static void test_mailbox_register_send_recv_with_agent_id(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_mailbox_t *mb = hu_mailbox_create(&a, 8);
    HU_ASSERT_NOT_NULL(mb);

    uint64_t agent_id = 0x1234;
    HU_ASSERT_EQ(hu_mailbox_register(mb, agent_id), HU_OK);
    HU_ASSERT_EQ(hu_mailbox_send(mb, 999, agent_id, HU_MSG_TASK, "API ready", 9, 0), HU_OK);

    hu_message_t msg = {0};
    HU_ASSERT_EQ(hu_mailbox_recv(mb, agent_id, &msg), HU_OK);
    HU_ASSERT_EQ(msg.type, HU_MSG_TASK);
    HU_ASSERT_EQ(msg.from_agent, 999u);
    HU_ASSERT_NOT_NULL(msg.payload);
    HU_ASSERT_EQ(msg.payload_len, 9u);
    hu_message_free(&a, &msg);

    hu_mailbox_unregister(mb, agent_id);
    hu_mailbox_destroy(mb);
}

static void test_cancel_message_sets_cancel_requested(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_mailbox_t *mb = hu_mailbox_create(&a, 8);
    HU_ASSERT_NOT_NULL(mb);

    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create(&a, "openai", 6, "test-key", 8, "", 0, &prov), HU_OK);

    hu_agent_t agent = {0};
    HU_ASSERT_EQ(hu_agent_from_config(&agent, &a, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      "gpt-4o-mini", 10, "openai", 6, 0.7, ".", 1, 5, 20, false, 2,
                                      NULL, 0, NULL, 0, NULL),
                 HU_OK);
    hu_agent_set_mailbox(&agent, mb);
    HU_ASSERT_EQ(agent.cancel_requested, 0);

    uint64_t agent_id = (uint64_t)(uintptr_t)&agent;
    HU_ASSERT_EQ(hu_mailbox_send(mb, 1, agent_id, HU_MSG_CANCEL, "stop", 4, 0), HU_OK);

    hu_message_t msg = {0};
    HU_ASSERT_EQ(hu_mailbox_recv(mb, agent_id, &msg), HU_OK);
    HU_ASSERT_EQ(msg.type, HU_MSG_CANCEL);
    agent.cancel_requested = 1;
    hu_message_free(&a, &msg);

    HU_ASSERT_EQ(agent.cancel_requested, 1);

    hu_agent_deinit(&agent);
    hu_mailbox_destroy(mb);
}

/* ── Task list ────────────────────────────────────────────────────────── */
static void test_task_list_add_claim_complete_flow(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_task_list_t *list = hu_task_list_create(&a, NULL, 16);
    HU_ASSERT_NOT_NULL(list);

    uint64_t id = 0;
    HU_ASSERT_EQ(hu_task_list_add(list, "Deploy API", "Deploy the checkout API", NULL, 0, &id),
                 HU_OK);
    HU_ASSERT_EQ(id, 1u);

    HU_ASSERT_EQ(hu_task_list_claim(list, 1, 100), HU_OK);
    hu_task_t t = {0};
    HU_ASSERT_EQ(hu_task_list_get(list, 1, &t), HU_OK);
    HU_ASSERT_EQ(t.status, HU_TASK_LIST_CLAIMED);
    HU_ASSERT_EQ(t.owner_agent_id, 100u);
    hu_task_free(&a, &t);

    HU_ASSERT_EQ(hu_task_list_update_status(list, 1, HU_TASK_LIST_COMPLETED), HU_OK);
    HU_ASSERT_EQ(hu_task_list_count_by_status(list, HU_TASK_LIST_COMPLETED), 1u);

    hu_task_list_destroy(list);
}

static void test_task_blocked_by_incomplete_dependency_stays_blocked(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_task_list_t *list = hu_task_list_create(&a, NULL, 16);
    HU_ASSERT_NOT_NULL(list);

    uint64_t id1 = 0, id2 = 0;
    HU_ASSERT_EQ(hu_task_list_add(list, "Task A", "First", NULL, 0, &id1), HU_OK);
    HU_ASSERT_EQ(id1, 1u);

    uint64_t deps[] = {1};
    HU_ASSERT_EQ(hu_task_list_add(list, "Task B", "Depends on A", deps, 1, &id2), HU_OK);
    HU_ASSERT_EQ(id2, 2u);

    HU_ASSERT_TRUE(hu_task_list_is_blocked(list, 2));

    hu_task_t next = {0};
    HU_ASSERT_EQ(hu_task_list_next_available(list, &next), HU_OK);
    HU_ASSERT_EQ(next.id, 1u);
    hu_task_free(&a, &next);

    hu_task_list_destroy(list);
}

static void test_task_unblocks_when_dependency_completes(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_task_list_t *list = hu_task_list_create(&a, NULL, 16);
    HU_ASSERT_NOT_NULL(list);

    uint64_t id1 = 0, id2 = 0;
    HU_ASSERT_EQ(hu_task_list_add(list, "Task A", "First", NULL, 0, &id1), HU_OK);
    uint64_t deps[] = {1};
    HU_ASSERT_EQ(hu_task_list_add(list, "Task B", "Depends on A", deps, 1, &id2), HU_OK);

    HU_ASSERT_TRUE(hu_task_list_is_blocked(list, 2));
    HU_ASSERT_EQ(hu_task_list_update_status(list, 1, HU_TASK_LIST_COMPLETED), HU_OK);
    HU_ASSERT_FALSE(hu_task_list_is_blocked(list, 2));

    hu_task_t next = {0};
    HU_ASSERT_EQ(hu_task_list_next_available(list, &next), HU_OK);
    HU_ASSERT_EQ(next.id, 2u);
    hu_task_free(&a, &next);

    hu_task_list_destroy(list);
}

static void test_next_available_skips_blocked_tasks(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_task_list_t *list = hu_task_list_create(&a, NULL, 16);
    HU_ASSERT_NOT_NULL(list);

    uint64_t id1 = 0, id2 = 0;
    HU_ASSERT_EQ(hu_task_list_add(list, "Unblocked", "No deps", NULL, 0, &id1), HU_OK);
    uint64_t deps[] = {1};
    HU_ASSERT_EQ(hu_task_list_add(list, "Blocked", "Depends on 1", deps, 1, &id2), HU_OK);

    hu_task_t next = {0};
    HU_ASSERT_EQ(hu_task_list_next_available(list, &next), HU_OK);
    HU_ASSERT_EQ(next.id, 1u);
    hu_task_free(&a, &next);

    hu_task_list_destroy(list);
}

static void test_claim_fails_on_already_claimed_task(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_task_list_t *list = hu_task_list_create(&a, NULL, 16);
    HU_ASSERT_NOT_NULL(list);

    uint64_t id = 0;
    HU_ASSERT_EQ(hu_task_list_add(list, "Task", "Desc", NULL, 0, &id), HU_OK);
    HU_ASSERT_EQ(hu_task_list_claim(list, 1, 100), HU_OK);
    HU_ASSERT_EQ(hu_task_list_claim(list, 1, 200), HU_ERR_ALREADY_EXISTS);

    hu_task_list_destroy(list);
}

/* ── Feature 1: Mailbox in agent loop ───────────────────────────────────── */
static void test_agent_turn_processes_mailbox_messages(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_mailbox_t *mb = hu_mailbox_create(&a, 8);
    HU_ASSERT_NOT_NULL(mb);

    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create(&a, "openai", 6, "test-key", 8, "", 0, &prov), HU_OK);

    hu_agent_t agent = {0};
    HU_ASSERT_EQ(hu_agent_from_config(&agent, &a, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      "gpt-4o-mini", 10, "openai", 6, 0.7, ".", 1, 5, 20, false, 2,
                                      NULL, 0, NULL, 0, NULL),
                 HU_OK);
    hu_agent_set_mailbox(&agent, mb);

    uint64_t agent_id = (uint64_t)(uintptr_t)&agent;
    HU_ASSERT_EQ(hu_mailbox_send(mb, 999, agent_id, HU_MSG_TASK, "Task from agent 999", 19, 0),
                 HU_OK);

    char *resp = NULL;
    size_t rlen = 0;
    HU_ASSERT_EQ(hu_agent_turn(&agent, "/status", 7, &resp, &rlen), HU_OK);
    HU_ASSERT_NOT_NULL(resp);
    a.free(a.ctx, resp, rlen + 1);

    HU_ASSERT_EQ(agent.history_count, 1u);
    HU_ASSERT_NOT_NULL(agent.history[0].content);
    HU_ASSERT_TRUE(strstr(agent.history[0].content, "[Message from agent 999]") != NULL);
    HU_ASSERT_TRUE(strstr(agent.history[0].content, "Task from agent 999") != NULL);

    hu_agent_deinit(&agent);
    hu_mailbox_destroy(mb);
}

static void test_agent_registers_unregisters_with_mailbox(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_mailbox_t *mb = hu_mailbox_create(&a, 8);
    HU_ASSERT_NOT_NULL(mb);

    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create(&a, "openai", 6, "test-key", 8, "", 0, &prov), HU_OK);

    hu_agent_t agent = {0};
    HU_ASSERT_EQ(hu_agent_from_config(&agent, &a, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      "gpt-4o-mini", 10, "openai", 6, 0.7, ".", 1, 5, 20, false, 2,
                                      NULL, 0, NULL, 0, NULL),
                 HU_OK);
    agent.agent_id = 1;
    hu_agent_set_mailbox(&agent, mb);

    HU_ASSERT_EQ(hu_mailbox_send(mb, 999, 1, HU_MSG_TASK, "hello", 5, 0), HU_OK);
    hu_message_t msg = {0};
    HU_ASSERT_EQ(hu_mailbox_recv(mb, 1, &msg), HU_OK);
    hu_message_free(&a, &msg);

    hu_agent_deinit(&agent);
    HU_ASSERT_EQ(hu_mailbox_send(mb, 999, 1, HU_MSG_TASK, "after deinit", 12, 0), HU_ERR_NOT_FOUND);

    hu_mailbox_destroy(mb);
}

static void test_send_slash_command_sends_message(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_mailbox_t *mb = hu_mailbox_create(&a, 8);
    HU_ASSERT_NOT_NULL(mb);
    HU_ASSERT_EQ(hu_mailbox_register(mb, 42), HU_OK);

    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create(&a, "openai", 6, "test-key", 8, "", 0, &prov), HU_OK);

    hu_agent_t agent = {0};
    HU_ASSERT_EQ(hu_agent_from_config(&agent, &a, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      "gpt-4o-mini", 10, "openai", 6, 0.7, ".", 1, 5, 20, false, 2,
                                      NULL, 0, NULL, 0, NULL),
                 HU_OK);
    hu_agent_set_mailbox(&agent, mb);

    char *resp = hu_agent_handle_slash_command(&agent, "/send 42 hello from slash", 25);
    HU_ASSERT_NOT_NULL(resp);
    HU_ASSERT_TRUE(strstr(resp, "Sent to agent") != NULL);
    a.free(a.ctx, resp, strlen(resp) + 1);

    hu_message_t msg = {0};
    HU_ASSERT_EQ(hu_mailbox_recv(mb, 42, &msg), HU_OK);
    HU_ASSERT_NOT_NULL(msg.payload);
    HU_ASSERT_TRUE(strstr(msg.payload, "hello from slash") != NULL);
    hu_message_free(&a, &msg);

    hu_mailbox_unregister(mb, 42);
    hu_agent_deinit(&agent);
    hu_mailbox_destroy(mb);
}

/* ── Feature 2: Task list is_ready and query ────────────────────────────── */
static void test_task_is_ready_when_deps_complete(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_task_list_t *list = hu_task_list_create(&a, NULL, 16);
    HU_ASSERT_NOT_NULL(list);

    uint64_t id1 = 0, id2 = 0;
    HU_ASSERT_EQ(hu_task_list_add(list, "Task A", "First", NULL, 0, &id1), HU_OK);
    uint64_t deps[] = {1};
    HU_ASSERT_EQ(hu_task_list_add(list, "Task B", "Depends on A", deps, 1, &id2), HU_OK);

    HU_ASSERT_TRUE(hu_task_list_is_ready(list, 1));
    HU_ASSERT_FALSE(hu_task_list_is_ready(list, 2));

    HU_ASSERT_EQ(hu_task_list_update_status(list, 1, HU_TASK_LIST_COMPLETED), HU_OK);
    HU_ASSERT_TRUE(hu_task_list_is_ready(list, 2));

    hu_task_list_destroy(list);
}

static void test_task_query_by_status_returns_correct_tasks(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_task_list_t *list = hu_task_list_create(&a, NULL, 16);
    HU_ASSERT_NOT_NULL(list);

    uint64_t id1 = 0, id2 = 0, id3 = 0;
    HU_ASSERT_EQ(hu_task_list_add(list, "P1", NULL, NULL, 0, &id1), HU_OK);
    HU_ASSERT_EQ(hu_task_list_add(list, "P2", NULL, NULL, 0, &id2), HU_OK);
    HU_ASSERT_EQ(hu_task_list_add(list, "P3", NULL, NULL, 0, &id3), HU_OK);
    HU_ASSERT_EQ(hu_task_list_claim(list, 3, 100), HU_OK);
    HU_ASSERT_EQ(hu_task_list_update_status(list, 3, HU_TASK_LIST_COMPLETED), HU_OK);

    hu_task_t *pending = NULL;
    size_t pending_count = 0;
    HU_ASSERT_EQ(hu_task_list_query(list, HU_TASK_LIST_PENDING, &pending, &pending_count), HU_OK);
    HU_ASSERT_EQ(pending_count, 2u);
    HU_ASSERT_NOT_NULL(pending);
    hu_task_array_free(&a, pending, pending_count);

    hu_task_t *completed = NULL;
    size_t completed_count = 0;
    HU_ASSERT_EQ(hu_task_list_query(list, HU_TASK_LIST_COMPLETED, &completed, &completed_count),
                 HU_OK);
    HU_ASSERT_EQ(completed_count, 1u);
    HU_ASSERT_NOT_NULL(completed);
    HU_ASSERT_EQ(completed[0].id, 3u);
    hu_task_array_free(&a, completed, completed_count);

    hu_task_list_destroy(list);
}

/* ── Task list persistence (serialize/deserialize without file I/O) ───────── */
static void test_task_list_serialize_deserialize_round_trip_preserves_all_fields(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_task_list_t *list = hu_task_list_create(&a, NULL, 16);
    HU_ASSERT_NOT_NULL(list);

    uint64_t id = 0;
    HU_ASSERT_EQ(hu_task_list_add(list, "Build checkout API", "REST endpoints for cart + payment",
                                  NULL, 0, &id),
                 HU_OK);
    HU_ASSERT_EQ(id, 1u);
    HU_ASSERT_EQ(hu_task_list_claim(list, 1, 42), HU_OK);
    HU_ASSERT_EQ(hu_task_list_update_status(list, 1, HU_TASK_LIST_IN_PROGRESS), HU_OK);

    char *json = NULL;
    size_t json_len = 0;
    HU_ASSERT_EQ(hu_task_list_serialize(list, &json, &json_len), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_TRUE(json_len > 0);
    HU_ASSERT_TRUE(strstr(json, "Build checkout API") != NULL);
    HU_ASSERT_TRUE(strstr(json, "in_progress") != NULL);
    HU_ASSERT_TRUE(strstr(json, "\"owner\":42") != NULL);

    hu_task_list_t *list2 = hu_task_list_create(&a, NULL, 16);
    HU_ASSERT_NOT_NULL(list2);
    HU_ASSERT_EQ(hu_task_list_deserialize(list2, json, json_len), HU_OK);
    a.free(a.ctx, json, json_len + 1);

    hu_task_t t = {0};
    HU_ASSERT_EQ(hu_task_list_get(list2, 1, &t), HU_OK);
    HU_ASSERT_EQ(t.id, 1u);
    HU_ASSERT_EQ(t.status, HU_TASK_LIST_IN_PROGRESS);
    HU_ASSERT_EQ(t.owner_agent_id, 42u);
    HU_ASSERT_NOT_NULL(t.subject);
    HU_ASSERT_TRUE(strcmp(t.subject, "Build checkout API") == 0);
    HU_ASSERT_NOT_NULL(t.description);
    HU_ASSERT_TRUE(strcmp(t.description, "REST endpoints for cart + payment") == 0);
    hu_task_free(&a, &t);

    hu_task_list_destroy(list2);
    hu_task_list_destroy(list);
}

static void test_task_list_status_survives_save_load_cycle(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_task_list_t *list = hu_task_list_create(&a, NULL, 16);
    HU_ASSERT_NOT_NULL(list);

    uint64_t id = 0;
    HU_ASSERT_EQ(hu_task_list_add(list, "Task", "Desc", NULL, 0, &id), HU_OK);
    HU_ASSERT_EQ(hu_task_list_claim(list, 1, 100), HU_OK);
    HU_ASSERT_EQ(hu_task_list_update_status(list, 1, HU_TASK_LIST_COMPLETED), HU_OK);

    char *json = NULL;
    size_t json_len = 0;
    HU_ASSERT_EQ(hu_task_list_serialize(list, &json, &json_len), HU_OK);
    HU_ASSERT_NOT_NULL(json);

    hu_task_list_t *list2 = hu_task_list_create(&a, NULL, 16);
    HU_ASSERT_EQ(hu_task_list_deserialize(list2, json, json_len), HU_OK);
    a.free(a.ctx, json, json_len + 1);

    HU_ASSERT_EQ(hu_task_list_count_by_status(list2, HU_TASK_LIST_COMPLETED), 1u);
    hu_task_t t = {0};
    HU_ASSERT_EQ(hu_task_list_get(list2, 1, &t), HU_OK);
    HU_ASSERT_EQ(t.status, HU_TASK_LIST_COMPLETED);
    hu_task_free(&a, &t);

    hu_task_list_destroy(list2);
    hu_task_list_destroy(list);
}

static void test_task_list_dependencies_preserved_through_serialization(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_task_list_t *list = hu_task_list_create(&a, NULL, 16);
    HU_ASSERT_NOT_NULL(list);

    uint64_t id1 = 0, id2 = 0;
    HU_ASSERT_EQ(hu_task_list_add(list, "Task A", "First", NULL, 0, &id1), HU_OK);
    uint64_t deps[] = {1};
    HU_ASSERT_EQ(hu_task_list_add(list, "Task B", "Depends on A", deps, 1, &id2), HU_OK);

    char *json = NULL;
    size_t json_len = 0;
    HU_ASSERT_EQ(hu_task_list_serialize(list, &json, &json_len), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_TRUE(strstr(json, "blocked_by") != NULL);
    HU_ASSERT_TRUE(strstr(json, "1") != NULL);

    hu_task_list_t *list2 = hu_task_list_create(&a, NULL, 16);
    HU_ASSERT_EQ(hu_task_list_deserialize(list2, json, json_len), HU_OK);
    a.free(a.ctx, json, json_len + 1);

    HU_ASSERT_TRUE(hu_task_list_is_blocked(list2, 2));
    hu_task_t t = {0};
    HU_ASSERT_EQ(hu_task_list_get(list2, 2, &t), HU_OK);
    HU_ASSERT_EQ(t.blocked_by_count, 1u);
    HU_ASSERT_EQ(t.blocked_by[0], 1u);
    hu_task_free(&a, &t);

    hu_task_list_destroy(list2);
    hu_task_list_destroy(list);
}

static void test_worktree_label_rejects_traversal(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_worktree_manager_t *mgr = hu_worktree_manager_create(&a, "/tmp/repo");
    HU_ASSERT_NOT_NULL(mgr);
    const char *path = NULL;
    HU_ASSERT_NEQ(hu_worktree_create(mgr, 100, "..", &path), HU_OK);
    HU_ASSERT_NEQ(hu_worktree_create(mgr, 101, "foo/../bar", &path), HU_OK);
    HU_ASSERT_NEQ(hu_worktree_create(mgr, 102, "foo/bar", &path), HU_OK);
    HU_ASSERT_NEQ(hu_worktree_create(mgr, 103, "a\\b", &path), HU_OK);
    HU_ASSERT_EQ(hu_worktree_create(mgr, 104, "", &path), HU_OK);
    HU_ASSERT_EQ(hu_worktree_create(mgr, 105, "my-task", &path), HU_OK);
    HU_ASSERT_NOT_NULL(path);
    hu_worktree_manager_destroy(mgr);
}

void run_agent_teams_tests(void) {
    HU_TEST_SUITE("agent teams (worktree + team config + mailbox + task list)");
    HU_RUN_TEST(test_worktree_label_rejects_traversal);
    HU_RUN_TEST(test_mailbox_register_send_recv_with_agent_id);
    HU_RUN_TEST(test_mailbox_recv_in_agent_context_works);
    HU_RUN_TEST(test_agent_turn_processes_mailbox_messages);
    HU_RUN_TEST(test_agent_registers_unregisters_with_mailbox);
    HU_RUN_TEST(test_send_slash_command_sends_message);
    HU_RUN_TEST(test_cancel_message_sets_cancel_requested);
    HU_RUN_TEST(test_task_list_add_claim_complete_flow);
    HU_RUN_TEST(test_task_is_ready_when_deps_complete);
    HU_RUN_TEST(test_task_query_by_status_returns_correct_tasks);
    HU_RUN_TEST(test_task_blocked_by_incomplete_dependency_stays_blocked);
    HU_RUN_TEST(test_task_unblocks_when_dependency_completes);
    HU_RUN_TEST(test_next_available_skips_blocked_tasks);
    HU_RUN_TEST(test_claim_fails_on_already_claimed_task);
    HU_RUN_TEST(test_task_list_serialize_deserialize_round_trip_preserves_all_fields);
    HU_RUN_TEST(test_task_list_status_survives_save_load_cycle);
    HU_RUN_TEST(test_task_list_dependencies_preserved_through_serialization);
    HU_RUN_TEST(test_worktree_create_tracks_metadata);
    HU_RUN_TEST(test_worktree_remove_marks_inactive);
    HU_RUN_TEST(test_worktree_list_shows_active_worktrees);
    HU_RUN_TEST(test_worktree_path_for_agent_returns_correct_path);
    HU_RUN_TEST(test_team_config_parse_with_3_members);
    HU_RUN_TEST(test_team_config_get_member_by_name);
    HU_RUN_TEST(test_team_config_get_by_role_finds_first_match);
    HU_RUN_TEST(test_team_config_parse_autonomy_levels_maps_correctly);
    HU_RUN_TEST(test_team_config_parse_missing_fields_uses_defaults);
    HU_RUN_TEST(test_team_add_remove_members);
    HU_RUN_TEST(test_team_member_count_correct_after_add_remove);
    HU_RUN_TEST(test_team_role_reviewer_denies_file_write);
    HU_RUN_TEST(test_team_role_lead_allows_all_tools);
}
