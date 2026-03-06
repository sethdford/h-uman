#include "seaclaw/core/error.h"
#include "seaclaw/persona.h"
#include <string.h>

sc_error_t sc_persona_cli_parse(int argc, const char **argv, sc_persona_cli_args_t *out) {
    if (!argv || !out)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (argc < 3)
        return SC_ERR_INVALID_ARGUMENT;
    if (strcmp(argv[1], "persona") != 0)
        return SC_ERR_INVALID_ARGUMENT;

    const char *action = argv[2];
    if (strcmp(action, "create") == 0) {
        out->action = SC_PERSONA_ACTION_CREATE;
        if (argc < 4)
            return SC_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--from-imessage") == 0)
                out->from_imessage = true;
            else if (strcmp(argv[i], "--from-gmail") == 0)
                out->from_gmail = true;
            else if (strcmp(argv[i], "--from-facebook") == 0)
                out->from_facebook = true;
            else if (strcmp(argv[i], "--interactive") == 0)
                out->interactive = true;
        }
    } else if (strcmp(action, "update") == 0) {
        out->action = SC_PERSONA_ACTION_UPDATE;
        if (argc < 4)
            return SC_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--from-imessage") == 0)
                out->from_imessage = true;
            else if (strcmp(argv[i], "--from-gmail") == 0)
                out->from_gmail = true;
            else if (strcmp(argv[i], "--from-facebook") == 0)
                out->from_facebook = true;
            else if (strcmp(argv[i], "--interactive") == 0)
                out->interactive = true;
        }
    } else if (strcmp(action, "show") == 0) {
        out->action = SC_PERSONA_ACTION_SHOW;
        if (argc < 4)
            return SC_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
    } else if (strcmp(action, "list") == 0) {
        out->action = SC_PERSONA_ACTION_LIST;
    } else if (strcmp(action, "delete") == 0) {
        out->action = SC_PERSONA_ACTION_DELETE;
        if (argc < 4)
            return SC_ERR_INVALID_ARGUMENT;
        out->name = argv[3];
    } else {
        return SC_ERR_INVALID_ARGUMENT;
    }
    return SC_OK;
}

sc_error_t sc_persona_cli_run(sc_allocator_t *alloc, const sc_persona_cli_args_t *args) {
    (void)alloc;
    (void)args;
    return SC_OK;
}
