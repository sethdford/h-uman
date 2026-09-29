#include "human/channel.h"
#include "human/daemon/share_queue.h"
#include "human/persona.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool is_link(const char *w, size_t n) {
    return (n > 8 && strncmp(w, "https://", 8) == 0) || (n > 7 && strncmp(w, "http://", 7) == 0);
}

/* Exactly one http(s) word in text[0..len) -> copied to out (trailing , . ) ! ? dropped). */
static bool single_link(const char *text, size_t len, char *out, size_t cap) {
    size_t found = 0;
    for (size_t i = 0; i < len;) {
        while (i < len && is_ws(text[i]))
            i++;
        size_t start = i;
        while (i < len && !is_ws(text[i]))
            i++;
        size_t n = i - start;
        if (n == 0 || !is_link(text + start, n))
            continue;
        if (++found > 1)
            return false;
        while (n > 0 && strchr(",.)!?", text[start + n - 1]))
            n--;
        if (n + 1 > cap)
            return false;
        memcpy(out, text + start, n);
        out[n] = '\0';
    }
    return found == 1;
}

bool hu_share_capture_parse(const char *text, size_t len, hu_share_capture_t *out) {
    if (!text || !out)
        return false;
    memset(out, 0, sizeof(*out));
    size_t i = 0;
    while (i < len && is_ws(text[i]))
        i++;
    if (len - i >= 4 && strncasecmp(text + i, "save", 4) == 0 &&
        (i + 4 == len || is_ws(text[i + 4]) || text[i + 4] == ':')) {
        /* "save <link>" */
    } else if (len - i >= 4 && strncasecmp(text + i, "for ", 4) == 0) {
        i += 4;
        while (i < len && is_ws(text[i]))
            i++;
        size_t n = 0;
        while (i + n < len && (isalnum((unsigned char)text[i + n]) || text[i + n] == '\'') &&
               n + 1 < sizeof(out->for_name))
            n++;
        if (n == 0)
            return false;
        memcpy(out->for_name, text + i, n);
        out->for_name[n] = '\0';
    } else {
        return false; /* a link on its own, or in a sentence, is conversation */
    }
    if (!single_link(text, len, out->url, sizeof(out->url))) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

bool hu_share_is_owner(const struct hu_persona *p, const char *handle, size_t len) {
    if (!p || !handle || len == 0)
        return false;
    for (size_t i = 0; i < p->contacts_count; i++) {
        const hu_contact_profile_t *c = &p->contacts[i];
        if (c->contact_id && c->relationship && strcmp(c->relationship, "test") == 0 &&
            strlen(c->contact_id) == len && memcmp(c->contact_id, handle, len) == 0)
            return true;
    }
    return false;
}

/* "mom" names a relationship; the rest name a person by first name. */
static const char *relationship_for(const char *name) {
    static const char *const map[][2] = {{"mom", "mother"}, {"mum", "mother"},  {"dad", "father"},
                                         {"sis", "sister"}, {"bro", "brother"}, {"son", "son"}};
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcasecmp(name, map[i][0]) == 0)
            return map[i][1];
    return NULL;
}

bool hu_share_resolve_contact(const struct hu_persona *p, const char *name, char *handle_out,
                              size_t cap) {
    if (!p || !name || !name[0] || !handle_out || cap == 0)
        return false;
    handle_out[0] = '\0';
    const char *rel = relationship_for(name);
    const hu_contact_profile_t *hit = NULL;
    size_t nlen = strlen(name);
    for (size_t i = 0; i < p->contacts_count; i++) {
        const hu_contact_profile_t *c = &p->contacts[i];
        if (!c->contact_id || (c->relationship && strcmp(c->relationship, "test") == 0))
            continue;
        bool first = c->name && strncasecmp(c->name, name, nlen) == 0 &&
                     (c->name[nlen] == '\0' || c->name[nlen] == ' ');
        bool by_rel = rel && c->relationship && strcasecmp(c->relationship, rel) == 0;
        if (!first && !by_rel)
            continue;
        if (hit)
            return false; /* ambiguous: ask for nothing rather than guess */
        hit = c;
    }
    if (!hit || strlen(hit->contact_id) + 1 > cap)
        return false;
    memcpy(handle_out, hit->contact_id, strlen(hit->contact_id) + 1);
    return true;
}

/* Line: ts \t new|sent \t handle-or-empty \t url \n */
hu_error_t hu_share_queue_add(const char *path, int64_t ts, const char *url,
                              const char *for_handle) {
    if (!path || !url || !url[0] || strpbrk(url, "\t\n") ||
        (for_handle && strpbrk(for_handle, "\t\n")))
        return HU_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "a");
    if (!f)
        return HU_ERR_IO;
    int n = fprintf(f, "%lld\tnew\t%s\t%s\n", (long long)ts, for_handle ? for_handle : "", url);
    int c = fclose(f);
    return (n > 0 && c == 0) ? HU_OK : HU_ERR_IO;
}

