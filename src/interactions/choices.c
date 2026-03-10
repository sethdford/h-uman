#include "human/interactions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_CHOICE_BUF_SIZE 64

#ifdef HU_IS_TEST
hu_error_t hu_choices_prompt(const char *question, const hu_choice_t *choices, size_t count,
                             hu_choice_result_t *out) {
    (void)question;
    if (!choices || !out || count == 0)
        return HU_ERR_INVALID_ARGUMENT;
    size_t default_idx = 0;
    for (size_t i = 0; i < count; i++) {
        if (choices[i].is_default) {
            default_idx = i;
            break;
        }
    }
    out->selected_index = default_idx;
    out->selected_value = choices[default_idx].value;
    return HU_OK;
}

bool hu_choices_confirm(const char *question, bool default_yes) {
    (void)question;
    return default_yes;
}
#else
hu_error_t hu_choices_prompt(const char *question, const hu_choice_t *choices, size_t count,
                             hu_choice_result_t *out) {
    if (!question || !choices || !out || count == 0)
        return HU_ERR_INVALID_ARGUMENT;

    size_t default_idx = 0;
    for (size_t i = 0; i < count; i++) {
        if (choices[i].is_default) {
            default_idx = i;
            break;
        }
    }

    printf("%s\n", question);
    for (size_t i = 0; i < count; i++) {
        if (choices[i].is_default)
            printf("  [%zu] %s (default)\n", i + 1, choices[i].label);
        else
            printf("  [%zu] %s\n", i + 1, choices[i].label);
    }
    printf("Choice [%zu]: ", default_idx + 1);
    fflush(stdout);

    char buf[HU_CHOICE_BUF_SIZE];
    if (!fgets(buf, (int)sizeof(buf), stdin)) {
        out->selected_index = default_idx;
        out->selected_value = choices[default_idx].value;
        return HU_OK;
    }

    /* Trim trailing newline */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    /* Empty input = default */
    if (len == 0) {
        out->selected_index = default_idx;
        out->selected_value = choices[default_idx].value;
        return HU_OK;
    }

    char *end;
    unsigned long n = strtoul(buf, &end, 10);
    if (end == buf || n < 1 || n > count) {
        out->selected_index = default_idx;
        out->selected_value = choices[default_idx].value;
        return HU_OK;
    }
    size_t idx = (size_t)(n - 1);
    out->selected_index = idx;
    out->selected_value = choices[idx].value;
    return HU_OK;
}

bool hu_choices_confirm(const char *question, bool default_yes) {
    printf("%s [%s]: ", question, default_yes ? "Y/n" : "y/N");
    fflush(stdout);

    char buf[HU_CHOICE_BUF_SIZE];
    if (!fgets(buf, (int)sizeof(buf), stdin))
        return default_yes;

    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    if (len == 0)
        return default_yes;

    if (buf[0] == 'y' || buf[0] == 'Y')
        return true;
    if (buf[0] == 'n' || buf[0] == 'N')
        return false;
    return default_yes;
}
#endif
