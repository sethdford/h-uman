/**
 * daemon_comfort_summary.c — Post-conversation learning helpers extracted from
 * daemon.c (DDD Phase E2 daemon split, chip 2 — see
 * docs/plans/2026-05-29-ddd-bounded-contexts/phase-2-daemon-split.md).
 *
 * Production-only post-reply path (every symbol is compiled out under
 * HU_IS_TEST, mirroring the original inline definitions):
 *   - hu_daemon_record_topic_baselines_from_text — topic keyword baselines (SQLite)
 *   - hu_daemon_score_comfort_engagement — engagement score from a reply
 *   - hu_daemon_store_conversation_summary — summary -> long-term memory + GraphRAG
 *
 * (classify_comfort_response_type was extracted separately in chip 1 to
 * src/daemon/daemon_director.c.)
 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "human/daemon_comfort_summary.h"
#include "human/agent.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/memory.h"
#include "human/memory/deep_extract.h"
#include "human/memory/graph.h"
#ifdef HU_ENABLE_SQLITE
#include "human/memory/superhuman.h"
#endif
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
/* F23: Extract significant topic keywords from user text and record baselines.
 * Skips stopwords, records each significant word (3–32 chars) via topic_baselines. */
#define HU_DAEMON_TOPIC_BASELINE_MAX 8
#define HU_DAEMON_TOPIC_BUF          32

void hu_daemon_record_topic_baselines_from_text(hu_memory_t *memory, const char *contact_id,
                                             size_t contact_id_len, const char *text,
                                             size_t text_len) {
    if (!memory || !contact_id || contact_id_len == 0 || !text || text_len == 0)
        return;
    static const char *const stop[] = {
        "i",     "the",   "a",      "is",    "was", "that", "this", "it",    "to",  "and",  "but",
        "so",    "just",  "really", "what",  "how", "why",  "when", "where", "who", "can",  "will",
        "would", "could", "should", "have",  "has", "had",  "do",   "does",  "did", "am",   "are",
        "were",  "be",    "been",   "being", "of",  "in",   "on",   "at",    "for", "with", "about",
        "from",  "as",    "or",     "if",    "not", "no",   "yes",  "oh",    "um",  "like", NULL,
    };
    char topics[HU_DAEMON_TOPIC_BASELINE_MAX][HU_DAEMON_TOPIC_BUF];
    size_t topic_count = 0;
    memset(topics, 0, sizeof(topics));

    const char *p = text;
    const char *end = text + text_len;
    while (p < end && topic_count < HU_DAEMON_TOPIC_BASELINE_MAX) {
        while (p < end && !isalnum((unsigned char)*p))
            p++;
        if (p >= end)
            break;
        const char *start = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '\'' || *p == '-'))
            p++;
        size_t wlen = (size_t)(p - start);
        if (wlen < 3 || wlen >= HU_DAEMON_TOPIC_BUF - 1)
            continue;
        bool is_stop = false;
        for (const char *const *sw = stop; *sw; sw++) {
            size_t swlen = strlen(*sw);
            if (wlen == swlen && strncasecmp(start, *sw, wlen) == 0) {
                is_stop = true;
                break;
            }
        }
        if (is_stop)
            continue;
        /* Dedupe */
        bool dup = false;
        for (size_t k = 0; k < topic_count; k++) {
            if (strncasecmp(topics[k], start, wlen) == 0 && topics[k][wlen] == '\0') {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        memcpy(topics[topic_count], start, wlen);
        topics[topic_count][wlen] = '\0';
        for (size_t i = 0; i < wlen; i++)
            topics[topic_count][i] = (char)tolower((unsigned char)topics[topic_count][i]);
        (void)hu_superhuman_topic_baseline_record(memory, contact_id, contact_id_len,
                                                  topics[topic_count], wlen);
        topic_count++;
    }
}
#else
void hu_daemon_record_topic_baselines_from_text(hu_memory_t *memory, const char *contact_id,
                                                size_t contact_id_len, const char *text,
                                                size_t text_len) {
    (void)memory;
    (void)contact_id;
    (void)contact_id_len;
    (void)text;
    (void)text_len;
}
#endif /* HU_ENABLE_SQLITE */