typedef struct {
    char status[8];
    char handle[128];
    char url[512];
} share_line_t;

static bool parse_line(char *line, share_line_t *l) {
    char *save = NULL;
    char *ts = strtok_r(line, "\t", &save);
    char *st = strtok_r(NULL, "\t", &save);
    char *rest = save; /* handle may be empty: split the remainder by hand */
    if (!ts || !st || !rest)
        return false;
    char *tab = strchr(rest, '\t');
    if (!tab)
        return false;
    *tab = '\0';
    char *url = tab + 1;
    url[strcspn(url, "\n")] = '\0';
    snprintf(l->status, sizeof(l->status), "%s", st);
    snprintf(l->handle, sizeof(l->handle), "%s", rest);
    snprintf(l->url, sizeof(l->url), "%s", url);
    return l->url[0] != '\0';
}

bool hu_share_queue_next(const char *path, const char *handle, char *url_out, size_t cap) {
    if (!path || !handle || !url_out || cap == 0)
        return false;
    url_out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[1024], anyone[512] = "";
    bool tagged = false;
    share_line_t l;
    while (!tagged && fgets(line, sizeof(line), f)) {
        if (!parse_line(line, &l) || strcmp(l.status, "new") != 0)
            continue;
        if (strcmp(l.handle, handle) == 0) {
            snprintf(url_out, cap, "%s", l.url);
            tagged = true;
        } else if (!l.handle[0] && !anyone[0]) {
            snprintf(anyone, sizeof(anyone), "%s", l.url);
        }
    }
    fclose(f);
    if (!tagged && anyone[0])
        snprintf(url_out, cap, "%s", anyone);
    return url_out[0] != '\0';
}

hu_error_t hu_share_queue_mark_sent(const char *path, const char *url) {
    if (!path || !url || !url[0])
        return HU_ERR_INVALID_ARGUMENT;
    FILE *in = fopen(path, "r");
    if (!in)
        return HU_ERR_IO;
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *out = fopen(tmp, "w");
    if (!out) {
        fclose(in);
        return HU_ERR_IO;
    }
    char line[1024], copy[1024];
    bool marked = false;
    share_line_t l;
    while (fgets(line, sizeof(line), in)) {
        memcpy(copy, line, sizeof(copy));
        if (!marked && parse_line(copy, &l) && strcmp(l.status, "new") == 0 &&
            strcmp(l.url, url) == 0) {
            char *tab = strchr(line, '\t');
            if (tab && strncmp(tab + 1, "new\t", 4) == 0) {
                fwrite(line, 1, (size_t)(tab + 1 - line), out);
                fputs("sent", out);
                fputs(tab + 4, out);
                marked = true;
                continue;
            }
        }
        fputs(line, out);
    }
    fclose(in);
    if (fclose(out) != 0) {
        remove(tmp);
        return HU_ERR_IO;
    }
    return rename(tmp, path) == 0 ? HU_OK : HU_ERR_IO; /* atomic swap */
}

bool hu_share_capture_handle(const struct hu_persona *p, const char *sender, size_t sender_len,
                             const char *text, size_t len, const char *queue_path, int64_t now,
                             char *ack, size_t ack_cap) {
    if (!queue_path || !ack || ack_cap == 0 || !hu_share_is_owner(p, sender, sender_len))
        return false;
    hu_share_capture_t c;
    if (!hu_share_capture_parse(text, len, &c))
        return false;
    char handle[128] = "";
    bool tagged = c.for_name[0] && hu_share_resolve_contact(p, c.for_name, handle, sizeof(handle));
    if (hu_share_queue_add(queue_path, now, c.url, tagged ? handle : "") != HU_OK)
        return false;
    if (tagged) {
        c.for_name[0] = (char)toupper((unsigned char)c.for_name[0]);
        snprintf(ack, ack_cap, "saved for %s \xF0\x9F\x91\x8D", c.for_name);
    } else if (c.for_name[0]) {
        snprintf(ack, ack_cap, "saved, didn't know who \"%s\" is so it's for anyone", c.for_name);
    } else {
        snprintf(ack, ack_cap, "saved \xF0\x9F\x91\x8D");
    }
    return true;
}

bool hu_share_send_saved(struct hu_channel *ch, const char *target, size_t target_len,
                         const char *queue_path) {
    if (!ch || !ch->vtable || !ch->vtable->send || !target || target_len == 0 ||
        target_len >= 128 || !queue_path)
        return false;
    char handle[128];
    memcpy(handle, target, target_len);
    handle[target_len] = '\0';
    char url[512];
    if (!hu_share_queue_next(queue_path, handle, url, sizeof(url)))
        return false;
    /* Exactly the URL, alone: that is the bubble iMessage turns into a card. */
    if (ch->vtable->send(ch->ctx, target, target_len, url, strlen(url), NULL, 0) != HU_OK)
        return false;
    (void)hu_share_queue_mark_sent(queue_path, url);
    return true;
}
