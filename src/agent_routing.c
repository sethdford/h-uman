#include "seaclaw/agent_routing.h"
#include "seaclaw/core/string.h"
#include "seaclaw/util.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#define NORM_BUF 64

static int str_eq(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len != b_len) return 0;
    if (a_len == 0) return 1;
    if (!a || !b) return 0;
    return memcmp(a, b, a_len) == 0;
}

const char *sc_agent_routing_normalize_id(char *buf, size_t buf_size,
                                          const char *input, size_t len) {
    if (!buf || buf_size == 0) return "default";
    if (!input || len == 0) return "default";

    size_t out = 0;
    for (size_t i = 0; i < len && out < buf_size; i++) {
        char c = input[i];
        if (isalnum((unsigned char)c))
            buf[out++] = (char)tolower((unsigned char)c);
        else
            buf[out++] = '-';
    }
    size_t start = 0;
    while (start < out && buf[start] == '-') start++;
    while (out > start && buf[out - 1] == '-') out--;
    if (start >= out) return "default";
    buf[out] = '\0';
    return buf + start;
}

const char *sc_agent_routing_resolve_linked_peer(
    const char *peer_id, size_t peer_len,
    const sc_identity_link_t *links, size_t links_len) {
    for (size_t i = 0; i < links_len; i++) {
        const sc_identity_link_t *link = &links[i];
        for (size_t j = 0; j < link->peers_len; j++) {
            const char *p = link->peers[j];
            if (p && str_eq(peer_id, peer_len, p, strlen(p)))
                return link->canonical;
        }
    }
    return peer_id;
}

sc_error_t sc_agent_routing_build_session_key(
    sc_allocator_t *alloc,
    const char *agent_id, const char *channel,
    const sc_peer_ref_t *peer,
    char **out_key) {
    return sc_agent_routing_build_session_key_with_scope(
        alloc, agent_id, channel, peer,
        DirectScopePerChannelPeer, NULL, NULL, 0, out_key);
}

sc_error_t sc_agent_routing_build_session_key_with_scope(
    sc_allocator_t *alloc,
    const char *agent_id, const char *channel,
    const sc_peer_ref_t *peer,
    sc_dm_scope_t dm_scope,
    const char *account_id,
    const sc_identity_link_t *identity_links, size_t links_len,
    char **out_key) {
    if (!alloc || !out_key) return SC_ERR_INVALID_ARGUMENT;
    *out_key = NULL;

    char norm_buf[NORM_BUF];
    const char *norm_agent = sc_agent_routing_normalize_id(
        norm_buf, NORM_BUF, agent_id, agent_id ? strlen(agent_id) : 0);

    if (peer && peer->id) {
        const char *kind_str = "direct";
        if (peer->kind == ChatGroup) kind_str = "group";
        else if (peer->kind == ChatChannel) kind_str = "channel";

        if (peer->kind != ChatDirect) {
            char *key = sc_sprintf(alloc, "agent:%s:%s:%s:%.*s",
                                  norm_agent, channel, kind_str,
                                  (int)(peer->id_len ? peer->id_len : strlen(peer->id)),
                                  peer->id);
            if (!key) return SC_ERR_OUT_OF_MEMORY;
            *out_key = key;
            return SC_OK;
        }

        const char *resolved = sc_agent_routing_resolve_linked_peer(
            peer->id, peer->id_len ? peer->id_len : strlen(peer->id),
            identity_links, links_len);

        switch (dm_scope) {
            case DirectScopeMain: {
                char *key = sc_sprintf(alloc, "agent:%s:main", norm_agent);
                if (!key) return SC_ERR_OUT_OF_MEMORY;
                *out_key = key;
                return SC_OK;
            }
            case DirectScopePerPeer: {
                char *key = sc_sprintf(alloc, "agent:%s:direct:%s", norm_agent, resolved);
                if (!key) return SC_ERR_OUT_OF_MEMORY;
                *out_key = key;
                return SC_OK;
            }
            case DirectScopePerChannelPeer: {
                char *key = sc_sprintf(alloc, "agent:%s:%s:direct:%s", norm_agent, channel, resolved);
                if (!key) return SC_ERR_OUT_OF_MEMORY;
                *out_key = key;
                return SC_OK;
            }
            case DirectScopePerAccountChannelPeer: {
                const char *acct = account_id && account_id[0] ? account_id : "default";
                char *key = sc_sprintf(alloc, "agent:%s:%s:%s:direct:%s",
                                      norm_agent, channel, acct, resolved);
                if (!key) return SC_ERR_OUT_OF_MEMORY;
                *out_key = key;
                return SC_OK;
            }
        }
    }

    char *key = sc_sprintf(alloc, "agent:%s:%s:none:none", norm_agent, channel);
    if (!key) return SC_ERR_OUT_OF_MEMORY;
    *out_key = key;
    return SC_OK;
}

