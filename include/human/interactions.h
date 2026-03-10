#ifndef HU_INTERACTIONS_H
#define HU_INTERACTIONS_H

#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct hu_choice {
    const char *label;
    const char *value;
    bool is_default;
} hu_choice_t;

typedef struct hu_choice_result {
    size_t selected_index;
    const char *selected_value;
} hu_choice_result_t;

/**
 * Present choices to user, return selection.
 * Prints numbered options; reads user input (number or Enter for default).
 * In HU_IS_TEST mode, always returns the default choice.
 */
hu_error_t hu_choices_prompt(const char *question, const hu_choice_t *choices, size_t count,
                             hu_choice_result_t *out);

/**
 * Yes/No shorthand. Returns true for yes, false for no.
 * In HU_IS_TEST mode, returns default_yes.
 */
bool hu_choices_confirm(const char *question, bool default_yes);

#endif /* HU_INTERACTIONS_H */
