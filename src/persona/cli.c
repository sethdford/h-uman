#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/persona.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <unistd.h>
#endif

#define SC_PERSONA_PATH_MAX 512

static const char *persona_dir_path(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = ".";
    int n = snprintf(buf, cap, "%s/.seaclaw/personas", home);
    return (n > 0 && (size_t)n < cap) ? buf : NULL;
}

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
    if (!alloc || !args)
        return SC_ERR_INVALID_ARGUMENT;

    switch (args->action) {
    case SC_PERSONA_ACTION_SHOW: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for show\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        sc_persona_t p = {0};
        sc_error_t err = sc_persona_load(alloc, args->name, strlen(args->name), &p);
        if (err != SC_OK) {
            fprintf(stderr, "Persona not found: %s\n", args->name);
            return err;
        }
        char *prompt = NULL;
        size_t prompt_len = 0;
        err = sc_persona_build_prompt(alloc, &p, NULL, 0, &prompt, &prompt_len);
        if (err == SC_OK && prompt) {
            fprintf(stdout, "%s", prompt);
            alloc->free(alloc->ctx, prompt, prompt_len + 1);
        }
        sc_persona_deinit(alloc, &p);
        return err;
    }
    case SC_PERSONA_ACTION_LIST: {
#if defined(__unix__) || defined(__APPLE__)
        char dir_buf[SC_PERSONA_PATH_MAX];
        const char *dir = persona_dir_path(dir_buf, sizeof(dir_buf));
        if (!dir) {
            fprintf(stderr, "Could not resolve persona directory\n");
            return SC_ERR_NOT_FOUND;
        }
        DIR *d = opendir(dir);
        if (!d) {
            return SC_OK;
        }
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '\0' || e->d_name[0] == '.')
                continue;
            size_t len = strlen(e->d_name);
            if (len < 6 || strcmp(e->d_name + len - 5, ".json") != 0)
                continue;
            char name[256];
            size_t name_len = len - 5;
            if (name_len >= sizeof(name))
                continue;
            memcpy(name, e->d_name, name_len);
            name[name_len] = '\0';
            fprintf(stdout, "%s\n", name);
        }
        closedir(d);
        return SC_OK;
#else
        fprintf(stderr, "Persona list requires POSIX (opendir/readdir)\n");
        return SC_ERR_NOT_SUPPORTED;
#endif
    }
    case SC_PERSONA_ACTION_DELETE: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for delete\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
#if defined(__unix__) || defined(__APPLE__)
        char path[SC_PERSONA_PATH_MAX];
        const char *dir = persona_dir_path(path, sizeof(path));
        if (!dir) {
            fprintf(stderr, "Could not resolve persona directory\n");
            return SC_ERR_NOT_FOUND;
        }
        int n = snprintf(path, sizeof(path), "%s/%s.json", dir, args->name);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            fprintf(stderr, "Invalid persona name\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        if (unlink(path) != 0) {
            fprintf(stderr, "Persona not found: %s\n", args->name);
            return SC_ERR_NOT_FOUND;
        }
        return SC_OK;
#else
        fprintf(stderr, "Persona delete requires POSIX (unlink)\n");
        return SC_ERR_NOT_SUPPORTED;
#endif
    }
    case SC_PERSONA_ACTION_CREATE:
    case SC_PERSONA_ACTION_UPDATE: {
        char sources[128];
        int off = 0;
        if (args->from_imessage)
            off += snprintf(sources + off, sizeof(sources) - (size_t)off, "%simessage",
                            off ? ", " : "");
        if (args->from_gmail)
            off +=
                snprintf(sources + off, sizeof(sources) - (size_t)off, "%sgmail", off ? ", " : "");
        if (args->from_facebook)
            off += snprintf(sources + off, sizeof(sources) - (size_t)off, "%sfacebook",
                            off ? ", " : "");
        if (off == 0)
            off = snprintf(sources, sizeof(sources), "none");
        fprintf(stdout, "%s persona '%s' from: %s\n",
                args->action == SC_PERSONA_ACTION_CREATE ? "Creating" : "Updating",
                args->name ? args->name : "?", sources);
#if defined(SC_IS_TEST) && SC_IS_TEST
        return SC_OK;
#else
        fprintf(stderr, "Persona creation requires an active provider connection. Use the persona "
                        "tool in an agent session instead.\n");
        return SC_ERR_NOT_SUPPORTED;
#endif
    }
    }
    return SC_ERR_INVALID_ARGUMENT;
}