sc_error_t sc_agent_routing_build_main_session_key(
    sc_allocator_t *alloc, const char *agent_id, char **out_key) {
    if (!alloc || !out_key) return SC_ERR_INVALID_ARGUMENT;
    char norm_buf[NORM_BUF];
    const char *norm = sc_agent_routing_normalize_id(
        norm_buf, NORM_BUF, agent_id, agent_id ? strlen(agent_id) : 0);
    char *key = sc_sprintf(alloc, "agent:%s:main", norm);
    if (!key) return SC_ERR_OUT_OF_MEMORY;
    *out_key = key;
    return SC_OK;
}

sc_error_t sc_agent_routing_build_thread_session_key(
    sc_allocator_t *alloc, const char *base_key, const char *thread_id, char **out_key) {
    if (!alloc || !out_key) return SC_ERR_INVALID_ARGUMENT;
    char *key = sc_sprintf(alloc, "%s:thread:%s", base_key, thread_id ? thread_id : "");
    if (!key) return SC_ERR_OUT_OF_MEMORY;
    *out_key = key;
    return SC_OK;
}

int sc_agent_routing_resolve_thread_parent(const char *key, size_t *out_prefix_len) {
    if (!key || !out_prefix_len) return -1;
    const char *marker = ":thread:";
    const char *last = strstr(key, marker);
    const char *p = key;
    while ((p = strstr(p, marker)) != NULL) { last = p; p++; }
    if (!last) return -1;
    *out_prefix_len = (size_t)(last - key);
    return 0;
}

const char *sc_agent_routing_find_default_agent(
    const sc_named_agent_config_t *agents, size_t agents_len) {
    if (agents_len > 0 && agents[0].name) return agents[0].name;
    return "main";
}

bool sc_agent_routing_peer_matches(const sc_peer_ref_t *a, const sc_peer_ref_t *b) {
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    size_t al = a->id_len ? a->id_len : (a->id ? strlen(a->id) : 0);
    size_t bl = b->id_len ? b->id_len : (b->id ? strlen(b->id) : 0);
    return str_eq(a->id, al, b->id, bl);
}

bool sc_agent_routing_binding_matches_scope(
    const sc_agent_binding_t *binding, const sc_route_input_t *input) {
    if (!binding || !input) return false;
    if (binding->match.channel) {
        if (!input->channel || strcmp(binding->match.channel, input->channel) != 0)
            return false;
    }
    if (binding->match.account_id) {
        if (!input->account_id || strcmp(binding->match.account_id, input->account_id) != 0)
            return false;
    }
    return true;
}

static bool has_matching_role(const char **binding_roles, size_t br_len,
                              const char **member_roles, size_t mr_len) {
    for (size_t i = 0; i < br_len; i++) {
        for (size_t j = 0; j < mr_len; j++) {
            if (binding_roles[i] && member_roles[j] &&
                strcmp(binding_roles[i], member_roles[j]) == 0)
                return true;
        }
    }
    return false;
}

static bool is_account_only(const sc_agent_binding_t *b) {
    return !b->match.peer && !b->match.guild_id && !b->match.team_id &&
           b->match.roles_len == 0;
}

static bool is_channel_only(const sc_agent_binding_t *b) {
    return !b->match.account_id && !b->match.peer && !b->match.guild_id &&
           !b->match.team_id && b->match.roles_len == 0;
}

