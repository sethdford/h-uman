#include "human/agent/session_persist.h"
#include "human/agent.h"
#include "human/core/io_secure.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── Helpers ────────────────────────────────────────────────────────────── */

static const char *role_to_str(hu_role_t role) {
    switch (role) {
    case HU_ROLE_SYSTEM:
        return "system";
    case HU_ROLE_USER:
        return "user";
    case HU_ROLE_ASSISTANT:
        return "assistant";
    case HU_ROLE_TOOL:
        return "tool";
    default:
        return "unknown";
    }
}

static hu_role_t str_to_role(const char *s) {
    if (!s)
        return HU_ROLE_USER;
    if (strcmp(s, "system") == 0)
        return HU_ROLE_SYSTEM;
    if (strcmp(s, "user") == 0)
        return HU_ROLE_USER;
    if (strcmp(s, "assistant") == 0)
        return HU_ROLE_ASSISTANT;
    if (strcmp(s, "tool") == 0)
        return HU_ROLE_TOOL;
    return HU_ROLE_USER;
}

static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 0;
    return mkdir(path, 0700);
}

/* ── ID Generation ──────────────────────────────────────────────────────── */

void hu_session_generate_id(char *buf, size_t buf_size) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    snprintf(buf, buf_size, "session_%04d%02d%02d_%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ── Default Dir ────────────────────────────────────────────────────────── */

char *hu_session_default_dir(hu_allocator_t *alloc) {
    const char *home = getenv("HOME");
    if (!home)
        home = "/tmp";
    return hu_sprintf(alloc, "%s/.human/sessions", home);
}

/* ── Serialize Messages to JSON ─────────────────────────────────────────── */

static hu_error_t serialize_tool_calls(hu_allocator_t *alloc, const hu_tool_call_t *tcs,
                                       size_t count, hu_json_value_t **out) {
    hu_json_value_t *arr = hu_json_array_new(alloc);
    if (!arr)
        return HU_ERR_OUT_OF_MEMORY;

    for (size_t i = 0; i < count; i++) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj) {
            hu_json_free(alloc, arr);
            return HU_ERR_OUT_OF_MEMORY;
        }

        if (tcs[i].id) {
            hu_json_value_t *v = hu_json_string_new(alloc, tcs[i].id, tcs[i].id_len);
            if (!v) {
                hu_json_free(alloc, obj);
                hu_json_free(alloc, arr);
                return HU_ERR_OUT_OF_MEMORY;
            }
            hu_json_object_set(alloc, obj, "id", v);
        }
        if (tcs[i].name) {
            hu_json_value_t *v = hu_json_string_new(alloc, tcs[i].name, tcs[i].name_len);
            if (!v) {
                hu_json_free(alloc, obj);
                hu_json_free(alloc, arr);
                return HU_ERR_OUT_OF_MEMORY;
            }
            hu_json_object_set(alloc, obj, "name", v);
        }
        if (tcs[i].arguments) {
            hu_json_value_t *v = hu_json_string_new(alloc, tcs[i].arguments, tcs[i].arguments_len);
            if (!v) {
                hu_json_free(alloc, obj);
                hu_json_free(alloc, arr);
                return HU_ERR_OUT_OF_MEMORY;
            }
            hu_json_object_set(alloc, obj, "arguments", v);
        }
        hu_json_array_push(alloc, arr, obj);
    }
    *out = arr;
    return HU_OK;
}

