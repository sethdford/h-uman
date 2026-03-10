#include "human/state.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
typedef CRITICAL_SECTION hu_mutex_t;
#define hu_mutex_lock(m)   EnterCriticalSection(m)
#define hu_mutex_unlock(m) LeaveCriticalSection(m)
#else
#include <pthread.h>
typedef pthread_mutex_t hu_mutex_t;
#define hu_mutex_lock(m)   pthread_mutex_lock(m)
#define hu_mutex_unlock(m) pthread_mutex_unlock(m)
#endif

static hu_mutex_t s_state_mutex;
static bool s_state_mutex_init = false;

static void ensure_mutex(void) {
    if (!s_state_mutex_init) {
#if defined(_WIN32) || defined(_WIN64)
        InitializeCriticalSection(&s_state_mutex);
#else
        pthread_mutex_init(&s_state_mutex, NULL);
#endif
        s_state_mutex_init = true;
    }
}

hu_error_t hu_state_manager_init(hu_state_manager_t *mgr, hu_allocator_t *alloc,
                                 const char *state_path) {
    if (!mgr || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    memset(mgr, 0, sizeof(*mgr));
    mgr->alloc = alloc;
    mgr->process_state = HU_PROCESS_STATE_STARTING;
    if (state_path) {
        size_t len = strlen(state_path) + 1;
        mgr->state_path = alloc->alloc(alloc->ctx, len);
        if (mgr->state_path)
            memcpy(mgr->state_path, state_path, len);
    }
    return HU_OK;
}

void hu_state_manager_deinit(hu_state_manager_t *mgr) {
    if (!mgr || !mgr->alloc)
        return;
    if (mgr->state_path) {
        mgr->alloc->free(mgr->alloc->ctx, mgr->state_path, strlen(mgr->state_path) + 1);
    }
    memset(mgr, 0, sizeof(*mgr));
}

void hu_state_set_process(hu_state_manager_t *mgr, hu_process_state_t state) {
    if (!mgr)
        return;
    ensure_mutex();
    hu_mutex_lock(&s_state_mutex);
    mgr->process_state = state;
    hu_mutex_unlock(&s_state_mutex);
}

hu_process_state_t hu_state_get_process(const hu_state_manager_t *mgr) {
    if (!mgr)
        return HU_PROCESS_STATE_STOPPED;
    ensure_mutex();
    hu_mutex_lock((hu_mutex_t *)&s_state_mutex);
    hu_process_state_t s = mgr->process_state;
    hu_mutex_unlock((hu_mutex_t *)&s_state_mutex);
    return s;
}

void hu_state_set_last_channel(hu_state_manager_t *mgr, const char *channel, const char *chat_id) {
    if (!mgr)
        return;
    ensure_mutex();
    hu_mutex_lock(&s_state_mutex);
    if (channel) {
        size_t n = strlen(channel);
        if (n >= HU_STATE_CHANNEL_LEN)
            n = HU_STATE_CHANNEL_LEN - 1;
        memcpy(mgr->data.last_channel, channel, n);
        mgr->data.last_channel[n] = '\0';
    } else {
        mgr->data.last_channel[0] = '\0';
    }
    if (chat_id) {
        size_t n = strlen(chat_id);
        if (n >= HU_STATE_CHAT_ID_LEN)
            n = HU_STATE_CHAT_ID_LEN - 1;
        memcpy(mgr->data.last_chat_id, chat_id, n);
        mgr->data.last_chat_id[n] = '\0';
    } else {
        mgr->data.last_chat_id[0] = '\0';
    }
    mgr->data.updated_at = (int64_t)time(NULL);
    hu_mutex_unlock(&s_state_mutex);
}

void hu_state_get_last_channel(const hu_state_manager_t *mgr, char *channel_out,
                               size_t channel_size, char *chat_id_out, size_t chat_id_size) {
    if (!mgr)
        return;
    ensure_mutex();
    hu_mutex_lock((hu_mutex_t *)&s_state_mutex);
    if (channel_out && channel_size > 0) {
        strncpy(channel_out, mgr->data.last_channel, channel_size - 1);
        channel_out[channel_size - 1] = '\0';
    }
    if (chat_id_out && chat_id_size > 0) {
        strncpy(chat_id_out, mgr->data.last_chat_id, chat_id_size - 1);
        chat_id_out[chat_id_size - 1] = '\0';
    }
    hu_mutex_unlock((hu_mutex_t *)&s_state_mutex);
}

static void json_escape(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        switch (*s) {
        case '"':
            fputs("\\\"", f);
            break;
        case '\\':
            fputs("\\\\", f);
            break;
        case '\n':
            fputs("\\n", f);
            break;
        case '\r':
            fputs("\\r", f);
            break;
        case '\t':
            fputs("\\t", f);
            break;
        default:
            fputc(*s, f);
            break;
        }
    }
    fputc('"', f);
}

