#ifndef HU_MEMORY_KNOWLEDGE_H
#define HU_MEMORY_KNOWLEDGE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_knowledge_source {
    HU_KNOWLEDGE_DIRECT = 0, /* told them directly in conversation */
    HU_KNOWLEDGE_GROUP_CHAT,  /* shared in a group they were in */
    HU_KNOWLEDGE_INFERRED,    /* they might know via mutual contacts */
    HU_KNOWLEDGE_PUBLIC       /* posted publicly (social media) */
} hu_knowledge_source_t;

typedef struct hu_knowledge_entry {
    int64_t id;
    char *topic; /* "job_interview", "new_apartment", "health_issue" */
    size_t topic_len;
    char *detail; /* "had interview at Google on Tuesday" */
    size_t detail_len;
    char *contact_id; /* who knows this */
    size_t contact_id_len;
    uint64_t shared_at; /* when it was shared */
    hu_knowledge_source_t source;
    double confidence; /* 1.0=explicitly told, 0.8=group, 0.5=inferred, 0.3=maybe */
    char *source_contact_id; /* if inferred, who did we originally tell */
    size_t source_contact_id_len;
} hu_knowledge_entry_t;

typedef struct hu_knowledge_summary {
    char *contact_id;
    size_t contact_id_len;
    char **known_topics;   /* array of topic strings they know about */
    size_t known_count;
    char **unknown_topics; /* topics explicitly NOT shared with this contact */
    size_t unknown_count;
    char **uncertain_topics; /* topics with confidence 0.3-0.7 */
    size_t uncertain_count;
} hu_knowledge_summary_t;

/* Build SQL to create the knowledge table */
hu_error_t hu_knowledge_create_table_sql(char *buf, size_t cap, size_t *out_len);

/* Build SQL to insert a knowledge entry */
hu_error_t hu_knowledge_insert_sql(const hu_knowledge_entry_t *entry, char *buf, size_t cap,
                                   size_t *out_len);

/* Build SQL to query what a contact knows */
hu_error_t hu_knowledge_query_sql(const char *contact_id, size_t contact_id_len,
                                  double min_confidence, char *buf, size_t cap, size_t *out_len);

/* Check if contact likely already knows about a topic (pure logic, no DB) */
bool hu_knowledge_contact_knows(const hu_knowledge_entry_t *entries, size_t entry_count,
                                const char *contact_id, size_t contact_id_len,
                                const char *topic, size_t topic_len, double min_confidence);

/* Check if sharing a topic with target_contact would leak info from source_contact.
   Returns true if the topic was only shared with source_contact (or inferred from them)
   and NOT with target_contact. */
bool hu_knowledge_would_leak(const hu_knowledge_entry_t *entries, size_t entry_count,
                             const char *topic, size_t topic_len,
                             const char *source_contact_id, size_t source_len,
                             const char *target_contact_id, size_t target_len);

/* Build a prompt-ready summary of what a contact knows/doesn't know.
   Allocates summary fields. Caller must free via hu_knowledge_summary_deinit. */
hu_error_t hu_knowledge_build_summary(hu_allocator_t *alloc,
                                      const hu_knowledge_entry_t *entries, size_t entry_count,
                                      const char *contact_id, size_t contact_id_len,
                                      const char **all_topics, size_t topic_count,
                                      hu_knowledge_summary_t *out);

/* Convert summary to prompt text string.
   Allocates *out. Caller frees via alloc->free. */
hu_error_t hu_knowledge_summary_to_prompt(hu_allocator_t *alloc,
                                          const hu_knowledge_summary_t *summary, char **out,
                                          size_t *out_len);

void hu_knowledge_entry_deinit(hu_allocator_t *alloc, hu_knowledge_entry_t *entry);
void hu_knowledge_summary_deinit(hu_allocator_t *alloc, hu_knowledge_summary_t *summary);

#endif