static hu_error_t serialize_messages(hu_allocator_t *alloc, const hu_owned_message_t *msgs,
                                     size_t count, hu_json_value_t **out) {
    hu_json_value_t *arr = hu_json_array_new(alloc);
    if (!arr)
        return HU_ERR_OUT_OF_MEMORY;

    for (size_t i = 0; i < count; i++) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj) {
            hu_json_free(alloc, arr);
            return HU_ERR_OUT_OF_MEMORY;
        }

        const char *role_s = role_to_str(msgs[i].role);
        hu_json_value_t *rv = hu_json_string_new(alloc, role_s, strlen(role_s));
        if (!rv) {
            hu_json_free(alloc, obj);
            hu_json_free(alloc, arr);
            return HU_ERR_OUT_OF_MEMORY;
        }
        hu_json_object_set(alloc, obj, "role", rv);

        if (msgs[i].content) {
            hu_json_value_t *cv = hu_json_string_new(alloc, msgs[i].content, msgs[i].content_len);
            if (!cv) {
                hu_json_free(alloc, obj);
                hu_json_free(alloc, arr);
                return HU_ERR_OUT_OF_MEMORY;
            }
            hu_json_object_set(alloc, obj, "content", cv);
        }
        if (msgs[i].name) {
            hu_json_value_t *nv = hu_json_string_new(alloc, msgs[i].name, msgs[i].name_len);
            if (!nv) {
                hu_json_free(alloc, obj);
                hu_json_free(alloc, arr);
                return HU_ERR_OUT_OF_MEMORY;
            }
            hu_json_object_set(alloc, obj, "name", nv);
        }
        if (msgs[i].tool_call_id) {
            hu_json_value_t *tv =
                hu_json_string_new(alloc, msgs[i].tool_call_id, msgs[i].tool_call_id_len);
            if (!tv) {
                hu_json_free(alloc, obj);
                hu_json_free(alloc, arr);
                return HU_ERR_OUT_OF_MEMORY;
            }
            hu_json_object_set(alloc, obj, "tool_call_id", tv);
        }
        if (msgs[i].tool_calls && msgs[i].tool_calls_count > 0) {
            hu_json_value_t *tc_arr = NULL;
            hu_error_t err =
                serialize_tool_calls(alloc, msgs[i].tool_calls, msgs[i].tool_calls_count, &tc_arr);
            if (err != HU_OK) {
                hu_json_free(alloc, obj);
                hu_json_free(alloc, arr);
                return err;
            }
            hu_json_object_set(alloc, obj, "tool_calls", tc_arr);
        }
        hu_json_array_push(alloc, arr, obj);
    }
    *out = arr;
    return HU_OK;
}

/* ── Save ───────────────────────────────────────────────────────────────── */

hu_error_t hu_session_persist_save(hu_allocator_t *alloc, const hu_agent_t *agent,
                                   const char *session_dir, char *session_id_out) {
    if (!alloc || !agent || !session_dir)
        return HU_ERR_INVALID_ARGUMENT;

    if (ensure_dir(session_dir) != 0)
        return HU_ERR_IO;

    char sid[HU_SESSION_ID_MAX];
    if (agent->session_id[0] != '\0') {
        memcpy(sid, agent->session_id, HU_SESSION_ID_MAX);
    } else {
        hu_session_generate_id(sid, sizeof(sid));
    }
    if (session_id_out)
        memcpy(session_id_out, sid, HU_SESSION_ID_MAX);

    /* Build JSON tree */
    hu_json_value_t *root = hu_json_object_new(alloc);
    if (!root)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_object_set(alloc, root, "schema_version",
                       hu_json_number_new(alloc, HU_SESSION_SCHEMA_VERSION));

    /* Metadata */
    hu_json_value_t *meta = hu_json_object_new(alloc);
    if (!meta) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_json_object_set(alloc, meta, "id", hu_json_string_new(alloc, sid, strlen(sid)));
    hu_json_object_set(alloc, meta, "created_at", hu_json_number_new(alloc, (double)time(NULL)));
    if (agent->model_name)
        hu_json_object_set(alloc, meta, "model_name",
                           hu_json_string_new(alloc, agent->model_name, agent->model_name_len));
    if (agent->workspace_dir)
        hu_json_object_set(
            alloc, meta, "workspace_dir",
            hu_json_string_new(alloc, agent->workspace_dir, agent->workspace_dir_len));
    hu_json_object_set(alloc, meta, "message_count",
                       hu_json_number_new(alloc, (double)agent->history_count));
    hu_json_object_set(alloc, root, "metadata", meta);

    /* Messages */
    hu_json_value_t *msgs_json = NULL;
    hu_error_t err = serialize_messages(alloc, agent->history, agent->history_count, &msgs_json);
    if (err != HU_OK) {
        hu_json_free(alloc, root);
        return err;
    }
    hu_json_object_set(alloc, root, "messages", msgs_json);

    /* Stringify */
    char *json_str = NULL;
    size_t json_len = 0;
    err = hu_json_stringify(alloc, root, &json_str, &json_len);
    hu_json_free(alloc, root);
    if (err != HU_OK)
        return err;

    /* Atomic write: temp file → rename */
    char tmp_path[1024], final_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.tmp_%s.json", session_dir, sid);
    snprintf(final_path, sizeof(final_path), "%s/%s.json", session_dir, sid);

    /* Session JSON contains conversation history — not secrets per se,
     * but PII (user names, addresses, anything they typed) that should
     * not be world-readable. Use HU_IO_PERM_SECRET (0600) for the same
     * reasoning we apply to OAuth tokens. */
    FILE *f = NULL;
    if (hu_io_secure_open(tmp_path, HU_IO_PERM_SECRET, "wb", &f) != HU_OK || !f) {
        alloc->free(alloc->ctx, json_str, json_len + 1);
        return HU_ERR_IO;
    }
    size_t written = fwrite(json_str, 1, json_len, f);
    fclose(f);
    alloc->free(alloc->ctx, json_str, json_len + 1);

    if (written != json_len) {
        unlink(tmp_path);
        return HU_ERR_IO;
    }

    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return HU_ERR_IO;
    }

    return HU_OK;
}

