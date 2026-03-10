#include "human/session.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t hash_key(const char *key) {
    uint32_t h = 5381;
    while (*key)
        h = ((h << 5) + h) + (unsigned char)*key++;
    return h;
}

static hu_session_entry_t **find_bucket(hu_session_manager_t *mgr, const char *key) {
    uint32_t h = hash_key(key) % HU_SESSION_MAP_CAP;
    return &mgr->buckets[h];
}

static hu_session_t *find_session(hu_session_manager_t *mgr, const char *key) {
    hu_session_entry_t **head = find_bucket(mgr, key);
    for (hu_session_entry_t *e = *head; e; e = e->next) {
        if (strcmp(e->key, key) == 0)
            return e->session;
    }
    return NULL;
}

static void free_session_messages(hu_session_t *s, hu_allocator_t *alloc) {
    if (!s || !alloc)
        return;
    for (size_t i = 0; i < s->message_count; i++) {
        if (s->messages[i].content) {
            alloc->free(alloc->ctx, s->messages[i].content, strlen(s->messages[i].content) + 1);
        }
    }
    if (s->messages) {
        alloc->free(alloc->ctx, s->messages, s->message_cap * sizeof(hu_session_message_t));
    }
    s->messages = NULL;
    s->message_count = 0;
    s->message_cap = 0;
}

static void destroy_session(hu_session_t *s, hu_allocator_t *alloc) {
    free_session_messages(s, alloc);
    alloc->free(alloc->ctx, s, sizeof(hu_session_t));
}

