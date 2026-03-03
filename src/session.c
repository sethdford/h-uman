#include "seaclaw/session.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdint.h>
#include "seaclaw/util.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>

static uint32_t hash_key(const char *key) {
    uint32_t h = 5381;
    while (*key) h = ((h << 5) + h) + (unsigned char)*key++;
    return h;
}

static sc_session_entry_t **find_bucket(sc_session_manager_t *mgr,
                                        const char *key) {
    uint32_t h = hash_key(key) % SC_SESSION_MAP_CAP;
    return &mgr->buckets[h];
}

static sc_session_t *find_session(sc_session_manager_t *mgr,
                                   const char *key) {
    sc_session_entry_t **head = find_bucket(mgr, key);
    for (sc_session_entry_t *e = *head; e; e = e->next) {
        if (strcmp(e->key, key) == 0) return e->session;
    }
    return NULL;
}

static void free_session_messages(sc_session_t *s, sc_allocator_t *alloc) {
    if (!s || !alloc) return;
    for (size_t i = 0; i < s->message_count; i++) {
        if (s->messages[i].content) {
            alloc->free(alloc->ctx, s->messages[i].content,
                       strlen(s->messages[i].content) + 1);
        }
    }
    if (s->messages) {
        alloc->free(alloc->ctx, s->messages,
                    s->message_cap * sizeof(sc_session_message_t));
    }
    s->messages = NULL;
    s->message_count = 0;
    s->message_cap = 0;
}

static void destroy_session(sc_session_t *s, sc_allocator_t *alloc) {
    free_session_messages(s, alloc);
    alloc->free(alloc->ctx, s, sizeof(sc_session_t));
}

sc_error_t sc_session_manager_init(sc_session_manager_t *mgr,
                                   sc_allocator_t *alloc) {
    if (!mgr || !alloc) return SC_ERR_INVALID_ARGUMENT;
    memset(mgr, 0, sizeof(*mgr));
    mgr->alloc = alloc;
    return SC_OK;
}

void sc_session_manager_deinit(sc_session_manager_t *mgr) {
    if (!mgr || !mgr->alloc) return;
    for (size_t i = 0; i < SC_SESSION_MAP_CAP; i++) {
        sc_session_entry_t *e = mgr->buckets[i];
        while (e) {
            sc_session_entry_t *next = e->next;
            if (e->session) destroy_session(e->session, mgr->alloc);
            mgr->alloc->free(mgr->alloc->ctx, e, sizeof(sc_session_entry_t));
            e = next;
        }
        mgr->buckets[i] = NULL;
    }
    mgr->count = 0;
}

sc_session_t *sc_session_get_or_create(sc_session_manager_t *mgr,
                                        const char *session_key) {
    if (!mgr || !mgr->alloc || !session_key) return NULL;
    sc_session_t *s = find_session(mgr, session_key);
    if (s) {
        s->last_active = (int64_t)time(NULL);
        return s;
    }
    sc_session_t *new_s = mgr->alloc->alloc(mgr->alloc->ctx,
                                             sizeof(sc_session_t));
    if (!new_s) return NULL;
    memset(new_s, 0, sizeof(*new_s));
    size_t klen = strlen(session_key);
    if (klen >= SC_SESSION_KEY_LEN) klen = SC_SESSION_KEY_LEN - 1;
    memcpy(new_s->session_key, session_key, klen);
    new_s->session_key[klen] = '\0';
    new_s->created_at = (int64_t)time(NULL);
    new_s->last_active = new_s->created_at;

    sc_session_entry_t *ent = mgr->alloc->alloc(mgr->alloc->ctx,
                                                 sizeof(sc_session_entry_t));
    if (!ent) {
        mgr->alloc->free(mgr->alloc->ctx, new_s, sizeof(sc_session_t));
        return NULL;
    }
    memset(ent, 0, sizeof(*ent));
    memcpy(ent->key, new_s->session_key, SC_SESSION_KEY_LEN);
    ent->session = new_s;
    ent->next = *find_bucket(mgr, session_key);
    *find_bucket(mgr, session_key) = ent;
    mgr->count++;
    return new_s;
}

