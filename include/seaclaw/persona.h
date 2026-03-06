#ifndef SC_PERSONA_H
#define SC_PERSONA_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct sc_persona_overlay {
    char *channel;
    char *formality;
    char *avg_length;
    char *emoji_usage;
    char **style_notes;
    size_t style_notes_count;
} sc_persona_overlay_t;

typedef struct sc_persona_example {
    char *context;
    char *incoming;
    char *response;
} sc_persona_example_t;

typedef struct sc_persona_example_bank {
    char *channel;
    sc_persona_example_t *examples;
    size_t examples_count;
} sc_persona_example_bank_t;

typedef struct sc_persona {
    char *name;
    size_t name_len;
    char *identity;
    char **traits;
    size_t traits_count;
    char **preferred_vocab;
    size_t preferred_vocab_count;
    char **avoided_vocab;
    size_t avoided_vocab_count;
    char **slang;
    size_t slang_count;
    char **communication_rules;
    size_t communication_rules_count;
    char **values;
    size_t values_count;
    char *decision_style;
    sc_persona_overlay_t *overlays;
    size_t overlays_count;
    sc_persona_example_bank_t *example_banks;
    size_t example_banks_count;
} sc_persona_t;

sc_error_t sc_persona_load(sc_allocator_t *alloc, const char *name, size_t name_len,
                           sc_persona_t *out);

sc_error_t sc_persona_load_json(sc_allocator_t *alloc, const char *json, size_t json_len,
                                sc_persona_t *out);

sc_error_t sc_persona_examples_load_json(sc_allocator_t *alloc, const char *channel,
                                         size_t channel_len, const char *json, size_t json_len,
                                         sc_persona_example_bank_t *out);

void sc_persona_deinit(sc_allocator_t *alloc, sc_persona_t *persona);

sc_error_t sc_persona_build_prompt(sc_allocator_t *alloc, const sc_persona_t *persona,
                                   const char *channel, size_t channel_len, char **out,
                                   size_t *out_len);

sc_error_t sc_persona_select_examples(const sc_persona_t *persona, const char *channel,
                                      size_t channel_len, const char *topic, size_t topic_len,
                                      const sc_persona_example_t **out, size_t *out_count,
                                      size_t max_examples);

const sc_persona_overlay_t *sc_persona_find_overlay(const sc_persona_t *persona,
                                                    const char *channel, size_t channel_len);

#endif /* SC_PERSONA_H */
