#ifndef SC_AGENT_ROUTING_H
#define SC_AGENT_ROUTING_H

#include "seaclaw/config_types.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* Peer/conversation kind for routing */
typedef enum sc_peer_kind {
    ChatDirect,
    ChatGroup,
    ChatChannel,
} sc_peer_kind_t;

typedef struct sc_peer_ref {
    const char *id;
    size_t id_len;
    sc_peer_kind_t kind;
} sc_peer_ref_t;

typedef struct sc_agent_binding_match {
    const char *channel;
    const char *account_id;
    const sc_peer_ref_t *peer;
    const char *guild_id;
    const char *team_id;
    const char **roles;
    size_t roles_len;
} sc_agent_binding_match_t;

typedef struct sc_agent_binding {
    const char *agent_id;
    sc_agent_binding_match_t match;
} sc_agent_binding_t;

typedef struct sc_route_input {
    const char *channel;
    const char *account_id;
    const sc_peer_ref_t *peer;
    const sc_peer_ref_t *parent_peer;
    const char *guild_id;
    const char *team_id;
    const char **member_role_ids;
    size_t member_role_ids_len;
} sc_route_input_t;

typedef enum sc_matched_by {
    MatchedPeer,
    MatchedParentPeer,
    MatchedGuildRoles,
    MatchedGuild,
    MatchedTeam,
    MatchedAccount,
    MatchedChannelOnly,
    MatchedDefault,
} sc_matched_by_t;

typedef struct sc_resolved_route {
    char *session_key;
    char *main_session_key;
    const char *agent_id;
    const char *channel;
    const char *account_id;
    sc_matched_by_t matched_by;
} sc_resolved_route_t;

const char *sc_agent_routing_normalize_id(char *buf, size_t buf_size,
                                          const char *input, size_t len);

const char *sc_agent_routing_resolve_linked_peer(
    const char *peer_id, size_t peer_len,
    const sc_identity_link_t *links, size_t links_len);

sc_error_t sc_agent_routing_build_session_key(
    sc_allocator_t *alloc,
    const char *agent_id, const char *channel,
    const sc_peer_ref_t *peer,
    char **out_key);

sc_error_t sc_agent_routing_build_session_key_with_scope(
    sc_allocator_t *alloc,
    const char *agent_id, const char *channel,
    const sc_peer_ref_t *peer,
    sc_dm_scope_t dm_scope,
    const char *account_id,
    const sc_identity_link_t *identity_links, size_t links_len,
    char **out_key);

sc_error_t sc_agent_routing_build_main_session_key(
    sc_allocator_t *alloc, const char *agent_id, char **out_key);

sc_error_t sc_agent_routing_build_thread_session_key(
    sc_allocator_t *alloc, const char *base_key, const char *thread_id, char **out_key);

int sc_agent_routing_resolve_thread_parent(const char *key, size_t *out_prefix_len);

const char *sc_agent_routing_find_default_agent(
    const sc_named_agent_config_t *agents, size_t agents_len);

bool sc_agent_routing_peer_matches(const sc_peer_ref_t *a, const sc_peer_ref_t *b);

bool sc_agent_routing_binding_matches_scope(
    const sc_agent_binding_t *binding, const sc_route_input_t *input);

sc_error_t sc_agent_routing_resolve_route(
    sc_allocator_t *alloc,
    const sc_route_input_t *input,
    const sc_agent_binding_t *bindings, size_t bindings_len,
    const sc_named_agent_config_t *agents, size_t agents_len,
    sc_resolved_route_t *out);

sc_error_t sc_agent_routing_resolve_route_with_session(
    sc_allocator_t *alloc,
    const sc_route_input_t *input,
    const sc_agent_binding_t *bindings, size_t bindings_len,
    const sc_named_agent_config_t *agents, size_t agents_len,
    const sc_session_config_t *session,
    sc_resolved_route_t *out);

void sc_agent_routing_free_route(sc_allocator_t *alloc, sc_resolved_route_t *route);

#endif /* SC_AGENT_ROUTING_H */