hu_error_t hu_session_manager_init(hu_session_manager_t *mgr, hu_allocator_t *alloc) {
    if (!mgr || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    memset(mgr, 0, sizeof(*mgr));
    mgr->alloc = alloc;
    return HU_OK;
}

void hu_session_manager_deinit(hu_session_manager_t *mgr) {
    if (!mgr || !mgr->alloc)
        return;
    for (size_t i = 0; i < HU_SESSION_MAP_CAP; i++) {
        hu_session_entry_t *e = mgr->buckets[i];
        while (e) {
            hu_session_entry_t *next = e->next;
            if (e->session)
                destroy_session(e->session, mgr->alloc);
            mgr->alloc->free(mgr->alloc->ctx, e, sizeof(hu_session_entry_t));
            e = next;
        }
        mgr->buckets[i] = NULL;
    }
    mgr->count = 0;
}

hu_session_t *hu_session_get_or_create(hu_session_manager_t *mgr, const char *session_key) {
    if (!mgr || !mgr->alloc || !session_key)
        return NULL;
    hu_session_t *s = find_session(mgr, session_key);
    if (s) {
        s->last_active = (int64_t)time(NULL);
        return s;
    }
    hu_session_t *new_s = mgr->alloc->alloc(mgr->alloc->ctx, sizeof(hu_session_t));
    if (!new_s)
        return NULL;
    memset(new_s, 0, sizeof(*new_s));
    size_t klen = strlen(session_key);
    if (klen >= HU_SESSION_KEY_LEN)
        klen = HU_SESSION_KEY_LEN - 1;
    memcpy(new_s->session_key, session_key, klen);
    new_s->session_key[klen] = '\0';
    new_s->created_at = (int64_t)time(NULL);
    new_s->last_active = new_s->created_at;

    hu_session_entry_t *ent = mgr->alloc->alloc(mgr->alloc->ctx, sizeof(hu_session_entry_t));
    if (!ent) {
        mgr->alloc->free(mgr->alloc->ctx, new_s, sizeof(hu_session_t));
        return NULL;
    }
    memset(ent, 0, sizeof(*ent));
    memcpy(ent->key, new_s->session_key, HU_SESSION_KEY_LEN);
    ent->session = new_s;
    ent->next = *find_bucket(mgr, session_key);
    *find_bucket(mgr, session_key) = ent;
    mgr->count++;
    return new_s;
}

hu_error_t hu_session_append_message(hu_session_t *s, hu_allocator_t *alloc, const char *role,
                                     const char *content) {
    if (!s || !alloc || !role || !content)
        return HU_ERR_INVALID_ARGUMENT;
    if (s->message_count >= s->message_cap) {
        size_t new_cap = s->message_cap ? s->message_cap * 2 : 8;
        if (new_cap > HU_SESSION_MSG_CAP)
            new_cap = HU_SESSION_MSG_CAP;
        hu_session_message_t *n =
            alloc->realloc(alloc->ctx, s->messages, s->message_cap * sizeof(hu_session_message_t),
                           new_cap * sizeof(hu_session_message_t));
        if (!n)
            return HU_ERR_OUT_OF_MEMORY;
        s->messages = n;
        s->message_cap = new_cap;
    }
    hu_session_message_t *m = &s->messages[s->message_count++];
    size_t rlen = strlen(role);
    if (rlen >= HU_SESSION_MSG_ROLE_LEN)
        rlen = HU_SESSION_MSG_ROLE_LEN - 1;
    memcpy(m->role, role, rlen);
    m->role[rlen] = '\0';
    size_t clen = strlen(content) + 1;
    if (clen > HU_SESSION_MSG_CONTENT_CAP)
        clen = HU_SESSION_MSG_CONTENT_CAP;
    m->content = alloc->alloc(alloc->ctx, clen);
    if (!m->content)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(m->content, content, clen - 1);
    m->content[clen - 1] = '\0';
    s->last_active = (int64_t)time(NULL);
    s->turn_count++;
    return HU_OK;
}

char *hu_session_gen_id(hu_allocator_t *alloc) {
    if (!alloc)
        return NULL;
    return hu_util_gen_session_id(alloc->ctx, alloc->alloc);
}

size_t hu_session_evict_idle(hu_session_manager_t *mgr, uint64_t max_idle_secs) {
    if (!mgr || !mgr->alloc)
        return 0;
    int64_t now = (int64_t)time(NULL);
    size_t evicted = 0;
    for (size_t i = 0; i < HU_SESSION_MAP_CAP; i++) {
        hu_session_entry_t **pp = &mgr->buckets[i];
        while (*pp) {
            hu_session_entry_t *e = *pp;
            uint64_t idle = (uint64_t)(now - e->session->last_active);
            if (idle > max_idle_secs) {
                *pp = e->next;
                if (e->session)
                    destroy_session(e->session, mgr->alloc);
                mgr->alloc->free(mgr->alloc->ctx, e, sizeof(hu_session_entry_t));
                mgr->count--;
                evicted++;
            } else {
                pp = &e->next;
            }
        }
    }
    return evicted;
}

size_t hu_session_count(const hu_session_manager_t *mgr) {
    return mgr ? mgr->count : 0;
}

hu_session_summary_t *hu_session_list(hu_session_manager_t *mgr, hu_allocator_t *alloc,
                                      size_t *out_count) {
    if (!mgr || !alloc || !out_count)
        return NULL;
    *out_count = 0;
    size_t n = mgr->count;
    if (n == 0)
        return NULL;
    hu_session_summary_t *arr = alloc->alloc(alloc->ctx, n * sizeof(hu_session_summary_t));
    if (!arr)
        return NULL;
    size_t idx = 0;
    for (size_t i = 0; i < HU_SESSION_MAP_CAP && idx < n; i++) {
        for (hu_session_entry_t *e = mgr->buckets[i]; e; e = e->next) {
            if (!e->session || idx >= n)
                break;
            hu_session_t *s = e->session;
            size_t klen = strnlen(s->session_key, HU_SESSION_KEY_LEN - 1);
            memcpy(arr[idx].session_key, s->session_key, klen);
            arr[idx].session_key[klen] = '\0';
            size_t llen = strnlen(s->label, HU_SESSION_LABEL_LEN - 1);
            memcpy(arr[idx].label, s->label, llen);
            arr[idx].label[llen] = '\0';
            arr[idx].created_at = s->created_at;
            arr[idx].last_active = s->last_active;
            arr[idx].turn_count = s->turn_count;
            idx++;
        }
    }
    *out_count = idx;
    return arr;
}

hu_error_t hu_session_delete(hu_session_manager_t *mgr, const char *session_key) {
    if (!mgr || !mgr->alloc || !session_key)
        return HU_ERR_INVALID_ARGUMENT;
    hu_session_entry_t **pp = find_bucket(mgr, session_key);
    for (hu_session_entry_t *e = *pp; e; pp = &e->next, e = *pp) {
        if (strcmp(e->key, session_key) != 0)
            continue;
        *pp = e->next;
        if (e->session)
            destroy_session(e->session, mgr->alloc);
        mgr->alloc->free(mgr->alloc->ctx, e, sizeof(hu_session_entry_t));
        mgr->count--;
        return HU_OK;
    }
    return HU_ERR_NOT_FOUND;
}

hu_error_t hu_session_patch(hu_session_manager_t *mgr, const char *session_key, const char *label) {
    if (!mgr || !session_key)
        return HU_ERR_INVALID_ARGUMENT;
    if (!label)
        return HU_ERR_INVALID_ARGUMENT;
    hu_session_t *s = find_session(mgr, session_key);
    if (!s)
        return HU_ERR_NOT_FOUND;
    size_t len = strnlen(label, HU_SESSION_LABEL_LEN - 1);
    memcpy(s->label, label, len);
    s->label[len] = '\0';
    return HU_OK;
}

static void json_esc(FILE *f, const char *s) {
    for (; s && *s; s++) {
        if (*s == '"')
            fputs("\\\"", f);
        else if (*s == '\\')
            fputs("\\\\", f);
        else if (*s == '\n')
            fputs("\\n", f);
        else if (*s == '\r')
            fputs("\\r", f);
        else if (*s == '\t')
            fputs("\\t", f);
        else if ((unsigned char)*s < 0x20)
            fprintf(f, "\\u%04x", (unsigned char)*s);
        else
            fputc(*s, f);
    }
}

hu_error_t hu_session_save(hu_session_manager_t *mgr, const char *path) {
    if (!mgr || !path)
        return HU_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "w");
    if (!f)
        return HU_ERR_IO;
    fputs("[", f);
    bool first_s = true;
    for (size_t i = 0; i < HU_SESSION_MAP_CAP; i++) {
        for (hu_session_entry_t *e = mgr->buckets[i]; e; e = e->next) {
            hu_session_t *s = e->session;
            if (!s)
                continue;
            if (!first_s)
                fputs(",", f);
            first_s = false;
            fputs("\n{\"key\":\"", f);
            json_esc(f, s->session_key);
            fputs("\",\"label\":\"", f);
            json_esc(f, s->label);
            fprintf(f,
                    "\",\"created_at\":%lld,\"last_active\":%lld,"
                    "\"turn_count\":%llu,\"messages\":[",
                    (long long)s->created_at, (long long)s->last_active,
                    (unsigned long long)s->turn_count);
            for (size_t m = 0; m < s->message_count; m++) {
                if (m > 0)
                    fputc(',', f);
                fputs("{\"role\":\"", f);
                json_esc(f, s->messages[m].role);
                fputs("\",\"content\":\"", f);
                json_esc(f, s->messages[m].content);
                fputs("\"}", f);
            }
            fputs("]}", f);
        }
    }
    fputs("\n]\n", f);
    fclose(f);
    return HU_OK;
}

