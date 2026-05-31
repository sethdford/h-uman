#ifndef HU_DAEMON_COMFORT_SUMMARY_H
#define HU_DAEMON_COMFORT_SUMMARY_H

#include "core/allocator.h"
#include "memory.h"
#include "memory/graph.h"
#include <stddef.h>

/**
 * daemon_comfort_summary.h — Post-conversation learning helpers extracted from
 * daemon.c (DDD Phase E2 daemon split, chip 2 — see
 * docs/plans/2026-05-29-ddd-bounded-contexts/phase-2-daemon-split.md).
 *
 * These run on the daemon's post-reply path and are PRODUCTION-ONLY: every
 * symbol is compiled out under HU_IS_TEST, mirroring the original inline
 * definitions in daemon.c. Callers in daemon.c remain under the same guards.
 *
 * (classify_comfort_response_type was extracted in chip 1 to
 * src/daemon/daemon_director.c — declared in human/daemon/director.h.)
 */

struct hu_agent;

#ifndef HU_IS_TEST

#ifdef HU_ENABLE_SQLITE
/** F23: Extract significant topic keywords from user text and record baselines.
 *  Skips stopwords; records each significant word (3–32 chars) via
 *  topic_baselines. SQLite-only. */
void hu_daemon_record_topic_baselines_from_text(hu_memory_t *memory, const char *contact_id,
                                                size_t contact_id_len, const char *text,
                                                size_t text_len);
#endif

/** F27: Score engagement from their reply. reply_len>20 + positive words -> 0.8;
 *  brief thanks -> 0.4; very short -> 0.2. */
float hu_daemon_score_comfort_engagement(const char *reply, size_t reply_len);

/** Store a conversation summary as long-term memory. Concatenates the user
 *  message and agent response, runs deep-extract on the full exchange, and
 *  stores extracted facts scoped to the contact. When graph is non-NULL (and
 *  on SQLite builds), also upserts facts and relations into the GraphRAG
 *  knowledge graph. agent may be NULL; when non-NULL and bth_metrics set,
 *  increments facts_extracted. */
void hu_daemon_store_conversation_summary(hu_allocator_t *alloc, hu_memory_t *memory,
                                          hu_graph_t *graph, struct hu_agent *agent,
                                          const char *session_id, size_t session_id_len,
                                          const char *user_msg, size_t user_msg_len,
                                          const char *response, size_t response_len);

#endif /* !HU_IS_TEST */

#endif /* HU_DAEMON_COMFORT_SUMMARY_H */
