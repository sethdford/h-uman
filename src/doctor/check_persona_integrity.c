/* src/doctor/check_persona_integrity.c — see the header for the incident. */
#include "human/doctor/check_persona_integrity.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/file.h"
#include "human/core/json.h"
#include "human/doctor/check.h"
#include "human/persona.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static char s_reason_buf[768];
static char s_detail_json_buf[512];

/* A persona file large enough to hold years of contacts and banks is well under
 * this; anything bigger is not a persona we should slurp into a doctor run. */
#define HU_DOCTOR_PERSONA_MAX_BYTES (8ul * 1024ul * 1024ul)

typedef struct persona_shape {
    size_t keys;
    size_t contacts;
    bool has_proactive;
    hu_json_value_t *root; /* owned; NULL when unreadable */
} persona_shape_t;

static hu_error_t read_json_file(hu_allocator_t *alloc, const char *path, hu_json_value_t **out) {
    *out = NULL;
    char *buf = NULL;
    size_t got = 0;
    hu_error_t err = hu_file_slurp(alloc, path, HU_DOCTOR_PERSONA_MAX_BYTES, &buf, &got);
    if (err != HU_OK)
        return err;
    if (got == 0) {
        alloc->free(alloc->ctx, buf, 1);
        return HU_ERR_INVALID_FORMAT;
    }
    err = hu_json_parse(alloc, buf, got, out);
    alloc->free(alloc->ctx, buf, got + 1);
    if (err == HU_OK && (!*out || (*out)->type != HU_JSON_OBJECT)) {
        if (*out)
            hu_json_free(alloc, *out);
        *out = NULL;
        return HU_ERR_INVALID_FORMAT;
    }
    return err;
}

static size_t count_contacts(const hu_json_value_t *root) {
    const hu_json_value_t *c = hu_json_object_get(root, "contacts");
    if (!c)
        return 0;
    if (c->type == HU_JSON_OBJECT)
        return c->data.object.len;
    if (c->type == HU_JSON_ARRAY)
        return c->data.array.len;
    return 0;
}

static void shape_of(persona_shape_t *s, hu_json_value_t *root) {
    s->root = root;
    s->keys = root ? root->data.object.len : 0;
    s->contacts = root ? count_contacts(root) : 0;
    s->has_proactive = root && hu_json_object_get(root, "proactive") != NULL;
}

/* Append up to `max` top-level keys of `from` that `live` lacks. */
static void list_lost_keys(const hu_json_value_t *from, const hu_json_value_t *live, char *out,
                           size_t cap, size_t max) {
    size_t n = 0, pos = 0;
    out[0] = '\0';
    for (size_t i = 0; i < from->data.object.len && n < max; i++) {
        const char *k = from->data.object.pairs[i].key;
        if (!k || hu_json_object_get(live, k))
            continue;
        int w = snprintf(out + pos, cap > pos ? cap - pos : 0, "%s%s", n ? ", " : "", k);
        if (w < 0 || (size_t)w >= cap - pos)
            break;
        pos += (size_t)w;
        n++;
    }
}

