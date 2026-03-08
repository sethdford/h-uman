#ifndef SC_DEEP_EXTRACT_H
#define SC_DEEP_EXTRACT_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

#define SC_DE_MAX_FACTS     20
#define SC_DE_MAX_RELATIONS 10

typedef struct sc_extracted_fact {
    char *subject;
    char *predicate;
    char *object;
    double confidence;
} sc_extracted_fact_t;

typedef struct sc_extracted_relation {
    char *entity_a;
    char *relation;
    char *entity_b;
    double confidence;
} sc_extracted_relation_t;

typedef struct sc_deep_extract_result {
    sc_extracted_fact_t facts[SC_DE_MAX_FACTS];
    size_t fact_count;
    sc_extracted_relation_t relations[SC_DE_MAX_RELATIONS];
    size_t relation_count;
    char *summary;
    size_t summary_len;
} sc_deep_extract_result_t;

sc_error_t sc_deep_extract_build_prompt(sc_allocator_t *alloc, const char *conversation,
                                        size_t conversation_len, char **out, size_t *out_len);
sc_error_t sc_deep_extract_parse(sc_allocator_t *alloc, const char *response, size_t response_len,
                                 sc_deep_extract_result_t *out);
void sc_deep_extract_result_deinit(sc_deep_extract_result_t *result, sc_allocator_t *alloc);

sc_error_t sc_deep_extract_lightweight(sc_allocator_t *alloc, const char *text, size_t text_len,
                                       sc_deep_extract_result_t *out);

#endif