sc_error_t sc_session_append_message(sc_session_t *s,
                                     sc_allocator_t *alloc,
                                     const char *role,
                                     const char *content) {
    if (!s || !alloc || !role || !content) return SC_ERR_INVALID_ARGUMENT;
    if (s->message_count >= s->message_cap) {
        size_t new_cap = s->message_cap ? s->message_cap * 2 : 8;
        if (new_cap > SC_SESSION_MSG_CAP) new_cap = SC_SESSION_MSG_CAP;
        sc_session_message_t *n = alloc->realloc(alloc->ctx, s->messages,
                                                 s->message_cap * sizeof(sc_session_message_t),
                                                 new_cap * sizeof(sc_session_message_t));
        if (!n) return SC_ERR_OUT_OF_MEMORY;
        s->messages = n;
        s->message_cap = new_cap;
    }
    sc_session_message_t *m = &s->messages[s->message_count++];
    size_t rlen = strlen(role);
    if (rlen >= SC_SESSION_MSG_ROLE_LEN) rlen = SC_SESSION_MSG_ROLE_LEN - 1;
    memcpy(m->role, role, rlen);
    m->role[rlen] = '\0';
    size_t clen = strlen(content) + 1;
    if (clen > SC_SESSION_MSG_CONTENT_CAP) clen = SC_SESSION_MSG_CONTENT_CAP;
    m->content = alloc->alloc(alloc->ctx, clen);
    if (!m->content) return SC_ERR_OUT_OF_MEMORY;
    memcpy(m->content, content, clen - 1);
    m->content[clen - 1] = '\0';
    s->last_active = (int64_t)time(NULL);
    s->turn_count++;
    return SC_OK;
}

char *sc_session_gen_id(sc_allocator_t *alloc) {
    if (!alloc) return NULL;
    return sc_util_gen_session_id(alloc->ctx, alloc->alloc);
}

size_t sc_session_evict_idle(sc_session_manager_t *mgr, uint64_t max_idle_secs) {
    if (!mgr || !mgr->alloc) return 0;
    int64_t now = (int64_t)time(NULL);
    size_t evicted = 0;
    for (size_t i = 0; i < SC_SESSION_MAP_CAP; i++) {
        sc_session_entry_t **pp = &mgr->buckets[i];
        while (*pp) {
            sc_session_entry_t *e = *pp;
            uint64_t idle = (uint64_t)(now - e->session->last_active);
            if (idle > max_idle_secs) {
                *pp = e->next;
                if (e->session) destroy_session(e->session, mgr->alloc);
                mgr->alloc->free(mgr->alloc->ctx, e, sizeof(sc_session_entry_t));
                mgr->count--;
                evicted++;
            } else {
                pp = &e->next;
            }
        }
    }
    return evicted;
}

size_t sc_session_count(const sc_session_manager_t *mgr) {
    return mgr ? mgr->count : 0;
}

