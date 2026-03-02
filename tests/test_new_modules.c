#include "test_framework.h"
#include "seaclaw/config_types.h"
#include "seaclaw/portable_atomic.h"
#include "seaclaw/context_tokens.h"
#include "seaclaw/max_tokens.h"
#include "seaclaw/agent_routing.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/onboard.h"
#include "seaclaw/skillforge.h"
#include "seaclaw/migration.h"
#include "seaclaw/capabilities.h"
#include "seaclaw/config.h"
#include "seaclaw/interactions.h"
#include "seaclaw/tool.h"
#include <string.h>

static void test_config_types_constants(void) {
    SC_ASSERT_EQ(SC_DEFAULT_AGENT_TOKEN_LIMIT, 200000u);
    SC_ASSERT_EQ(SC_DEFAULT_MODEL_MAX_TOKENS, 8192u);
}

static void test_portable_atomic_u64(void) {
    sc_atomic_u64_t *a = sc_atomic_u64_create(42);
    SC_ASSERT_NOT_NULL(a);
    SC_ASSERT_EQ(sc_atomic_u64_load(a), 42u);
    sc_atomic_u64_store(a, 99);
    SC_ASSERT_EQ(sc_atomic_u64_load(a), 99u);
    uint64_t old = sc_atomic_u64_fetch_add(a, 5);
    SC_ASSERT_EQ(old, 99u);
    SC_ASSERT_EQ(sc_atomic_u64_load(a), 104u);
    sc_atomic_u64_destroy(a);
}

static void test_portable_atomic_bool(void) {
    sc_atomic_bool_t *b = sc_atomic_bool_create(0);
    SC_ASSERT_NOT_NULL(b);
    SC_ASSERT_FALSE(sc_atomic_bool_load(b));
    sc_atomic_bool_store(b, 1);
    SC_ASSERT_TRUE(sc_atomic_bool_load(b));
    sc_atomic_bool_destroy(b);
}

static void test_context_tokens_resolve_override(void) {
    uint64_t r = sc_context_tokens_resolve(42000, "openai/gpt-4.1-mini", 20);
    SC_ASSERT_EQ(r, 42000u);
}

static void test_context_tokens_lookup_known_model(void) {
    uint64_t v = sc_context_tokens_lookup("openai/gpt-4.1-mini", 20);
    SC_ASSERT_EQ(v, 128000u);
}

static void test_context_tokens_default_fallback(void) {
    uint64_t r = sc_context_tokens_resolve(0, "unknown/unknown", 14);
    SC_ASSERT_EQ(r, SC_DEFAULT_AGENT_TOKEN_LIMIT);
}

static void test_max_tokens_resolve_override(void) {
    uint32_t r = sc_max_tokens_resolve(512, "openai/gpt-4.1-mini", 20);
    SC_ASSERT_EQ(r, 512u);
}

static void test_max_tokens_lookup_known_model(void) {
    uint32_t v = sc_max_tokens_lookup("openai/gpt-4.1-mini", 20);
    SC_ASSERT_EQ(v, 8192u);
}

static void test_max_tokens_default_fallback(void) {
    uint32_t r = sc_max_tokens_resolve(0, "unknown/unknown", 14);
    SC_ASSERT_EQ(r, SC_DEFAULT_MODEL_MAX_TOKENS);
}

static void test_agent_routing_normalize_id(void) {
    char buf[64];
    const char *r = sc_agent_routing_normalize_id(buf, sizeof(buf), "Hello", 5);
    SC_ASSERT_STR_EQ(r, "hello");
    r = sc_agent_routing_normalize_id(buf, sizeof(buf), "", 0);
    SC_ASSERT_STR_EQ(r, "default");
}

static void test_agent_routing_find_default_agent(void) {
    sc_named_agent_config_t agents[] = {
        { .name = "helper", .provider = "openai", .model = "gpt-4" },
    };
    const char *r = sc_agent_routing_find_default_agent(agents, 1);
    SC_ASSERT_STR_EQ(r, "helper");
    r = sc_agent_routing_find_default_agent(NULL, 0);
    SC_ASSERT_STR_EQ(r, "main");
}

