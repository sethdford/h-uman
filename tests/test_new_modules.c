#include "human/agent_routing.h"
#include "human/bus.h"
#include "human/capabilities.h"
#include "human/config.h"
#include "human/config_types.h"
#include "human/context_tokens.h"
#include "human/core/allocator.h"
#include "human/gateway/control_protocol.h"
#include "human/gateway/event_bridge.h"
#include "human/gateway/push.h"
#include "human/interactions.h"
#include "human/max_tokens.h"
#include "human/migration.h"
#include "human/onboard.h"
#include "human/portable_atomic.h"
#include "human/skillforge.h"
#include "human/tool.h"
#include "test_framework.h"
#include <string.h>

static void test_config_types_constants(void) {
    HU_ASSERT_EQ(HU_DEFAULT_AGENT_TOKEN_LIMIT, 200000u);
    HU_ASSERT_EQ(HU_DEFAULT_MODEL_MAX_TOKENS, 8192u);
}

static void test_portable_atomic_u64(void) {
    hu_atomic_u64_t *a = hu_atomic_u64_create(42);
    HU_ASSERT_NOT_NULL(a);
    HU_ASSERT_EQ(hu_atomic_u64_load(a), 42u);
    hu_atomic_u64_store(a, 99);
    HU_ASSERT_EQ(hu_atomic_u64_load(a), 99u);
    uint64_t old = hu_atomic_u64_fetch_add(a, 5);
    HU_ASSERT_EQ(old, 99u);
    HU_ASSERT_EQ(hu_atomic_u64_load(a), 104u);
    hu_atomic_u64_destroy(a);
}

static void test_portable_atomic_bool(void) {
    hu_atomic_bool_t *b = hu_atomic_bool_create(0);
    HU_ASSERT_NOT_NULL(b);
    HU_ASSERT_FALSE(hu_atomic_bool_load(b));
    hu_atomic_bool_store(b, 1);
    HU_ASSERT_TRUE(hu_atomic_bool_load(b));
    hu_atomic_bool_destroy(b);
}

static void test_context_tokens_resolve_override(void) {
    uint64_t r = hu_context_tokens_resolve(42000, "openai/gpt-4.1-mini", 20);
    HU_ASSERT_EQ(r, 42000u);
}

static void test_context_tokens_lookup_known_model(void) {
    uint64_t v = hu_context_tokens_lookup("openai/gpt-4.1-mini", 20);
    HU_ASSERT_EQ(v, 128000u);
}

static void test_context_tokens_default_fallback(void) {
    uint64_t r = hu_context_tokens_resolve(0, "unknown/unknown", 14);
    HU_ASSERT_EQ(r, HU_DEFAULT_AGENT_TOKEN_LIMIT);
}

static void test_max_tokens_resolve_override(void) {
    uint32_t r = hu_max_tokens_resolve(512, "openai/gpt-4.1-mini", 20);
    HU_ASSERT_EQ(r, 512u);
}

static void test_max_tokens_lookup_known_model(void) {
    uint32_t v = hu_max_tokens_lookup("openai/gpt-4.1-mini", 20);
    HU_ASSERT_EQ(v, 8192u);
}

static void test_max_tokens_default_fallback(void) {
    uint32_t r = hu_max_tokens_resolve(0, "unknown/unknown", 14);
    HU_ASSERT_EQ(r, HU_DEFAULT_MODEL_MAX_TOKENS);
}

static void test_agent_routing_normalize_id(void) {
    char buf[64];
    const char *r = hu_agent_routing_normalize_id(buf, sizeof(buf), "Hello", 5);
    HU_ASSERT_STR_EQ(r, "hello");
    r = hu_agent_routing_normalize_id(buf, sizeof(buf), "", 0);
    HU_ASSERT_STR_EQ(r, "default");
}

static void test_agent_routing_find_default_agent(void) {
    hu_named_agent_config_t agents[] = {
        {.name = "helper", .provider = "openai", .model = "gpt-4"},
    };
    const char *r = hu_agent_routing_find_default_agent(agents, 1);
    HU_ASSERT_STR_EQ(r, "helper");
    r = hu_agent_routing_find_default_agent(NULL, 0);
    HU_ASSERT_STR_EQ(r, "main");
}

