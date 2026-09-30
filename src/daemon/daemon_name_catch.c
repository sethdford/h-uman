/* daemon_name_catch.c — per-turn name catcher wiring (spec 2026-09-29 §4.2).
 * See include/human/daemon/name_catch.h. Graph API only (no sqlite3.h). */
#include "human/config.h"
#include "human/core/log.h"
#include "human/daemon/name_catch.h"

#include <stdatomic.h>
#include <string.h>

hu_gate_mode_t hu_name_catch_mode(void) {
    /* Default OFF. LIVE writes the graph, and those rows reach replies through
     * the grounding lexical and contact-fallback paths (fallback is LIVE in
     * prod); HU_GRAPH_NAMES only governs typed-name selection. Measured by
     * scripts/eval_name_grounding.py. */
    return hu_gate_mode_from_env("HU_NAME_CATCH", HU_GATE_OFF);
}

bool hu_name_catch_eligible(const hu_channel_loop_msg_t *msg, const struct hu_config *config) {
    if (!msg || msg->is_group)
        return false;
    const char *self = config ? config->channels.imessage.loopback_handle : NULL;
    return !(self && self[0] && strcmp(msg->session_key, self) == 0);
}

hu_name_catch_action_t hu_name_catch_action(hu_gate_mode_t mode, hu_name_kind_t kind) {
    if (mode != HU_GATE_LIVE)
        return HU_NAME_CATCH_SKIP;
    return kind == HU_NAME_KNOWN ? HU_NAME_CATCH_BUMP : HU_NAME_CATCH_INSERT;
}

static hu_error_t nc_write(hu_graph_t *g, hu_name_catch_action_t action, const char *cid,
                           size_t cid_len, const hu_name_candidate_t *cand) {
    int64_t id = 0;
    if (action == HU_NAME_CATCH_BUMP) /* KNOWN: the stored spelling, so no new row */
        return hu_graph_upsert_entity(g, cid, cid_len, cand->name, cand->len, HU_ENTITY_UNKNOWN,
                                      NULL, &id);
    return hu_graph_upsert_entity_typed(g, cid, cid_len, cand->name, cand->len, HU_ENTITY_UNKNOWN,
                                        HU_NAME_CATCH_PROVENANCE, HU_NAME_CATCH_CONFIDENCE, 0u,
                                        &id);
}

hu_error_t hu_daemon_name_catch(hu_allocator_t *alloc, hu_graph_t *g, hu_gate_mode_t mode,
                                const char *contact_id, size_t contact_id_len, const char *inbound,
                                size_t inbound_len, hu_name_catch_counts_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!alloc || !g || !contact_id || contact_id_len == 0 || !inbound || inbound_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (mode == HU_GATE_OFF)
        return HU_OK;
    hu_graph_entity_t *ents = NULL;
    size_t n_ents = 0;
    if (hu_graph_list_entities(g, alloc, contact_id, contact_id_len, HU_NAME_CATCH_KNOWN_LIMIT,
                               &ents, &n_ents) != HU_OK)
        n_ents = 0; /* no known names: new Capitalized names still count */
    hu_name_ref_t *refs =
        n_ents ? (hu_name_ref_t *)alloc->alloc(alloc->ctx, n_ents * sizeof(*refs)) : NULL;
    size_t n_refs = 0;
    for (size_t i = 0; refs && i < n_ents; i++) {
        if (hu_name_entity_is_nameable(ents[i].type, ents[i].name, ents[i].name_len)) {
            refs[n_refs].name = ents[i].name;
            refs[n_refs].len = ents[i].name_len;
            /* A row this catcher made and nothing has typed yet matches only
             * its own spelling, so "Going" never feeds on every "going". */
            refs[n_refs].exact_case = ents[i].type == HU_ENTITY_UNKNOWN &&
                                      strcmp(ents[i].provenance, HU_NAME_CATCH_PROVENANCE) == 0;
            n_refs++;
        }
    }
    hu_name_candidate_t cands[HU_NAME_CATCH_MAX];
    size_t n_cands = hu_name_extract(inbound, inbound_len, refs, n_refs, cands, HU_NAME_CATCH_MAX);
    hu_error_t first_err = HU_OK;
    for (size_t i = 0; i < n_cands; i++) {
        if (cands[i].kind == HU_NAME_KNOWN)
            out->known++;
        else
            out->fresh++;
        hu_name_catch_action_t action = hu_name_catch_action(mode, cands[i].kind);
        if (action == HU_NAME_CATCH_SKIP)
            continue;
        hu_error_t werr = nc_write(g, action, contact_id, contact_id_len, &cands[i]);
        if (werr == HU_OK)
            out->written++;
        else if (first_err == HU_OK)
            first_err = werr;
    }
    /* KNOWN candidates point into ents[]: free only after every write. */
    if (refs)
        alloc->free(alloc->ctx, refs, n_ents * sizeof(*refs));
    if (ents)
        hu_graph_entities_free(alloc, ents, n_ents);
    return first_err;
}

void hu_daemon_name_catch_tick(hu_allocator_t *alloc, hu_graph_t *g, const char *contact_id,
                               size_t contact_id_len, const char *inbound, size_t inbound_len) {
    static atomic_bool announced = false;
    hu_gate_mode_t mode = hu_name_catch_mode();
    if (mode == HU_GATE_OFF) {
        hu_log_info_once(&announced, "name_catch", NULL,
                         "name_catch disabled (HU_NAME_CATCH unset or off); set "
                         "HU_NAME_CATCH=shadow or =live in the daemon's launchd plist "
                         "EnvironmentVariables to activate");
        return;
    }
    hu_log_info_once(&announced, "name_catch", NULL, "name_catch active: HU_NAME_CATCH=%s",
                     mode == HU_GATE_LIVE ? "live" : "shadow");
    if (!inbound || inbound_len == 0)
        return; /* attachment-only message: no text, nothing to catch, not an error */
    hu_name_catch_counts_t c;
    hu_error_t err =
        hu_daemon_name_catch(alloc, g, mode, contact_id, contact_id_len, inbound, inbound_len, &c);
    if (err != HU_OK)
        hu_log_warn("name_catch", NULL, "name_catch: graph write failed: %s (reply unaffected)",
                    hu_error_string(err));
    if (mode == HU_GATE_SHADOW)
        hu_log_info("name_catch", NULL, "name_catch shadow: known=%zu new=%zu (not written)",
                    c.known, c.fresh);
}

void hu_daemon_name_catch_batch(hu_allocator_t *alloc, hu_graph_t *graph,
                                const hu_channel_loop_msg_t *msgs, size_t start, size_t end,
                                const struct hu_config *config) {
    if (!alloc || !graph || !msgs || start > end || !hu_name_catch_eligible(&msgs[start], config))
        return;
    const char *key = msgs[start].session_key;
    size_t key_len = strnlen(key, sizeof(msgs[start].session_key));
    for (size_t b = start; b <= end; b++)
        hu_daemon_name_catch_tick(alloc, graph, key, key_len, msgs[b].content,
                                  strnlen(msgs[b].content, sizeof(msgs[b].content)));
}