/* ── Deserialize Messages from JSON ─────────────────────────────────────── */

static hu_error_t deserialize_tool_calls(hu_allocator_t *alloc, const hu_json_value_t *arr,
                                         hu_tool_call_t **out, size_t *out_count) {
    if (arr->type != HU_JSON_ARRAY)
        return HU_ERR_PARSE;
    size_t n = arr->data.array.len;
    if (n == 0) {
        *out = NULL;
        *out_count = 0;
        return HU_OK;
    }

    hu_tool_call_t *tcs = (hu_tool_call_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_tool_call_t));
    if (!tcs)
        return HU_ERR_OUT_OF_MEMORY;
    memset(tcs, 0, n * sizeof(hu_tool_call_t));

    for (size_t i = 0; i < n; i++) {
        const hu_json_value_t *obj = arr->data.array.items[i];
        if (obj->type != HU_JSON_OBJECT)
            continue;

        const char *s;
        s = hu_json_get_string(obj, "id");
        if (s) {
            tcs[i].id = hu_strdup(alloc, s);
            tcs[i].id_len = tcs[i].id ? strlen(s) : 0;
        }
        s = hu_json_get_string(obj, "name");
        if (s) {
            tcs[i].name = hu_strdup(alloc, s);
            tcs[i].name_len = tcs[i].name ? strlen(s) : 0;
        }
        s = hu_json_get_string(obj, "arguments");
        if (s) {
            tcs[i].arguments = hu_strdup(alloc, s);
            tcs[i].arguments_len = tcs[i].arguments ? strlen(s) : 0;
        }
    }
    *out = tcs;
    *out_count = n;
    return HU_OK;
}

static hu_error_t deserialize_messages(hu_allocator_t *alloc, const hu_json_value_t *arr,
                                       hu_owned_message_t **out, size_t *out_count) {
    if (!arr || arr->type != HU_JSON_ARRAY)
        return HU_ERR_PARSE;

    size_t n = arr->data.array.len;
    if (n == 0) {
        *out = NULL;
        *out_count = 0;
        return HU_OK;
    }

    hu_owned_message_t *msgs =
        (hu_owned_message_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_owned_message_t));
    if (!msgs)
        return HU_ERR_OUT_OF_MEMORY;
    memset(msgs, 0, n * sizeof(hu_owned_message_t));

    for (size_t i = 0; i < n; i++) {
        const hu_json_value_t *obj = arr->data.array.items[i];
        if (obj->type != HU_JSON_OBJECT) {
            continue;
        }

        const char *role_s = hu_json_get_string(obj, "role");
        msgs[i].role = str_to_role(role_s);

        const char *s;
        s = hu_json_get_string(obj, "content");
        if (s) {
            msgs[i].content = hu_strdup(alloc, s);
            msgs[i].content_len = strlen(s);
        }

        s = hu_json_get_string(obj, "name");
        if (s) {
            msgs[i].name = hu_strdup(alloc, s);
            msgs[i].name_len = strlen(s);
        }

        s = hu_json_get_string(obj, "tool_call_id");
        if (s) {
            msgs[i].tool_call_id = hu_strdup(alloc, s);
            msgs[i].tool_call_id_len = strlen(s);
        }

        const hu_json_value_t *tc = hu_json_object_get(obj, "tool_calls");
        if (tc && tc->type == HU_JSON_ARRAY && tc->data.array.len > 0) {
            deserialize_tool_calls(alloc, tc, &msgs[i].tool_calls, &msgs[i].tool_calls_count);
        }
    }
    *out = msgs;
    *out_count = n;
    return HU_OK;
}

