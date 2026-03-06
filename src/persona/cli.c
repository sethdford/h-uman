#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/persona.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef SC_ENABLE_SQLITE
#include <sqlite3.h>
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
    } else if (strcmp(action, "validate") == 0) {
        out->action = SC_PERSONA_ACTION_VALIDATE;
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
        fprintf(stdout, "Persona deleted: %s\n", args->name);
        return SC_OK;
#else
        fprintf(stderr, "Persona delete requires POSIX (unlink)\n");
        return SC_ERR_NOT_SUPPORTED;
#endif
    }
    case SC_PERSONA_ACTION_VALIDATE: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for validate\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
#if defined(SC_IS_TEST) && SC_IS_TEST
        (void)alloc;
        fprintf(stdout, "Persona '%s' is valid.\n", args->name);
        return SC_OK;
#else
#if defined(__unix__) || defined(__APPLE__)
        const char *home = getenv("HOME");
        if (!home || !home[0]) {
            fprintf(stderr, "HOME not set\n");
            return SC_ERR_NOT_FOUND;
        }
        char path[SC_PERSONA_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/.seaclaw/personas/%s.json", home, args->name);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            fprintf(stderr, "Invalid persona name\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "Persona not found: %s\n", args->name);
            return SC_ERR_NOT_FOUND;
        }
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            return SC_ERR_IO;
        }
        long sz = ftell(f);
        if (sz < 0 || sz > (long)(1024 * 1024)) {
            fclose(f);
            return sz < 0 ? SC_ERR_IO : SC_ERR_INVALID_ARGUMENT;
        }
        rewind(f);
        char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
        if (!buf) {
            fclose(f);
            return SC_ERR_OUT_OF_MEMORY;
        }
        size_t read_len = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        if (read_len != (size_t)sz) {
            alloc->free(alloc->ctx, buf, (size_t)sz + 1);
            return SC_ERR_IO;
        }
        buf[read_len] = '\0';
        char *err_msg = NULL;
        size_t err_len = 0;
        sc_error_t err = sc_persona_validate_json(alloc, buf, read_len, &err_msg, &err_len);
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        if (err != SC_OK) {
            fprintf(stderr, "Persona '%s' is invalid: %s\n", args->name,
                    err_msg ? err_msg : "unknown");
            if (err_msg)
                alloc->free(alloc->ctx, err_msg, err_len + 1);
            return err;
        }
        fprintf(stdout, "Persona '%s' is valid.\n", args->name);
        return SC_OK;
#else
        fprintf(stderr, "Persona validate requires POSIX\n");
        return SC_ERR_NOT_SUPPORTED;