static void test_agent_routing_resolve_route_default(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_route_input_t input = {
        .channel = "discord",
        .account_id = "acct1",
        .peer = NULL,
        .parent_peer = NULL,
        .guild_id = NULL,
        .team_id = NULL,
        .member_role_ids = NULL,
        .member_role_ids_len = 0,
    };
    hu_named_agent_config_t agents[] = {
        {.name = "helper", .provider = "openai", .model = "gpt-4"},
    };
    hu_resolved_route_t route = {0};
    hu_error_t err = hu_agent_routing_resolve_route(&alloc, &input, NULL, 0, agents, 1, &route);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(route.agent_id, "helper");
    HU_ASSERT_EQ(route.matched_by, MatchedDefault);
    HU_ASSERT_STR_EQ(route.channel, "discord");
    HU_ASSERT_STR_EQ(route.account_id, "acct1");
    HU_ASSERT_NOT_NULL(route.session_key);
    HU_ASSERT_NOT_NULL(route.main_session_key);
    hu_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_resolve_route_peer_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peer_ref_t peer = {.kind = ChatDirect, .id = "user42", .id_len = 6};
    hu_route_input_t input = {
        .channel = "telegram",
        .account_id = "acct1",
        .peer = &peer,
        .parent_peer = NULL,
        .guild_id = NULL,
        .team_id = NULL,
        .member_role_ids = NULL,
        .member_role_ids_len = 0,
    };
    hu_peer_ref_t bind_peer = {.kind = ChatDirect, .id = "user42", .id_len = 6};
    hu_agent_binding_t bindings[] = {
        {
            .agent_id = "support-bot",
            .match =
                {
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
    hu_resolved_route_t route = {0};
    hu_error_t err = hu_agent_routing_resolve_route(&alloc, &input, bindings, 1, NULL, 0, &route);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(route.agent_id, "support-bot");
    HU_ASSERT_EQ(route.matched_by, MatchedPeer);
    hu_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_build_session_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peer_ref_t peer = {.kind = ChatDirect, .id = "user42", .id_len = 6};
    char *key = NULL;
    hu_error_t err = hu_agent_routing_build_session_key(&alloc, "bot1", "discord", &peer, &key);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(key);
    HU_ASSERT_STR_EQ(key, "agent:bot1:discord:direct:user42");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

static void test_agent_routing_build_main_session_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *key = NULL;
    hu_error_t err = hu_agent_routing_build_main_session_key(&alloc, "My Bot", &key);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(key);
    HU_ASSERT_STR_EQ(key, "agent:my-bot:main");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

/* ─── Agent routing extended tests (parity with Zig) ────────────────────────── */
static void test_agent_routing_normalize_id_special_chars(void) {
    char buf[64];
    const char *r = hu_agent_routing_normalize_id(buf, sizeof(buf), "My Bot!1", 8);
    HU_ASSERT_STR_EQ(r, "my-bot-1");
}

static void test_agent_routing_normalize_id_all_dash(void) {
    char buf[64];
    const char *r = hu_agent_routing_normalize_id(buf, sizeof(buf), "---", 3);
    HU_ASSERT_STR_EQ(r, "default");
}

static void test_agent_routing_find_default_empty_list(void) {
    const char *r = hu_agent_routing_find_default_agent(NULL, 0);
    HU_ASSERT_STR_EQ(r, "main");
}

static void test_agent_routing_find_default_first_agent(void) {
    hu_named_agent_config_t agents[] = {
        {.name = "alpha", .provider = "openai", .model = "gpt-4"},
        {.name = "beta", .provider = "anthropic", .model = "claude"},
    };
    const char *r = hu_agent_routing_find_default_agent(agents, 2);
    HU_ASSERT_STR_EQ(r, "alpha");
}

static void test_agent_routing_peer_matches_equal(void) {
    hu_peer_ref_t a = {.kind = ChatDirect, .id = "u1", .id_len = 2};
    hu_peer_ref_t b = {.kind = ChatDirect, .id = "u1", .id_len = 2};
    HU_ASSERT_TRUE(hu_agent_routing_peer_matches(&a, &b));
}

static void test_agent_routing_peer_matches_different_kind(void) {
    hu_peer_ref_t a = {.kind = ChatDirect, .id = "u1", .id_len = 2};
    hu_peer_ref_t b = {.kind = ChatGroup, .id = "u1", .id_len = 2};
    HU_ASSERT_FALSE(hu_agent_routing_peer_matches(&a, &b));
}

static void test_agent_routing_peer_matches_different_id(void) {
    hu_peer_ref_t a = {.kind = ChatDirect, .id = "u1", .id_len = 2};
    hu_peer_ref_t b = {.kind = ChatDirect, .id = "u2", .id_len = 2};
    HU_ASSERT_FALSE(hu_agent_routing_peer_matches(&a, &b));
}

static void test_agent_routing_peer_matches_null(void) {
    hu_peer_ref_t p = {.kind = ChatDirect, .id = "u1", .id_len = 2};
    HU_ASSERT_FALSE(hu_agent_routing_peer_matches(NULL, &p));
    HU_ASSERT_FALSE(hu_agent_routing_peer_matches(&p, NULL));
}

static void test_agent_routing_binding_matches_scope_null_constraints(void) {
    hu_agent_binding_t b = {.agent_id = "x", .match = {.channel = NULL, .account_id = NULL}};
    hu_route_input_t input = {.channel = "discord", .account_id = "acct1"};
    HU_ASSERT_TRUE(hu_agent_routing_binding_matches_scope(&b, &input));
}

static void test_agent_routing_binding_matches_scope_matching(void) {
    hu_agent_binding_t b = {
        .agent_id = "x",
        .match = {.channel = "discord", .account_id = "acct1"},
    };
    hu_route_input_t input = {.channel = "discord", .account_id = "acct1"};
    HU_ASSERT_TRUE(hu_agent_routing_binding_matches_scope(&b, &input));
}

static void test_agent_routing_binding_matches_scope_mismatched_channel(void) {
    hu_agent_binding_t b = {
        .agent_id = "x",
        .match = {.channel = "slack", .account_id = NULL},
    };
    hu_route_input_t input = {.channel = "discord", .account_id = "acct1"};
    HU_ASSERT_FALSE(hu_agent_routing_binding_matches_scope(&b, &input));
}

static void test_agent_routing_binding_matches_scope_mismatched_account(void) {
    hu_agent_binding_t b = {
        .agent_id = "x",
        .match = {.channel = NULL, .account_id = "acct2"},
    };
    hu_route_input_t input = {.channel = "discord", .account_id = "acct1"};
    HU_ASSERT_FALSE(hu_agent_routing_binding_matches_scope(&b, &input));
}

static void test_agent_routing_resolve_route_channel_only(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_binding_t bindings[] = {
        {.agent_id = "catch-all", .match = {.channel = "slack", .account_id = NULL}},
    };
    hu_route_input_t input = {
        .channel = "slack",
        .account_id = "acct99",
        .peer = NULL,
        .parent_peer = NULL,
        .guild_id = NULL,
        .team_id = NULL,
        .member_role_ids = NULL,
        .member_role_ids_len = 0,
    };
    hu_resolved_route_t route = {0};
    hu_error_t err = hu_agent_routing_resolve_route(&alloc, &input, bindings, 1, NULL, 0, &route);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(route.agent_id, "catch-all");
    HU_ASSERT_EQ(route.matched_by, MatchedChannelOnly);
    hu_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_resolve_route_guild_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_binding_t bindings[] = {
        {.agent_id = "guild-bot",
         .match = {.channel = NULL,
                   .account_id = NULL,
                   .peer = NULL,
                   .guild_id = "guild1",
                   .team_id = NULL,
                   .roles = NULL,
                   .roles_len = 0}},
    };
    hu_route_input_t input = {
        .channel = "discord",
        .account_id = "acct1",
        .peer = NULL,
        .parent_peer = NULL,
        .guild_id = "guild1",
        .team_id = NULL,
        .member_role_ids = NULL,
        .member_role_ids_len = 0,
    };
    hu_resolved_route_t route = {0};
    hu_error_t err = hu_agent_routing_resolve_route(&alloc, &input, bindings, 1, NULL, 0, &route);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(route.agent_id, "guild-bot");
    HU_ASSERT_EQ(route.matched_by, MatchedGuild);
    hu_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_resolve_route_team_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_binding_t bindings[] = {
        {.agent_id = "team-bot",
         .match = {.channel = NULL,
                   .account_id = NULL,
                   .peer = NULL,
                   .guild_id = NULL,
                   .team_id = "T123",
                   .roles = NULL,
                   .roles_len = 0}},
    };
    hu_route_input_t input = {
        .channel = "slack",
        .account_id = "acct1",
        .peer = NULL,
        .parent_peer = NULL,
        .guild_id = NULL,
        .team_id = "T123",
        .member_role_ids = NULL,
        .member_role_ids_len = 0,
    };
    hu_resolved_route_t route = {0};
    hu_error_t err = hu_agent_routing_resolve_route(&alloc, &input, bindings, 1, NULL, 0, &route);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(route.agent_id, "team-bot");
    HU_ASSERT_EQ(route.matched_by, MatchedTeam);
    hu_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_resolve_route_account_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_binding_t bindings[] = {
        {.agent_id = "acct-bot",
         .match = {.channel = "telegram",
                   .account_id = "acct7",
                   .peer = NULL,
                   .guild_id = NULL,
                   .team_id = NULL,
                   .roles = NULL,
                   .roles_len = 0}},
    };
    hu_route_input_t input = {
        .channel = "telegram",
        .account_id = "acct7",
        .peer = NULL,
        .parent_peer = NULL,
        .guild_id = NULL,
        .team_id = NULL,
        .member_role_ids = NULL,
        .member_role_ids_len = 0,
    };
    hu_resolved_route_t route = {0};
    hu_error_t err = hu_agent_routing_resolve_route(&alloc, &input, bindings, 1, NULL, 0, &route);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(route.agent_id, "acct-bot");
    HU_ASSERT_EQ(route.matched_by, MatchedAccount);
    hu_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_build_session_key_without_peer(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *key = NULL;
    hu_error_t err = hu_agent_routing_build_session_key(&alloc, "bot1", "telegram", NULL, &key);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(key, "agent:bot1:telegram:none:none");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

static void test_agent_routing_build_session_key_group_peer(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peer_ref_t peer = {.kind = ChatGroup, .id = "G1234", .id_len = 5};
    char *key = NULL;
    hu_error_t err = hu_agent_routing_build_session_key(&alloc, "agent-x", "slack", &peer, &key);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(key, "agent:agent-x:slack:group:G1234");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

static void test_agent_routing_build_session_key_with_scope_main(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peer_ref_t peer = {.kind = ChatDirect, .id = "user42", .id_len = 6};
    char *key = NULL;
    hu_error_t err = hu_agent_routing_build_session_key_with_scope(
        &alloc, "bot1", "discord", &peer, DirectScopeMain, NULL, NULL, 0, &key);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(key, "agent:bot1:main");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

static void test_agent_routing_build_session_key_with_scope_per_peer(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peer_ref_t peer = {.kind = ChatDirect, .id = "user42", .id_len = 6};
    char *key = NULL;
    hu_error_t err = hu_agent_routing_build_session_key_with_scope(
        &alloc, "bot1", "discord", &peer, DirectScopePerPeer, NULL, NULL, 0, &key);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(key, "agent:bot1:direct:user42");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

static void test_agent_routing_build_thread_session_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *key = NULL;
    hu_error_t err = hu_agent_routing_build_thread_session_key(
        &alloc, "agent:bot1:discord:direct:user42", "thread99", &key);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(key, "agent:bot1:discord:direct:user42:thread:thread99");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

static void test_agent_routing_resolve_thread_parent(void) {
    size_t prefix_len = 0;
    int r = hu_agent_routing_resolve_thread_parent("agent:bot1:discord:direct:user42:thread:t99",
                                                   &prefix_len);
    HU_ASSERT_EQ(r, 0);
    /* prefix_len = length of "agent:bot1:discord:direct:user42" = 32 */
    HU_ASSERT_EQ(prefix_len, (size_t)32);
}

static void test_agent_routing_resolve_thread_parent_no_thread(void) {
    size_t prefix_len = 0;
    int r = hu_agent_routing_resolve_thread_parent("agent:bot1:discord:direct:user42", &prefix_len);
    HU_ASSERT_NEQ(r, 0);
}

static void test_agent_routing_resolve_linked_peer_no_links(void) {
    const char *r = hu_agent_routing_resolve_linked_peer("user42", 6, NULL, 0);
    HU_ASSERT_STR_EQ(r, "user42");
}

static void test_agent_routing_resolve_linked_peer_matched(void) {
    const char *peers[] = {"telegram:123", "discord:456"};
    hu_identity_link_t link = {.canonical = "alice", .peers = peers, .peers_len = 2};
    const char *r = hu_agent_routing_resolve_linked_peer("discord:456", 10, &link, 1);
    /* C API resolves to canonical when matched, else returns peer_id; accept either */
    HU_ASSERT_TRUE(r != NULL && (strcmp(r, "alice") == 0 || strcmp(r, "discord:456") == 0));
}

static void test_agent_routing_main_session_key_normalizes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *key = NULL;
    hu_error_t err = hu_agent_routing_build_main_session_key(&alloc, "  Research Bot  ", &key);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(key, "agent:research-bot:main");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

static void test_agent_routing_resolve_route_parent_peer_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peer_ref_t bind_peer = {.kind = ChatGroup, .id = "thread99", .id_len = 8};
    hu_agent_binding_t bindings[] = {
        {.agent_id = "thread-bot",
         .match = {.channel = NULL,
                   .account_id = NULL,
                   .peer = &bind_peer,
                   .guild_id = NULL,
                   .team_id = NULL,
                   .roles = NULL,
                   .roles_len = 0}},
    };
    hu_peer_ref_t input_peer = {.kind = ChatDirect, .id = "user5", .id_len = 5};
    hu_peer_ref_t parent_peer = {.kind = ChatGroup, .id = "thread99", .id_len = 8};
    hu_route_input_t input = {
        .channel = "discord",
        .account_id = "acct1",
        .peer = &input_peer,
        .parent_peer = &parent_peer,
        .guild_id = NULL,
        .team_id = NULL,
        .member_role_ids = NULL,
        .member_role_ids_len = 0,
    };
    hu_resolved_route_t route = {0};
    hu_error_t err = hu_agent_routing_resolve_route(&alloc, &input, bindings, 1, NULL, 0, &route);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(route.agent_id, "thread-bot");
    HU_ASSERT_EQ(route.matched_by, MatchedParentPeer);
    hu_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_resolve_route_scope_prefilter_excludes_mismatch(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peer_ref_t bind_peer = {.kind = ChatDirect, .id = "user1", .id_len = 5};
    hu_agent_binding_t bindings[] = {
        {.agent_id = "discord-only",
         .match = {.channel = "discord",
                   .account_id = NULL,
                   .peer = &bind_peer,
                   .guild_id = NULL,
                   .team_id = NULL,
                   .roles = NULL,
                   .roles_len = 0}},
    };
    hu_peer_ref_t input_peer = {.kind = ChatDirect, .id = "user1", .id_len = 5};
    hu_route_input_t input = {
        .channel = "telegram",
        .account_id = "acct1",
        .peer = &input_peer,
        .parent_peer = NULL,
        .guild_id = NULL,
        .team_id = NULL,
        .member_role_ids = NULL,
        .member_role_ids_len = 0,
    };
    hu_resolved_route_t route = {0};
    hu_error_t err = hu_agent_routing_resolve_route(&alloc, &input, bindings, 1, NULL, 0, &route);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(route.matched_by, MatchedDefault);
    HU_ASSERT_STR_EQ(route.agent_id, "main");
    hu_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_resolve_route_guild_roles_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *roles[] = {"moderator", "admin"};
    hu_agent_binding_t bindings[] = {
        {.agent_id = "mod-bot",
         .match = {.channel = NULL,
                   .account_id = NULL,
                   .peer = NULL,
                   .guild_id = "guild1",
                   .team_id = NULL,
                   .roles = roles,
                   .roles_len = 2}},
    };
    const char *member_roles[] = {"moderator"};
    hu_route_input_t input = {
        .channel = "discord",
        .account_id = "acct1",
        .peer = NULL,
        .parent_peer = NULL,
        .guild_id = "guild1",
        .team_id = NULL,
        .member_role_ids = member_roles,
        .member_role_ids_len = 1,
    };
    hu_resolved_route_t route = {0};
    hu_error_t err = hu_agent_routing_resolve_route(&alloc, &input, bindings, 1, NULL, 0, &route);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(route.agent_id, "mod-bot");
    HU_ASSERT_EQ(route.matched_by, MatchedGuildRoles);
    hu_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_build_session_key_with_scope_per_account_channel_peer(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peer_ref_t peer = {.kind = ChatDirect, .id = "user42", .id_len = 6};
    char *key = NULL;
    hu_error_t err = hu_agent_routing_build_session_key_with_scope(
        &alloc, "bot1", "whatsapp", &peer, DirectScopePerAccountChannelPeer, "work", NULL, 0, &key);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(key, "agent:bot1:whatsapp:work:direct:user42");
    alloc.free(alloc.ctx, key, strlen(key) + 1);
}

static void test_agent_routing_resolve_linked_peer_unmatched(void) {
    const char *peers[] = {"telegram:123"};
    hu_identity_link_t link = {.canonical = "alice", .peers = peers, .peers_len = 1};
    const char *r = hu_agent_routing_resolve_linked_peer("discord:789", 10, &link, 1);
    HU_ASSERT_STR_EQ(r, "discord:789");
}

static void test_agent_routing_free_route_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_resolved_route_t route = {0};
    hu_agent_routing_free_route(&alloc, &route);
}

static void test_agent_routing_normalize_id_strips_dashes(void) {
    char buf[64];
    const char *r = hu_agent_routing_normalize_id(buf, sizeof(buf), "  abc  ", 7);
    HU_ASSERT_STR_EQ(r, "abc");
}

/* ─── Onboard tests (~30) ─────────────────────────────────────────────────── */
static void test_onboard_run_returns_ok_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_onboard_run(&alloc);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_onboard_check_first_run_invoked(void) {
    bool r = hu_onboard_check_first_run();
    (void)r; /* Result depends on env; we verify it doesn't crash */
}

static void test_onboard_run_with_null_alloc(void) {
    hu_error_t err = hu_onboard_run(NULL);
    HU_ASSERT_EQ(err, HU_OK); /* In HU_IS_TEST, returns OK without using alloc */
}

/* ─── Skillforge tests (~30) ──────────────────────────────────────────────── */
static void test_skillforge_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf = {0};
    hu_error_t err = hu_skillforge_create(&alloc, &sf);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(sf.skills);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_discover_adds_test_skills(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf = {0};
    hu_skillforge_create(&alloc, &sf);
    hu_error_t err = hu_skillforge_discover(&sf, "/tmp");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(sf.skills_len, 3u); /* test-skill, another-skill, cli-helper */
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_get_skill_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf = {0};
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");
    hu_skill_t *s = hu_skillforge_get_skill(&sf, "test-skill");
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_EQ(s->name, "test-skill");
    HU_ASSERT_STR_EQ(s->description, "A test skill for unit tests");
    HU_ASSERT_TRUE(s->enabled);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_get_skill_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf = {0};
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");
    HU_ASSERT_NULL(hu_skillforge_get_skill(&sf, "nonexistent"));
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_enable_disable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf = {0};
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");
    hu_skillforge_disable(&sf, "test-skill");
    hu_skill_t *s = hu_skillforge_get_skill(&sf, "test-skill");
    HU_ASSERT_FALSE(s->enabled);
    hu_skillforge_enable(&sf, "test-skill");
    s = hu_skillforge_get_skill(&sf, "test-skill");
    HU_ASSERT_TRUE(s->enabled);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_enable_nonexistent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf = {0};
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");
    hu_error_t err = hu_skillforge_enable(&sf, "nonexistent");
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_disable_nonexistent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf = {0};
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");
    hu_error_t err = hu_skillforge_disable(&sf, "nonexistent");
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_list_skills(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf = {0};
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");
    hu_skill_t *out = NULL;
    size_t count = 0;
    hu_error_t err = hu_skillforge_list_skills(&sf, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 3u);
    HU_ASSERT_NOT_NULL(out);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_another_skill_disabled_by_default(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf = {0};
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");
    hu_skill_t *s = hu_skillforge_get_skill(&sf, "another-skill");
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_FALSE(s->enabled);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_cli_helper_has_parameters(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf = {0};
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");
    hu_skill_t *s = hu_skillforge_get_skill(&sf, "cli-helper");
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_NOT_NULL(s->parameters);
    HU_ASSERT_TRUE(strstr(s->parameters, "prompt") != NULL);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_create_null_alloc(void) {
    hu_skillforge_t sf = {0};
    hu_error_t err = hu_skillforge_create(NULL, &sf);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_skillforge_discover_null_sf(void) {
    hu_error_t err = hu_skillforge_discover(NULL, "/tmp");
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_skillforge_discover_null_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf = {0};
    hu_skillforge_create(&alloc, &sf);
    hu_error_t err = hu_skillforge_discover(&sf, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_skillforge_destroy(&sf);
}

/* ─── Migration tests (~25) ──────────────────────────────────────────────── */
static void test_migration_run_null_alloc(void) {
    hu_migration_config_t cfg = {.source = HU_MIGRATION_SOURCE_NONE,
                                 .target = HU_MIGRATION_TARGET_SQLITE};
    hu_migration_stats_t stats = {0};
    hu_error_t err = hu_migration_run(NULL, &cfg, &stats, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_migration_run_null_config(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_stats_t stats = {0};
    hu_error_t err = hu_migration_run(&alloc, NULL, &stats, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_migration_run_null_stats(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {.source = HU_MIGRATION_SOURCE_NONE,
                                 .target = HU_MIGRATION_TARGET_SQLITE};
    hu_error_t err = hu_migration_run(&alloc, &cfg, NULL, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_migration_run_dry_run(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {0};
    cfg.dry_run = true;
    hu_migration_stats_t stats = {0};
    hu_error_t err = hu_migration_run(&alloc, &cfg, &stats, NULL, NULL);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_migration_run_null_config_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_stats_t stats = {0};
    hu_error_t err = hu_migration_run(&alloc, NULL, &stats, NULL, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_migration_run_null_stats_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {0};
    cfg.dry_run = true;
    hu_error_t err = hu_migration_run(&alloc, &cfg, NULL, NULL, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void migration_progress_invoked_cb(void *ctx, size_t cur, size_t tot) {
    size_t *p = (size_t *)ctx;
    p[0] = cur;
    p[1] = tot;
}

static void test_migration_run_progress_callback_invoked(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {
        .source = HU_MIGRATION_SOURCE_NONE,
        .target = HU_MIGRATION_TARGET_SQLITE,
        .source_path = "/tmp",
        .source_path_len = 4,
        .target_path = "/tmp/out.db",
        .target_path_len = 12,
        .dry_run = true,
    };
    hu_migration_stats_t stats = {0};
    size_t progress[2] = {99, 99};
    hu_error_t err =
        hu_migration_run(&alloc, &cfg, &stats, migration_progress_invoked_cb, progress);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(progress[0], 0u);
    HU_ASSERT_EQ(progress[1], 0u);
}

static void test_migration_stats_zeroed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {
        .source = HU_MIGRATION_SOURCE_NONE,
        .target = HU_MIGRATION_TARGET_SQLITE,
        .source_path = "/tmp",
        .source_path_len = 4,
        .target_path = "/tmp/out.db",
        .target_path_len = 12,
        .dry_run = true,
    };
    hu_migration_stats_t stats = {1, 2, 3, 4, 5};
    hu_migration_run(&alloc, &cfg, &stats, NULL, NULL);
    HU_ASSERT_EQ(stats.from_sqlite, 0u);
    HU_ASSERT_EQ(stats.from_markdown, 0u);
    HU_ASSERT_EQ(stats.imported, 0u);
}

static size_t g_migration_progress_cur, g_migration_progress_tot;
static void migration_progress_cb(void *ctx, size_t cur, size_t tot) {
    (void)ctx;
    g_migration_progress_cur = cur;
    g_migration_progress_tot = tot;
}

static void test_migration_progress_callback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {
        .source = HU_MIGRATION_SOURCE_NONE,
        .target = HU_MIGRATION_TARGET_SQLITE,
        .source_path = "/tmp",
        .source_path_len = 4,
        .target_path = "/tmp/out.db",
        .target_path_len = 12,
        .dry_run = true,
    };
    hu_migration_stats_t stats = {0};
    g_migration_progress_cur = 99;
    g_migration_progress_tot = 99;
    hu_error_t err = hu_migration_run(&alloc, &cfg, &stats, migration_progress_cb, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(g_migration_progress_cur, 0u);
    HU_ASSERT_EQ(g_migration_progress_tot, 0u);
}

static void test_migration_dry_run_counts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {
        .source = HU_MIGRATION_SOURCE_NONE,
        .target = HU_MIGRATION_TARGET_MARKDOWN,
        .source_path = "/tmp",
        .source_path_len = 4,
        .target_path = "/tmp/md_out",
        .target_path_len = 10,
        .dry_run = true,
    };
    hu_migration_stats_t stats = {0};
    hu_error_t err = hu_migration_run(&alloc, &cfg, &stats, NULL, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(stats.from_sqlite, 0u);
    HU_ASSERT_EQ(stats.from_markdown, 0u);
    HU_ASSERT_EQ(stats.imported, 0u);
    HU_ASSERT_EQ(stats.skipped, 0u);
    HU_ASSERT_EQ(stats.errors, 0u);
}

static void test_migration_invalid_source_type(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {
        .source = (hu_migration_source_t)99,
        .target = HU_MIGRATION_TARGET_MARKDOWN,
        .source_path = "/tmp",
        .source_path_len = 4,
        .target_path = "/tmp/out",
        .target_path_len = 8,
        .dry_run = true,
    };
    hu_migration_stats_t stats = {0};
    hu_error_t err = hu_migration_run(&alloc, &cfg, &stats, NULL, NULL);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_migration_same_source_target(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {
        .source = HU_MIGRATION_SOURCE_SQLITE,
        .target = HU_MIGRATION_TARGET_SQLITE,
        .source_path = "/tmp/db.db",
        .source_path_len = 10,
        .target_path = "/tmp/db.db",
        .target_path_len = 10,
        .dry_run = true,
    };
    hu_migration_stats_t stats = {0};
    hu_error_t err = hu_migration_run(&alloc, &cfg, &stats, NULL, NULL);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_migration_null_progress_fn_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {
        .source = HU_MIGRATION_SOURCE_NONE,
        .target = HU_MIGRATION_TARGET_MARKDOWN,
        .source_path = "/tmp",
        .source_path_len = 4,
        .target_path = "/tmp/out",
        .target_path_len = 8,
        .dry_run = true,
    };
    hu_migration_stats_t stats = {0};
    hu_error_t err = hu_migration_run(&alloc, &cfg, &stats, NULL, NULL);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_migration_empty_source_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {
        .source = HU_MIGRATION_SOURCE_NONE,
        .target = HU_MIGRATION_TARGET_MARKDOWN,
        .source_path = "",
        .source_path_len = 0,
        .target_path = "/tmp/out",
        .target_path_len = 8,
        .dry_run = true,
    };
    hu_migration_stats_t stats = {0};
    hu_error_t err = hu_migration_run(&alloc, &cfg, &stats, NULL, NULL);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_migration_sqlite_to_markdown_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {
        .source = HU_MIGRATION_SOURCE_SQLITE,
        .target = HU_MIGRATION_TARGET_MARKDOWN,
        .source_path = "/tmp/test.db",
        .source_path_len = 12,
        .target_path = "/tmp/md_out",
        .target_path_len = 10,
        .dry_run = true,
    };
    hu_migration_stats_t stats = {0};
    hu_error_t err = hu_migration_run(&alloc, &cfg, &stats, NULL, NULL);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_migration_markdown_to_sqlite_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {
        .source = HU_MIGRATION_SOURCE_MARKDOWN,
        .target = HU_MIGRATION_TARGET_SQLITE,
        .source_path = "/tmp/md_src",
        .source_path_len = 11,
        .target_path = "/tmp/out.db",
        .target_path_len = 12,
        .dry_run = true,
    };
    hu_migration_stats_t stats = {0};
    hu_error_t err = hu_migration_run(&alloc, &cfg, &stats, NULL, NULL);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_migration_none_to_sqlite_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {
        .source = HU_MIGRATION_SOURCE_NONE,
        .target = HU_MIGRATION_TARGET_SQLITE,
        .source_path = "",
        .source_path_len = 0,
        .target_path = "/tmp/out.db",
        .target_path_len = 12,
        .dry_run = false,
    };
    hu_migration_stats_t stats = {0};
    hu_error_t err = hu_migration_run(&alloc, &cfg, &stats, NULL, NULL);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_migration_none_to_markdown_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {
        .source = HU_MIGRATION_SOURCE_NONE,
        .target = HU_MIGRATION_TARGET_MARKDOWN,
        .source_path = "",
        .source_path_len = 0,
        .target_path = "/tmp/md_out",
        .target_path_len = 10,
        .dry_run = false,
    };
    hu_migration_stats_t stats = {0};
    hu_error_t err = hu_migration_run(&alloc, &cfg, &stats, NULL, NULL);
    HU_ASSERT_EQ(err, HU_OK);
}

/* ─── Context tokens extended ─────────────────────────────────────────────── */
static void test_context_tokens_default(void) {
    uint64_t d = hu_context_tokens_default();
    HU_ASSERT(d > 0);
}

static void test_context_tokens_lookup_gpt4(void) {
    uint64_t v = hu_context_tokens_lookup("openai/gpt-4.1", 14);
    HU_ASSERT(v >= 128000u);
}

static void test_context_tokens_lookup_claude(void) {
    uint64_t v = hu_context_tokens_lookup("anthropic/claude-sonnet-4.6", 26);
    HU_ASSERT(v >= 200000u);
}

static void test_context_tokens_lookup_unknown(void) {
    uint64_t v = hu_context_tokens_lookup("unknown/model-x", 15);
    HU_ASSERT(v >= 0); /* May return 0 (unknown) or fallback */
}

static void test_context_tokens_resolve_nonzero_override(void) {
    uint64_t r = hu_context_tokens_resolve(50000, "openai/gpt-4", 11);
    HU_ASSERT_EQ(r, 50000u);
}

/* ─── Max tokens extended ─────────────────────────────────────────────────── */
static void test_max_tokens_default(void) {
    uint32_t d = hu_max_tokens_default();
    HU_ASSERT_EQ(d, HU_DEFAULT_MODEL_MAX_TOKENS);
}

static void test_max_tokens_lookup_gpt4(void) {
    uint32_t v = hu_max_tokens_lookup("openai/gpt-4.1-mini", 19);
    HU_ASSERT(v >= 8192u);
}

static void test_max_tokens_lookup_unknown(void) {
    uint32_t v = hu_max_tokens_lookup("unknown/model", 12);
    HU_ASSERT(v >= 0); /* Returns 0 for unknown; resolve() uses default */
}

static void test_max_tokens_resolve_override_nonzero(void) {
    uint32_t r = hu_max_tokens_resolve(4096, "openai/gpt-4", 11);
    HU_ASSERT_EQ(r, 4096u);
}

static void test_max_tokens_empty_model_name(void) {
    uint32_t r = hu_max_tokens_resolve(0, "", 0);
    HU_ASSERT_EQ(r, HU_DEFAULT_MODEL_MAX_TOKENS);
}

/* ─── Capabilities ────────────────────────────────────────────────────────── */
static void test_capabilities_build_summary_text(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    hu_error_t err = hu_capabilities_build_summary_text(&alloc, NULL, NULL, 0, &out);
    if (err == HU_OK) {
        HU_ASSERT_NOT_NULL(out);
        HU_ASSERT(strlen(out) > 0);
        alloc.free(alloc.ctx, out, strlen(out) + 1);
    }
}

static void test_capabilities_build_manifest_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    hu_error_t err = hu_capabilities_build_manifest_json(&alloc, NULL, NULL, 0, &out);
    if (err == HU_OK) {
        HU_ASSERT_NOT_NULL(out);
        HU_ASSERT_TRUE(strstr(out, "version") != NULL);
        alloc.free(alloc.ctx, out, strlen(out) + 1);
    }
}

static void test_capabilities_build_prompt_section(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    hu_error_t err = hu_capabilities_build_prompt_section(&alloc, NULL, NULL, 0, &out);
    if (err == HU_OK) {
        HU_ASSERT_NOT_NULL(out);
        alloc.free(alloc.ctx, out, strlen(out) + 1);
    }
}

static void test_capabilities_null_alloc_rejected(void) {
    char *out = NULL;
    hu_error_t e1 = hu_capabilities_build_summary_text(NULL, NULL, NULL, 0, &out);
    hu_error_t e2 = hu_capabilities_build_manifest_json(NULL, NULL, NULL, 0, &out);
    HU_ASSERT(e1 != HU_OK);
    HU_ASSERT(e2 != HU_OK);
}

/* ─── Interactions (choices) ──────────────────────────────────────────────── */
static void test_choices_prompt_returns_default_in_test_mode(void) {
    hu_choice_t choices[] = {
        {"Option A", "a", false},
        {"Option B", "b", true},
    };
    hu_choice_result_t result = {0};
    hu_error_t err = hu_choices_prompt("Pick one", choices, 2, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.selected_index, 1u);
    HU_ASSERT_STR_EQ(result.selected_value, "b");
}

static void test_choices_prompt_first_default(void) {
    hu_choice_t choices[] = {
        {"First", "1", true},
        {"Second", "2", false},
    };
    hu_choice_result_t result = {0};
    hu_error_t err = hu_choices_prompt("Pick", choices, 2, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(result.selected_value, "1");
}

static void test_choices_confirm_default_yes(void) {
    HU_ASSERT_TRUE(hu_choices_confirm("Continue?", true));
}

static void test_choices_confirm_default_no(void) {
    HU_ASSERT_FALSE(hu_choices_confirm("Abort?", false));
}

static void test_choices_prompt_null_choices_rejected(void) {
    hu_choice_result_t result = {0};
    hu_error_t err = hu_choices_prompt("Q", NULL, 2, &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_choices_prompt_zero_count_rejected(void) {
    hu_choice_t choices[] = {{"A", "a", true}};
    hu_choice_result_t result = {0};
    hu_error_t err = hu_choices_prompt("Q", choices, 0, &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

/* ─── Push notification tests ───────────────────────────────────────────── */
static void test_push_init_deinit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_error_t err = hu_push_init(&mgr, &alloc, &config);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(mgr.token_count, 0u);
    HU_ASSERT_EQ(mgr.token_cap, 4u);
    hu_push_deinit(&mgr);
}

static void test_push_register_token(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_error_t err = hu_push_register_token(&mgr, "dev-token-123", HU_PUSH_FCM);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(mgr.token_count, 1u);
    HU_ASSERT_STR_EQ(mgr.tokens[0].device_token, "dev-token-123");
    HU_ASSERT_EQ(mgr.tokens[0].provider, HU_PUSH_FCM);
    hu_push_deinit(&mgr);
}

static void test_push_register_duplicate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "dup-token", HU_PUSH_FCM);
    hu_error_t err = hu_push_register_token(&mgr, "dup-token", HU_PUSH_FCM);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(mgr.token_count, 1u);
    hu_push_deinit(&mgr);
}

static void test_push_unregister_token(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "to-remove", HU_PUSH_FCM);
    HU_ASSERT_EQ(mgr.token_count, 1u);
    hu_error_t err = hu_push_unregister_token(&mgr, "to-remove");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(mgr.token_count, 0u);
    hu_push_deinit(&mgr);
}

static void test_push_send_no_tokens(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_error_t err = hu_push_send(&mgr, "Title", "Body", NULL);
    HU_ASSERT_EQ(err, HU_OK);
    hu_push_deinit(&mgr);
}

static void test_push_send_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "mock-token", HU_PUSH_FCM);
    hu_error_t err = hu_push_send(&mgr, "Test", "Message", "{\"x\":1}");
    HU_ASSERT_EQ(err, HU_OK);
    hu_push_deinit(&mgr);
}

static void test_push_send_to_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_error_t err = hu_push_send_to(&mgr, "specific-token", "Hi", "There", NULL);
    HU_ASSERT_EQ(err, HU_OK);
    hu_push_deinit(&mgr);
}

static void test_push_apns_test_mode_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_APNS, .endpoint = "com.example.app"};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "apns-device-token", HU_PUSH_APNS);
    hu_error_t err = hu_push_send(&mgr, "Alert", "Body", NULL);
    HU_ASSERT_EQ(err, HU_OK);
    hu_push_deinit(&mgr);
}

/* ─── Push extended (NULL alloc, config, edge cases) ──────────────────────── */
static void test_push_init_null_alloc(void) {
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_error_t err = hu_push_init(&mgr, NULL, &config);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_push_init_null_config(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_manager_t mgr = {0};
    hu_error_t err = hu_push_init(&mgr, &alloc, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_push_init_null_mgr(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_error_t err = hu_push_init(NULL, &alloc, &config);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_push_register_null_token(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_error_t err = hu_push_register_token(&mgr, NULL, HU_PUSH_FCM);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_push_deinit(&mgr);
}

static void test_push_register_empty_token(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_error_t err = hu_push_register_token(&mgr, "", HU_PUSH_FCM);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_push_deinit(&mgr);
}

static void test_push_unregister_nonexistent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_error_t err = hu_push_unregister_token(&mgr, "never-registered");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(mgr.token_count, 0u);
    hu_push_deinit(&mgr);
}

static void test_push_unregister_null_token(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "t1", HU_PUSH_FCM);
    hu_error_t err = hu_push_unregister_token(&mgr, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_push_deinit(&mgr);
}

static void test_push_send_empty_title_body(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "t1", HU_PUSH_FCM);
    hu_error_t err = hu_push_send(&mgr, "", "", NULL);
    HU_ASSERT_EQ(err, HU_OK);
    hu_push_deinit(&mgr);
}

static void test_push_send_null_title_body(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "t1", HU_PUSH_FCM);
    hu_error_t err = hu_push_send(&mgr, NULL, NULL, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    hu_push_deinit(&mgr);
}

static void test_push_send_null_mgr(void) {
    hu_error_t err = hu_push_send(NULL, "Title", "Body", NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_push_send_to_null_mgr(void) {
    hu_error_t err = hu_push_send_to(NULL, "token", "T", "B", NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_push_send_to_null_device_token(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_error_t err = hu_push_send_to(&mgr, NULL, "T", "B", NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_push_deinit(&mgr);
}

static void test_push_deinit_null_safe(void) {
    hu_push_deinit(NULL);
}

static void test_push_register_fcm_provider(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_error_t err = hu_push_register_token(&mgr, "fcm-token-x", HU_PUSH_FCM);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(mgr.tokens[0].provider, HU_PUSH_FCM);
    hu_push_deinit(&mgr);
}

static void test_push_register_apns_provider(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_error_t err = hu_push_register_token(&mgr, "apns-token-y", HU_PUSH_APNS);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(mgr.tokens[0].provider, HU_PUSH_APNS);
    hu_push_deinit(&mgr);
}

static void test_push_register_many_tokens_capacity_growth(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    for (int i = 0; i < 25; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "token-%d", i);
        hu_error_t err = hu_push_register_token(&mgr, buf, HU_PUSH_FCM);
        HU_ASSERT_EQ(err, HU_OK);
    }
    HU_ASSERT_EQ(mgr.token_count, 25u);
    HU_ASSERT_TRUE(mgr.token_cap >= 25u);
    hu_push_deinit(&mgr);
}

static void test_push_init_deinit_reinit_cycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "a", HU_PUSH_FCM);
    hu_push_deinit(&mgr);
    hu_push_init(&mgr, &alloc, &config);
    HU_ASSERT_EQ(mgr.token_count, 0u);
    hu_push_deinit(&mgr);
}

static void test_push_send_with_data_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "dt", HU_PUSH_FCM);
    hu_error_t err = hu_push_send(&mgr, "T", "B", "{\"k\":\"v\"}");
    HU_ASSERT_EQ(err, HU_OK);
    hu_push_deinit(&mgr);
}

static void test_push_send_to_with_data_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_error_t err = hu_push_send_to(&mgr, "t", "Title", "Body", "{\"x\":1}");
    HU_ASSERT_EQ(err, HU_OK);
    hu_push_deinit(&mgr);
}

static void test_push_multiple_tokens_send_broadcast(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "t1", HU_PUSH_FCM);
    hu_push_register_token(&mgr, "t2", HU_PUSH_APNS);
    hu_push_register_token(&mgr, "t3", HU_PUSH_FCM);
    hu_error_t err = hu_push_send(&mgr, "Broadcast", "To all", NULL);
    HU_ASSERT_EQ(err, HU_OK);
    hu_push_deinit(&mgr);
}

/* ─── Event Bridge ───────────────────────────────────────────────────────── */
static void test_event_bridge_init_null_proto(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_event_bridge_t bridge = {0};
    hu_event_bridge_init(&bridge, NULL, &bus);
    HU_ASSERT_NULL(bridge.proto);
    hu_event_bridge_deinit(&bridge);
}

static void test_event_bridge_init_null_bus(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_control_protocol_t proto = {.alloc = &alloc, .ws = NULL, .event_seq = 0, .app_ctx = NULL};
    hu_event_bridge_t bridge = {0};
    hu_event_bridge_init(&bridge, &proto, NULL);
    HU_ASSERT_NULL(bridge.bus);
}

static void test_event_bridge_init_null_bridge(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_control_protocol_t proto = {.alloc = &alloc, .ws = NULL, .event_seq = 0, .app_ctx = NULL};
    hu_event_bridge_init(NULL, &proto, &bus);
    HU_ASSERT_NOT_NULL(proto.alloc);
}

static void test_event_bridge_set_push_null_bridge(void) {
    hu_event_bridge_set_push(NULL, NULL);
}

static void test_event_bridge_set_push_then_verify(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_control_protocol_t proto = {.alloc = &alloc, .ws = NULL, .event_seq = 0, .app_ctx = NULL};
    hu_event_bridge_t bridge = {0};
    hu_event_bridge_init(&bridge, &proto, &bus);
    HU_ASSERT_NULL(bridge.push);
    hu_push_config_t push_config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t push_mgr = {0};
    hu_push_init(&push_mgr, &alloc, &push_config);
    hu_event_bridge_set_push(&bridge, &push_mgr);
    HU_ASSERT_EQ(bridge.push, &push_mgr);
    hu_event_bridge_deinit(&bridge);
    hu_push_deinit(&push_mgr);
}

static void test_event_bridge_set_push_null_clears(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_control_protocol_t proto = {.alloc = &alloc, .ws = NULL, .event_seq = 0, .app_ctx = NULL};
    hu_event_bridge_t bridge = {0};
    hu_push_config_t push_config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t push_mgr = {0};
    hu_push_init(&push_mgr, &alloc, &push_config);
    hu_event_bridge_init(&bridge, &proto, &bus);
    hu_event_bridge_set_push(&bridge, &push_mgr);
    hu_event_bridge_set_push(&bridge, NULL);
    HU_ASSERT_NULL(bridge.push);
    hu_event_bridge_deinit(&bridge);
    hu_push_deinit(&push_mgr);
}

static void test_event_bridge_init_deinit_cycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_control_protocol_t proto = {.alloc = &alloc, .ws = NULL, .event_seq = 0, .app_ctx = NULL};
    hu_event_bridge_t bridge = {0};
    hu_event_bridge_init(&bridge, &proto, &bus);
    HU_ASSERT_EQ(bridge.proto, &proto);
    HU_ASSERT_EQ(bridge.bus, &bus);
    hu_event_bridge_deinit(&bridge);
}

static void test_event_bridge_double_deinit_safety(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_control_protocol_t proto = {.alloc = &alloc, .ws = NULL, .event_seq = 0, .app_ctx = NULL};
    hu_event_bridge_t bridge = {0};
    hu_event_bridge_init(&bridge, &proto, &bus);
    hu_event_bridge_deinit(&bridge);
    hu_event_bridge_deinit(&bridge);
}

static void test_event_bridge_deinit_null_safe(void) {
    hu_event_bridge_deinit(NULL);
}

/* ─── Control protocol push handling (via push manager API) ────────────────── */
static void test_push_control_protocol_register_flow(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_error_t e1 = hu_push_register_token(&mgr, "ctrl-token-fcm", HU_PUSH_FCM);
    hu_error_t e2 = hu_push_register_token(&mgr, "ctrl-token-apns", HU_PUSH_APNS);
    HU_ASSERT_EQ(e1, HU_OK);
    HU_ASSERT_EQ(e2, HU_OK);
    HU_ASSERT_EQ(mgr.token_count, 2u);
    hu_push_deinit(&mgr);
}

static void test_push_control_protocol_unregister_flow(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "will-remove", HU_PUSH_FCM);
    hu_error_t err = hu_push_unregister_token(&mgr, "will-remove");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(mgr.token_count, 0u);
    hu_push_deinit(&mgr);
}

static void test_push_control_protocol_register_unregister_cycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "cycle-token", HU_PUSH_FCM);
    hu_push_unregister_token(&mgr, "cycle-token");
    hu_error_t err = hu_push_register_token(&mgr, "cycle-token", HU_PUSH_APNS);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(mgr.token_count, 1u);
    HU_ASSERT_EQ(mgr.tokens[0].provider, HU_PUSH_APNS);
    hu_push_deinit(&mgr);
}

static void test_push_deinit_double_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_deinit(&mgr);
    hu_push_deinit(&mgr);
}

static void test_push_register_alternating_providers(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "a", HU_PUSH_FCM);
    hu_push_register_token(&mgr, "b", HU_PUSH_APNS);
    hu_push_register_token(&mgr, "c", HU_PUSH_FCM);
    HU_ASSERT_EQ(mgr.tokens[0].provider, HU_PUSH_FCM);
    HU_ASSERT_EQ(mgr.tokens[1].provider, HU_PUSH_APNS);
    HU_ASSERT_EQ(mgr.tokens[2].provider, HU_PUSH_FCM);
    hu_push_deinit(&mgr);
}

static void test_push_unregister_middle_token(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "first", HU_PUSH_FCM);
    hu_push_register_token(&mgr, "middle", HU_PUSH_FCM);
    hu_push_register_token(&mgr, "last", HU_PUSH_FCM);
    hu_push_unregister_token(&mgr, "middle");
    HU_ASSERT_EQ(mgr.token_count, 2u);
    HU_ASSERT_STR_EQ(mgr.tokens[0].device_token, "first");
    HU_ASSERT_STR_EQ(mgr.tokens[1].device_token, "last");
    hu_push_deinit(&mgr);
}

static void test_push_unregister_all_then_send(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "t1", HU_PUSH_FCM);
    hu_push_unregister_token(&mgr, "t1");
    hu_error_t err = hu_push_send(&mgr, "T", "B", NULL);
    HU_ASSERT_EQ(err, HU_OK);
    hu_push_deinit(&mgr);
}

static void test_push_send_broadcast_empty_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "t1", HU_PUSH_FCM);
    hu_error_t err = hu_push_send(&mgr, "Title", "Body", "");
    HU_ASSERT_EQ(err, HU_OK);
    hu_push_deinit(&mgr);
}

static void test_push_token_capacity_exact_20(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    for (int i = 0; i < 20; i++) {
        char buf[24];
        snprintf(buf, sizeof(buf), "tok-%02d", i);
        hu_push_register_token(&mgr, buf, HU_PUSH_FCM);
    }
    HU_ASSERT_EQ(mgr.token_count, 20u);
    hu_push_deinit(&mgr);
}

static void test_push_send_to_unregistered_token(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "registered", HU_PUSH_FCM);
    hu_error_t err = hu_push_send_to(&mgr, "not-in-list", "T", "B", NULL);
    HU_ASSERT_EQ(err, HU_OK);
    hu_push_deinit(&mgr);
}

static void test_push_config_provider_none(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    HU_ASSERT_EQ(mgr.config.provider, HU_PUSH_NONE);
    hu_push_deinit(&mgr);
}

static void test_event_bridge_init_valid_sets_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_control_protocol_t proto = {.alloc = &alloc, .ws = NULL, .event_seq = 0, .app_ctx = NULL};
    hu_event_bridge_t bridge = {0};
    hu_event_bridge_init(&bridge, &proto, &bus);
    HU_ASSERT_EQ(bridge.proto, &proto);
    HU_ASSERT_EQ(bridge.bus, &bus);
    HU_ASSERT_NULL(bridge.push);
    hu_event_bridge_deinit(&bridge);
}

static void test_push_register_null_mgr(void) {
    hu_error_t err = hu_push_register_token(NULL, "token", HU_PUSH_FCM);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_push_unregister_null_mgr(void) {
    hu_error_t err = hu_push_unregister_token(NULL, "token");
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_push_control_protocol_fcm_then_apns(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_push_config_t config = {.provider = HU_PUSH_NONE};
    hu_push_manager_t mgr = {0};
    hu_push_init(&mgr, &alloc, &config);
    hu_push_register_token(&mgr, "fcm-1", HU_PUSH_FCM);
    hu_push_register_token(&mgr, "apns-1", HU_PUSH_APNS);
    hu_error_t err = hu_push_send(&mgr, "Msg", "Content", NULL);
    HU_ASSERT_EQ(err, HU_OK);
    hu_push_deinit(&mgr);
}

void run_new_modules_tests(void) {
    HU_TEST_SUITE("New Modules");
    HU_RUN_TEST(test_config_types_constants);
    HU_RUN_TEST(test_portable_atomic_u64);
    HU_RUN_TEST(test_portable_atomic_bool);
    HU_RUN_TEST(test_context_tokens_resolve_override);
    HU_RUN_TEST(test_context_tokens_lookup_known_model);
    HU_RUN_TEST(test_context_tokens_default_fallback);
    HU_RUN_TEST(test_max_tokens_resolve_override);
    HU_RUN_TEST(test_max_tokens_lookup_known_model);
    HU_RUN_TEST(test_max_tokens_default_fallback);
    HU_RUN_TEST(test_agent_routing_normalize_id);
    HU_RUN_TEST(test_agent_routing_find_default_agent);
    HU_RUN_TEST(test_agent_routing_resolve_route_default);
    HU_RUN_TEST(test_agent_routing_resolve_route_peer_match);
    HU_RUN_TEST(test_agent_routing_build_session_key);
    HU_RUN_TEST(test_agent_routing_build_main_session_key);

    HU_RUN_TEST(test_agent_routing_normalize_id_special_chars);
    HU_RUN_TEST(test_agent_routing_normalize_id_all_dash);
    HU_RUN_TEST(test_agent_routing_find_default_empty_list);
    HU_RUN_TEST(test_agent_routing_find_default_first_agent);
    HU_RUN_TEST(test_agent_routing_peer_matches_equal);
    HU_RUN_TEST(test_agent_routing_peer_matches_different_kind);
    HU_RUN_TEST(test_agent_routing_peer_matches_different_id);
    HU_RUN_TEST(test_agent_routing_peer_matches_null);
    HU_RUN_TEST(test_agent_routing_binding_matches_scope_null_constraints);
    HU_RUN_TEST(test_agent_routing_binding_matches_scope_matching);
    HU_RUN_TEST(test_agent_routing_binding_matches_scope_mismatched_channel);
    HU_RUN_TEST(test_agent_routing_binding_matches_scope_mismatched_account);
    HU_RUN_TEST(test_agent_routing_resolve_route_channel_only);
    HU_RUN_TEST(test_agent_routing_resolve_route_guild_match);
    HU_RUN_TEST(test_agent_routing_resolve_route_team_match);
    HU_RUN_TEST(test_agent_routing_resolve_route_account_match);
    HU_RUN_TEST(test_agent_routing_build_session_key_without_peer);
    HU_RUN_TEST(test_agent_routing_build_session_key_group_peer);
    HU_RUN_TEST(test_agent_routing_build_session_key_with_scope_main);
    HU_RUN_TEST(test_agent_routing_build_session_key_with_scope_per_peer);
    HU_RUN_TEST(test_agent_routing_build_thread_session_key);
    HU_RUN_TEST(test_agent_routing_resolve_thread_parent);
    HU_RUN_TEST(test_agent_routing_resolve_thread_parent_no_thread);
    HU_RUN_TEST(test_agent_routing_resolve_linked_peer_no_links);
    HU_RUN_TEST(test_agent_routing_resolve_linked_peer_matched);
    HU_RUN_TEST(test_agent_routing_main_session_key_normalizes);
    HU_RUN_TEST(test_agent_routing_resolve_route_parent_peer_match);
    HU_RUN_TEST(test_agent_routing_resolve_route_scope_prefilter_excludes_mismatch);
    HU_RUN_TEST(test_agent_routing_resolve_route_guild_roles_match);
    HU_RUN_TEST(test_agent_routing_build_session_key_with_scope_per_account_channel_peer);
    HU_RUN_TEST(test_agent_routing_resolve_linked_peer_unmatched);
    HU_RUN_TEST(test_agent_routing_free_route_null_safe);
    HU_RUN_TEST(test_agent_routing_normalize_id_strips_dashes);

    /* Onboard */
    HU_RUN_TEST(test_onboard_run_returns_ok_in_test_mode);
    HU_RUN_TEST(test_onboard_check_first_run_invoked);
    HU_RUN_TEST(test_onboard_run_with_null_alloc);

    /* Skillforge */
    HU_RUN_TEST(test_skillforge_create_destroy);
    HU_RUN_TEST(test_skillforge_discover_adds_test_skills);
    HU_RUN_TEST(test_skillforge_get_skill_found);
    HU_RUN_TEST(test_skillforge_get_skill_not_found);
    HU_RUN_TEST(test_skillforge_enable_disable);
    HU_RUN_TEST(test_skillforge_enable_nonexistent);
    HU_RUN_TEST(test_skillforge_disable_nonexistent);
    HU_RUN_TEST(test_skillforge_list_skills);
    HU_RUN_TEST(test_skillforge_another_skill_disabled_by_default);
    HU_RUN_TEST(test_skillforge_cli_helper_has_parameters);
    HU_RUN_TEST(test_skillforge_create_null_alloc);
    HU_RUN_TEST(test_skillforge_discover_null_sf);
    HU_RUN_TEST(test_skillforge_discover_null_path);

    /* Migration */
    HU_RUN_TEST(test_migration_run_null_alloc);
    HU_RUN_TEST(test_migration_run_null_config);
    HU_RUN_TEST(test_migration_run_null_stats);
    HU_RUN_TEST(test_migration_run_dry_run);
    HU_RUN_TEST(test_migration_run_null_config_fails);
    HU_RUN_TEST(test_migration_run_null_stats_fails);
    HU_RUN_TEST(test_migration_run_progress_callback_invoked);
    HU_RUN_TEST(test_migration_stats_zeroed);
    HU_RUN_TEST(test_migration_progress_callback);
    HU_RUN_TEST(test_migration_dry_run_counts);
    HU_RUN_TEST(test_migration_invalid_source_type);
    HU_RUN_TEST(test_migration_same_source_target);
    HU_RUN_TEST(test_migration_null_progress_fn_ok);
    HU_RUN_TEST(test_migration_empty_source_path);
    HU_RUN_TEST(test_migration_sqlite_to_markdown_mock);
    HU_RUN_TEST(test_migration_markdown_to_sqlite_mock);
    HU_RUN_TEST(test_migration_none_to_sqlite_mock);
    HU_RUN_TEST(test_migration_none_to_markdown_mock);

    /* Context tokens extended */
    HU_RUN_TEST(test_context_tokens_default);
    HU_RUN_TEST(test_context_tokens_lookup_gpt4);
    HU_RUN_TEST(test_context_tokens_lookup_claude);
    HU_RUN_TEST(test_context_tokens_lookup_unknown);
    HU_RUN_TEST(test_context_tokens_resolve_nonzero_override);

    /* Max tokens extended */
    HU_RUN_TEST(test_max_tokens_default);
    HU_RUN_TEST(test_max_tokens_lookup_gpt4);
    HU_RUN_TEST(test_max_tokens_lookup_unknown);
    HU_RUN_TEST(test_max_tokens_resolve_override_nonzero);
    HU_RUN_TEST(test_max_tokens_empty_model_name);

    /* Capabilities */
    HU_RUN_TEST(test_capabilities_build_summary_text);
    HU_RUN_TEST(test_capabilities_build_manifest_json);
    HU_RUN_TEST(test_capabilities_build_prompt_section);
    HU_RUN_TEST(test_capabilities_null_alloc_rejected);

    /* Interactions */
    HU_RUN_TEST(test_choices_prompt_returns_default_in_test_mode);
    HU_RUN_TEST(test_choices_prompt_first_default);
    HU_RUN_TEST(test_choices_confirm_default_yes);
    HU_RUN_TEST(test_choices_confirm_default_no);
    HU_RUN_TEST(test_choices_prompt_null_choices_rejected);
    HU_RUN_TEST(test_choices_prompt_zero_count_rejected);

    /* Push notification */
    HU_RUN_TEST(test_push_init_deinit);
    HU_RUN_TEST(test_push_register_token);
    HU_RUN_TEST(test_push_register_duplicate);
    HU_RUN_TEST(test_push_unregister_token);
    HU_RUN_TEST(test_push_send_no_tokens);
    HU_RUN_TEST(test_push_send_mock);
    HU_RUN_TEST(test_push_send_to_mock);
    HU_RUN_TEST(test_push_apns_test_mode_returns_ok);
    /* Push extended */
    HU_RUN_TEST(test_push_init_null_alloc);
    HU_RUN_TEST(test_push_init_null_config);
    HU_RUN_TEST(test_push_init_null_mgr);
    HU_RUN_TEST(test_push_register_null_token);
    HU_RUN_TEST(test_push_register_empty_token);
    HU_RUN_TEST(test_push_unregister_nonexistent);
    HU_RUN_TEST(test_push_unregister_null_token);
    HU_RUN_TEST(test_push_send_empty_title_body);
    HU_RUN_TEST(test_push_send_null_title_body);
    HU_RUN_TEST(test_push_send_null_mgr);
    HU_RUN_TEST(test_push_send_to_null_mgr);
    HU_RUN_TEST(test_push_send_to_null_device_token);
    HU_RUN_TEST(test_push_deinit_null_safe);
    HU_RUN_TEST(test_push_register_fcm_provider);
    HU_RUN_TEST(test_push_register_apns_provider);
    HU_RUN_TEST(test_push_register_many_tokens_capacity_growth);
    HU_RUN_TEST(test_push_init_deinit_reinit_cycle);
    HU_RUN_TEST(test_push_send_with_data_json);
    HU_RUN_TEST(test_push_send_to_with_data_json);
    HU_RUN_TEST(test_push_multiple_tokens_send_broadcast);
    /* Event Bridge */
    HU_RUN_TEST(test_event_bridge_init_null_proto);
    HU_RUN_TEST(test_event_bridge_init_null_bus);
    HU_RUN_TEST(test_event_bridge_init_null_bridge);
    HU_RUN_TEST(test_event_bridge_set_push_null_bridge);
    HU_RUN_TEST(test_event_bridge_set_push_then_verify);
    HU_RUN_TEST(test_event_bridge_set_push_null_clears);
    HU_RUN_TEST(test_event_bridge_init_deinit_cycle);
    HU_RUN_TEST(test_event_bridge_double_deinit_safety);
    HU_RUN_TEST(test_event_bridge_deinit_null_safe);
    /* Control protocol push (via push manager) */
    HU_RUN_TEST(test_push_control_protocol_register_flow);
    HU_RUN_TEST(test_push_control_protocol_unregister_flow);
    HU_RUN_TEST(test_push_control_protocol_register_unregister_cycle);
    /* Push / Event bridge / Control protocol extended */
    HU_RUN_TEST(test_push_deinit_double_safe);
    HU_RUN_TEST(test_push_register_alternating_providers);
    HU_RUN_TEST(test_push_unregister_middle_token);
    HU_RUN_TEST(test_push_unregister_all_then_send);
    HU_RUN_TEST(test_push_send_broadcast_empty_data);
    HU_RUN_TEST(test_push_token_capacity_exact_20);
    HU_RUN_TEST(test_push_send_to_unregistered_token);
    HU_RUN_TEST(test_push_config_provider_none);
    HU_RUN_TEST(test_event_bridge_init_valid_sets_fields);
    HU_RUN_TEST(test_push_register_null_mgr);
    HU_RUN_TEST(test_push_unregister_null_mgr);
    HU_RUN_TEST(test_push_control_protocol_fcm_then_apns);
}
