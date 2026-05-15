#include "human/agent_routing.h"
#include "human/config.h"
#include "human/config_parse.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

/* These tests verify the session.identity_links JSON parser AND its end-to-end
 * effect on session-key routing. The contract:
 *
 *   With config:
 *     "session": {
 *       "identity_links": [
 *         { "canonical": "+18018285260",
 *           "peers": ["mindy@icloud.com", "mindy.alt@example.com"] }
 *       ]
 *     }
 *
 *   When chat.db gives us peer_id = "mindy@icloud.com", the routing layer
 *   resolves it to "+18018285260" — the canonical handle — so memory writes
 *   under either spelling land in the same session_key namespace.
 *
 * Bug context: prior to this loader, session.identity_links was a struct field
 * with no JSON parser, so resolve_linked_peer always returned peer_id verbatim
 * and contacts with multiple handles sharded their memory across spellings. */

static hu_config_t *make_config_with_arena(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    hu_config_t *cfg = (hu_config_t *)backing.alloc(backing.ctx, sizeof(hu_config_t));
    HU_ASSERT_NOT_NULL(cfg);
    memset(cfg, 0, sizeof(*cfg));
    cfg->arena = arena;
    cfg->allocator = hu_arena_allocator(arena);
    return cfg;
}

static void free_config(hu_config_t *cfg) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_deinit(cfg);
    backing.free(backing.ctx, cfg, sizeof(*cfg));
}

/* ── parse: single link, two peers ──────────────────────────────────────── */

static void parses_single_link_with_two_peers(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j =
        "{\"session\":{\"identity_links\":["
        "{\"canonical\":\"+18018285260\",\"peers\":[\"mindy@icloud.com\",\"alt@example.com\"]}"
        "]}}";
    HU_ASSERT_EQ(hu_config_parse_json(cfg, j, strlen(j)), HU_OK);
    HU_ASSERT_EQ(cfg->session.identity_links_len, 1u);
    HU_ASSERT_STR_EQ(cfg->session.identity_links[0].canonical, "+18018285260");
    HU_ASSERT_EQ(cfg->session.identity_links[0].peers_len, 2u);
    HU_ASSERT_STR_EQ(cfg->session.identity_links[0].peers[0], "mindy@icloud.com");
    HU_ASSERT_STR_EQ(cfg->session.identity_links[0].peers[1], "alt@example.com");
    free_config(cfg);
}

/* ── parse: multiple links ──────────────────────────────────────────────── */

static void parses_multiple_links_independently(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"session\":{\"identity_links\":["
                    "{\"canonical\":\"a\",\"peers\":[\"a1\",\"a2\"]},"
                    "{\"canonical\":\"b\",\"peers\":[\"b1\"]}"
                    "]}}";
    HU_ASSERT_EQ(hu_config_parse_json(cfg, j, strlen(j)), HU_OK);
    HU_ASSERT_EQ(cfg->session.identity_links_len, 2u);
    HU_ASSERT_STR_EQ(cfg->session.identity_links[0].canonical, "a");
    HU_ASSERT_STR_EQ(cfg->session.identity_links[1].canonical, "b");
    HU_ASSERT_EQ(cfg->session.identity_links[0].peers_len, 2u);
    HU_ASSERT_EQ(cfg->session.identity_links[1].peers_len, 1u);
    HU_ASSERT_STR_EQ(cfg->session.identity_links[1].peers[0], "b1");
    free_config(cfg);
}

/* ── parse: malformed entries are skipped silently ──────────────────────── */

static void entry_without_canonical_is_skipped(void) {
    hu_config_t *cfg = make_config_with_arena();
    /* First entry has no canonical → skip. Second is valid → kept. */
    const char *j = "{\"session\":{\"identity_links\":["
                    "{\"peers\":[\"orphan@example.com\"]},"
                    "{\"canonical\":\"good\",\"peers\":[\"g1\"]}"
                    "]}}";
    HU_ASSERT_EQ(hu_config_parse_json(cfg, j, strlen(j)), HU_OK);
    HU_ASSERT_EQ(cfg->session.identity_links_len, 1u);
    HU_ASSERT_STR_EQ(cfg->session.identity_links[0].canonical, "good");
    free_config(cfg);
}

static void entry_with_canonical_but_no_peers_is_kept(void) {
    /* Legal — declares a canonical handle with zero aliases. Useless on its
     * own but not an error; the resolver simply has nothing to match against. */
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"session\":{\"identity_links\":["
                    "{\"canonical\":\"loner\"}"
                    "]}}";
    HU_ASSERT_EQ(hu_config_parse_json(cfg, j, strlen(j)), HU_OK);
    HU_ASSERT_EQ(cfg->session.identity_links_len, 1u);
    HU_ASSERT_STR_EQ(cfg->session.identity_links[0].canonical, "loner");
    HU_ASSERT_EQ(cfg->session.identity_links[0].peers_len, 0u);
    free_config(cfg);
}

/* ── parse: empty array → no links ──────────────────────────────────────── */

static void empty_array_results_in_no_links(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"session\":{\"identity_links\":[]}}";
    HU_ASSERT_EQ(hu_config_parse_json(cfg, j, strlen(j)), HU_OK);
    HU_ASSERT_EQ(cfg->session.identity_links_len, 0u);
    HU_ASSERT(cfg->session.identity_links == NULL);
    free_config(cfg);
}

/* ── parse: absent key keeps defaults ───────────────────────────────────── */

static void absent_identity_links_keeps_defaults(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"session\":{\"idle_minutes\":42}}";
    HU_ASSERT_EQ(hu_config_parse_json(cfg, j, strlen(j)), HU_OK);
    HU_ASSERT_EQ(cfg->session.identity_links_len, 0u);
    HU_ASSERT(cfg->session.identity_links == NULL);
    free_config(cfg);
}