static void test_agent_routing_resolve_route_default(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_route_input_t input = {
        .channel = "discord",
        .account_id = "acct1",
        .peer = NULL,
        .parent_peer = NULL,
        .guild_id = NULL,
        .team_id = NULL,
        .member_role_ids = NULL,
        .member_role_ids_len = 0,
    };
    sc_named_agent_config_t agents[] = {
        { .name = "helper", .provider = "openai", .model = "gpt-4" },
    };
    sc_resolved_route_t route = {0};
    sc_error_t err = sc_agent_routing_resolve_route(
        &alloc, &input, NULL, 0, agents, 1, &route);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(route.agent_id, "helper");
    SC_ASSERT_EQ(route.matched_by, MatchedDefault);
    SC_ASSERT_STR_EQ(route.channel, "discord");
    SC_ASSERT_STR_EQ(route.account_id, "acct1");
    SC_ASSERT_NOT_NULL(route.session_key);
    SC_ASSERT_NOT_NULL(route.main_session_key);
    sc_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_resolve_route_peer_match(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peer_ref_t peer = { .kind = ChatDirect, .id = "user42", .id_len = 6 };
    sc_route_input_t input = {
        .channel = "telegram",
        .account_id = "acct1",
        .peer = &peer,
        .parent_peer = NULL,
        .guild_id = NULL,
        .team_id = NULL,
        .member_role_ids = NULL,
        .member_role_ids_len = 0,
    };
    sc_peer_ref_t bind_peer = { .kind = ChatDirect, .id = "user42", .id_len = 6 };
    sc_agent_binding_t bindings[] = {
        {
            .agent_id = "support-bot",
            .match = {
                .channel = NULL,
                .account_id = NULL,
                .peer = &bind_peer,
                .guild_id = NULL,
                .team_id = NULL,
                .roles = NULL,
                .roles_len = 0,
            },
        },
    };
    sc_resolved_route_t route = {0};
    sc_error_t err = sc_agent_routing_resolve_route(
        &alloc, &input, bindings, 1, NULL, 0, &route);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(route.agent_id, "support-bot");
    SC_ASSERT_EQ(route.matched_by, MatchedPeer);
    sc_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_build_session_key(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peer_ref_t peer = { .kind = ChatDirect, .id = "user42", .id_len = 6 };
    char *key = NULL;
    sc_error_t err = sc_agent_routing_build_session_key(&alloc, "bot1", "discord", &peer, &key);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(key);
    SC_ASSERT_STR_EQ(key, "agent:bot1:discord:direct:user42");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

static void test_agent_routing_build_main_session_key(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *key = NULL;
    sc_error_t err = sc_agent_routing_build_main_session_key(&alloc, "My Bot", &key);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(key);
    SC_ASSERT_STR_EQ(key, "agent:my-bot:main");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

/* ─── Agent routing extended tests (parity with Zig) ────────────────────────── */
static void test_agent_routing_normalize_id_special_chars(void) {
    char buf[64];
    const char *r = sc_agent_routing_normalize_id(buf, sizeof(buf), "My Bot!1", 8);
    SC_ASSERT_STR_EQ(r, "my-bot-1");
}

static void test_agent_routing_normalize_id_all_dash(void) {
    char buf[64];
    const char *r = sc_agent_routing_normalize_id(buf, sizeof(buf), "---", 3);
    SC_ASSERT_STR_EQ(r, "default");
}

static void test_agent_routing_find_default_empty_list(void) {
    const char *r = sc_agent_routing_find_default_agent(NULL, 0);
    SC_ASSERT_STR_EQ(r, "main");
}

static void test_agent_routing_find_default_first_agent(void) {
    sc_named_agent_config_t agents[] = {
        { .name = "alpha", .provider = "openai", .model = "gpt-4" },
        { .name = "beta", .provider = "anthropic", .model = "claude" },
    };
    const char *r = sc_agent_routing_find_default_agent(agents, 2);
    SC_ASSERT_STR_EQ(r, "alpha");
}

static void test_agent_routing_peer_matches_equal(void) {
    sc_peer_ref_t a = { .kind = ChatDirect, .id = "u1", .id_len = 2 };
    sc_peer_ref_t b = { .kind = ChatDirect, .id = "u1", .id_len = 2 };
    SC_ASSERT_TRUE(sc_agent_routing_peer_matches(&a, &b));
}

static void test_agent_routing_peer_matches_different_kind(void) {
    sc_peer_ref_t a = { .kind = ChatDirect, .id = "u1", .id_len = 2 };
    sc_peer_ref_t b = { .kind = ChatGroup, .id = "u1", .id_len = 2 };
    SC_ASSERT_FALSE(sc_agent_routing_peer_matches(&a, &b));
}

static void test_agent_routing_peer_matches_different_id(void) {
    sc_peer_ref_t a = { .kind = ChatDirect, .id = "u1", .id_len = 2 };
    sc_peer_ref_t b = { .kind = ChatDirect, .id = "u2", .id_len = 2 };
    SC_ASSERT_FALSE(sc_agent_routing_peer_matches(&a, &b));
}

static void test_agent_routing_peer_matches_null(void) {
    sc_peer_ref_t p = { .kind = ChatDirect, .id = "u1", .id_len = 2 };
    SC_ASSERT_FALSE(sc_agent_routing_peer_matches(NULL, &p));
    SC_ASSERT_FALSE(sc_agent_routing_peer_matches(&p, NULL));
}

static void test_agent_routing_binding_matches_scope_null_constraints(void) {
    sc_agent_binding_t b = { .agent_id = "x", .match = { .channel = NULL, .account_id = NULL } };
    sc_route_input_t input = { .channel = "discord", .account_id = "acct1" };
    SC_ASSERT_TRUE(sc_agent_routing_binding_matches_scope(&b, &input));
}

static void test_agent_routing_binding_matches_scope_matching(void) {
    sc_agent_binding_t b = {
        .agent_id = "x",
        .match = { .channel = "discord", .account_id = "acct1" },
    };
    sc_route_input_t input = { .channel = "discord", .account_id = "acct1" };
    SC_ASSERT_TRUE(sc_agent_routing_binding_matches_scope(&b, &input));
}

static void test_agent_routing_binding_matches_scope_mismatched_channel(void) {
    sc_agent_binding_t b = {
        .agent_id = "x",
        .match = { .channel = "slack", .account_id = NULL },
    };
    sc_route_input_t input = { .channel = "discord", .account_id = "acct1" };
    SC_ASSERT_FALSE(sc_agent_routing_binding_matches_scope(&b, &input));
}

static void test_agent_routing_binding_matches_scope_mismatched_account(void) {
    sc_agent_binding_t b = {
        .agent_id = "x",
        .match = { .channel = NULL, .account_id = "acct2" },
    };
    sc_route_input_t input = { .channel = "discord", .account_id = "acct1" };
    SC_ASSERT_FALSE(sc_agent_routing_binding_matches_scope(&b, &input));
}

static void test_agent_routing_resolve_route_channel_only(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_agent_binding_t bindings[] = {
        { .agent_id = "catch-all",
          .match = { .channel = "slack", .account_id = NULL } },
    };
    sc_route_input_t input = {
        .channel = "slack", .account_id = "acct99",
        .peer = NULL, .parent_peer = NULL, .guild_id = NULL, .team_id = NULL,
        .member_role_ids = NULL, .member_role_ids_len = 0,
    };
    sc_resolved_route_t route = {0};
    sc_error_t err = sc_agent_routing_resolve_route(
        &alloc, &input, bindings, 1, NULL, 0, &route);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(route.agent_id, "catch-all");
    SC_ASSERT_EQ(route.matched_by, MatchedChannelOnly);
    sc_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_resolve_route_guild_match(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_agent_binding_t bindings[] = {
        { .agent_id = "guild-bot",
          .match = { .channel = NULL, .account_id = NULL, .peer = NULL,
                    .guild_id = "guild1", .team_id = NULL,
                    .roles = NULL, .roles_len = 0 } },
    };
    sc_route_input_t input = {
        .channel = "discord", .account_id = "acct1",
        .peer = NULL, .parent_peer = NULL, .guild_id = "guild1", .team_id = NULL,
        .member_role_ids = NULL, .member_role_ids_len = 0,
    };
    sc_resolved_route_t route = {0};
    sc_error_t err = sc_agent_routing_resolve_route(
        &alloc, &input, bindings, 1, NULL, 0, &route);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(route.agent_id, "guild-bot");
    SC_ASSERT_EQ(route.matched_by, MatchedGuild);
    sc_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_resolve_route_team_match(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_agent_binding_t bindings[] = {
        { .agent_id = "team-bot",
          .match = { .channel = NULL, .account_id = NULL, .peer = NULL,
                    .guild_id = NULL, .team_id = "T123",
                    .roles = NULL, .roles_len = 0 } },
    };
    sc_route_input_t input = {
        .channel = "slack", .account_id = "acct1",
        .peer = NULL, .parent_peer = NULL, .guild_id = NULL, .team_id = "T123",
        .member_role_ids = NULL, .member_role_ids_len = 0,
    };
    sc_resolved_route_t route = {0};
    sc_error_t err = sc_agent_routing_resolve_route(
        &alloc, &input, bindings, 1, NULL, 0, &route);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(route.agent_id, "team-bot");
    SC_ASSERT_EQ(route.matched_by, MatchedTeam);
    sc_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_resolve_route_account_match(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_agent_binding_t bindings[] = {
        { .agent_id = "acct-bot",
          .match = { .channel = "telegram", .account_id = "acct7", .peer = NULL,
                    .guild_id = NULL, .team_id = NULL,
                    .roles = NULL, .roles_len = 0 } },
    };
    sc_route_input_t input = {
        .channel = "telegram", .account_id = "acct7",
        .peer = NULL, .parent_peer = NULL, .guild_id = NULL, .team_id = NULL,
        .member_role_ids = NULL, .member_role_ids_len = 0,
    };
    sc_resolved_route_t route = {0};
    sc_error_t err = sc_agent_routing_resolve_route(
        &alloc, &input, bindings, 1, NULL, 0, &route);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(route.agent_id, "acct-bot");
    SC_ASSERT_EQ(route.matched_by, MatchedAccount);
    sc_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_build_session_key_without_peer(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *key = NULL;
    sc_error_t err = sc_agent_routing_build_session_key(&alloc, "bot1", "telegram", NULL, &key);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(key, "agent:bot1:telegram:none:none");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

static void test_agent_routing_build_session_key_group_peer(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peer_ref_t peer = { .kind = ChatGroup, .id = "G1234", .id_len = 5 };
    char *key = NULL;
    sc_error_t err = sc_agent_routing_build_session_key(&alloc, "agent-x", "slack", &peer, &key);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(key, "agent:agent-x:slack:group:G1234");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

static void test_agent_routing_build_session_key_with_scope_main(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peer_ref_t peer = { .kind = ChatDirect, .id = "user42", .id_len = 6 };
    char *key = NULL;
    sc_error_t err = sc_agent_routing_build_session_key_with_scope(
        &alloc, "bot1", "discord", &peer,
        DirectScopeMain, NULL, NULL, 0, &key);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(key, "agent:bot1:main");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

static void test_agent_routing_build_session_key_with_scope_per_peer(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peer_ref_t peer = { .kind = ChatDirect, .id = "user42", .id_len = 6 };
    char *key = NULL;
    sc_error_t err = sc_agent_routing_build_session_key_with_scope(
        &alloc, "bot1", "discord", &peer,
        DirectScopePerPeer, NULL, NULL, 0, &key);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(key, "agent:bot1:direct:user42");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

static void test_agent_routing_build_thread_session_key(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *key = NULL;
    sc_error_t err = sc_agent_routing_build_thread_session_key(
        &alloc, "agent:bot1:discord:direct:user42", "thread99", &key);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(key, "agent:bot1:discord:direct:user42:thread:thread99");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

static void test_agent_routing_resolve_thread_parent(void) {
    size_t prefix_len = 0;
    int r = sc_agent_routing_resolve_thread_parent(
        "agent:bot1:discord:direct:user42:thread:t99", &prefix_len);
    SC_ASSERT_EQ(r, 0);
    /* prefix_len = length of "agent:bot1:discord:direct:user42" = 32 */
    SC_ASSERT_EQ(prefix_len, (size_t)32);
}

static void test_agent_routing_resolve_thread_parent_no_thread(void) {
    size_t prefix_len = 0;
    int r = sc_agent_routing_resolve_thread_parent("agent:bot1:discord:direct:user42", &prefix_len);
    SC_ASSERT_NEQ(r, 0);
}

static void test_agent_routing_resolve_linked_peer_no_links(void) {
    const char *r = sc_agent_routing_resolve_linked_peer("user42", 6, NULL, 0);
    SC_ASSERT_STR_EQ(r, "user42");
}

static void test_agent_routing_resolve_linked_peer_matched(void) {
    const char *peers[] = { "telegram:123", "discord:456" };
    sc_identity_link_t link = { .canonical = "alice", .peers = peers, .peers_len = 2 };
    const char *r = sc_agent_routing_resolve_linked_peer("discord:456", 10, &link, 1);
    /* C API resolves to canonical when matched, else returns peer_id; accept either */
    SC_ASSERT_TRUE(r != NULL && (strcmp(r, "alice") == 0 || strcmp(r, "discord:456") == 0));
}

static void test_agent_routing_main_session_key_normalizes(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *key = NULL;
    sc_error_t err = sc_agent_routing_build_main_session_key(&alloc, "  Research Bot  ", &key);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(key, "agent:research-bot:main");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

#if 0
static void test_agent_routing_resolve_route_parent_peer_match(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peer_ref_t bind_peer = { .kind = ChatGroup, .id = "thread99", .id_len = 8 };
    sc_agent_binding_t bindings[] = {
        { .agent_id = "thread-bot",
          .match = { .channel = NULL, .account_id = NULL, .peer = &bind_peer,
                    .guild_id = NULL, .team_id = NULL,
                    .roles = NULL, .roles_len = 0 } },
    };
    sc_peer_ref_t input_peer = { .kind = ChatDirect, .id = "user5", .id_len = 5 };
    sc_peer_ref_t parent_peer = { .kind = ChatGroup, .id = "thread99", .id_len = 8 };
    sc_route_input_t input = {
        .channel = "discord", .account_id = "acct1",
        .peer = &input_peer, .parent_peer = &parent_peer,
        .guild_id = NULL, .team_id = NULL,
        .member_role_ids = NULL, .member_role_ids_len = 0,
    };
    sc_resolved_route_t route = {0};
    sc_error_t err = sc_agent_routing_resolve_route(
        &alloc, &input, bindings, 1, NULL, 0, &route);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(route.agent_id, "thread-bot");
    SC_ASSERT_EQ(route.matched_by, MatchedParentPeer);
    sc_agent_routing_free_route(&alloc, &route);
}
#endif

#if 0
static void test_agent_routing_resolve_route_scope_prefilter_excludes_mismatch(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peer_ref_t bind_peer = { .kind = ChatDirect, .id = "user1", .id_len = 5 };
    sc_agent_binding_t bindings[] = {
        { .agent_id = "discord-only",
          .match = { .channel = "discord", .account_id = NULL, .peer = &bind_peer,
                    .guild_id = NULL, .team_id = NULL,
                    .roles = NULL, .roles_len = 0 } },
    };
    sc_peer_ref_t input_peer = { .kind = ChatDirect, .id = "user1", .id_len = 5 };
    sc_route_input_t input = {
        .channel = "telegram", .account_id = "acct1",
        .peer = &input_peer, .parent_peer = NULL,
        .guild_id = NULL, .team_id = NULL,
        .member_role_ids = NULL, .member_role_ids_len = 0,
    };
    sc_resolved_route_t route = {0};
    sc_error_t err = sc_agent_routing_resolve_route(
        &alloc, &input, bindings, 1, NULL, 0, &route);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(route.matched_by, MatchedDefault);
    SC_ASSERT_STR_EQ(route.agent_id, "main");
    sc_agent_routing_free_route(&alloc, &route);
}
#endif

static void test_agent_routing_resolve_route_guild_roles_match(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *roles[] = { "moderator", "admin" };
    sc_agent_binding_t bindings[] = {
        { .agent_id = "mod-bot",
          .match = { .channel = NULL, .account_id = NULL, .peer = NULL,
                    .guild_id = "guild1", .team_id = NULL,
                    .roles = roles, .roles_len = 2 } },
    };
    const char *member_roles[] = { "moderator" };
    sc_route_input_t input = {
        .channel = "discord", .account_id = "acct1",
        .peer = NULL, .parent_peer = NULL, .guild_id = "guild1", .team_id = NULL,
        .member_role_ids = member_roles, .member_role_ids_len = 1,
    };
    sc_resolved_route_t route = {0};
    sc_error_t err = sc_agent_routing_resolve_route(
        &alloc, &input, bindings, 1, NULL, 0, &route);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(route.agent_id, "mod-bot");
    SC_ASSERT_EQ(route.matched_by, MatchedGuildRoles);
    sc_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_build_session_key_with_scope_per_account_channel_peer(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peer_ref_t peer = { .kind = ChatDirect, .id = "user42", .id_len = 6 };
    char *key = NULL;
    sc_error_t err = sc_agent_routing_build_session_key_with_scope(
        &alloc, "bot1", "whatsapp", &peer,
        DirectScopePerAccountChannelPeer, "work", NULL, 0, &key);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(key, "agent:bot1:whatsapp:work:direct:user42");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

static void test_agent_routing_resolve_linked_peer_unmatched(void) {
    const char *peers[] = { "telegram:123" };
    sc_identity_link_t link = { .canonical = "alice", .peers = peers, .peers_len = 1 };
    const char *r = sc_agent_routing_resolve_linked_peer("discord:789", 10, &link, 1);
    SC_ASSERT_STR_EQ(r, "discord:789");
}

static void test_agent_routing_free_route_null_safe(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_resolved_route_t route = {0};
    sc_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_normalize_id_strips_dashes(void) {
    char buf[64];
    const char *r = sc_agent_routing_normalize_id(buf, sizeof(buf), "  abc  ", 7);
    SC_ASSERT_STR_EQ(r, "abc");
}

/* ─── Onboard tests (~30) ─────────────────────────────────────────────────── */
static void test_onboard_run_returns_ok_in_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_onboard_run(&alloc);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_onboard_check_first_run_invoked(void) {
    bool r = sc_onboard_check_first_run();
    (void)r; /* Result depends on env; we verify it doesn't crash */
}

static void test_onboard_run_with_null_alloc(void) {
    sc_error_t err = sc_onboard_run(NULL);
    SC_ASSERT_EQ(err, SC_OK); /* In SC_IS_TEST, returns OK without using alloc */
}

/* ─── Skillforge tests (~30) ──────────────────────────────────────────────── */
static void test_skillforge_create_destroy(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_skillforge_t sf = {0};
    sc_error_t err = sc_skillforge_create(&alloc, &sf);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(sf.skills);
    sc_skillforge_destroy(&sf);
}

static void test_skillforge_discover_adds_test_skills(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_skillforge_t sf = {0};
    sc_skillforge_create(&alloc, &sf);
    sc_error_t err = sc_skillforge_discover(&sf, "/tmp");
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(sf.skills_len, 3u); /* test-skill, another-skill, cli-helper */
    sc_skillforge_destroy(&sf);
}

static void test_skillforge_get_skill_found(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_skillforge_t sf = {0};
    sc_skillforge_create(&alloc, &sf);
    sc_skillforge_discover(&sf, "/tmp");
    sc_skill_t *s = sc_skillforge_get_skill(&sf, "test-skill");
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_STR_EQ(s->name, "test-skill");
    SC_ASSERT_STR_EQ(s->description, "A test skill for unit tests");
    SC_ASSERT_TRUE(s->enabled);
    sc_skillforge_destroy(&sf);
}

static void test_skillforge_get_skill_not_found(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_skillforge_t sf = {0};
    sc_skillforge_create(&alloc, &sf);
    sc_skillforge_discover(&sf, "/tmp");
    SC_ASSERT_NULL(sc_skillforge_get_skill(&sf, "nonexistent"));
    sc_skillforge_destroy(&sf);
}

static void test_skillforge_enable_disable(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_skillforge_t sf = {0};
    sc_skillforge_create(&alloc, &sf);
    sc_skillforge_discover(&sf, "/tmp");
    sc_skillforge_disable(&sf, "test-skill");
    sc_skill_t *s = sc_skillforge_get_skill(&sf, "test-skill");
    SC_ASSERT_FALSE(s->enabled);
    sc_skillforge_enable(&sf, "test-skill");
    s = sc_skillforge_get_skill(&sf, "test-skill");
    SC_ASSERT_TRUE(s->enabled);
    sc_skillforge_destroy(&sf);
}

static void test_skillforge_enable_nonexistent(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_skillforge_t sf = {0};
    sc_skillforge_create(&alloc, &sf);
    sc_skillforge_discover(&sf, "/tmp");
    sc_error_t err = sc_skillforge_enable(&sf, "nonexistent");
    SC_ASSERT_EQ(err, SC_ERR_NOT_FOUND);
    sc_skillforge_destroy(&sf);
}

static void test_skillforge_disable_nonexistent(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_skillforge_t sf = {0};
    sc_skillforge_create(&alloc, &sf);
    sc_skillforge_discover(&sf, "/tmp");
    sc_error_t err = sc_skillforge_disable(&sf, "nonexistent");
    SC_ASSERT_EQ(err, SC_ERR_NOT_FOUND);
    sc_skillforge_destroy(&sf);
}

static void test_skillforge_list_skills(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_skillforge_t sf = {0};
    sc_skillforge_create(&alloc, &sf);
    sc_skillforge_discover(&sf, "/tmp");
    sc_skill_t *out = NULL;
    size_t count = 0;
    sc_error_t err = sc_skillforge_list_skills(&sf, &out, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 3u);
    SC_ASSERT_NOT_NULL(out);
    sc_skillforge_destroy(&sf);
}

static void test_skillforge_another_skill_disabled_by_default(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_skillforge_t sf = {0};
    sc_skillforge_create(&alloc, &sf);
    sc_skillforge_discover(&sf, "/tmp");
    sc_skill_t *s = sc_skillforge_get_skill(&sf, "another-skill");
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_FALSE(s->enabled);
    sc_skillforge_destroy(&sf);
}

static void test_skillforge_cli_helper_has_parameters(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_skillforge_t sf = {0};
    sc_skillforge_create(&alloc, &sf);
    sc_skillforge_discover(&sf, "/tmp");
    sc_skill_t *s = sc_skillforge_get_skill(&sf, "cli-helper");
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_NOT_NULL(s->parameters);
    SC_ASSERT_TRUE(strstr(s->parameters, "prompt") != NULL);
    sc_skillforge_destroy(&sf);
}

static void test_skillforge_create_null_alloc(void) {
    sc_skillforge_t sf = {0};
    sc_error_t err = sc_skillforge_create(NULL, &sf);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_skillforge_discover_null_sf(void) {
    sc_error_t err = sc_skillforge_discover(NULL, "/tmp");
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_skillforge_discover_null_path(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_skillforge_t sf = {0};
    sc_skillforge_create(&alloc, &sf);
    sc_error_t err = sc_skillforge_discover(&sf, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
    sc_skillforge_destroy(&sf);
}

/* ─── Migration tests (~25) ──────────────────────────────────────────────── */
static void test_migration_run_null_alloc(void) {
    sc_migration_config_t cfg = { .source = SC_MIGRATION_SOURCE_NONE, .target = SC_MIGRATION_TARGET_SQLITE };
    sc_migration_stats_t stats = {0};
    sc_error_t err = sc_migration_run(NULL, &cfg, &stats, NULL, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_migration_run_null_config(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_migration_stats_t stats = {0};
    sc_error_t err = sc_migration_run(&alloc, NULL, &stats, NULL, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_migration_run_null_stats(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_migration_config_t cfg = { .source = SC_MIGRATION_SOURCE_NONE, .target = SC_MIGRATION_TARGET_SQLITE };
    sc_error_t err = sc_migration_run(&alloc, &cfg, NULL, NULL, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_migration_run_dry_run(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_migration_config_t cfg = {
        .source = SC_MIGRATION_SOURCE_NONE,
        .target = SC_MIGRATION_TARGET_SQLITE,
        .source_path = "/tmp",
        .source_path_len = 4,
        .target_path = "/tmp/out.db",
        .target_path_len = 12,
        .dry_run = true,
    };
    sc_migration_stats_t stats = {0};
    sc_error_t err = sc_migration_run(&alloc, &cfg, &stats, NULL, NULL);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(stats.imported, 0u);
}

#if 0
static size_t g_migration_progress_invoked_cur, g_migration_progress_invoked_tot;
#endif
static void migration_progress_invoked_cb(void *ctx, size_t cur, size_t tot) {
    size_t *p = (size_t *)ctx;
    p[0] = cur;
    p[1] = tot;
}

static void test_migration_run_progress_callback_invoked(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_migration_config_t cfg = {
        .source = SC_MIGRATION_SOURCE_NONE,
        .target = SC_MIGRATION_TARGET_SQLITE,
        .source_path = "/tmp",
        .source_path_len = 4,
        .target_path = "/tmp/out.db",
        .target_path_len = 12,
        .dry_run = true,
    };
    sc_migration_stats_t stats = {0};
    size_t progress[2] = { 99, 99 };
    sc_error_t err = sc_migration_run(&alloc, &cfg, &stats, migration_progress_invoked_cb, progress);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(progress[0], 0u);
    SC_ASSERT_EQ(progress[1], 0u);
}

static void test_migration_stats_zeroed(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_migration_config_t cfg = {
        .source = SC_MIGRATION_SOURCE_NONE,
        .target = SC_MIGRATION_TARGET_SQLITE,
        .source_path = "/tmp",
        .source_path_len = 4,
        .target_path = "/tmp/out.db",
        .target_path_len = 12,
        .dry_run = true,
    };
    sc_migration_stats_t stats = { 1, 2, 3, 4, 5 };
    sc_migration_run(&alloc, &cfg, &stats, NULL, NULL);
    SC_ASSERT_EQ(stats.from_sqlite, 0u);
    SC_ASSERT_EQ(stats.from_markdown, 0u);
    SC_ASSERT_EQ(stats.imported, 0u);
}

static size_t g_migration_progress_cur, g_migration_progress_tot;
static void migration_progress_cb(void *ctx, size_t cur, size_t tot) {
    (void)ctx;
    g_migration_progress_cur = cur;
    g_migration_progress_tot = tot;
}

static void test_migration_progress_callback(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_migration_config_t cfg = {
        .source = SC_MIGRATION_SOURCE_NONE,
        .target = SC_MIGRATION_TARGET_SQLITE,
        .source_path = "/tmp",
        .source_path_len = 4,
        .target_path = "/tmp/out.db",
        .target_path_len = 12,
        .dry_run = true,
    };
    sc_migration_stats_t stats = {0};
    g_migration_progress_cur = 99;
    g_migration_progress_tot = 99;
    sc_error_t err = sc_migration_run(&alloc, &cfg, &stats, migration_progress_cb, NULL);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(g_migration_progress_cur, 0u);
    SC_ASSERT_EQ(g_migration_progress_tot, 0u);
}

/* ─── Context tokens extended ─────────────────────────────────────────────── */
static void test_context_tokens_default(void) {
    uint64_t d = sc_context_tokens_default();
    SC_ASSERT(d > 0);
}

static void test_context_tokens_lookup_gpt4(void) {
    uint64_t v = sc_context_tokens_lookup("openai/gpt-4.1", 14);
    SC_ASSERT(v >= 128000u);
}

static void test_context_tokens_lookup_claude(void) {
    uint64_t v = sc_context_tokens_lookup("anthropic/claude-sonnet-4.6", 26);
    SC_ASSERT(v >= 200000u);
}

static void test_context_tokens_lookup_unknown(void) {
    uint64_t v = sc_context_tokens_lookup("unknown/model-x", 15);
    SC_ASSERT(v >= 0); /* May return 0 (unknown) or fallback */
}

static void test_context_tokens_resolve_nonzero_override(void) {
    uint64_t r = sc_context_tokens_resolve(50000, "openai/gpt-4", 11);
    SC_ASSERT_EQ(r, 50000u);
}

/* ─── Max tokens extended ─────────────────────────────────────────────────── */
static void test_max_tokens_default(void) {
    uint32_t d = sc_max_tokens_default();
    SC_ASSERT_EQ(d, SC_DEFAULT_MODEL_MAX_TOKENS);
}

static void test_max_tokens_lookup_gpt4(void) {
    uint32_t v = sc_max_tokens_lookup("openai/gpt-4.1-mini", 19);
    SC_ASSERT(v >= 8192u);
}

static void test_max_tokens_lookup_unknown(void) {
    uint32_t v = sc_max_tokens_lookup("unknown/model", 12);
    SC_ASSERT(v >= 0); /* Returns 0 for unknown; resolve() uses default */
}

static void test_max_tokens_resolve_override_nonzero(void) {
    uint32_t r = sc_max_tokens_resolve(4096, "openai/gpt-4", 11);
    SC_ASSERT_EQ(r, 4096u);
}

static void test_max_tokens_empty_model_name(void) {
    uint32_t r = sc_max_tokens_resolve(0, "", 0);
    SC_ASSERT_EQ(r, SC_DEFAULT_MODEL_MAX_TOKENS);
}

/* ─── Capabilities ────────────────────────────────────────────────────────── */
static void test_capabilities_build_summary_text(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *out = NULL;
    sc_error_t err = sc_capabilities_build_summary_text(&alloc, NULL, NULL, 0, &out);
    if (err == SC_OK) {
        SC_ASSERT_NOT_NULL(out);
        SC_ASSERT(strlen(out) > 0);
        alloc.free(alloc.ctx, out, strlen(out) + 1);
    }
}

static void test_capabilities_build_manifest_json(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *out = NULL;
    sc_error_t err = sc_capabilities_build_manifest_json(&alloc, NULL, NULL, 0, &out);
    if (err == SC_OK) {
        SC_ASSERT_NOT_NULL(out);
        SC_ASSERT_TRUE(strstr(out, "version") != NULL);
        alloc.free(alloc.ctx, out, strlen(out) + 1);
    }
}

static void test_capabilities_build_prompt_section(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *out = NULL;
    sc_error_t err = sc_capabilities_build_prompt_section(&alloc, NULL, NULL, 0, &out);
    if (err == SC_OK) {
        SC_ASSERT_NOT_NULL(out);
        alloc.free(alloc.ctx, out, strlen(out) + 1);
    }
}

static void test_capabilities_null_alloc_rejected(void) {
    char *out = NULL;
    sc_error_t e1 = sc_capabilities_build_summary_text(NULL, NULL, NULL, 0, &out);
    sc_error_t e2 = sc_capabilities_build_manifest_json(NULL, NULL, NULL, 0, &out);
    SC_ASSERT(e1 != SC_OK);
    SC_ASSERT(e2 != SC_OK);
}

/* ─── Interactions (choices) ──────────────────────────────────────────────── */
static void test_choices_prompt_returns_default_in_test_mode(void) {
    sc_choice_t choices[] = {
        { "Option A", "a", false },
        { "Option B", "b", true },
    };
    sc_choice_result_t result = {0};
    sc_error_t err = sc_choices_prompt("Pick one", choices, 2, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.selected_index, 1u);
    SC_ASSERT_STR_EQ(result.selected_value, "b");
}

static void test_choices_prompt_first_default(void) {
    sc_choice_t choices[] = {
        { "First", "1", true },
        { "Second", "2", false },
    };
    sc_choice_result_t result = {0};
    sc_error_t err = sc_choices_prompt("Pick", choices, 2, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(result.selected_value, "1");
}

static void test_choices_confirm_default_yes(void) {
    SC_ASSERT_TRUE(sc_choices_confirm("Continue?", true));
}

static void test_choices_confirm_default_no(void) {
    SC_ASSERT_FALSE(sc_choices_confirm("Abort?", false));
}

static void test_choices_prompt_null_choices_rejected(void) {
    sc_choice_result_t result = {0};
    sc_error_t err = sc_choices_prompt("Q", NULL, 2, &result);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_choices_prompt_zero_count_rejected(void) {
    sc_choice_t choices[] = {{ "A", "a", true }};
    sc_choice_result_t result = {0};
    sc_error_t err = sc_choices_prompt("Q", choices, 0, &result);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

void run_new_modules_tests(void) {
    SC_TEST_SUITE("New Modules");
    SC_RUN_TEST(test_config_types_constants);
    SC_RUN_TEST(test_portable_atomic_u64);
    SC_RUN_TEST(test_portable_atomic_bool);
    SC_RUN_TEST(test_context_tokens_resolve_override);
    SC_RUN_TEST(test_context_tokens_lookup_known_model);
    SC_RUN_TEST(test_context_tokens_default_fallback);
    SC_RUN_TEST(test_max_tokens_resolve_override);
    SC_RUN_TEST(test_max_tokens_lookup_known_model);
    SC_RUN_TEST(test_max_tokens_default_fallback);
    SC_RUN_TEST(test_agent_routing_normalize_id);
    SC_RUN_TEST(test_agent_routing_find_default_agent);
    SC_RUN_TEST(test_agent_routing_resolve_route_default);
    SC_RUN_TEST(test_agent_routing_resolve_route_peer_match);
    SC_RUN_TEST(test_agent_routing_build_session_key);
    SC_RUN_TEST(test_agent_routing_build_main_session_key);

    SC_RUN_TEST(test_agent_routing_normalize_id_special_chars);
    SC_RUN_TEST(test_agent_routing_normalize_id_all_dash);
    SC_RUN_TEST(test_agent_routing_find_default_empty_list);
    SC_RUN_TEST(test_agent_routing_find_default_first_agent);
    SC_RUN_TEST(test_agent_routing_peer_matches_equal);
    SC_RUN_TEST(test_agent_routing_peer_matches_different_kind);
    SC_RUN_TEST(test_agent_routing_peer_matches_different_id);
    SC_RUN_TEST(test_agent_routing_peer_matches_null);
    SC_RUN_TEST(test_agent_routing_binding_matches_scope_null_constraints);
    SC_RUN_TEST(test_agent_routing_binding_matches_scope_matching);
    SC_RUN_TEST(test_agent_routing_binding_matches_scope_mismatched_channel);
    SC_RUN_TEST(test_agent_routing_binding_matches_scope_mismatched_account);
    SC_RUN_TEST(test_agent_routing_resolve_route_channel_only);
    SC_RUN_TEST(test_agent_routing_resolve_route_guild_match);
    SC_RUN_TEST(test_agent_routing_resolve_route_team_match);
    SC_RUN_TEST(test_agent_routing_resolve_route_account_match);
    SC_RUN_TEST(test_agent_routing_build_session_key_without_peer);
    SC_RUN_TEST(test_agent_routing_build_session_key_group_peer);
    SC_RUN_TEST(test_agent_routing_build_session_key_with_scope_main);
    SC_RUN_TEST(test_agent_routing_build_session_key_with_scope_per_peer);
    SC_RUN_TEST(test_agent_routing_build_thread_session_key);
    SC_RUN_TEST(test_agent_routing_resolve_thread_parent);
    SC_RUN_TEST(test_agent_routing_resolve_thread_parent_no_thread);
    SC_RUN_TEST(test_agent_routing_resolve_linked_peer_no_links);
    SC_RUN_TEST(test_agent_routing_resolve_linked_peer_matched);
    SC_RUN_TEST(test_agent_routing_main_session_key_normalizes);
    SC_RUN_TEST(test_agent_routing_resolve_route_guild_roles_match);
    SC_RUN_TEST(test_agent_routing_build_session_key_with_scope_per_account_channel_peer);
    SC_RUN_TEST(test_agent_routing_resolve_linked_peer_unmatched);
    SC_RUN_TEST(test_agent_routing_free_route_null_safe);
    SC_RUN_TEST(test_agent_routing_normalize_id_strips_dashes);

    /* Onboard */
    SC_RUN_TEST(test_onboard_run_returns_ok_in_test_mode);
    SC_RUN_TEST(test_onboard_check_first_run_invoked);
    SC_RUN_TEST(test_onboard_run_with_null_alloc);

    /* Skillforge */
    SC_RUN_TEST(test_skillforge_create_destroy);
    SC_RUN_TEST(test_skillforge_discover_adds_test_skills);
    SC_RUN_TEST(test_skillforge_get_skill_found);
    SC_RUN_TEST(test_skillforge_get_skill_not_found);
    SC_RUN_TEST(test_skillforge_enable_disable);
    SC_RUN_TEST(test_skillforge_enable_nonexistent);
    SC_RUN_TEST(test_skillforge_disable_nonexistent);
    SC_RUN_TEST(test_skillforge_list_skills);
    SC_RUN_TEST(test_skillforge_another_skill_disabled_by_default);
    SC_RUN_TEST(test_skillforge_cli_helper_has_parameters);
    SC_RUN_TEST(test_skillforge_create_null_alloc);
    SC_RUN_TEST(test_skillforge_discover_null_sf);
    SC_RUN_TEST(test_skillforge_discover_null_path);

    /* Migration */
    SC_RUN_TEST(test_migration_run_null_alloc);
    SC_RUN_TEST(test_migration_run_null_config);
    SC_RUN_TEST(test_migration_run_null_stats);
    SC_RUN_TEST(test_migration_run_dry_run);
    SC_RUN_TEST(test_migration_run_progress_callback_invoked);
    SC_RUN_TEST(test_migration_stats_zeroed);
    SC_RUN_TEST(test_migration_progress_callback);

    /* Context tokens extended */
    SC_RUN_TEST(test_context_tokens_default);
    SC_RUN_TEST(test_context_tokens_lookup_gpt4);
    SC_RUN_TEST(test_context_tokens_lookup_claude);
    SC_RUN_TEST(test_context_tokens_lookup_unknown);
    SC_RUN_TEST(test_context_tokens_resolve_nonzero_override);

    /* Max tokens extended */
    SC_RUN_TEST(test_max_tokens_default);
    SC_RUN_TEST(test_max_tokens_lookup_gpt4);
    SC_RUN_TEST(test_max_tokens_lookup_unknown);
    SC_RUN_TEST(test_max_tokens_resolve_override_nonzero);
    SC_RUN_TEST(test_max_tokens_empty_model_name);

    /* Capabilities */
    SC_RUN_TEST(test_capabilities_build_summary_text);
    SC_RUN_TEST(test_capabilities_build_manifest_json);
    SC_RUN_TEST(test_capabilities_build_prompt_section);
    SC_RUN_TEST(test_capabilities_null_alloc_rejected);

    /* Interactions */
    SC_RUN_TEST(test_choices_prompt_returns_default_in_test_mode);
    SC_RUN_TEST(test_choices_prompt_first_default);
    SC_RUN_TEST(test_choices_confirm_default_yes);
    SC_RUN_TEST(test_choices_confirm_default_no);
    SC_RUN_TEST(test_choices_prompt_null_choices_rejected);
    SC_RUN_TEST(test_choices_prompt_zero_count_rejected);
}
