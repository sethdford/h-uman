#include "human/agent/approval_gate.h"
#include "human/core/string.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── Gate Manager ────────────────────────────────────────────────────────── */

struct hu_gate_manager {
    char gates_dir[1024]; /* directory for persisting gate JSON files */
};

hu_error_t hu_gate_manager_create(hu_allocator_t *alloc, const char *gates_dir,
                                  hu_gate_manager_t **out) {
    if (!alloc || !gates_dir || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_gate_manager_t *mgr = (hu_gate_manager_t *)alloc->alloc(alloc->ctx, sizeof(*mgr));
    if (!mgr)
        return HU_ERR_OUT_OF_MEMORY;

    memset(mgr, 0, sizeof(*mgr));
    size_t dir_len = strlen(gates_dir);
    if (dir_len >= sizeof(mgr->gates_dir)) {
        alloc->free(alloc->ctx, mgr, sizeof(*mgr));
        return HU_ERR_INVALID_ARGUMENT;
    }

    memcpy(mgr->gates_dir, gates_dir, dir_len);
    mgr->gates_dir[dir_len] = '\0';

    /* Ensure directory exists */
#ifdef HU_IS_TEST
    /* In test mode, use a different directory per test run to avoid conflicts.
     * The "%.980s" precision cap bounds the directory component so GCC's
     * -Wformat-truncation pass can prove the result fits test_dir[1024]
     * (980 + ".test." + up to 20 digits of pid < 1024). It also closes a
     * latent overflow: the subsequent memcpy back into gates_dir[1024] can
     * never exceed the buffer because test_dir is now provably bounded. */
    char test_dir[1024];
    snprintf(test_dir, sizeof(test_dir), "%.980s.test.%lu", mgr->gates_dir,
             (unsigned long)getpid());
    size_t test_dir_len = strlen(test_dir);
    memcpy(mgr->gates_dir, test_dir, test_dir_len);
    mgr->gates_dir[test_dir_len] = '\0';
#endif

    mkdir(mgr->gates_dir, 0755);

    *out = mgr;
    return HU_OK;
}

const char *hu_gate_manager_dir(hu_gate_manager_t *mgr) {
    if (!mgr)
        return NULL;
    return mgr->gates_dir;
}

void hu_gate_manager_destroy(hu_gate_manager_t *mgr, hu_allocator_t *alloc) {
    if (!mgr || !alloc)
        return;
    alloc->free(alloc->ctx, mgr, sizeof(*mgr));
}

/* ── File I/O ────────────────────────────────────────────────────────────── */

/* Generate filename for a gate ID. Caller must free. */
static char *gate_filename(hu_allocator_t *alloc, const char *gates_dir, const char *gate_id) {
    size_t dir_len = strlen(gates_dir);
    size_t id_len = strlen(gate_id);
    size_t total = dir_len + 1 + id_len + 5; /* /gate_id.json */
    char *path = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!path)
        return NULL;
    snprintf(path, total + 1, "%s/%s.json", gates_dir, gate_id);
    return path;
}

/* Simple JSON encoding for gate. Returns owned string; caller must free. */
static char *gate_to_json(hu_allocator_t *alloc, const hu_approval_gate_t *gate) {
    if (!gate)
        return NULL;

    /* Estimate size: rough bounds for JSON */
    size_t est = 512 + gate->description_len + gate->context_json_len + gate->response_len;
    char *buf = (char *)alloc->alloc(alloc->ctx, est);
    if (!buf)
        return NULL;

    int written = snprintf(
        buf, est,
        "{\"gate_id\":\"%s\",\"description\":\"%s\",\"context_json\":%s,\"status\":%d,"
        "\"created_at\":%ld,\"timeout_at\":%ld,\"resolved_at\":%ld,\"response\":\"%s\"}",
        gate->gate_id, gate->description ? gate->description : "",
        gate->context_json ? gate->context_json : "null", (int)gate->status, (long)gate->created_at,
        (long)gate->timeout_at, (long)gate->resolved_at, gate->response ? gate->response : "");

    if (written < 0 || (size_t)written >= est) {
        alloc->free(alloc->ctx, buf, est);
        return NULL;
    }

    return buf;
}

