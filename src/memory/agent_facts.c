/* src/memory/agent_facts.c — C3: the daemon's own outbound text becomes
 * first-class facts with provenance. See include/human/memory/agent_facts.h
 * for the full rationale. */
#include "human/memory/agent_facts.h"

#include "human/context/conversation.h"
#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/memory/graph_ingest.h"

#include <stdio.h>
#include <string.h>

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
        bool found = hu_conversation_detect_commitment(reply, reply_len, commitment_out,
                                                       commitment_cap, who_out, who_cap,
                                                       /*from_me=*/true);
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
    if (hu_conversation_detect_commitment(reply, reply_len, commitment, sizeof(commitment), who,
                                          sizeof(who), /*from_me=*/true)) {
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