/* F27: Score engagement from their reply. reply_len>20 + positive words -> 0.8;
 * brief thanks -> 0.4; very short -> 0.2. */
float hu_daemon_score_comfort_engagement(const char *reply, size_t reply_len) {
    if (!reply)
        return 0.2f;
    if (reply_len <= 5)
        return 0.2f;
    char lower[256];
    size_t copy = reply_len < sizeof(lower) - 1 ? reply_len : sizeof(lower) - 1;
    for (size_t i = 0; i < copy; i++) {
        char c = reply[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    lower[copy] = '\0';
    if (strstr(lower, "thanks") || strstr(lower, "thank you") || strstr(lower, "ty ") ||
        strstr(lower, "thx")) {
        if (reply_len > 20)
            return 0.8f;
        return 0.4f;
    }
    if (strstr(lower, "yes") || strstr(lower, "yeah") || strstr(lower, "that helped") ||
        strstr(lower, "good point") || strstr(lower, "makes sense")) {
        return 0.8f;
    }
    if (reply_len > 20)
        return 0.6f;
    return 0.3f;
}

#ifndef HU_IS_TEST
/* Store a conversation summary as long-term memory.
 * Concatenates the user message and agent response, runs deep-extract
 * on the full exchange, and stores extracted facts scoped to the contact.
 * When graph is non-NULL, also upserts facts and relations into the GraphRAG knowledge graph.
 * agent may be NULL; when non-NULL and bth_metrics set, increments facts_extracted. */
void hu_daemon_store_conversation_summary(hu_allocator_t *alloc, hu_memory_t *memory,
                                       hu_graph_t *graph, hu_agent_t *agent, const char *session_id,
                                       size_t session_id_len, const char *user_msg,
                                       size_t user_msg_len, const char *response,
                                       size_t response_len) {
    if (!alloc || !memory || !memory->vtable || !memory->vtable->store)
        return;
    if (!user_msg || user_msg_len == 0)
        return;
    /* The director can record non-text turns (tapback/silence) with a NULL
     * response; never feed NULL into the "%.*s" formatter below. */
    if (!response) {
        response = "";
        response_len = 0;
    }
#ifndef HU_ENABLE_SQLITE
    (void)graph;
#endif

    /* Build "them: ... | me: ..." for richer extraction context */
    if (response_len > SIZE_MAX - user_msg_len)
        return;
    if (user_msg_len + response_len > SIZE_MAX - 17)
        return;
    size_t total = user_msg_len + response_len + 16;
    char *combined = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!combined)
        return;
    int n = snprintf(combined, total + 1, "them: %.*s | me: %.*s", (int)user_msg_len, user_msg,
                     (int)response_len, response);
    if (n <= 0) {
        alloc->free(alloc->ctx, combined, total + 1);
        return;
    }
    size_t combined_len = (size_t)n < total ? (size_t)n : total;

    hu_deep_extract_result_t de;
    memset(&de, 0, sizeof(de));
    hu_error_t err = hu_deep_extract_lightweight(alloc, combined, combined_len, &de);
    if (err == HU_OK && de.fact_count > 0) {
        if (agent && agent->bth_metrics)
            agent->bth_metrics->facts_extracted += de.fact_count;
        static const char cat_name[] = "conversation_summary";
        hu_memory_category_t cat = {
            .tag = HU_MEMORY_CATEGORY_CUSTOM,
            .data.custom = {.name = cat_name, .name_len = sizeof(cat_name) - 1},
        };
        for (size_t i = 0; i < de.fact_count; i++) {
            const hu_extracted_fact_t *f = &de.facts[i];
            if (!f->subject || !f->predicate || !f->object)
                continue;
            char key_buf[256];
            int kn =
                snprintf(key_buf, sizeof(key_buf), "%s:%s:%s", f->subject, f->predicate, f->object);
            if (kn > 0 && (size_t)kn < sizeof(key_buf)) {
                (void)memory->vtable->store(memory->ctx, key_buf, (size_t)kn, f->object,
                                            strlen(f->object), &cat, session_id ? session_id : "",
                                            session_id ? session_id_len : 0);
            }
#ifdef HU_ENABLE_SQLITE
            if (graph) {
                int64_t src_id = 0;
                int64_t tgt_id = 0;
                size_t subj_len = strlen(f->subject);
                size_t obj_len = strlen(f->object);
                hu_relation_type_t rel_type =
                    hu_relation_type_from_string(f->predicate, strlen(f->predicate));
                if (hu_graph_upsert_entity(graph, session_id, session_id_len, f->subject, subj_len,
                                           HU_ENTITY_UNKNOWN, NULL, &src_id) == HU_OK &&
                    hu_graph_upsert_entity(graph, session_id, session_id_len, f->object, obj_len,
                                           HU_ENTITY_UNKNOWN, NULL, &tgt_id) == HU_OK) {
                    hu_error_t rel_err =
                        hu_graph_upsert_relation(graph, session_id, session_id_len, src_id, tgt_id,
                                                 rel_type, 1.0f, f->object, obj_len);
                    if (rel_err != HU_OK)
                        hu_log_error("daemon", agent ? agent->observer : NULL,
                                     "graph: relation upsert failed: %s", hu_error_string(rel_err));
                }
            }
#endif
        }
    }
#ifdef HU_ENABLE_SQLITE
    if (graph && err == HU_OK && de.relation_count > 0) {
        for (size_t i = 0; i < de.relation_count; i++) {
            const hu_extracted_relation_t *r = &de.relations[i];
            if (!r->entity_a || !r->relation || !r->entity_b)
                continue;
            int64_t src_id = 0;
            int64_t tgt_id = 0;
            size_t a_len = strlen(r->entity_a);
            size_t b_len = strlen(r->entity_b);
            size_t rel_len = strlen(r->relation);
            hu_relation_type_t rel_type = hu_relation_type_from_string(r->relation, rel_len);
            if (hu_graph_upsert_entity(graph, session_id, session_id_len, r->entity_a, a_len,
                                       HU_ENTITY_UNKNOWN, NULL, &src_id) == HU_OK &&
                hu_graph_upsert_entity(graph, session_id, session_id_len, r->entity_b, b_len,
                                       HU_ENTITY_UNKNOWN, NULL, &tgt_id) == HU_OK) {
                hu_error_t rel_err =
                    hu_graph_upsert_relation(graph, session_id, session_id_len, src_id, tgt_id,
                                             rel_type, 1.0f, r->entity_b, b_len);
                if (rel_err != HU_OK)
                    hu_log_error("daemon", agent ? agent->observer : NULL,
                                 "graph: relation upsert failed: %s", hu_error_string(rel_err));
            }
        }
    }
#endif
    hu_deep_extract_result_deinit(&de, alloc);
    alloc->free(alloc->ctx, combined, total + 1);
}
#else
void hu_daemon_store_conversation_summary(hu_allocator_t *alloc, hu_memory_t *memory,
                                          hu_graph_t *graph, hu_agent_t *agent,
                                          const char *session_id, size_t session_id_len,
                                          const char *user_msg, size_t user_msg_len,
                                          const char *response, size_t response_len) {
    (void)alloc;
    (void)memory;
    (void)graph;
    (void)agent;
    (void)session_id;
    (void)session_id_len;
    (void)user_msg;
    (void)user_msg_len;
    (void)response;
    (void)response_len;
}
#endif /* !HU_IS_TEST */
