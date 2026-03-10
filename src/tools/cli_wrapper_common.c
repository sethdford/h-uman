/*
 * Shared CLI wrapper helpers — sanitize and split for gcloud, firebase, etc.
 */
#include "human/tools/cli_wrapper_common.h"
#include <ctype.h>
#include <string.h>

bool hu_cli_sanitize_command(const char *cmd) {
    if (!cmd)
        return true;
    if (strstr(cmd, "$(") || strchr(cmd, '`') || strchr(cmd, '|') || strchr(cmd, ';'))
        return false;
    return true;
}

size_t hu_cli_split_args(char *cmd, const char **argv_out, size_t max_out) {
    size_t argc = 0;
    char *p = cmd;
    while (*p && argc < max_out) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        char *start = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
        if (*p) {
            *p = '\0';
            p++;
        }
        argv_out[argc++] = start;
    }
    return argc;
}
