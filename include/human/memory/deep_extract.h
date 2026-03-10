#ifndef HU_DEEP_EXTRACT_H
#define HU_DEEP_EXTRACT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

#define HU_DE_MAX_FACTS     20
#define HU_DE_MAX_RELATIONS 10

typedef struct hu_extracted_fact {
    char *subject;
    char *predicate;
    char *object;
    double confidence;
} hu_extracted_fact_t;

typedef struct hu_extracted_relation {
    char *entity_a;
    char *relation;
    char *entity_b;
    double confidence;
} hu_extracted_relation_t;

typedef struct hu_deep_extract_result {
    hu_extracted_fact_t facts[HU_DE_MAX_FACTS];
    size_t fact_count;
    hu_extracted_relation_t relations[HU_DE_MAX_RELATIONS];
    size_t relation_count;
    char *summary;
    size_t summary_len;
} hu_deep_extract_result_t;

hu_error_t hu_deep_extract_build_prompt(hu_allocator_t *alloc, const char *conversation,
                                        size_t conversation_len, char **out, size_t *out_len);
hu_error_t hu_deep_extract_parse(hu_allocator_t *alloc, const char *response, size_t response_len,
                                 hu_deep_extract_result_t *out);
void hu_deep_extract_result_deinit(hu_deep_extract_result_t *result, hu_allocator_t *alloc);

hu_error_t hu_deep_extract_lightweight(hu_allocator_t *alloc, const char *text, size_t text_len,
                                       hu_deep_extract_result_t *out);

#endif
