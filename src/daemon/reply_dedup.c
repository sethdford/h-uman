#include "human/daemon/reply_dedup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── helpers (mirror contact_send_recency.c) ─────────────────────────────── */

static size_t bounded_len(size_t chat_id_len) {
    if (chat_id_len >= HU_REPLY_DEDUP_CHAT_ID_MAX)
        return HU_REPLY_DEDUP_CHAT_ID_MAX - 1;
    return chat_id_len;
}

static int find_entry(const hu_reply_dedup_t *r, const char *chat_id, size_t stored_len) {
    for (int i = 0; i < HU_REPLY_DEDUP_CAPACITY; ++i) {
        const hu_reply_dedup_entry_t *e = &r->entries[i];
        if (!e->in_use || e->chat_id_len != stored_len)
            continue;
        if (memcmp(e->chat_id, chat_id, stored_len) == 0)
            return i;
    }
    return -1;
}

static int find_free(const hu_reply_dedup_t *r) {
    for (int i = 0; i < HU_REPLY_DEDUP_CAPACITY; ++i) {
        if (!r->entries[i].in_use)
            return i;
    }
    return -1;
}

static int find_lru(const hu_reply_dedup_t *r) {
    int lru_idx = 0;
    uint64_t lru_seq = r->entries[0].last_used_seq;
    for (int i = 1; i < HU_REPLY_DEDUP_CAPACITY; ++i) {
        if (r->entries[i].last_used_seq < lru_seq) {
            lru_seq = r->entries[i].last_used_seq;
            lru_idx = i;
        }
    }
    return lru_idx;
}

/* ── public API ──────────────────────────────────────────────────────────── */

void hu_reply_dedup_record(hu_reply_dedup_t *r, const char *chat_id, size_t chat_id_len,
                           int64_t rowid) {
    if (!r || !chat_id || rowid <= 0)
        return;

    size_t stored_len = bounded_len(chat_id_len);

    int slot = find_entry(r, chat_id, stored_len);
    if (slot < 0) {
        slot = find_free(r);
        if (slot < 0)
            slot = find_lru(r);
        memcpy(r->entries[slot].chat_id, chat_id, stored_len);
        r->entries[slot].chat_id[stored_len] = '\0';
        r->entries[slot].chat_id_len = stored_len;
        r->entries[slot].last_replied_rowid = 0;
        r->entries[slot].in_use = true;
    }

    /* Monotonic: never lower the watermark on an out-of-order replay. */
    if (rowid > r->entries[slot].last_replied_rowid)
        r->entries[slot].last_replied_rowid = rowid;
    r->entries[slot].last_used_seq = ++r->next_seq;
}

bool hu_daemon_already_replied(const hu_reply_dedup_t *r, const char *chat_id, size_t chat_id_len,
                               int64_t rowid) {
    if (!r || !chat_id || rowid <= 0)
        return false;
    size_t stored_len = bounded_len(chat_id_len);
    int slot = find_entry(r, chat_id, stored_len);
    if (slot < 0)
        return false; /* nothing recorded for this contact — proceed */
    return r->entries[slot].last_replied_rowid >= rowid;
}

/* ── persistence ─────────────────────────────────────────────────────────── */

static void write_json_escaped(FILE *f, const char *s, size_t len) {
    for (size_t i = 0; i < len && s[i]; i++) {
        char c = s[i];
        if (c == '"' || c == '\\')
            fputc('\\', f);
        fputc(c, f);
    }
}

hu_error_t hu_reply_dedup_save(const hu_reply_dedup_t *r, const char *path, size_t path_len) {
    if (!r || !path || path_len == 0 || path_len >= 480)
        return HU_ERR_INVALID_ARGUMENT;

    char path_buf[512];
    memcpy(path_buf, path, path_len);
    path_buf[path_len] = '\0';

    /* Atomic write: tmp + fflush + fsync + rename, so a crash mid-write leaves
     * the prior file intact (personal_model_save pattern). */
    char tmp[512];
    int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", path_buf);
    if (tn <= 0 || (size_t)tn >= sizeof(tmp))
        return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(tmp, "w");
    if (!f)
        return HU_ERR_IO;

    fprintf(f, "[\n");
    bool first = true;
    for (int i = 0; i < HU_REPLY_DEDUP_CAPACITY; i++) {
        const hu_reply_dedup_entry_t *e = &r->entries[i];
        if (!e->in_use)
            continue;
        fprintf(f, "%s  {\"chat\":\"", first ? "" : ",\n");
        write_json_escaped(f, e->chat_id, e->chat_id_len);
        fprintf(f, "\",\"rowid\":%lld}", (long long)e->last_replied_rowid);
        first = false;
    }
    fprintf(f, "%s]\n", first ? "" : "\n");

    if (fflush(f) != 0) {
        fclose(f);
        remove(tmp);
        return HU_ERR_IO;
    }
    int fd = fileno(f);
    if (fd >= 0)
        (void)fsync(fd);
    if (fclose(f) != 0) {
        remove(tmp);
        return HU_ERR_IO;
    }
    if (rename(tmp, path_buf) != 0) {
        remove(tmp);
        return HU_ERR_IO;
    }
    return HU_OK;
}

hu_error_t hu_reply_dedup_load(hu_reply_dedup_t *r, const char *path, size_t path_len) {
    if (!r || !path || path_len == 0 || path_len >= 480)
        return HU_ERR_INVALID_ARGUMENT;

    char path_buf[512];
    memcpy(path_buf, path, path_len);
    path_buf[path_len] = '\0';

    FILE *f = fopen(path_buf, "r");
    if (!f)
        return HU_ERR_IO; /* caller treats absent file as empty store */

    memset(r, 0, sizeof(*r));

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        const char *cstart = strstr(line, "\"chat\":\"");
        const char *rstart = strstr(line, "\"rowid\":");
        if (!cstart || !rstart)
            continue;
        cstart += 8; /* past "chat":" */

        /* Find the unescaped closing quote. */
        const char *cend = cstart;
        while (*cend && !(*cend == '"' && (cend == cstart || *(cend - 1) != '\\')))
            cend++;
        if (!*cend || cend == cstart)
            continue;
        size_t raw_clen = (size_t)(cend - cstart);

        long long rowid = 0;
        if (sscanf(rstart + 8, "%lld", &rowid) != 1 || rowid <= 0)
            continue;

        /* Unescape chat_id: \" -> ", \\ -> \ */
        char chat[HU_REPLY_DEDUP_CHAT_ID_MAX];
        size_t di = 0;
        for (size_t si = 0; si < raw_clen && di < HU_REPLY_DEDUP_CHAT_ID_MAX - 1; si++) {
            if (cstart[si] == '\\' && si + 1 < raw_clen &&
                (cstart[si + 1] == '"' || cstart[si + 1] == '\\')) {
                chat[di++] = cstart[si + 1];
                si++;
            } else {
                chat[di++] = cstart[si];
            }
        }
        chat[di] = '\0';
        if (di > 0)
            hu_reply_dedup_record(r, chat, di, (int64_t)rowid);
    }
    fclose(f);
    return HU_OK;
}