/* ── end-to-end: parsed links drive the routing resolver ────────────────── */

static void resolver_maps_peer_to_canonical_after_parse(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"session\":{\"identity_links\":["
                    "{\"canonical\":\"+18018285260\","
                    "\"peers\":[\"mindy@icloud.com\",\"mindy.alt@example.com\"]}"
                    "]}}";
    HU_ASSERT_EQ(hu_config_parse_json(cfg, j, strlen(j)), HU_OK);

    /* Each peer spelling should resolve to the canonical phone. */
    const char *p1 = "mindy@icloud.com";
    const char *resolved1 = hu_agent_routing_resolve_linked_peer(
        p1, strlen(p1), cfg->session.identity_links, cfg->session.identity_links_len);
    HU_ASSERT_STR_EQ(resolved1, "+18018285260");

    const char *p2 = "mindy.alt@example.com";
    const char *resolved2 = hu_agent_routing_resolve_linked_peer(
        p2, strlen(p2), cfg->session.identity_links, cfg->session.identity_links_len);
    HU_ASSERT_STR_EQ(resolved2, "+18018285260");

    /* An unrelated handle should pass through unchanged. */
    const char *p3 = "alice@example.com";
    const char *resolved3 = hu_agent_routing_resolve_linked_peer(
        p3, strlen(p3), cfg->session.identity_links, cfg->session.identity_links_len);
    HU_ASSERT_STR_EQ(resolved3, p3);

    free_config(cfg);
}

/* ── end-to-end: session_key uses canonical after resolution ────────────── */

static void session_key_uses_canonical_for_aliased_peer(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"session\":{\"identity_links\":["
                    "{\"canonical\":\"+18018285260\",\"peers\":[\"mindy@icloud.com\"]}"
                    "]}}";
    HU_ASSERT_EQ(hu_config_parse_json(cfg, j, strlen(j)), HU_OK);

    /* Build session keys for both the canonical phone and the iCloud alias —
     * with identity_links configured, both must produce the same key. */
    hu_peer_ref_t peer_canonical = {.id = "+18018285260", .id_len = 12, .kind = ChatDirect};
    hu_peer_ref_t peer_alias = {.id = "mindy@icloud.com", .id_len = 16, .kind = ChatDirect};

    char *key_canonical = NULL;
    char *key_alias = NULL;
    hu_allocator_t backing = hu_system_allocator();
    HU_ASSERT_EQ(hu_agent_routing_build_session_key_with_scope(
                     &backing, "seth", "imessage", &peer_canonical, DirectScopePerChannelPeer, NULL,
                     cfg->session.identity_links, cfg->session.identity_links_len, &key_canonical),
                 HU_OK);
    HU_ASSERT_EQ(hu_agent_routing_build_session_key_with_scope(
                     &backing, "seth", "imessage", &peer_alias, DirectScopePerChannelPeer, NULL,
                     cfg->session.identity_links, cfg->session.identity_links_len, &key_alias),
                 HU_OK);

    HU_ASSERT_STR_EQ(key_canonical, key_alias);

    backing.free(backing.ctx, key_canonical, strlen(key_canonical) + 1);
    backing.free(backing.ctx, key_alias, strlen(key_alias) + 1);
    free_config(cfg);
}

/* Regression guard: without identity_links, aliased peers get DIFFERENT keys
 * (this is the bug we're fixing — documents the prior behavior so we never
 * silently regress to it). */
static void session_keys_diverge_without_identity_links(void) {
    hu_peer_ref_t peer_canonical = {.id = "+18018285260", .id_len = 12, .kind = ChatDirect};
    hu_peer_ref_t peer_alias = {.id = "mindy@icloud.com", .id_len = 16, .kind = ChatDirect};
    char *key_canonical = NULL;
    char *key_alias = NULL;
    hu_allocator_t backing = hu_system_allocator();
    HU_ASSERT_EQ(hu_agent_routing_build_session_key_with_scope(
                     &backing, "seth", "imessage", &peer_canonical, DirectScopePerChannelPeer, NULL,
                     NULL, 0, &key_canonical),
                 HU_OK);
    HU_ASSERT_EQ(hu_agent_routing_build_session_key_with_scope(
                     &backing, "seth", "imessage", &peer_alias, DirectScopePerChannelPeer, NULL,
                     NULL, 0, &key_alias),
                 HU_OK);
    HU_ASSERT(strcmp(key_canonical, key_alias) != 0); /* the bug, by design here */
    backing.free(backing.ctx, key_canonical, strlen(key_canonical) + 1);
    backing.free(backing.ctx, key_alias, strlen(key_alias) + 1);
}

/* ── runner ──────────────────────────────────────────────────────────────── */

void run_config_identity_links_tests(void);

void run_config_identity_links_tests(void) {
    HU_TEST_SUITE("Config: session.identity_links");

    HU_RUN_TEST(parses_single_link_with_two_peers);
    HU_RUN_TEST(parses_multiple_links_independently);
    HU_RUN_TEST(entry_without_canonical_is_skipped);
    HU_RUN_TEST(entry_with_canonical_but_no_peers_is_kept);
    HU_RUN_TEST(empty_array_results_in_no_links);
    HU_RUN_TEST(absent_identity_links_keeps_defaults);
    HU_RUN_TEST(resolver_maps_peer_to_canonical_after_parse);
    HU_RUN_TEST(session_key_uses_canonical_for_aliased_peer);
    HU_RUN_TEST(session_keys_diverge_without_identity_links);
}
