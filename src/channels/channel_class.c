#include "human/channel_class.h"
#include <ctype.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    const char *name;
    hu_channel_class_t cls;
} hu_channel_class_entry_t;

static const hu_channel_class_entry_t k_channel_class_table[] = {
    {"imessage", HU_CHANNEL_CLASS_TEXT_FAST},    {"sms", HU_CHANNEL_CLASS_TEXT_FAST},
    {"slack", HU_CHANNEL_CLASS_TEXT_ASYNC},      {"discord", HU_CHANNEL_CLASS_TEXT_ASYNC},
    {"telegram", HU_CHANNEL_CLASS_TEXT_ASYNC},   {"teams", HU_CHANNEL_CLASS_TEXT_ASYNC},
    {"whatsapp", HU_CHANNEL_CLASS_TEXT_ASYNC},   {"signal", HU_CHANNEL_CLASS_TEXT_ASYNC},
    {"matrix", HU_CHANNEL_CLASS_TEXT_ASYNC},     {"line", HU_CHANNEL_CLASS_TEXT_ASYNC},
    {"mattermost", HU_CHANNEL_CLASS_TEXT_ASYNC}, {"voice", HU_CHANNEL_CLASS_VOICE},
};

#define K_TABLE_LEN (sizeof(k_channel_class_table) / sizeof(k_channel_class_table[0]))
#define K_MAX_NAME  63

hu_channel_class_t hu_channel_class_for_name(const char *name) {
    if (name == NULL) {
        return HU_CHANNEL_CLASS_UNKNOWN;
    }

    /* Lowercase the input into a stack buffer; reject overlong names. */
    char lower[K_MAX_NAME + 1];
    size_t i = 0;
    while (name[i] != '\0') {
        if (i >= K_MAX_NAME) {
            return HU_CHANNEL_CLASS_UNKNOWN;
        }
        lower[i] = (char)tolower((unsigned char)name[i]);
        i++;
    }
    lower[i] = '\0';

    if (i == 0) {
        return HU_CHANNEL_CLASS_UNKNOWN;
    }

    for (size_t j = 0; j < K_TABLE_LEN; j++) {
        if (strcmp(lower, k_channel_class_table[j].name) == 0) {
            return k_channel_class_table[j].cls;
        }
    }

    return HU_CHANNEL_CLASS_UNKNOWN;
}
