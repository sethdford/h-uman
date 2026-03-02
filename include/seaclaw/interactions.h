#ifndef SC_INTERACTIONS_H
#define SC_INTERACTIONS_H

#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct sc_choice {
    const char *label;
    const char *value;
    bool is_default;
} sc_choice_t;

typedef struct sc_choice_result {
    size_t selected_index;
    const char *selected_value;
} sc_choice_result_t;

/**
 * Present choices to user, return selection.
 * Prints numbered options; reads user input (number or Enter for default).
 * In SC_IS_TEST mode, always returns the default choice.
 */
sc_error_t sc_choices_prompt(const char *question,
    const sc_choice_t *choices, size_t count,
    sc_choice_result_t *out);

/**
 * Yes/No shorthand. Returns true for yes, false for no.
 * In SC_IS_TEST mode, returns default_yes.
 */
bool sc_choices_confirm(const char *question, bool default_yes);

#endif /* SC_INTERACTIONS_H */
