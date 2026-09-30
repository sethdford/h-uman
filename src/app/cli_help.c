/* CLI help routing — see include/human/cli_help.h for the contract. */
#include "human/cli_help.h"

#include <string.h>

bool hu_cli_help_requested(int argc, char *const *argv) {
    if (!argv)
        return false;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!a)
            continue;
        if (strcmp(a, "--") == 0)
            return false;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0)
            return true;
    }
    return false;
}

void hu_cli_print_summary(FILE *out, const char *name, const char *description) {
    if (!out || !name)
        return;
    fprintf(out, "Usage: human %s [options]\n\n", name);
    if (description && description[0])
        fprintf(out, "%s\n", description);
}