/* Load gate from JSON file. Returns HU_OK if found. */
static hu_error_t load_gate_from_file(hu_allocator_t *alloc, const char *path,
                                      hu_approval_gate_t *out) {
    if (!path || !out)
        return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "r");
    if (!f)
        return HU_ERR_NOT_FOUND;

    /* Read entire file into buffer */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 65536) {
        fclose(f);
        return HU_ERR_INVALID_ARGUMENT;
    }

    char *json = (char *)alloc->alloc(alloc->ctx, (size_t)fsize + 1);
    if (!json) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t read = fread(json, 1, (size_t)fsize, f);
    fclose(f);

    if (read != (size_t)fsize) {
        alloc->free(alloc->ctx, json, (size_t)fsize + 1);
        return HU_ERR_IO;
    }

    json[fsize] = '\0';

    /* Very basic JSON parsing (no external library) */
    memset(out, 0, sizeof(*out));

    /* Extract gate_id */
    const char *gate_id_str = strstr(json, "\"gate_id\":\"");
    if (gate_id_str) {
        const char *start = gate_id_str + 11;
        const char *end = strchr(start, '"');
        if (end && (size_t)(end - start) < sizeof(out->gate_id)) {
            memcpy(out->gate_id, start, (size_t)(end - start));
            out->gate_id[(size_t)(end - start)] = '\0';
        }
    }

    /* Extract description */
    const char *desc_str = strstr(json, "\"description\":\"");
    if (desc_str) {
        const char *start = desc_str + 15;
        const char *end = strchr(start, '"');
        if (end) {
            size_t desc_len = (size_t)(end - start);
            out->description = (char *)alloc->alloc(alloc->ctx, desc_len + 1);
            if (out->description) {
                memcpy(out->description, start, desc_len);
                out->description[desc_len] = '\0';
                out->description_len = desc_len;
            }
        }
    }

    /* Extract status */
    const char *status_str = strstr(json, "\"status\":");
    if (status_str) {
        int status_val = 0;
        sscanf(status_str + 9, "%d", &status_val);
        out->status = (hu_gate_status_t)status_val;
    }

    /* Extract created_at */
    const char *created_str = strstr(json, "\"created_at\":");
    if (created_str) {
        sscanf(created_str + 13, "%ld", (long *)&out->created_at);
    }

    /* Extract timeout_at */
    const char *timeout_str = strstr(json, "\"timeout_at\":");
    if (timeout_str) {
        sscanf(timeout_str + 13, "%ld", (long *)&out->timeout_at);
    }

    /* Extract resolved_at */
    const char *resolved_str = strstr(json, "\"resolved_at\":");
    if (resolved_str) {
        sscanf(resolved_str + 14, "%ld", (long *)&out->resolved_at);
    }

    /* Extract response */
    const char *response_str = strstr(json, "\"response\":\"");
    if (response_str) {
        const char *start = response_str + 12;
        const char *end = strchr(start, '"');
        if (end) {
            size_t resp_len = (size_t)(end - start);
            out->response = (char *)alloc->alloc(alloc->ctx, resp_len + 1);
            if (out->response) {
                memcpy(out->response, start, resp_len);
                out->response[resp_len] = '\0';
                out->response_len = resp_len;
            }
        }
    }

    /* Extract context_json (simplified — assume it's plain JSON object) */
    const char *ctx_str = strstr(json, "\"context_json\":");
    if (ctx_str) {
        const char *start = ctx_str + 15;
        if (*start == '{') {
            /* Find matching closing brace (naive; works for simple objects) */
            int depth = 0;
            const char *p = start;
            while (*p) {
                if (*p == '{')
                    depth++;
                else if (*p == '}') {
                    depth--;
                    if (depth == 0) {
                        size_t ctx_len = (size_t)(p - start + 1);
                        out->context_json = (char *)alloc->alloc(alloc->ctx, ctx_len + 1);
                        if (out->context_json) {
                            memcpy(out->context_json, start, ctx_len);
                            out->context_json[ctx_len] = '\0';
                            out->context_json_len = ctx_len;
                        }
                        break;
                    }
                }
                p++;
            }
        }
    }

    alloc->free(alloc->ctx, json, (size_t)fsize + 1);
    return HU_OK;
}