hu_error_t hu_state_save(hu_state_manager_t *mgr) {
    if (!mgr || !mgr->state_path)
        return HU_OK;
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", mgr->state_path);
    FILE *f = fopen(tmp_path, "wb");
    if (!f)
        return HU_ERR_IO;
    fprintf(f, "{\n  \"last_channel\": ");
    if (mgr->data.last_channel[0])
        json_escape(f, mgr->data.last_channel);
    else
        fputs("null", f);
    fprintf(f, ",\n  \"last_chat_id\": ");
    if (mgr->data.last_chat_id[0])
        json_escape(f, mgr->data.last_chat_id);
    else
        fputs("null", f);
    fprintf(f, ",\n  \"updated_at\": %lld\n}\n", (long long)mgr->data.updated_at);
    fclose(f);
#ifdef _WIN32
    remove(mgr->state_path);
    rename(tmp_path, mgr->state_path);
#else
    rename(tmp_path, mgr->state_path);
#endif
    return HU_OK;
}

hu_error_t hu_state_load(hu_state_manager_t *mgr) {
    if (!mgr || !mgr->state_path || !mgr->alloc)
        return HU_OK;
    FILE *f = fopen(mgr->state_path, "rb");
    if (!f)
        return HU_OK; /* No file is ok */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) {
        fclose(f);
        return HU_OK;
    }
    char *buf = mgr->alloc->alloc(mgr->alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nr] = '\0';
    const char *ch = strstr(buf, "\"last_channel\"");
    const char *ci = strstr(buf, "\"last_chat_id\"");
    const char *ua = strstr(buf, "\"updated_at\"");
    if (ch && ci && ua) {
        ch = strchr(ch, ':');
        if (ch) {
            ch = strchr(ch, '"');
            if (ch) {
                ch++;
                const char *end = strchr(ch, '"');
                if (end && (size_t)(end - ch) < HU_STATE_CHANNEL_LEN) {
                    size_t n = (size_t)(end - ch);
                    memcpy(mgr->data.last_channel, ch, n);
                    mgr->data.last_channel[n] = '\0';
                }
            }
        }
        ci = strchr(ci, ':');
        if (ci) {
            ci = strchr(ci, '"');
            if (ci) {
                ci++;
                const char *end = strchr(ci, '"');
                if (end && (size_t)(end - ci) < HU_STATE_CHAT_ID_LEN) {
                    size_t n = (size_t)(end - ci);
                    memcpy(mgr->data.last_chat_id, ci, n);
                    mgr->data.last_chat_id[n] = '\0';
                }
            }
        }
        ua = strchr(ua, ':');
        if (ua) {
            ua++;
            long long v = 0;
            if (sscanf(ua, "%lld", &v) == 1)
                mgr->data.updated_at = (int64_t)v;
        }
    }
    mgr->alloc->free(mgr->alloc->ctx, buf, (size_t)sz + 1);
    return HU_OK;
}

char *hu_state_default_path(hu_allocator_t *alloc, const char *workspace_dir) {
    if (!alloc || !workspace_dir)
        return NULL;
    size_t dlen = strlen(workspace_dir);
    size_t need = dlen + 12;
    char *p = alloc->alloc(alloc->ctx, need);
    if (!p)
        return NULL;
    snprintf(p, need, "%s/state.json", workspace_dir);
    return p;
}