static hu_doctor_check_result_t check_persona_integrity_run(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_check_result_t r = {.verdict = HU_DOCTOR_NA, .reason = NULL, .detail_json = NULL};
    const hu_doctor_check_persona_integrity_ctx_t *c =
        (const hu_doctor_check_persona_integrity_ctx_t *)ctx;
    const char *name = NULL;
    if (c && c->persona_name && c->persona_name[0])
        name = c->persona_name;
    else if (c && c->cfg && c->cfg->agent.persona && c->cfg->agent.persona[0])
        name = c->cfg->agent.persona;
    if (!name) {
        r.reason = "no persona configured (agent.persona)";
        return r;
    }

    char base[512];
    if (!hu_persona_base_dir(base, sizeof(base))) {
        r.verdict = HU_DOCTOR_FAIL;
        r.reason = "persona base dir unresolved";
        return r;
    }
    char live_path[768];
    snprintf(live_path, sizeof(live_path), "%s/%s.json", base, name);

    hu_allocator_t alloc = hu_system_allocator();
    persona_shape_t live = {0};
    hu_json_value_t *live_root = NULL;
    hu_error_t lerr = read_json_file(&alloc, live_path, &live_root);
    if (lerr != HU_OK) {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "persona '%s' file %s (%s) — the daemon runs with no identity", name,
                 lerr == HU_ERR_NOT_FOUND ? "missing" : "unreadable/unparsable", live_path);
        r.verdict = HU_DOCTOR_FAIL;
        r.reason = s_reason_buf;
        return r;
    }
    shape_of(&live, live_root);

    /* Reference backup: the NEWEST sibling `<name>.json.<anything>` by mtime,
     * ties broken by key count. Newest, not largest: the persona schema has
     * been pruned on purpose more than once (a 2026-05 `pre-v2-refresh` copy
     * still carries 35 keys the current schema dropped), and measuring
     * against that would flag every healthy file forever. The incident shape
     * is "the copy taken just before this write still has what live lost". */
    persona_shape_t best = {0};
    char best_name[256] = "";
    time_t best_mtime = 0;
    char prefix[300];
    int plen = snprintf(prefix, sizeof(prefix), "%s.json.", name);
    DIR *d = opendir(base);
    if (d && plen > 0) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, prefix, (size_t)plen) != 0)
                continue;
            if (strstr(e->d_name, ".tmp-"))
                continue; /* an in-flight writer temp, never a backup */
            char p[1024];
            snprintf(p, sizeof(p), "%s/%s", base, e->d_name);
            struct stat st;
            if (stat(p, &st) != 0)
                continue;
            hu_json_value_t *root = NULL;
            if (read_json_file(&alloc, p, &root) != HU_OK)
                continue;
            persona_shape_t s;
            shape_of(&s, root);
            bool better =
                !best.root || st.st_mtime > best_mtime ||
                (st.st_mtime == best_mtime &&
                 (s.keys > best.keys || (s.keys == best.keys && s.contacts > best.contacts)));
            if (better)
                best_mtime = st.st_mtime;
            if (better) {
                if (best.root)
                    hu_json_free(&alloc, best.root);
                best = s;
                snprintf(best_name, sizeof(best_name), "%s", e->d_name);
            } else {
                hu_json_free(&alloc, root);
            }
        }
        closedir(d);
    }

    bool lost_contacts = live.contacts == 0 && best.root && best.contacts > 0;
    bool lost_keys = best.root && best.keys >= live.keys + HU_DOCTOR_PERSONA_MAX_LOST_KEYS;

    snprintf(s_detail_json_buf, sizeof(s_detail_json_buf),
             "{\"persona\":\"%s\",\"live_keys\":%zu,\"live_contacts\":%zu,\"proactive\":%s,"
             "\"backup\":\"%s\",\"backup_keys\":%zu,\"backup_contacts\":%zu}",
             name, live.keys, live.contacts, live.has_proactive ? "true" : "false", best_name,
             best.keys, best.contacts);
    r.detail_json = s_detail_json_buf;

    if (lost_contacts || lost_keys) {
        char lost[256];
        list_lost_keys(best.root, live.root, lost, sizeof(lost), 8);
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "persona '%s' has %zu keys / %zu contacts but backup %s has %zu keys / %zu "
                 "contacts — lost: %s. Every proactive path reads these; restore by merging the "
                 "missing keys back (never copy the backup over live edits)",
                 name, live.keys, live.contacts, best_name, best.keys, best.contacts, lost);
        r.verdict = HU_DOCTOR_FAIL;
        r.reason = s_reason_buf;
    } else {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "persona '%s': %zu keys, %zu contacts, proactive block %s%s%s", name, live.keys,
                 live.contacts, live.has_proactive ? "present" : "absent",
                 best.root ? " (newest backup " : "", best.root ? best_name : "");
        if (best.root) {
            size_t l = strlen(s_reason_buf);
            snprintf(s_reason_buf + l, sizeof(s_reason_buf) - l, " = %zu)", best.keys);
        }
        r.verdict = HU_DOCTOR_PASS;
        r.reason = s_reason_buf;
    }

    if (best.root)
        hu_json_free(&alloc, best.root);
    hu_json_free(&alloc, live_root);
    return r;
}

hu_doctor_check_t hu_doctor_check_persona_integrity = {
    .name = "persona_integrity",
    .description =
        "Live persona has not lost authored keys (contacts/proactive/...) vs its backups",
    .run = check_persona_integrity_run,
    .fix = NULL, /* restoring is a key-merge an operator must review */
    .user_data = NULL,
};