static bool all_constraints_match(const sc_agent_binding_t *b,
                                  const sc_route_input_t *input,
                                  const sc_peer_ref_t *check_peer) {
    if (b->match.peer) {
        if (!check_peer) return false;
        if (b->match.peer->kind != check_peer->kind) return false;
        size_t al = b->match.peer->id_len ? b->match.peer->id_len : strlen(b->match.peer->id);
        size_t bl = check_peer->id_len ? check_peer->id_len : strlen(check_peer->id);
        if (!str_eq(b->match.peer->id, al, check_peer->id, bl)) return false;
    }
    if (b->match.guild_id) {
        if (!input->guild_id || strcmp(b->match.guild_id, input->guild_id) != 0)
            return false;
    }
    if (b->match.team_id) {
        if (!input->team_id || strcmp(b->match.team_id, input->team_id) != 0)
            return false;
    }
    if (b->match.roles_len > 0) {
        if (!has_matching_role(b->match.roles, b->match.roles_len,
                              input->member_role_ids, input->member_role_ids_len))
            return false;
    }
    return true;
}

static sc_error_t build_route(sc_allocator_t *alloc, const char *agent_id,
                              const sc_route_input_t *input, sc_matched_by_t matched_by,
                              sc_resolved_route_t *out) {
    sc_error_t err = sc_agent_routing_build_session_key(
        alloc, agent_id, input->channel, input->peer, &out->session_key);
    if (err != SC_OK) return err;
    err = sc_agent_routing_build_main_session_key(alloc, agent_id, &out->main_session_key);
    if (err != SC_OK) {
        if (out->session_key) alloc->free(alloc->ctx, out->session_key, strlen(out->session_key) + 1);
        return err;
    }
    out->agent_id = agent_id;
    out->channel = input->channel;
    out->account_id = input->account_id;
    out->matched_by = matched_by;
    return SC_OK;
}

