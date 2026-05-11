#include "human/agent/workflow_event.h"
#include "human/core/json.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HU_IS_TEST
#include <unistd.h>
#endif

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

struct hu_workflow_event_log {
    char *path;           /* file path (owned) */
    size_t path_len;
    uint64_t next_seq;    /* next sequence number to assign */
    size_t event_count;   /* cached count of events in file */
};


hu_error_t hu_workflow_event_log_create(hu_allocator_t *alloc, const char *path,
                                        hu_workflow_event_log_t **out) {
    if (!alloc || !path || !out) {
        return HU_ERR_INVALID_ARGUMENT;
    }

#ifdef HU_IS_TEST
    /* Use temp directory in test mode */
    const char *temp_dir = getenv("TMPDIR");
    if (!temp_dir) {
#ifdef _WIN32
        temp_dir = getenv("TEMP");
        if (!temp_dir) {
            temp_dir = "C:\\Temp";
        }
#else
        temp_dir = "/tmp";
#endif
    }

    /* Build temp path with unique suffix based on input path */
    size_t temp_len = strlen(temp_dir) + strlen("/hu_workflow_") + 100;
    char *temp_path = (char *)alloc->alloc(alloc->ctx, temp_len);
    if (!temp_path) {
        return HU_ERR_OUT_OF_MEMORY;
    }

    /* Use path basename (after last /) to create deterministic directories within tests
     * This allows the same path to map to the same directory (for persist tests)
     * while different paths get different directories.
     * Add a counter to avoid collisions between test runs. */
    static unsigned long test_run_counter = 0;
    if (test_run_counter == 0) {
        /* Initialize with current time (seconds since epoch) to avoid conflicts between runs */
        test_run_counter = (unsigned long)time(NULL);
        if (test_run_counter == 0) test_run_counter = 1;
    }

    const char *basename = strrchr(path, '/');
    if (!basename) {
        basename = path;
    } else {
        basename++; /* skip the slash */
    }

    /* Remove .jsonl suffix if present for cleaner names */
    size_t basename_len = strlen(basename);
    if (basename_len > 6 && strcmp(basename + basename_len - 6, ".jsonl") == 0) {
        basename_len -= 6;
    }

    snprintf(temp_path, temp_len, "%s/hu_workflow_%lu_%.*s", temp_dir, test_run_counter, (int)basename_len, basename);

    /* Try to create directory (ignore error if exists) */
    mkdir(temp_path, 0755);

    /* Build full path for events.jsonl */
    size_t full_len = strlen(temp_path) + strlen("/events.jsonl") + 1;
    char *full_path = (char *)alloc->alloc(alloc->ctx, full_len);
    if (!full_path) {
        alloc->free(alloc->ctx, temp_path, temp_len);
        return HU_ERR_OUT_OF_MEMORY;
    }
    snprintf(full_path, full_len, "%s/events.jsonl", temp_path);
    alloc->free(alloc->ctx, temp_path, temp_len);
    path = full_path;
#else
    /* Ensure directory exists */
    const char *last_slash = strrchr(path, '/');
    if (last_slash) {
        size_t dir_len = (size_t)(last_slash - path);
        char *dir = (char *)alloc->alloc(alloc->ctx, dir_len + 1);
        if (!dir) {
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(dir, path, dir_len);
        dir[dir_len] = '\0';
        /* Try to create directory (ignore error if exists) */
        mkdir(dir, 0755);
        alloc->free(alloc->ctx, dir, dir_len + 1);
    }
#endif

    /* Allocate log structure */
    hu_workflow_event_log_t *log =
        (hu_workflow_event_log_t *)alloc->alloc(alloc->ctx, sizeof(*log));
    if (!log) {
#ifdef HU_IS_TEST
        alloc->free(alloc->ctx, full_path, full_len);
#endif
        return HU_ERR_OUT_OF_MEMORY;
    }

    memset(log, 0, sizeof(*log));

    /* Copy path */
    size_t path_len = strlen(path);
    log->path = (char *)alloc->alloc(alloc->ctx, path_len + 1);
    if (!log->path) {
        alloc->free(alloc->ctx, log, sizeof(*log));
#ifdef HU_IS_TEST
        alloc->free(alloc->ctx, full_path, full_len);
#endif
        return HU_ERR_OUT_OF_MEMORY;
    }

    memcpy(log->path, path, path_len);
    log->path[path_len] = '\0';
    log->path_len = path_len;

#ifdef HU_IS_TEST
    /* In test builds `path` was retargeted to point at `full_path`, which is
     * an allocation we own. log->path now holds its own copy, so free the
     * temp-redirect buffer (was 51 B × ~60 test agents ≈ 3.2 KB ASan leak). */
    alloc->free(alloc->ctx, full_path, full_len);
#endif

    /* Count existing events in file (if it exists) */
    log->next_seq = 0;
    log->event_count = 0;

    FILE *f = fopen(log->path, "r");
    if (f) {
        int c;
        while ((c = fgetc(f)) != EOF) {
            if (c == '\n') {
                log->event_count++;
                log->next_seq++;
            }
        }
        fclose(f);
    }

    *out = log;
    return HU_OK;
}

hu_error_t hu_workflow_event_log_append(hu_workflow_event_log_t *log, hu_allocator_t *alloc,
                                        const hu_workflow_event_t *event) {
    if (!log || !alloc || !event) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Assign sequence number */
    uint64_t seq = log->next_seq++;

    /* Build JSON object for the event */
    hu_json_buf_t buf;
    hu_error_t err = hu_json_buf_init(&buf, alloc);
    if (err != HU_OK) {
        return err;
    }

    /* Start object */
    err = hu_json_buf_append_raw(&buf, "{", 1);
    if (err != HU_OK) {
        hu_json_buf_free(&buf);
        return err;
    }

    /* Add fields */
    err = hu_json_append_key_int(&buf, "type", 4, (long long)event->type);
    if (err == HU_OK) {
        err = hu_json_buf_append_raw(&buf, ",", 1);
    }
    if (err == HU_OK) {
        err = hu_json_append_key_int(&buf, "sequence_num", 12, (long long)seq);
    }
    if (err == HU_OK) {
        err = hu_json_buf_append_raw(&buf, ",", 1);
    }
    if (err == HU_OK) {
        err = hu_json_append_key_int(&buf, "timestamp", 9, event->timestamp);
    }
    if (err == HU_OK) {
        err = hu_json_buf_append_raw(&buf, ",", 1);
    }
    if (err == HU_OK) {
        err = hu_json_append_key_value(&buf, "workflow_id", 11, event->workflow_id,
                                       event->workflow_id_len);
    }
    if (err == HU_OK && event->step_id_len > 0) {
        err = hu_json_buf_append_raw(&buf, ",", 1);
        if (err == HU_OK) {
            err = hu_json_append_key_value(&buf, "step_id", 7, event->step_id,
                                           event->step_id_len);
        }
    }
    if (err == HU_OK && event->data_json && event->data_json_len > 0) {
        hu_json_value_t *parsed = NULL;
        hu_error_t perr = hu_json_parse(alloc, event->data_json, event->data_json_len, &parsed);
        if (perr == HU_OK) {
            err = hu_json_buf_append_raw(&buf, ",\"data\":", 8);
            if (err == HU_OK) {
                err = hu_json_buf_append_raw(&buf, event->data_json, event->data_json_len);
            }
        }
        if (parsed) {
            hu_json_free(alloc, parsed);
        }
    }
    if (err == HU_OK && event->idempotency_key_len > 0) {
        err = hu_json_buf_append_raw(&buf, ",", 1);
        if (err == HU_OK) {
            err = hu_json_append_key_value(&buf, "idempotency_key", 15, event->idempotency_key,
                                           event->idempotency_key_len);
        }
    }

    if (err == HU_OK) {
        err = hu_json_buf_append_raw(&buf, "}", 1);
    }

    if (err != HU_OK) {
        hu_json_buf_free(&buf);
        return err;
    }

    /* Append to file with newline */
    FILE *f = fopen(log->path, "a");
    if (!f) {
        hu_json_buf_free(&buf);
        return HU_ERR_IO;
    }

    if (fwrite(buf.ptr, 1, buf.len, f) != buf.len) {
        fclose(f);
        hu_json_buf_free(&buf);
        return HU_ERR_IO;
    }

    if (fputc('\n', f) == EOF) {
        fclose(f);
        hu_json_buf_free(&buf);
        return HU_ERR_IO;
    }

    fclose(f);
    log->event_count++;

    hu_json_buf_free(&buf);
    return HU_OK;
}

hu_error_t hu_workflow_event_log_replay(hu_workflow_event_log_t *log, hu_allocator_t *alloc,
                                        hu_workflow_event_t **out_events, size_t *out_count) {
    if (!log || !alloc || !out_events || !out_count) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    *out_count = 0;

    if (log->event_count == 0) {
        *out_events = NULL;
        return HU_OK;
    }

    /* Allocate array for events */
    hu_workflow_event_t *events =
        (hu_workflow_event_t *)alloc->alloc(alloc->ctx, sizeof(*events) * log->event_count);
    if (!events) {
        return HU_ERR_OUT_OF_MEMORY;
    }

    memset(events, 0, sizeof(*events) * log->event_count);

    /* Read file line by line */
    FILE *f = fopen(log->path, "r");
    if (!f) {
        alloc->free(alloc->ctx, events, sizeof(*events) * log->event_count);
        return HU_ERR_IO;
    }

    char line[8192];
    size_t idx = 0;

    while (fgets(line, sizeof(line), f) && idx < log->event_count) {
        /* Remove trailing newline */
        size_t line_len = strlen(line);
        if (line_len > 0 && line[line_len - 1] == '\n') {
            line[line_len - 1] = '\0';
            line_len--;
        }

        /* Parse JSON */
        hu_json_value_t *json = NULL;
        hu_error_t err = hu_json_parse(alloc, line, line_len, &json);
        if (err != HU_OK || !json || json->type != HU_JSON_OBJECT) {
            if (json) {
                hu_json_free(alloc, json);
            }
            continue;
        }

        hu_workflow_event_t *event = &events[idx];

        /* Extract fields */
        hu_json_value_t *type_val = hu_json_object_get(json, "type");
        if (type_val && type_val->type == HU_JSON_NUMBER) {
            event->type = (hu_workflow_event_type_t)(int)type_val->data.number;
        }

        hu_json_value_t *seq_val = hu_json_object_get(json, "sequence_num");
        if (seq_val && seq_val->type == HU_JSON_NUMBER) {
            event->sequence_num = (uint64_t)seq_val->data.number;
        }

        hu_json_value_t *ts_val = hu_json_object_get(json, "timestamp");
        if (ts_val && ts_val->type == HU_JSON_NUMBER) {
            event->timestamp = (int64_t)ts_val->data.number;
        }

        /* Extract workflow_id */
        hu_json_value_t *wf_val = hu_json_object_get(json, "workflow_id");
        if (wf_val && wf_val->type == HU_JSON_STRING) {
            event->workflow_id_len = wf_val->data.string.len;
            event->workflow_id =
                (char *)alloc->alloc(alloc->ctx, event->workflow_id_len + 1);
            if (!event->workflow_id) {
                hu_json_free(alloc, json);
                /* Clean up already-allocated events */
                for (size_t i = 0; i <= idx; i++) {
                    hu_workflow_event_free(alloc, &events[i]);
                }
                alloc->free(alloc->ctx, events, sizeof(*events) * log->event_count);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(event->workflow_id, wf_val->data.string.ptr, event->workflow_id_len);
            event->workflow_id[event->workflow_id_len] = '\0';
        }

        /* Extract step_id */
        hu_json_value_t *step_val = hu_json_object_get(json, "step_id");
        if (step_val && step_val->type == HU_JSON_STRING) {
            event->step_id_len = step_val->data.string.len;
            event->step_id = (char *)alloc->alloc(alloc->ctx, event->step_id_len + 1);
            if (!event->step_id) {
                hu_json_free(alloc, json);
                for (size_t i = 0; i <= idx; i++) {
                    hu_workflow_event_free(alloc, &events[i]);
                }
                alloc->free(alloc->ctx, events, sizeof(*events) * log->event_count);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(event->step_id, step_val->data.string.ptr, event->step_id_len);
            event->step_id[event->step_id_len] = '\0';
        }

        /* Extract data_json (keep as raw JSON) */
        hu_json_value_t *data_val = hu_json_object_get(json, "data");
        if (data_val) {
            /* Serialize data back to JSON string */
            char *data_str = NULL;
            size_t data_len = 0;
            hu_error_t err2 = hu_json_stringify(alloc, data_val, &data_str, &data_len);
            if (err2 == HU_OK && data_str) {
                event->data_json_len = data_len;
                event->data_json =
                    (char *)alloc->alloc(alloc->ctx, event->data_json_len + 1);
                if (!event->data_json) {
                    alloc->free(alloc->ctx, data_str, data_len + 1);
                    hu_json_free(alloc, json);
                    for (size_t i = 0; i <= idx; i++) {
                        hu_workflow_event_free(alloc, &events[i]);
                    }
                    alloc->free(alloc->ctx, events, sizeof(*events) * log->event_count);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                memcpy(event->data_json, data_str, event->data_json_len);
                event->data_json[event->data_json_len] = '\0';
                alloc->free(alloc->ctx, data_str, data_len + 1);
            }
        }

        /* Extract idempotency_key */
        hu_json_value_t *key_val = hu_json_object_get(json, "idempotency_key");
        if (key_val && key_val->type == HU_JSON_STRING) {
            event->idempotency_key_len = key_val->data.string.len;
            event->idempotency_key =
                (char *)alloc->alloc(alloc->ctx, event->idempotency_key_len + 1);
            if (!event->idempotency_key) {
                hu_json_free(alloc, json);
                for (size_t i = 0; i <= idx; i++) {
                    hu_workflow_event_free(alloc, &events[i]);
                }
                alloc->free(alloc->ctx, events, sizeof(*events) * log->event_count);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(event->idempotency_key, key_val->data.string.ptr,
                   event->idempotency_key_len);
            event->idempotency_key[event->idempotency_key_len] = '\0';
        }

        hu_json_free(alloc, json);
        idx++;
    }

    fclose(f);

    *out_events = events;
    *out_count = idx;

    return HU_OK;
}

hu_error_t hu_workflow_event_log_find_by_key(hu_workflow_event_log_t *log,
                                              const char *idempotency_key, hu_workflow_event_t *out,
                                              bool *found) {
    if (!log || !idempotency_key || !out || !found) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    *found = false;
    memset(out, 0, sizeof(*out));

    hu_allocator_t temp_alloc = hu_system_allocator();

    hu_workflow_event_t *events = NULL;
    size_t count = 0;

    hu_error_t err = hu_workflow_event_log_replay(log, &temp_alloc, &events, &count);
    if (err != HU_OK) {
        return err;
    }

    hu_error_t find_err = HU_OK;

    /* Search for matching key */
    for (size_t i = 0; i < count; i++) {
        if (events[i].idempotency_key && strcmp(events[i].idempotency_key, idempotency_key) == 0) {
            /* Copy matching event to output */
            out->type = events[i].type;
            out->sequence_num = events[i].sequence_num;
            out->timestamp = events[i].timestamp;

            bool copy_ok = true;
            if (events[i].workflow_id_len > 0) {
                out->workflow_id = (char *)malloc(events[i].workflow_id_len + 1);
                if (!out->workflow_id)
                    copy_ok = false;
                else {
                    memcpy(out->workflow_id, events[i].workflow_id, events[i].workflow_id_len);
                    out->workflow_id[events[i].workflow_id_len] = '\0';
                    out->workflow_id_len = events[i].workflow_id_len;
                }
            }

            if (copy_ok && events[i].step_id_len > 0) {
                out->step_id = (char *)malloc(events[i].step_id_len + 1);
                if (!out->step_id)
                    copy_ok = false;
                else {
                    memcpy(out->step_id, events[i].step_id, events[i].step_id_len);
                    out->step_id[events[i].step_id_len] = '\0';
                    out->step_id_len = events[i].step_id_len;
                }
            }

            if (copy_ok && events[i].data_json_len > 0) {
                out->data_json = (char *)malloc(events[i].data_json_len + 1);
                if (!out->data_json)
                    copy_ok = false;
                else {
                    memcpy(out->data_json, events[i].data_json, events[i].data_json_len);
                    out->data_json[events[i].data_json_len] = '\0';
                    out->data_json_len = events[i].data_json_len;
                }
            }

            if (copy_ok && events[i].idempotency_key_len > 0) {
                out->idempotency_key = (char *)malloc(events[i].idempotency_key_len + 1);
                if (!out->idempotency_key)
                    copy_ok = false;
                else {
                    memcpy(out->idempotency_key, events[i].idempotency_key,
                           events[i].idempotency_key_len);
                    out->idempotency_key[events[i].idempotency_key_len] = '\0';
                    out->idempotency_key_len = events[i].idempotency_key_len;
                }
            }

            if (!copy_ok) {
                free(out->workflow_id);
                free(out->step_id);
                free(out->data_json);
                free(out->idempotency_key);
                memset(out, 0, sizeof(*out));
                *found = false;
                find_err = HU_ERR_OUT_OF_MEMORY;
            } else {
                *found = true;
            }
            break;
        }
    }

    /* Clean up replayed events */
    for (size_t i = 0; i < count; i++) {
        hu_workflow_event_free(&temp_alloc, &events[i]);
    }
    if (events) {
        temp_alloc.free(temp_alloc.ctx, events, sizeof(*events) * count);
    }

    return find_err;
}

size_t hu_workflow_event_log_count(const hu_workflow_event_log_t *log) {
    if (!log) {
        return 0;
    }
    return log->event_count;
}

void hu_workflow_event_log_destroy(hu_workflow_event_log_t *log, hu_allocator_t *alloc) {
    if (!log || !alloc) {
        return;
    }

    if (log->path) {
        alloc->free(alloc->ctx, log->path, log->path_len + 1);
    }

    alloc->free(alloc->ctx, log, sizeof(*log));
}

void hu_workflow_event_free(hu_allocator_t *alloc, hu_workflow_event_t *event) {
    if (!event || !alloc) {
        return;
    }

    if (event->workflow_id) {
        alloc->free(alloc->ctx, event->workflow_id, event->workflow_id_len + 1);
    }
    if (event->step_id) {
        alloc->free(alloc->ctx, event->step_id, event->step_id_len + 1);
    }
    if (event->data_json) {
        alloc->free(alloc->ctx, event->data_json, event->data_json_len + 1);
    }
    if (event->idempotency_key) {
        alloc->free(alloc->ctx, event->idempotency_key, event->idempotency_key_len + 1);
    }

    memset(event, 0, sizeof(*event));
}

int64_t hu_workflow_event_current_timestamp_ms(void) {
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 199309L
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#else
    return (int64_t)time(NULL) * 1000;
#endif
}

const char *hu_workflow_event_type_name(hu_workflow_event_type_t type) {
    switch (type) {
    case HU_WF_EVENT_WORKFLOW_STARTED:
        return "workflow_started";
    case HU_WF_EVENT_WORKFLOW_COMPLETED:
        return "workflow_completed";
    case HU_WF_EVENT_WORKFLOW_FAILED:
        return "workflow_failed";
    case HU_WF_EVENT_STEP_STARTED:
        return "step_started";
    case HU_WF_EVENT_STEP_COMPLETED:
        return "step_completed";
    case HU_WF_EVENT_STEP_FAILED:
        return "step_failed";
    case HU_WF_EVENT_TOOL_CALLED:
        return "tool_called";
    case HU_WF_EVENT_TOOL_RESULT:
        return "tool_result";
    case HU_WF_EVENT_HUMAN_GATE_WAITING:
        return "human_gate_waiting";
    case HU_WF_EVENT_HUMAN_GATE_RESOLVED:
        return "human_gate_resolved";
    case HU_WF_EVENT_CHECKPOINT_SAVED:
        return "checkpoint_saved";
    case HU_WF_EVENT_RETRY_ATTEMPTED:
        return "retry_attempted";
    default:
        return "unknown";
    }
}