sc_session_summary_t *sc_session_list(sc_session_manager_t *mgr,
                                       sc_allocator_t *alloc,
                                       size_t *out_count) {
    if (!mgr || !alloc || !out_count) return NULL;
    *out_count = 0;
    size_t n = mgr->count;
    if (n == 0) return NULL;

    sc_session_summary_t *arr = alloc->alloc(alloc->ctx,
        n * sizeof(sc_session_summary_t));
    if (!arr) return NULL;

    size_t idx = 0;
    for (size_t i = 0; i < SC_SESSION_MAP_CAP && idx < n; i++) {
        for (sc_session_entry_t *e = mgr->buckets[i]; e; e = e->next) {
            if (!e->session || idx >= n) break;
            sc_session_t *s = e->session;
            size_t klen = strnlen(s->session_key, SC_SESSION_KEY_LEN - 1);
            memcpy(arr[idx].session_key, s->session_key, klen);
            arr[idx].session_key[klen] = '\0';
            size_t llen = strnlen(s->label, SC_SESSION_LABEL_LEN - 1);
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

sc_error_t sc_session_delete(sc_session_manager_t *mgr, const char *session_key) {
    if (!mgr || !mgr->alloc || !session_key) return SC_ERR_INVALID_ARGUMENT;

    sc_session_entry_t **pp = find_bucket(mgr, session_key);
    for (sc_session_entry_t *e = *pp; e; pp = &e->next, e = *pp) {
        if (strcmp(e->key, session_key) != 0) continue;
        *pp = e->next;
        if (e->session) destroy_session(e->session, mgr->alloc);
        mgr->alloc->free(mgr->alloc->ctx, e, sizeof(sc_session_entry_t));
        mgr->count--;
        return SC_OK;
    }
    return SC_ERR_NOT_FOUND;
}

sc_error_t sc_session_patch(sc_session_manager_t *mgr, const char *session_key,
                            const char *label) {
    if (!mgr || !session_key) return SC_ERR_INVALID_ARGUMENT;
    if (!label) return SC_ERR_INVALID_ARGUMENT;

    sc_session_t *s = find_session(mgr, session_key);
    if (!s) return SC_ERR_NOT_FOUND;

    size_t len = strnlen(label, SC_SESSION_LABEL_LEN - 1);
    memcpy(s->label, label, len);
    s->label[len] = '\0';
    return SC_OK;
}


static void json_escape_to_f(FILE *f, const char *s) {
    for (; *s; s++) {
        switch (*s) {
            case '"': fputs("\"" , f); break;
            case '\': fputs("\\\\", f); break;
            case '
': fputs("\n", f); break;
            case '
': fputs("\r", f); break;
            case '	': fputs("\t", f); break;
            default: if ((unsigned char)*s < 0x20) fprintf(f, "\u%04x", (unsigned char)*s); else fputc(*s, f);
        }
    }
}

sc_error_t sc_session_save(sc_session_manager_t *mgr, const char *path) {
    if (!mgr || !path) return SC_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "w");
    if (!f) return SC_ERR_IO;
    fputs("[", f);
    bool first_s = true;
    for (size_t i = 0; i < SC_SESSION_MAP_CAP; i++) {
        for (sc_session_entry_t *e = mgr->buckets[i]; e; e = e->next) {
            sc_session_t *s = e->session;
            if (!s) continue;
            if (!first_s) fputs(",", f);
            first_s = false;
            fprintf(f, "
{"key":"");
            json_escape_to_f(f, s->session_key);
            fprintf(f, "","label":"");
            json_escape_to_f(f, s->label);
            fprintf(f, "","created_at":%lld,"last_active":%lld,"turn_count":%llu,"messages":[",
                (long long)s->created_at, (long long)s->last_active, (unsigned long long)s->turn_count);
            for (size_t m = 0; m < s->message_count; m++) {
                if (m > 0) fputc(',', f);
                fputs("{"role":"", f);
                json_escape_to_f(f, s->messages[m].role);
                fputs("","content":"", f);
                if (s->messages[m].content) json_escape_to_f(f, s->messages[m].content);
                fputs(""}", f);
            }
            fputs("]}", f);
        }
    }
    fputs("
]
", f);
    fclose(f);
    return SC_OK;
}

sc_error_t sc_session_load(sc_session_manager_t *mgr, const char *path) {
    if (!mgr || !mgr->alloc || !path) return SC_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "r");
    if (!f) return SC_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END); long flen = ftell(f); fseek(f, 0, SEEK_SET);
    if (flen <= 2 || flen > 10*1024*1024) { fclose(f); return SC_OK; }
    char *buf = mgr->alloc->alloc(mgr->alloc->ctx, (size_t)flen + 1);
    if (!buf) { fclose(f); return SC_ERR_OUT_OF_MEMORY; }
    size_t rd = fread(buf, 1, (size_t)flen, f); buf[rd] = ' '; fclose(f);
    sc_json_value_t *root = NULL;
    if (sc_json_parse(mgr->alloc, buf, rd, &root) != SC_OK || !root) {
        mgr->alloc->free(mgr->alloc->ctx, buf, (size_t)flen + 1); return SC_ERR_PARSE;
    }
    if (root->type != SC_JSON_ARRAY) { sc_json_free(mgr->alloc, root); mgr->alloc->free(mgr->alloc->ctx, buf, (size_t)flen + 1); return SC_OK; }
    for (size_t i = 0; i < root->data.array.len; i++) {
        sc_json_value_t *item = root->data.array.items[i];
        if (!item || item->type != SC_JSON_OBJECT) continue;
        const char *key = sc_json_get_string(item, "key");
        if (!key || !key[0]) continue;
        sc_session_t *s = sc_session_get_or_create(mgr, key);
        if (!s) continue;
        const char *label = sc_json_get_string(item, "label");
        if (label) { size_t llen = strlen(label); if (llen >= SC_SESSION_LABEL_LEN) llen = SC_SESSION_LABEL_LEN - 1; memcpy(s->label, label, llen); s->label[llen] = ' '; }
        s->created_at = (int64_t)sc_json_get_number(item, "created_at", 0);
        s->last_active = (int64_t)sc_json_get_number(item, "last_active", 0);
        s->turn_count = (uint64_t)sc_json_get_number(item, "turn_count", 0);
        sc_json_value_t *msgs = sc_json_object_get(item, "messages");
        if (msgs && msgs->type == SC_JSON_ARRAY) {
            for (size_t m = 0; m < msgs->data.array.len; m++) {
                sc_json_value_t *msg = msgs->data.array.items[m];
                if (!msg) continue;
                const char *role = sc_json_get_string(msg, "role");
                const char *content = sc_json_get_string(msg, "content");
                if (role && content) sc_session_append_message(s, mgr->alloc, role, content);
            }
        }
    }
    sc_json_free(mgr->alloc, root);
    mgr->alloc->free(mgr->alloc->ctx, buf, (size_t)flen + 1);
    return SC_OK;
}