/* ── Load ───────────────────────────────────────────────────────────────── */

hu_error_t hu_session_persist_load(hu_allocator_t *alloc, hu_agent_t *agent,
                                   const char *session_dir, const char *session_id) {
    if (!alloc || !agent || !session_dir || !session_id)
        return HU_ERR_INVALID_ARGUMENT;

    /* Validate session_id: no path separators */
    if (strchr(session_id, '/') || strchr(session_id, '\\'))
        return HU_ERR_INVALID_ARGUMENT;

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.json", session_dir, session_id);

    struct stat st;
    if (stat(path, &st) != 0)
        return HU_ERR_NOT_FOUND;
    if ((size_t)st.st_size > HU_SESSION_MAX_FILE_SIZE)
        return HU_ERR_IO;

    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_IO;

    size_t file_size = (size_t)st.st_size;
    char *buf = (char *)alloc->alloc(alloc->ctx, file_size + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t rd = fread(buf, 1, file_size, f);
    fclose(f);
    buf[rd] = '\0';

    /* Parse JSON */
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, buf, rd, &root);
    alloc->free(alloc->ctx, buf, file_size + 1);
    if (err != HU_OK)
        return HU_ERR_PARSE;

    /* Validate schema version */
    double sv = hu_json_get_number(root, "schema_version", 0);
    if ((int)sv != HU_SESSION_SCHEMA_VERSION) {
        hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }

    /* Extract messages */
    const hu_json_value_t *msgs_json = hu_json_object_get(root, "messages");
    if (!msgs_json || msgs_json->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }

    hu_owned_message_t *msgs = NULL;
    size_t msg_count = 0;
    err = deserialize_messages(alloc, msgs_json, &msgs, &msg_count);
    hu_json_free(alloc, root);
    if (err != HU_OK)
        return err;

    /* Clear existing history and replace */
    hu_agent_clear_history(agent);

    /* Ensure capacity */
    if (msg_count > agent->history_cap) {
        hu_owned_message_t *new_hist = (hu_owned_message_t *)alloc->realloc(
            alloc->ctx, agent->history, agent->history_cap * sizeof(hu_owned_message_t),
            msg_count * sizeof(hu_owned_message_t));
        if (!new_hist) {
            alloc->free(alloc->ctx, msgs, msg_count * sizeof(hu_owned_message_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
        agent->history = new_hist;
        agent->history_cap = msg_count;
    }

    memcpy(agent->history, msgs, msg_count * sizeof(hu_owned_message_t));
    agent->history_count = msg_count;
    alloc->free(alloc->ctx, msgs, msg_count * sizeof(hu_owned_message_t));

    /* Copy session_id into agent */
    size_t sid_len = strlen(session_id);
    if (sid_len >= HU_SESSION_ID_MAX)
        sid_len = HU_SESSION_ID_MAX - 1;
    memcpy(agent->session_id, session_id, sid_len);
    agent->session_id[sid_len] = '\0';

    return HU_OK;
}

/* ── List ───────────────────────────────────────────────────────────────── */

hu_error_t hu_session_persist_list(hu_allocator_t *alloc, const char *session_dir,
                                   hu_session_metadata_t **out, size_t *out_count) {
    if (!alloc || !session_dir || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_count = 0;

    DIR *d = opendir(session_dir);
    if (!d)
        return HU_OK; /* no dir = no sessions */

    /* First pass: count .json files */
    size_t cap = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen > 5 && strcmp(ent->d_name + nlen - 5, ".json") == 0)
            cap++;
    }
    if (cap == 0) {
        closedir(d);
        return HU_OK;
    }

    hu_session_metadata_t *arr =
        (hu_session_metadata_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_session_metadata_t));
    if (!arr) {
        closedir(d);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(arr, 0, cap * sizeof(hu_session_metadata_t));

    /* Second pass: read metadata from each file */
    rewinddir(d);
    size_t idx = 0;
    while ((ent = readdir(d)) != NULL && idx < cap) {
        size_t nlen = strlen(ent->d_name);
        if (nlen <= 5 || strcmp(ent->d_name + nlen - 5, ".json") != 0)
            continue;

        char fpath[4096]; /* 4096: session_dir + "/" + dirent name can exceed 1024 (GCC
                             -Werror=format-truncation). */
        snprintf(fpath, sizeof(fpath), "%s/%s", session_dir, ent->d_name);

        /* Extract ID from filename (strip .json) */
        size_t id_len = nlen - 5;
        if (id_len >= HU_SESSION_ID_MAX)
            id_len = HU_SESSION_ID_MAX - 1;
        memcpy(arr[idx].id, ent->d_name, id_len);
        arr[idx].id[id_len] = '\0';

        /* Quick read metadata from file */
        struct stat st;
        if (stat(fpath, &st) != 0)
            continue;
        if ((size_t)st.st_size > HU_SESSION_MAX_FILE_SIZE)
            continue;

        FILE *f = fopen(fpath, "rb");
        if (!f)
            continue;

        size_t file_size = (size_t)st.st_size;
        char *buf = (char *)alloc->alloc(alloc->ctx, file_size + 1);
        if (!buf) {
            fclose(f);
            continue;
        }
        size_t rd = fread(buf, 1, file_size, f);
        fclose(f);
        buf[rd] = '\0';

        hu_json_value_t *root = NULL;
        hu_error_t err = hu_json_parse(alloc, buf, rd, &root);
        alloc->free(alloc->ctx, buf, file_size + 1);
        if (err != HU_OK || !root)
            continue;

        const hu_json_value_t *meta = hu_json_object_get(root, "metadata");
        if (meta && meta->type == HU_JSON_OBJECT) {
            arr[idx].created_at = (time_t)hu_json_get_number(meta, "created_at", 0);
            arr[idx].message_count = (size_t)hu_json_get_number(meta, "message_count", 0);

            const char *mn = hu_json_get_string(meta, "model_name");
            if (mn) {
                arr[idx].model_name = hu_strdup(alloc, mn);
                arr[idx].model_name_len = strlen(mn);
            }
            const char *wd = hu_json_get_string(meta, "workspace_dir");
            if (wd) {
                arr[idx].workspace_dir = hu_strdup(alloc, wd);
                arr[idx].workspace_dir_len = strlen(wd);
            }
        }
        hu_json_free(alloc, root);
        idx++;
    }
    closedir(d);

    *out = arr;
    *out_count = idx;
    return HU_OK;
}

/* ── Delete ─────────────────────────────────────────────────────────────── */

hu_error_t hu_session_persist_delete(const char *session_dir, const char *session_id) {
    if (!session_dir || !session_id)
        return HU_ERR_INVALID_ARGUMENT;
    if (strchr(session_id, '/') || strchr(session_id, '\\'))
        return HU_ERR_INVALID_ARGUMENT;

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.json", session_dir, session_id);

    if (unlink(path) != 0)
        return (errno == ENOENT) ? HU_ERR_NOT_FOUND : HU_ERR_IO;
    return HU_OK;
}

/* ── Metadata Free ──────────────────────────────────────────────────────── */

void hu_session_metadata_free(hu_allocator_t *alloc, hu_session_metadata_t *arr, size_t count) {
    if (!arr)
        return;
    for (size_t i = 0; i < count; i++) {
        if (arr[i].model_name)
            alloc->free(alloc->ctx, arr[i].model_name, arr[i].model_name_len + 1);
        if (arr[i].workspace_dir)
            alloc->free(alloc->ctx, arr[i].workspace_dir, arr[i].workspace_dir_len + 1);
    }
    alloc->free(alloc->ctx, arr, count * sizeof(hu_session_metadata_t));
}
