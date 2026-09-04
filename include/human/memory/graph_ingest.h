#ifndef HUMAN_MEMORY_GRAPH_INGEST_H
#define HUMAN_MEMORY_GRAPH_INGEST_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/graph.h"

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
 * Returns HU_ERR_INVALID_ARGUMENT on NULL graph or empty subject/predicate/object. */
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