#endif
#endif
    }
    case SC_PERSONA_ACTION_CREATE:
    case SC_PERSONA_ACTION_UPDATE: {
        if (!args->name || !args->name[0]) {
            fprintf(stderr, "Persona name required for create/update\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        if (!args->from_imessage && !args->from_gmail && !args->from_facebook) {
            fprintf(stderr, "No source specified. Use --from-imessage, --from-gmail, or "
                            "--from-facebook.\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        if (args->from_facebook && !args->facebook_export_path) {
            fprintf(stderr, "Facebook export requires a file path. Use: --from-facebook "
                            "/path/to/export.json\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        if (args->from_gmail && !args->gmail_export_path) {
            fprintf(stderr, "Gmail export requires a file path. Use: --from-gmail "
                            "/path/to/export.json\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
#if defined(SC_IS_TEST) && SC_IS_TEST
        (void)alloc;
        return SC_OK;
#else
        if (args->from_imessage) {
#if defined(__APPLE__) && defined(__MACH__) && defined(SC_ENABLE_SQLITE)
            size_t msg_count = 0;
            const char *home = getenv("HOME");
            if (!home || !home[0]) {
                fprintf(stderr, "HOME not set\n");
                return SC_ERR_NOT_FOUND;
            }
            char db_path[SC_PERSONA_PATH_MAX];
            int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
            if (n <= 0 || (size_t)n >= sizeof(db_path))
                return SC_ERR_INVALID_ARGUMENT;
            sqlite3 *db = NULL;
            if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
                if (db)
                    sqlite3_close(db);
                fprintf(stderr, "Could not open iMessage chat.db (Full Disk Access required)\n");
                return SC_ERR_IO;
            }
            char query[512];
            size_t query_len = 0;
            sc_error_t qerr =
                sc_persona_sampler_imessage_query(query, sizeof(query), &query_len, 500);
            if (qerr != SC_OK) {
                sqlite3_close(db);
                return qerr;
            }
            sqlite3_stmt *stmt = NULL;
            if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
                sqlite3_close(db);
                return SC_ERR_IO;
            }
            while (sqlite3_step(stmt) == SQLITE_ROW && msg_count < 500) {
                const char *text = (const char *)sqlite3_column_text(stmt, 0);
                if (text && text[0])
                    msg_count++;
            }
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            fprintf(stdout, "Found %zu messages from iMessage\n", msg_count);
#else
            fprintf(stderr, "iMessage sampling requires macOS and SQLite\n");
            return SC_ERR_NOT_SUPPORTED;
#endif
        }
        if (args->from_facebook && args->facebook_export_path) {
            FILE *f = fopen(args->facebook_export_path, "rb");
            if (!f) {
                fprintf(stderr, "Could not open Facebook export: %s\n", args->facebook_export_path);
                return SC_ERR_IO;
            }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz <= 0 || sz > (long)(10 * 1024 * 1024)) {
                fclose(f);
                fprintf(stderr, "Facebook export file too large or empty\n");
                return SC_ERR_INVALID_ARGUMENT;
            }
            char *json = (char *)malloc((size_t)sz + 1);
            if (!json) {
                fclose(f);
                return SC_ERR_OUT_OF_MEMORY;
            }
            size_t nr = fread(json, 1, (size_t)sz, f);
            fclose(f);
            json[nr] = '\0';

            char **messages = NULL;
            size_t msg_count = 0;
            sc_error_t perr = sc_persona_sampler_facebook_parse(json, nr, &messages, &msg_count);
            free(json);
            if (perr != SC_OK) {
                fprintf(stderr, "Failed to parse Facebook export\n");
                return perr;
            }
            fprintf(stdout, "Found %zu messages from Facebook\n", msg_count);
            if (messages) {
                for (size_t i = 0; i < msg_count; i++)
                    free(messages[i]);
                free(messages);
            }
        }
        if (args->from_gmail && args->gmail_export_path) {
            FILE *f = fopen(args->gmail_export_path, "rb");
            if (!f) {
                fprintf(stderr, "Could not open Gmail export: %s\n", args->gmail_export_path);
                return SC_ERR_IO;
            }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz <= 0 || sz > (long)(10 * 1024 * 1024)) {
                fclose(f);
                fprintf(stderr, "Gmail export file too large or empty\n");
                return SC_ERR_INVALID_ARGUMENT;
            }
            char *json = (char *)malloc((size_t)sz + 1);
            if (!json) {
                fclose(f);
                return SC_ERR_OUT_OF_MEMORY;
            }
            size_t nr = fread(json, 1, (size_t)sz, f);
            fclose(f);
            json[nr] = '\0';

            char **messages = NULL;
            size_t msg_count = 0;
            sc_error_t perr = sc_persona_sampler_gmail_parse(json, nr, &messages, &msg_count);
            free(json);
            if (perr != SC_OK) {
                fprintf(stderr, "Failed to parse Gmail export\n");
                return perr;
            }
            fprintf(stdout, "Found %zu messages from Gmail\n", msg_count);
            if (messages) {
                for (size_t i = 0; i < msg_count; i++)
                    free(messages[i]);
                free(messages);
            }
        }
#endif
        sc_persona_t template = {0};
        template.name = sc_strdup(alloc, args->name);
        if (!template.name)
            return SC_ERR_OUT_OF_MEMORY;
        template.name_len = strlen(args->name);
        sc_error_t err = sc_persona_creator_write(alloc, &template);
        sc_persona_deinit(alloc, &template);
        if (err != SC_OK)
            return err;
        char dir_buf[SC_PERSONA_PATH_MAX];
        const char *dir = persona_dir_path(dir_buf, sizeof(dir_buf));
        if (dir)
            fprintf(stdout, "Persona template created at %s/%s.json\n", dir, args->name);
        else
            fprintf(stdout, "Persona template created at ~/.seaclaw/personas/%s.json\n",
                    args->name);
        if (args->interactive)
            fprintf(stdout, "Edit the persona file and run 'seaclaw persona update' when ready.\n");
        return SC_OK;
#endif
    }
    }
    return SC_ERR_INVALID_ARGUMENT;
}
