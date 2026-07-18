#ifndef HU_MEMORY_COGNITIVE_H
#define HU_MEMORY_COGNITIVE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The F65 opinion surface (a second, colliding hu_opinion_t + raw SQL-string
 * builders) was removed 2026-07-18. The one hu_opinion_t definition and its
 * repo-backed API live in human/memory/opinions.h; the pure challenge
 * detector lives in human/memory/opinion_challenge.h. */

/* --- F66 Life Chapters --- */
#ifndef HU_COGNITIVE_SKIP_LIFE_CHAPTER
typedef struct hu_life_chapter {
    int64_t id;
    char *theme;
    size_t theme_len;
    char *mood;
    size_t mood_len;
    uint64_t started_at;
    uint64_t ended_at;
    char *key_threads;
    size_t key_threads_len;
    bool active;
} hu_life_chapter_t;

hu_error_t hu_chapters_create_table_sql(char *buf, size_t cap, size_t *out_len);
hu_error_t hu_chapters_insert_sql(const hu_life_chapter_t *ch, char *buf, size_t cap,
                                  size_t *out_len);
hu_error_t hu_chapters_query_active_sql(char *buf, size_t cap, size_t *out_len);
hu_error_t hu_chapters_close_sql(int64_t id, uint64_t ended_at, char *buf, size_t cap,
                                 size_t *out_len);
hu_error_t hu_chapters_build_prompt(hu_allocator_t *alloc, const hu_life_chapter_t *chapters,
                                    size_t count, char **out, size_t *out_len);
void hu_chapter_deinit(hu_allocator_t *alloc, hu_life_chapter_t *ch);

#endif /* HU_COGNITIVE_SKIP_LIFE_CHAPTER */

/* --- F67 Social Graph --- */
typedef enum hu_social_rel_type {
    HU_SOCIAL_FAMILY = 0,
    HU_SOCIAL_FRIEND,
    HU_SOCIAL_COWORKER,
    HU_SOCIAL_ACQUAINTANCE,
    HU_SOCIAL_PARTNER
} hu_social_rel_type_t;

typedef struct hu_social_link {
    char *contact_a;
    size_t contact_a_len;
    char *contact_b;
    size_t contact_b_len;
    hu_social_rel_type_t rel_type;
    double closeness;
    char *context;
    size_t context_len;
} hu_social_link_t;

hu_error_t hu_social_graph_create_table_sql(char *buf, size_t cap, size_t *out_len);
hu_error_t hu_social_graph_insert_link_sql(const hu_social_link_t *link, char *buf, size_t cap,
                                           size_t *out_len);
hu_error_t hu_social_graph_query_for_contact_sql(const char *contact_id, size_t len, char *buf,
                                                 size_t cap, size_t *out_len);
bool hu_social_graph_contacts_connected(const hu_social_link_t *links, size_t count, const char *a,
                                        size_t a_len, const char *b, size_t b_len);
const char *hu_social_rel_type_str(hu_social_rel_type_t t);
hu_error_t hu_social_graph_build_prompt(hu_allocator_t *alloc, const hu_social_link_t *links,
                                        size_t count, const char *contact_id, size_t cid_len,
                                        char **out, size_t *out_len);
void hu_social_link_deinit(hu_allocator_t *alloc, hu_social_link_t *link);

#endif /* HU_MEMORY_COGNITIVE_H */
