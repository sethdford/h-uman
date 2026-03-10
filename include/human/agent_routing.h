#ifndef HU_AGENT_ROUTING_H
#define HU_AGENT_ROUTING_H

#include "human/config_types.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* Peer/conversation kind for routing */
typedef enum hu_peer_kind {
    ChatDirect,
    ChatGroup,
    ChatChannel,
} hu_peer_kind_t;

typedef struct hu_peer_ref {
    const char *id;
    size_t id_len;
    hu_peer_kind_t kind;
} hu_peer_ref_t;

typedef struct hu_agent_binding_match {
    const char *channel;
    const char *account_id;
    const hu_peer_ref_t *peer;
    const char *guild_id;
    const char *team_id;
    const char **roles;
    size_t roles_len;
} hu_agent_binding_match_t;

typedef struct hu_agent_binding {
    const char *agent_id;
    hu_agent_binding_match_t match;
} hu_agent_binding_t;

typedef struct hu_route_input {
    const char *channel;
    const char *account_id;
    const hu_peer_ref_t *peer;
    const hu_peer_ref_t *parent_peer;
    const char *guild_id;
    const char *team_id;
    const char **member_role_ids;
    size_t member_role_ids_len;
} hu_route_input_t;

typedef enum hu_matched_by {
    MatchedPeer,
    MatchedParentPeer,
    MatchedGuildRoles,
    MatchedGuild,
    MatchedTeam,
    MatchedAccount,
    MatchedChannelOnly,
    MatchedDefault,
} hu_matched_by_t;

typedef struct hu_resolved_route {
    char *session_key;
    char *main_session_key;
    const char *agent_id;
    const char *channel;
    const char *account_id;
    hu_matched_by_t matched_by;
} hu_resolved_route_t;

const char *hu_agent_routing_normalize_id(char *buf, size_t buf_size, const char *input,
                                          size_t len);

const char *hu_agent_routing_resolve_linked_peer(const char *peer_id, size_t peer_len,
                                                 const hu_identity_link_t *links, size_t links_len);

hu_error_t hu_agent_routing_build_session_key(hu_allocator_t *alloc, const char *agent_id,
                                              const char *channel, const hu_peer_ref_t *peer,
                                              char **out_key);

hu_error_t hu_agent_routing_build_session_key_with_scope(
    hu_allocator_t *alloc, const char *agent_id, const char *channel, const hu_peer_ref_t *peer,
    hu_dm_scope_t dm_scope, const char *account_id, const hu_identity_link_t *identity_links,
    size_t links_len, char **out_key);

hu_error_t hu_agent_routing_build_main_session_key(hu_allocator_t *alloc, const char *agent_id,
                                                   char **out_key);

hu_error_t hu_agent_routing_build_thread_session_key(hu_allocator_t *alloc, const char *base_key,
                                                     const char *thread_id, char **out_key);

int hu_agent_routing_resolve_thread_parent(const char *key, size_t *out_prefix_len);

const char *hu_agent_routing_find_default_agent(const hu_named_agent_config_t *agents,
                                                size_t agents_len);

bool hu_agent_routing_peer_matches(const hu_peer_ref_t *a, const hu_peer_ref_t *b);

bool hu_agent_routing_binding_matches_scope(const hu_agent_binding_t *binding,
                                            const hu_route_input_t *input);

hu_error_t hu_agent_routing_resolve_route(hu_allocator_t *alloc, const hu_route_input_t *input,
                                          const hu_agent_binding_t *bindings, size_t bindings_len,
                                          const hu_named_agent_config_t *agents, size_t agents_len,
                                          hu_resolved_route_t *out);

hu_error_t hu_agent_routing_resolve_route_with_session(
    hu_allocator_t *alloc, const hu_route_input_t *input, const hu_agent_binding_t *bindings,
    size_t bindings_len, const hu_named_agent_config_t *agents, size_t agents_len,
    const hu_session_config_t *session, hu_resolved_route_t *out);

void hu_agent_routing_free_route(hu_allocator_t *alloc, hu_resolved_route_t *route);

#endif /* HU_AGENT_ROUTING_H */