sc_error_t sc_agent_routing_resolve_route(
    sc_allocator_t *alloc,
    const sc_route_input_t *input,
    const sc_agent_binding_t *bindings, size_t bindings_len,
    const sc_named_agent_config_t *agents, size_t agents_len,
    sc_resolved_route_t *out) {
    if (!alloc || !input || !out) return SC_ERR_INVALID_ARGUMENT;

    sc_agent_binding_t *candidates = (sc_agent_binding_t *)alloc->alloc(
        alloc->ctx, bindings_len * sizeof(sc_agent_binding_t));
    if (!candidates && bindings_len > 0) return SC_ERR_OUT_OF_MEMORY;

    size_t nc = 0;
    for (size_t i = 0; i < bindings_len; i++) {
        if (sc_agent_routing_binding_matches_scope(&bindings[i], input))
            candidates[nc++] = bindings[i];
    }

    /* Tier 1: peer */
    if (input->peer) {
        for (size_t i = 0; i < nc; i++) {
            if (candidates[i].match.peer &&
                all_constraints_match(&candidates[i], input, input->peer)) {
                const char *agent_id = candidates[i].agent_id;
                if (candidates) alloc->free(alloc->ctx, candidates, bindings_len * sizeof(sc_agent_binding_t));
                return build_route(alloc, agent_id, input, MatchedPeer, out);
            }
        }
    }

    /* Tier 2: parent_peer */
    if (input->parent_peer && input->parent_peer->id &&
        (input->parent_peer->id_len ? input->parent_peer->id_len : strlen(input->parent_peer->id)) > 0) {
        for (size_t i = 0; i < nc; i++) {
            if (candidates[i].match.peer &&
                all_constraints_match(&candidates[i], input, input->parent_peer)) {
                const char *agent_id = candidates[i].agent_id;
                if (candidates) alloc->free(alloc->ctx, candidates, bindings_len * sizeof(sc_agent_binding_t));
                return build_route(alloc, agent_id, input, MatchedParentPeer, out);
            }
        }
    }

    /* Tier 3: guild_roles */
    if (input->guild_id && input->member_role_ids_len > 0) {
        for (size_t i = 0; i < nc; i++) {
            if (candidates[i].match.guild_id && candidates[i].match.roles_len > 0 &&
                all_constraints_match(&candidates[i], input, input->peer)) {
                const char *agent_id = candidates[i].agent_id;
                if (candidates) alloc->free(alloc->ctx, candidates, bindings_len * sizeof(sc_agent_binding_t));
                return build_route(alloc, agent_id, input, MatchedGuildRoles, out);
            }
        }
    }

    /* Tier 4: guild only */
    if (input->guild_id) {
        for (size_t i = 0; i < nc; i++) {
            if (candidates[i].match.guild_id && candidates[i].match.roles_len == 0 &&
                all_constraints_match(&candidates[i], input, input->peer)) {
                const char *agent_id = candidates[i].agent_id;
                if (candidates) alloc->free(alloc->ctx, candidates, bindings_len * sizeof(sc_agent_binding_t));
                return build_route(alloc, agent_id, input, MatchedGuild, out);
            }
        }
    }

    /* Tier 5: team */
    if (input->team_id) {
        for (size_t i = 0; i < nc; i++) {
            if (candidates[i].match.team_id &&
                all_constraints_match(&candidates[i], input, input->peer)) {
                const char *agent_id = candidates[i].agent_id;
                if (candidates) alloc->free(alloc->ctx, candidates, bindings_len * sizeof(sc_agent_binding_t));
                return build_route(alloc, agent_id, input, MatchedTeam, out);
            }
        }
    }

    /* Tier 6: account only */
    for (size_t i = 0; i < nc; i++) {
        if (candidates[i].match.account_id && is_account_only(&candidates[i])) {
            const char *agent_id = candidates[i].agent_id;
            if (candidates) alloc->free(alloc->ctx, candidates, bindings_len * sizeof(sc_agent_binding_t));
            return build_route(alloc, agent_id, input, MatchedAccount, out);
        }
    }

    /* Tier 7: channel only */
    for (size_t i = 0; i < nc; i++) {
        if (is_channel_only(&candidates[i])) {
            const char *agent_id = candidates[i].agent_id;
            if (candidates) alloc->free(alloc->ctx, candidates, bindings_len * sizeof(sc_agent_binding_t));
            return build_route(alloc, agent_id, input, MatchedChannelOnly, out);
        }
    }

    const char *default_id = sc_agent_routing_find_default_agent(agents, agents_len);
    if (candidates) alloc->free(alloc->ctx, candidates, bindings_len * sizeof(sc_agent_binding_t));
    return build_route(alloc, default_id, input, MatchedDefault, out);
}

sc_error_t sc_agent_routing_resolve_route_with_session(
    sc_allocator_t *alloc,
    const sc_route_input_t *input,
    const sc_agent_binding_t *bindings, size_t bindings_len,
    const sc_named_agent_config_t *agents, size_t agents_len,
    const sc_session_config_t *session,
    sc_resolved_route_t *out) {
    sc_error_t err = sc_agent_routing_resolve_route(
        alloc, input, bindings, bindings_len, agents, agents_len, out);
    if (err != SC_OK) return err;
    if (!session) return SC_OK;

    if (out->session_key) {
        alloc->free(alloc->ctx, out->session_key, strlen(out->session_key) + 1);
        out->session_key = NULL;
    }
    err = sc_agent_routing_build_session_key_with_scope(
        alloc, out->agent_id, input->channel, input->peer,
        session->dm_scope, input->account_id,
        session->identity_links, session->identity_links_len, &out->session_key);
    if (err != SC_OK) {
        sc_agent_routing_free_route(alloc, out);
        return err;
    }
    return SC_OK;
}

void sc_agent_routing_free_route(sc_allocator_t *alloc, sc_resolved_route_t *route) {
    if (!alloc || !route) return;
    if (route->session_key) {
        alloc->free(alloc->ctx, route->session_key, strlen(route->session_key) + 1);
        route->session_key = NULL;
    }
    if (route->main_session_key) {
        alloc->free(alloc->ctx, route->main_session_key, strlen(route->main_session_key) + 1);
        route->main_session_key = NULL;
    }
}