/* Atomically write gate to file (temp + rename). */
static hu_error_t save_gate_to_file(hu_allocator_t *alloc, const char *path,
                                    const hu_approval_gate_t *gate) {
    if (!path || !gate)
        return HU_ERR_INVALID_ARGUMENT;

    char *json = gate_to_json(alloc, gate);
    if (!json)
        return HU_ERR_OUT_OF_MEMORY;

    /* Write to temp file first */
    char temp_path[1024];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    FILE *f = fopen(temp_path, "w");
    if (!f) {
        alloc->free(alloc->ctx, json, strlen(json) + 1);
        return HU_ERR_IO;
    }

    size_t json_len = strlen(json);
    size_t written = fwrite(json, 1, json_len, f);
    fclose(f);

    alloc->free(alloc->ctx, json, json_len + 1);

    if (written != json_len) {
        unlink(temp_path);
        return HU_ERR_IO;
    }

    /* Atomic rename */
    if (rename(temp_path, path) != 0) {
        unlink(temp_path);
        return HU_ERR_IO;
    }

    return HU_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

hu_error_t hu_gate_create(hu_gate_manager_t *mgr, hu_allocator_t *alloc, const char *description,
                          size_t description_len, const char *context_json, size_t context_json_len,
                          int64_t timeout_sec, char *gate_id_out) {
    if (!mgr || !alloc || !description || !gate_id_out)
        return HU_ERR_INVALID_ARGUMENT;

    /* Generate unique gate ID */
    static uint64_t gate_counter = 0;
    int64_t now = (int64_t)time(NULL);
    snprintf(gate_id_out, 64, "gate-%ld-%lu", (long)now, (unsigned long)(gate_counter++));

    /* Create gate struct */
    hu_approval_gate_t gate;
    memset(&gate, 0, sizeof(gate));
    memcpy(gate.gate_id, gate_id_out, strlen(gate_id_out));
    gate.status = HU_GATE_PENDING;
    gate.created_at = now;
    gate.timeout_at = (timeout_sec > 0) ? (now + timeout_sec) : 0;
    gate.resolved_at = 0;

    /* Copy description */
    gate.description = (char *)alloc->alloc(alloc->ctx, description_len + 1);
    if (!gate.description)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(gate.description, description, description_len);
    gate.description[description_len] = '\0';
    gate.description_len = description_len;

    /* Copy context_json */
    if (context_json && context_json_len > 0) {
        gate.context_json = (char *)alloc->alloc(alloc->ctx, context_json_len + 1);
        if (!gate.context_json) {
            alloc->free(alloc->ctx, gate.description, description_len + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(gate.context_json, context_json, context_json_len);
        gate.context_json[context_json_len] = '\0';
        gate.context_json_len = context_json_len;
    }

    /* Persist to file */
    char *path = gate_filename(alloc, mgr->gates_dir, gate_id_out);
    if (!path) {
        hu_gate_free(alloc, &gate);
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_error_t err = save_gate_to_file(alloc, path, &gate);
    hu_gate_free(alloc, &gate);
    alloc->free(alloc->ctx, path, strlen(path) + 1);

    return err;
}

/* Allocator wrapper functions */
static void *gate_alloc_wrapper(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void gate_free_wrapper(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}

hu_error_t hu_gate_check(hu_gate_manager_t *mgr, const char *gate_id,
                         hu_gate_status_t *out_status) {
    if (!mgr || !gate_id || !out_status)
        return HU_ERR_INVALID_ARGUMENT;

    hu_approval_gate_t gate;
    memset(&gate, 0, sizeof(gate));

    /* Create allocator for this operation */
    hu_allocator_t tmp_alloc;
    tmp_alloc.ctx = NULL;
    tmp_alloc.alloc = gate_alloc_wrapper;
    tmp_alloc.free = gate_free_wrapper;
    tmp_alloc.realloc = NULL;

    char *path = gate_filename(&tmp_alloc, mgr->gates_dir, gate_id);
    if (!path)
        return HU_ERR_OUT_OF_MEMORY;

    hu_error_t err = load_gate_from_file(&tmp_alloc, path, &gate);
    if (err == HU_OK) {
        /* Check timeout */
        if (gate.timeout_at > 0 && (int64_t)time(NULL) > gate.timeout_at &&
            gate.status == HU_GATE_PENDING) {
            gate.status = HU_GATE_TIMED_OUT;
            gate.resolved_at = (int64_t)time(NULL);
            save_gate_to_file(&tmp_alloc, path, &gate);
        }
        *out_status = gate.status;
        hu_gate_free(&tmp_alloc, &gate);
    }

    tmp_alloc.free(&tmp_alloc.ctx, path, strlen(path) + 1);
    return err;
}

hu_error_t hu_gate_load(hu_gate_manager_t *mgr, hu_allocator_t *alloc, const char *gate_id,
                        hu_approval_gate_t *out) {
    if (!mgr || !alloc || !gate_id || !out)
        return HU_ERR_INVALID_ARGUMENT;

    char *path = gate_filename(alloc, mgr->gates_dir, gate_id);
    if (!path)
        return HU_ERR_OUT_OF_MEMORY;

    hu_error_t err = load_gate_from_file(alloc, path, out);
    alloc->free(alloc->ctx, path, strlen(path) + 1);

    if (err == HU_OK) {
        /* Check and update timeout status */
        if (out->timeout_at > 0 && (int64_t)time(NULL) > out->timeout_at &&
            out->status == HU_GATE_PENDING) {
            out->status = HU_GATE_TIMED_OUT;
            out->resolved_at = (int64_t)time(NULL);

            /* Re-persist with updated status */
            char *path2 = gate_filename(alloc, mgr->gates_dir, gate_id);
            if (path2) {
                save_gate_to_file(alloc, path2, out);
                alloc->free(alloc->ctx, path2, strlen(path2) + 1);
            }
        }
    }

    return err;
}

hu_error_t hu_gate_resolve(hu_gate_manager_t *mgr, hu_allocator_t *alloc, const char *gate_id,
                           hu_gate_status_t decision, const char *response, size_t response_len) {
    if (!mgr || !alloc || !gate_id ||
        (decision != HU_GATE_APPROVED && decision != HU_GATE_REJECTED))
        return HU_ERR_INVALID_ARGUMENT;

    /* Load existing gate */
    hu_approval_gate_t gate;
    hu_error_t err = hu_gate_load(mgr, alloc, gate_id, &gate);
    if (err != HU_OK)
        return err;

    /* Update status */
    gate.status = decision;
    gate.resolved_at = (int64_t)time(NULL);

    /* Set response */
    if (gate.response) {
        alloc->free(alloc->ctx, gate.response, gate.response_len + 1);
        gate.response = NULL;
        gate.response_len = 0;
    }

    if (response && response_len > 0) {
        gate.response = (char *)alloc->alloc(alloc->ctx, response_len + 1);
        if (gate.response) {
            memcpy(gate.response, response, response_len);
            gate.response[response_len] = '\0';
            gate.response_len = response_len;
        }
    }

    /* Persist */
    char *path = gate_filename(alloc, mgr->gates_dir, gate_id);
    if (!path) {
        hu_gate_free(alloc, &gate);
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_error_t save_err = save_gate_to_file(alloc, path, &gate);
    hu_gate_free(alloc, &gate);
    alloc->free(alloc->ctx, path, strlen(path) + 1);

    return save_err;
}

hu_error_t hu_gate_list_pending(hu_gate_manager_t *mgr, hu_allocator_t *alloc,
                                hu_approval_gate_t **out, size_t *out_count) {
    if (!mgr || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_count = 0;

    /* Scan directory for .json files */
    DIR *dir = opendir(mgr->gates_dir);
    if (!dir)
        return HU_ERR_IO;

    size_t cap = 32;
    hu_approval_gate_t *gates =
        (hu_approval_gate_t *)alloc->alloc(alloc->ctx, cap * sizeof(*gates));
    if (!gates) {
        closedir(dir);
        return HU_ERR_OUT_OF_MEMORY;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strlen(entry->d_name) < 6 ||
            strcmp(entry->d_name + strlen(entry->d_name) - 5, ".json") != 0)
            continue;

        /* Extract gate_id (remove .json suffix) */
        char gate_id[64];
        size_t id_len = strlen(entry->d_name) - 5;
        if (id_len >= sizeof(gate_id))
            continue;
        memcpy(gate_id, entry->d_name, id_len);
        gate_id[id_len] = '\0';

        /* Load gate */
        char *path = gate_filename(alloc, mgr->gates_dir, gate_id);
        if (!path)
            continue;

        hu_approval_gate_t gate;
        hu_error_t err = load_gate_from_file(alloc, path, &gate);
        alloc->free(alloc->ctx, path, strlen(path) + 1);

        if (err != HU_OK)
            continue;

        /* Check timeout */
        if (gate.timeout_at > 0 && (int64_t)time(NULL) > gate.timeout_at &&
            gate.status == HU_GATE_PENDING) {
            gate.status = HU_GATE_TIMED_OUT;
            gate.resolved_at = (int64_t)time(NULL);
        }

        /* Filter: only include if pending (or other status as needed) */
        if (gate.status != HU_GATE_PENDING) {
            hu_gate_free(alloc, &gate);
            continue;
        }

        /* Resize if needed */
        if (*out_count >= cap) {
            cap *= 2;
            hu_approval_gate_t *new_gates =
                (hu_approval_gate_t *)alloc->alloc(alloc->ctx, cap * sizeof(*gates));
            if (!new_gates) {
                hu_gate_free(alloc, &gate);
                closedir(dir);
                hu_gate_free_array(alloc, gates, *out_count);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(new_gates, gates, *out_count * sizeof(*gates));
            alloc->free(alloc->ctx, gates, (*out_count) * sizeof(*gates));
            gates = new_gates;
        }

        gates[(*out_count)++] = gate;
    }

    closedir(dir);

    /* Return NULL if no gates found */
    if (*out_count == 0) {
        alloc->free(alloc->ctx, gates, cap * sizeof(*gates));
        *out = NULL;
    } else {
        *out = gates;
    }
    return HU_OK;
}

void hu_gate_free(hu_allocator_t *alloc, hu_approval_gate_t *gate) {
    if (!alloc || !gate)
        return;
    if (gate->description) {
        alloc->free(alloc->ctx, gate->description, gate->description_len + 1);
        gate->description = NULL;
    }
    if (gate->context_json) {
        alloc->free(alloc->ctx, gate->context_json, gate->context_json_len + 1);
        gate->context_json = NULL;
    }
    if (gate->response) {
        alloc->free(alloc->ctx, gate->response, gate->response_len + 1);
        gate->response = NULL;
    }
}

void hu_gate_free_array(hu_allocator_t *alloc, hu_approval_gate_t *gates, size_t count) {
    if (!gates || count == 0)
        return;
    for (size_t i = 0; i < count; i++) {
        hu_gate_free(alloc, &gates[i]);
    }
    alloc->free(alloc->ctx, gates, count * sizeof(*gates));
}

const char *hu_gate_status_name(hu_gate_status_t status) {
    switch (status) {
    case HU_GATE_PENDING:
        return "pending";
    case HU_GATE_APPROVED:
        return "approved";
    case HU_GATE_REJECTED:
        return "rejected";
    case HU_GATE_TIMED_OUT:
        return "timed_out";
    default:
        return "unknown";
    }
}