hu_error_t hu_session_load(hu_session_manager_t *mgr, const char *path) {
    if (!mgr || !mgr->alloc || !path)
        return HU_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "r");
    if (!f)
        return HU_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen <= 2 || flen > 10 * 1024 * 1024) {
        fclose(f);
        return HU_OK;
    }
    char *buf = mgr->alloc->alloc(mgr->alloc->ctx, (size_t)flen + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t rd = fread(buf, 1, (size_t)flen, f);
    buf[rd] = '\0';
    fclose(f);
    hu_json_value_t *root = NULL;
    if (hu_json_parse(mgr->alloc, buf, rd, &root) != HU_OK || !root) {
        mgr->alloc->free(mgr->alloc->ctx, buf, (size_t)flen + 1);
        return HU_ERR_PARSE;
    }
    if (root->type != HU_JSON_ARRAY) {
        hu_json_free(mgr->alloc, root);
        mgr->alloc->free(mgr->alloc->ctx, buf, (size_t)flen + 1);
        return HU_OK;
    }
    for (size_t i = 0; i < root->data.array.len; i++) {
        hu_json_value_t *item = root->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;
        const char *key = hu_json_get_string(item, "key");
        if (!key || !key[0])
            continue;
        hu_session_t *s = hu_session_get_or_create(mgr, key);
        if (!s)
            continue;
        const char *label = hu_json_get_string(item, "label");
        if (label) {
            size_t llen = strlen(label);
            if (llen >= HU_SESSION_LABEL_LEN)
                llen = HU_SESSION_LABEL_LEN - 1;
            memcpy(s->label, label, llen);
            s->label[llen] = '\0';
        }
        s->created_at = (int64_t)hu_json_get_number(item, "created_at", 0);
        s->last_active = (int64_t)hu_json_get_number(item, "last_active", 0);
        s->turn_count = (uint64_t)hu_json_get_number(item, "turn_count", 0);
        hu_json_value_t *msgs = hu_json_object_get(item, "messages");
        if (msgs && msgs->type == HU_JSON_ARRAY) {
            for (size_t m = 0; m < msgs->data.array.len; m++) {
                hu_json_value_t *msg = msgs->data.array.items[m];
                if (!msg)
                    continue;
                const char *role = hu_json_get_string(msg, "role");
                const char *content = hu_json_get_string(msg, "content");
                if (role && content)
                    hu_session_append_message(s, mgr->alloc, role, content);
            }
        }
    }
    hu_json_free(mgr->alloc, root);
    mgr->alloc->free(mgr->alloc->ctx, buf, (size_t)flen + 1);
    return HU_OK;
}
