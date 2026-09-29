#ifndef HUMAN_MEMORY_GRAPH_INGEST_H
#define HUMAN_MEMORY_GRAPH_INGEST_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/graph.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The one ingest path for a (subject)-[predicate]->(object) fact.
 *
 * Every live writer (the deep-extract turn in daemon.c, the comfort-summary
 * fact merge, the history importer) goes through here so that facts are
 * bi-temporal by construction: the edge is valid from `now` and open-ended,
 * and a prior OPEN edge with the same (contact, subject, single-valued type)
 * but a different object is CLOSED (event_end = now) and linked through
 * supersedes_id by hu_graph_upsert_relation_with_belief's conflict resolver.
 * The grounding read (hu_graph_relations_in_window) then sees one current
 * truth — "lives_in st pete" — instead of both places forever.
 *
 * `predicate` is mapped with hu_relation_type_from_string; unknown predicates
 * become HU_REL_RELATED_TO (kept, not dropped — the object text still grounds).
 * `provenance` is stored verbatim (e.g. "chat.db:1234", "turn:<id>"), NULL ok.
 * Returns HU_ERR_INVALID_ARGUMENT on NULL graph or empty subject/predicate/object,
 * or when the subject or object is NON-REFERENTIAL (see below). */

/* Name hygiene at the WRITE path (2026-09-23).
 *
 * The grounding read (src/agent/graph_grounding.c:230) ranks candidates by
 * mention_count with NO type filter, so whatever tops that count is what the
 * prompt's entity slots get spent on. Measured on the live graph, the top
 * candidates were: "user" 527 mentions across 7 rows, then two raw phone
 * numbers (256, 93), then "work" 12, "you" 8, "home" 8. Real names —
 * "Utah", "Zillow", "St Petersburg FL" — ranked below the noise. That
 * starves the reply of nameable specifics, and the specificity gate reads
 * daemon insider mentions at 49% of the persona's at matched length.
 *
 * Filtering this at READ time is what specificity_score.py's STOP_INSIDER set
 * already does ({"user","self","home","work",...}) — it makes the measurement
 * look clean while retrieval keeps eating the noise. So it is rejected here,
 * at the one ingest path, where it never enters the ranking at all.
 *
 * Two classes, handled differently:
 *
 *  SELF PLACEHOLDER ("user", "me", "i", "self", "myself"). The extractor
 *  writes facts about the persona with a placeholder subject. These facts are
 *  REAL — ("user", "lives_in", "st petersburg") is worth keeping — so the
 *  subject is RESOLVED to the contact's own PERSON node (named by contact_id,
 *  per the convention hu_memory_* establishes) instead of minting a "user"
 *  entity. Dropping the fact would lose information; minting the node poisons
 *  the ranking.
 *
 *  NON-REFERENTIAL ("you", "it", "this", "that", "they", "someone", "thing").
 *  A fact whose subject or object is a bare pronoun carries no referent —
 *  ("user", "likes", "it") grounds nothing — so the fact is REJECTED.
 *
 * Both predicates are exposed so tests can pin them directly and so the
 * cleanup script and the C path can never disagree on the vocabulary. */
bool hu_graph_name_is_self_placeholder(const char *name, size_t len);
bool hu_graph_name_is_nonreferential(const char *name, size_t len);
hu_error_t hu_graph_ingest_fact(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                                const char *subject, const char *predicate, const char *object,
                                float confidence, int64_t now, const char *provenance);

/* Import facts from a JSONL file (one object per line: contact, subject,
 * predicate, object, confidence, ts, source) into `g` via hu_graph_ingest_fact,
 * in ascending `ts` order so supersession is chronological. `exclude` is an
 * optional comma-separated predicate list to skip (e.g. "asking_about" —
 * a question is not a fact about the user). Counts are always written.
 * Returns HU_ERR_NOT_FOUND when the file is unreadable OR nothing was
 * imported: an empty import must never look like a finished one. */
hu_error_t hu_graph_import_facts_jsonl(hu_allocator_t *alloc, hu_graph_t *g, const char *path,
                                       const char *exclude, size_t *imported_out,
                                       size_t *skipped_out);

#ifdef __cplusplus
}
#endif

#endif /* HUMAN_MEMORY_GRAPH_INGEST_H */
