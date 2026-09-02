/* src/memory/agent_facts.c — C3: the daemon's own outbound text becomes
 * first-class facts with provenance. See include/human/memory/agent_facts.h
 * for the full rationale. */
#include "human/memory/agent_facts.h"

#include "human/context/conversation.h"
#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/daemon/promise_keeper.h"
#include "human/memory/graph_ingest.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Env var name and provenance prefix are the wire contract other modules
 * (world-model, grounding reads, the CLI dry-run hook) key on to recognize
 * an agent-authored edge — keep both in one place. */
#define HU_AGENT_FACTS_ENV               "HU_AGENT_FACTS"
#define HU_AGENT_FACTS_PROVENANCE_PREFIX "agent:"
/* An agent's paraphrase of its own reply is a weaker signal than an
 * observation of what the user said — the extractor's raw confidence is
 * for USER-authored text. Scale it down so an agent-sourced fact never
 * outweighs a genuine user-sourced one at read time. */
#define HU_AGENT_FACTS_CONFIDENCE_SCALE 0.6f

static void relabel_subject_assistant(hu_fact_extract_result_t *result) {
    for (size_t i = 0; i < result->fact_count; i++)
        snprintf(result->facts[i].subject, sizeof(result->facts[i].subject), "assistant");
}

/* Follow-up (2026-09-02, C3 measurement): reuses the EXISTING promise-keeper
 * predicate (hu_promise_keeper_is_courtesy_invitation) rather than
 * re-deriving the same "let me know..." filter -- the first
 * eval_agent_promise_recall.py pass showed 3/20 sampled agent-promise
 * candidates were bare courtesy invitations. Both the LIVE/SHADOW storage
 * path and the dry-run CLI hook call this ONE helper so they always agree
 * on what counts as a storable commitment.
 *
 * Returns true and fills commitment_out/who_out iff
 * hu_conversation_detect_commitment(from_me=true) fired AND the result is
 * NOT a courtesy invitation per hu_promise_keeper_is_courtesy_invitation. */
static bool detect_storable_commitment(const char *reply, size_t reply_len, char *commitment_out,
                                       size_t commitment_cap, char *who_out, size_t who_cap,
                                       int64_t now) {
    if (!hu_conversation_detect_commitment(reply, reply_len, commitment_out, commitment_cap,
                                           who_out, who_cap, /*from_me=*/true))
        return false;
    int64_t deadline = hu_conversation_parse_deadline(reply, reply_len, now);
    if (hu_promise_keeper_is_courtesy_invitation(commitment_out, strlen(commitment_out), deadline))
        return false;
    return true;
}

hu_error_t hu_agent_facts_dry_run(const char *reply, size_t reply_len,
                                  hu_fact_extract_result_t *facts_out, char *commitment_out,
                                  size_t commitment_cap, char *who_out, size_t who_cap,
                                  bool *has_commitment_out) {
    if (has_commitment_out)
        *has_commitment_out = false;
    if (!reply || reply_len == 0 || !facts_out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = hu_fact_extract(reply, reply_len, facts_out);
    if (err != HU_OK)
        return err;
    relabel_subject_assistant(facts_out);

    if (commitment_out && commitment_cap > 0 && who_out && who_cap > 0) {
        /* Dry-run is a preview of what hu_agent_facts_record_reply would do
         * "right now" -- there is no caller-supplied `now` in this read-only
         * API, so wall-clock time is the only sensible deadline reference
         * (matches how hu_daemon_promise_keeper_scan_outbound's live path
         * itself uses time(NULL)). */
        bool found = detect_storable_commitment(reply, reply_len, commitment_out, commitment_cap,
                                                who_out, who_cap, (int64_t)time(NULL));
        if (has_commitment_out)
            *has_commitment_out = found;
    }
    return HU_OK;
}

hu_error_t hu_agent_facts_record_reply(hu_graph_t *g, hu_memory_t *mem, const char *contact_id,
                                       size_t cid_len, const char *reply, size_t reply_len,
                                       const char *message_ref, int64_t now) {
    if (!contact_id || cid_len == 0 || !reply || reply_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    /* C3 activation gated on scripts/eval_agent_promise_recall.py: do not
     * flip HU_AGENT_FACTS to default-on without a measurement showing the
     * daemon can actually recall its own commitments from the rows this
     * records (see feature-gate-requires-measurement.md). Default OFF:
     * zero behavior change, zero perf cost. SHADOW extracts + logs what it
     * would write, without touching the graph or memory store. */
    hu_gate_mode_t mode = hu_gate_mode_from_env(HU_AGENT_FACTS_ENV, HU_GATE_OFF);
    if (mode == HU_GATE_OFF)
        return HU_OK;

    char provenance[128];
    snprintf(provenance, sizeof(provenance), "%s%s", HU_AGENT_FACTS_PROVENANCE_PREFIX,
             (message_ref && message_ref[0]) ? message_ref : "unknown");

    hu_fact_extract_result_t facts;
    hu_error_t err = hu_fact_extract(reply, reply_len, &facts);
    if (err != HU_OK)
        return err;
    relabel_subject_assistant(&facts);

    int cid_trunc = (int)(cid_len > 32 ? 32 : cid_len);
    for (size_t i = 0; i < facts.fact_count; i++) {
        const hu_heuristic_fact_t *f = &facts.facts[i];
        float scaled_conf = f->confidence * HU_AGENT_FACTS_CONFIDENCE_SCALE;
        if (mode == HU_GATE_SHADOW) {
            hu_log_info("human", NULL,
                        "[agent-facts SHADOW] would ingest %s %s %s (conf=%.2f, prov=%s) for %.*s",
                        f->subject, f->predicate, f->object, (double)scaled_conf, provenance,
                        cid_trunc, contact_id);
            continue;
        }
        if (!g)
            continue;
        hu_error_t ing_err = hu_graph_ingest_fact(g, contact_id, cid_len, f->subject, f->predicate,
                                                  f->object, scaled_conf, now, provenance);
        if (ing_err != HU_OK && ing_err != HU_ERR_NOT_SUPPORTED)
            hu_log_warn("human", NULL, "[agent-facts] ingest failed (%d) for %.*s", (int)ing_err,
                        cid_trunc, contact_id);
    }

    char commitment[512];
    char who[64];
    if (detect_storable_commitment(reply, reply_len, commitment, sizeof(commitment), who,
                                   sizeof(who), now)) {
        char key[192];
        snprintf(key, sizeof(key), "agent-promise:%.*s:%lld", cid_trunc, contact_id,
                 (long long)now);
        if (mode == HU_GATE_SHADOW) {
            hu_log_info("human", NULL, "[agent-facts SHADOW] would store '%s' key=%s", commitment,
                        key);
        } else if (mem && mem->vtable && mem->vtable->store) {
            hu_memory_category_t cat;
            memset(&cat, 0, sizeof(cat));
            cat.tag = HU_MEMORY_CATEGORY_CORE;
            hu_error_t store_err = mem->vtable->store(mem->ctx, key, strlen(key), commitment,
                                                      strlen(commitment), &cat, NULL, 0);
            if (store_err != HU_OK)
                hu_log_warn("human", NULL, "[agent-facts] promise store failed (%d) key=%s",
                            (int)store_err, key);
        }
    }
    return HU_OK;
}
