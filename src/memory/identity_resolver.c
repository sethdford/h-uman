/*
 * identity_resolver — see include/human/memory/identity_resolver.h
 *
 * Algorithm:
 *   1. For each input handle, compute a "strong canonical key" if
 *      possible (phone canonical or email canonical). Slack/Discord/
 *      Telegram/raw IDs have no strong key.
 *   2. Union-find over input indices:
 *        - Strong-key match between two handles → merge with HIGH.
 *        - No strong key on either side, but display-name first-token
 *          match → merge with LOW.
 *        - A handle with a strong key + a handle without, joined only
 *          by display-name first-token → merge with LOW.
 *   3. Each contact's merge_confidence is the WEAKEST union-merge in
 *      its chain. A contact built from a single alias has confidence
 *      NONE (no merge happened).
 *
 * Privacy defaults:
 *   - "alice" matching "alice" on two different opaque platforms with
 *     no other signal stays at LOW. Callers must NOT use LOW contacts
 *     for fact unification.
 *   - Same first-name + different strong canonical keys (different
 *     phones / different gmail addresses) → DO NOT merge. The strong
 *     keys being different is treated as a "they're different people"
 *     signal.
 */

#include "human/memory/identity_resolver.h"
#include "human/core/error.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* On-disk header. Keeping it tiny — the layout is opaque to outside
 * code; bumping HU_IDENTITY_FORMAT_VERSION forces a clean load (we
 * return HU_ERR_PARSE rather than attempting migration; the graph
 * is rebuildable from handle lists so a one-time loss is acceptable). */
#define HU_IDENTITY_MAGIC          0x49444E54u /* 'IDNT' */
#define HU_IDENTITY_FORMAT_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t reserved0;
    uint32_t reserved1;
} hu_identity_header_t;

/* -------------------- canonicalization -------------------- */

size_t hu_identity_canonicalize_phone(const char *input, char *out, size_t cap) {
    if (!input || !out || cap == 0)
        return 0;
    out[0] = '\0';

    char digits[64];
    size_t n = 0;
    for (size_t i = 0; input[i] && n < sizeof(digits) - 1; i++) {
        if (input[i] >= '0' && input[i] <= '9')
            digits[n++] = input[i];
    }
    digits[n] = '\0';

    if (n < 7) {
        return 0; /* too short to be a real phone */
    }

    /* If >=11 digits, drop the leading country-code prefix and keep
     * the last 10. If exactly 10, use as-is. If 7..9, use as-is — but
     * such short numbers are weak matches; callers will see the same
     * key only if both sides have the same trailing digits. */
    const char *tail = digits;
    size_t tail_len = n;
    if (n > 10) {
        tail = digits + (n - 10);
        tail_len = 10;
    }

    if (tail_len + 1 > cap)
        tail_len = cap - 1;
    memcpy(out, tail, tail_len);
    out[tail_len] = '\0';
    return tail_len;
}

static int looks_like_phone(const char *s) {
    if (!s || !*s)
        return 0;
    size_t digits = 0;
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isdigit(c))
            digits++;
        else if (c == '+' || c == '-' || c == ' ' || c == '(' || c == ')' || c == '.')
            continue; /* phone-formatting characters; tolerate but don't count */
        else
            return 0; /* any letter or other char → not a phone */
    }
    return digits >= 7;
}

static int is_gmail_domain(const char *domain) {
    if (!domain)
        return 0;
    /* case-insensitive compare against gmail.com / googlemail.com */
    return strcasecmp(domain, "gmail.com") == 0 || strcasecmp(domain, "googlemail.com") == 0;
}

size_t hu_identity_canonicalize_email(const char *input, char *out, size_t cap) {
    if (!input || !out || cap == 0)
        return 0;
    out[0] = '\0';

    const char *at = strchr(input, '@');
    if (!at || at == input || !*(at + 1))
        return 0; /* not an email */

    /* Find a second '@' — definitely not an email if present. */
    if (strchr(at + 1, '@'))
        return 0;

    /* Lowercase + dot-strip-if-gmail in the local part. */
    char local[128];
    size_t li = 0;
    int gmail = is_gmail_domain(at + 1);
    for (const char *p = input; p < at && li < sizeof(local) - 1; p++) {
        char c = (char)tolower((unsigned char)*p);
        if (gmail && c == '.')
            continue;
        /* Strip gmail "+suffix" address tags: anything after '+' before '@'
         * routes to the same mailbox in Gmail. */
        if (gmail && c == '+') {
            /* skip the rest of the local part */
            while (p + 1 < at)
                p++;
            break;
        }
        local[li++] = c;
    }
    local[li] = '\0';

    if (li == 0)
        return 0;

    /* Lowercase the domain. */
    char domain[128];
    size_t di = 0;
    for (const char *p = at + 1; *p && di < sizeof(domain) - 1; p++) {
        domain[di++] = (char)tolower((unsigned char)*p);
    }
    domain[di] = '\0';

    int wrote = snprintf(out, cap, "%s@%s", local, domain);
    if (wrote < 0)
        return 0;
    if ((size_t)wrote >= cap)
        return cap - 1;
    return (size_t)wrote;
}

/* Extract the leading first-name token from a display name, lowercased.
 * "Alice Smith" → "alice"; "alice" → "alice"; "" → "". */
static size_t first_name_token(const char *name, char *out, size_t cap) {
    if (!name || !out || cap == 0) {
        if (out && cap > 0)
            out[0] = '\0';
        return 0;
    }
    /* Skip leading whitespace. */
    while (*name && isspace((unsigned char)*name))
        name++;
    size_t n = 0;
    while (*name && !isspace((unsigned char)*name) && n + 1 < cap) {
        out[n++] = (char)tolower((unsigned char)*name);
        name++;
    }
    out[n] = '\0';
    return n;
}

/* -------------------- union-find over input indices -------------------- */

typedef struct {
    /* Strong-key canonical form: phone (10 digits) or email canonical
     * form. Empty if no strong key for this input. */
    char strong_key[128];
    int strong_kind; /* 0 = none, 1 = phone, 2 = email */

    /* First-name token (lowercased) — '\0' if no display name provided. */
    char name_token[64];

    size_t parent;
    /* Minimum confidence step recorded for this node's merge-chain to
     * the root. Updated whenever a union-merge happens with a lower
     * confidence than the current value. */
    hu_identity_confidence_t min_step;
} hu_identity_node_t;

static size_t uf_find(hu_identity_node_t *nodes, size_t i) {
    while (nodes[i].parent != i) {
        nodes[i].parent = nodes[nodes[i].parent].parent;
        i = nodes[i].parent;
    }
    return i;
}

/* Union i and j with the given confidence. The root's min_step is
 * the smallest min_step in the merged set, clamped further by the
 * incoming `step` (each merge contributes at least one step at its
 * confidence level). */
static void uf_union(hu_identity_node_t *nodes, size_t i, size_t j, hu_identity_confidence_t step) {
    size_t ri = uf_find(nodes, i);
    size_t rj = uf_find(nodes, j);
    if (ri == rj) {
        /* Same set already, but this merge edge may have lower
         * confidence than existing ones. Track the weakest. */
        if (step != HU_IDENTITY_CONFIDENCE_NONE && step < nodes[ri].min_step)
            nodes[ri].min_step = step;
        return;
    }
    /* Pick smaller index as root for determinism. */
    size_t root = ri < rj ? ri : rj;
    size_t child = ri < rj ? rj : ri;

    hu_identity_confidence_t new_min = nodes[ri].min_step;
    if (nodes[rj].min_step != HU_IDENTITY_CONFIDENCE_NONE &&
        (new_min == HU_IDENTITY_CONFIDENCE_NONE || nodes[rj].min_step < new_min))
        new_min = nodes[rj].min_step;
    if (step != HU_IDENTITY_CONFIDENCE_NONE &&
        (new_min == HU_IDENTITY_CONFIDENCE_NONE || step < new_min))
        new_min = step;

    nodes[child].parent = root;
    nodes[root].min_step = new_min;
}

/* -------------------- helpers -------------------- */

static void safe_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void ensure_parent_dir(const char *path) {
    if (!path || !*path)
        return;
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path)
        return;
    size_t plen = (size_t)(slash - path);
    char buf[1024];
    if (plen + 1 >= sizeof(buf))
        return;
    memcpy(buf, path, plen);
    buf[plen] = '\0';
    for (size_t i = 1; i < plen; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            (void)mkdir(buf, 0700);
            buf[i] = '/';
        }
    }
    (void)mkdir(buf, 0700);
}

/* -------------------- main resolve -------------------- */

hu_error_t hu_identity_resolve(const char *const *handles, const char *const *channels,
                               const char *const *display_names, size_t handle_count,
                               hu_identity_graph_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (handle_count == 0)
        return HU_OK;
    if (!handles || !channels)
        return HU_ERR_INVALID_ARGUMENT;

    /* Clamp to MAX_CONTACTS (each handle becomes at most one root). */
    size_t n = handle_count;
    if (n > HU_IDENTITY_MAX_CONTACTS)
        n = HU_IDENTITY_MAX_CONTACTS;

    /* Allocate nodes on stack — sized by MAX_CONTACTS (256), fits
     * comfortably in any thread stack. */
    hu_identity_node_t nodes[HU_IDENTITY_MAX_CONTACTS];
    memset(nodes, 0, sizeof(nodes));

    for (size_t i = 0; i < n; i++) {
        nodes[i].parent = i;
        nodes[i].min_step = HU_IDENTITY_CONFIDENCE_NONE;
        const char *h = handles[i];
        if (!h)
            h = "";

        /* Strong-key extraction: try email first (presence of '@' is a
         * strong signal), then phone. */
        if (strchr(h, '@')) {
            size_t k =
                hu_identity_canonicalize_email(h, nodes[i].strong_key, sizeof(nodes[i].strong_key));
            if (k > 0)
                nodes[i].strong_kind = 2;
        } else if (looks_like_phone(h)) {
            size_t k =
                hu_identity_canonicalize_phone(h, nodes[i].strong_key, sizeof(nodes[i].strong_key));
            if (k > 0)
                nodes[i].strong_kind = 1;
        }

        const char *name = (display_names && display_names[i]) ? display_names[i] : NULL;
        first_name_token(name, nodes[i].name_token, sizeof(nodes[i].name_token));
    }

    /* Pass 1: union by strong-key equality. HIGH confidence.
     * Strong keys of different KIND (phone vs email) never match. */
    for (size_t i = 0; i < n; i++) {
        if (nodes[i].strong_kind == 0)
            continue;
        for (size_t j = i + 1; j < n; j++) {
            if (nodes[j].strong_kind != nodes[i].strong_kind)
                continue;
            if (strcmp(nodes[i].strong_key, nodes[j].strong_key) == 0)
                uf_union(nodes, i, j, HU_IDENTITY_CONFIDENCE_HIGH);
        }
    }

    /* Pass 2: union by display-name first-token. LOW confidence.
     * Privacy guard: do NOT bridge two handles that BOTH have
     * strong keys of the same kind but DIFFERENT strong keys —
     * that pair is treated as evidence they're different people. */
    for (size_t i = 0; i < n; i++) {
        if (!nodes[i].name_token[0])
            continue;
        for (size_t j = i + 1; j < n; j++) {
            if (!nodes[j].name_token[0])
                continue;
            if (strcmp(nodes[i].name_token, nodes[j].name_token) != 0)
                continue;

            /* Both have a strong key, same kind, but different values
             * → explicit evidence of distinct people. Skip. */
            if (nodes[i].strong_kind != 0 && nodes[j].strong_kind != 0 &&
                nodes[i].strong_kind == nodes[j].strong_kind &&
                strcmp(nodes[i].strong_key, nodes[j].strong_key) != 0) {
                continue;
            }
            /* Both have the SAME strong key (same kind + same value) →
             * they were already HIGH-merged in Pass 1. A LOW name match
             * here would only corroborate the existing strong merge;
             * recording it would (incorrectly) downgrade min_step to
             * LOW. Skip — name match adds no new info. */
            if (nodes[i].strong_kind != 0 && nodes[j].strong_kind != 0 &&
                nodes[i].strong_kind == nodes[j].strong_kind &&
                strcmp(nodes[i].strong_key, nodes[j].strong_key) == 0) {
                continue;
            }
            /* Cross-kind strong keys (one email, one phone) with the
             * same display name: that's a LOW bridge. */
            uf_union(nodes, i, j, HU_IDENTITY_CONFIDENCE_LOW);
        }
    }

    /* Pass 3: emit contacts. One contact per union root. */
    size_t root_to_contact[HU_IDENTITY_MAX_CONTACTS];
    for (size_t i = 0; i < HU_IDENTITY_MAX_CONTACTS; i++)
        root_to_contact[i] = (size_t)-1;

    for (size_t i = 0; i < n; i++) {
        size_t r = uf_find(nodes, i);
        size_t cidx = root_to_contact[r];
        if (cidx == (size_t)-1) {
            if (out->contact_count >= HU_IDENTITY_MAX_CONTACTS)
                continue;
            cidx = out->contact_count++;
            root_to_contact[r] = cidx;
            memset(&out->contacts[cidx], 0, sizeof(out->contacts[cidx]));

            /* Canonical name: prefer a non-empty display_name on the
             * root index, otherwise the first non-empty name in the
             * group seen as we walk. For now, seed from this entry. */
            const char *seed_name = (display_names && display_names[i]) ? display_names[i] : NULL;
            if (!seed_name || !*seed_name)
                seed_name = handles[i];
            safe_copy(out->contacts[cidx].canonical_name,
                      sizeof(out->contacts[cidx].canonical_name), seed_name);
            out->contacts[cidx].merge_confidence = nodes[r].min_step;
        }

        /* Append alias if room. */
        hu_identity_contact_t *c = &out->contacts[cidx];
        if (c->alias_count < HU_IDENTITY_MAX_ALIASES) {
            safe_copy(c->aliases[c->alias_count], sizeof(c->aliases[c->alias_count]),
                      handles[i] ? handles[i] : "");
            safe_copy(c->alias_channels[c->alias_count], sizeof(c->alias_channels[c->alias_count]),
                      channels[i] ? channels[i] : "");
            c->alias_count++;
        }
        /* If a later alias in the same contact has a non-empty
         * display name and the contact's current canonical_name is
         * just a handle (no whitespace, looks like an ID), prefer
         * the human label. */
        if (display_names && display_names[i] && display_names[i][0]) {
            const char *cur = c->canonical_name;
            int cur_looks_human = 0;
            for (size_t k = 0; cur[k]; k++) {
                if (cur[k] == ' ') {
                    cur_looks_human = 1;
                    break;
                }
            }
            if (!cur_looks_human) {
                /* Replace only if we haven't yet captured a real name. */
                if (!strchr(cur, ' ')) {
                    safe_copy(c->canonical_name, sizeof(c->canonical_name), display_names[i]);
                }
            }
        }
    }

    /* Single-alias contacts must report NONE — no merge happened.
     * (uf_find on a never-unioned root leaves min_step = NONE.) */
    for (size_t ci = 0; ci < out->contact_count; ci++) {
        if (out->contacts[ci].alias_count <= 1)
            out->contacts[ci].merge_confidence = HU_IDENTITY_CONFIDENCE_NONE;
    }

    return HU_OK;
}

const hu_identity_contact_t *hu_identity_lookup(const hu_identity_graph_t *graph,
                                                const char *handle) {
    if (!graph || !handle)
        return NULL;
    for (size_t ci = 0; ci < graph->contact_count; ci++) {
        const hu_identity_contact_t *c = &graph->contacts[ci];
        for (size_t ai = 0; ai < c->alias_count; ai++) {
            if (strcmp(c->aliases[ai], handle) == 0)
                return c;
        }
    }
    return NULL;
}

/* -------------------- persistence -------------------- */

hu_error_t hu_identity_save(const hu_identity_graph_t *graph, const char *path) {
    if (!graph || !path || !*path)
        return HU_ERR_INVALID_ARGUMENT;
    ensure_parent_dir(path);

    char tmp[1024];
    int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (tn < 0 || (size_t)tn >= sizeof(tmp))
        return HU_ERR_INVALID_ARGUMENT;

    FILE *fp = fopen(tmp, "wb");
    if (!fp)
        return HU_ERR_IO;

    hu_identity_header_t hdr = {.magic = HU_IDENTITY_MAGIC,
                                .version = HU_IDENTITY_FORMAT_VERSION,
                                .reserved0 = 0,
                                .reserved1 = 0};
    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1 || fwrite(graph, sizeof(*graph), 1, fp) != 1) {
        fclose(fp);
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    if (fflush(fp) != 0) {
        fclose(fp);
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    int fd = fileno(fp);
    if (fd >= 0 && fsync(fd) != 0) {
        fclose(fp);
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    if (fclose(fp) != 0) {
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    if (rename(tmp, path) != 0) {
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    return HU_OK;
}

hu_error_t hu_identity_load(hu_identity_graph_t *out, const char *path) {
    if (!out || !path || !*path)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return HU_ERR_NOT_FOUND;
    hu_identity_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) {
        fclose(fp);
        return HU_ERR_PARSE;
    }
    if (hdr.magic != HU_IDENTITY_MAGIC || hdr.version != HU_IDENTITY_FORMAT_VERSION) {
        fclose(fp);
        return HU_ERR_PARSE;
    }
    hu_identity_graph_t tmp;
    if (fread(&tmp, sizeof(tmp), 1, fp) != 1) {
        fclose(fp);
        memset(out, 0, sizeof(*out));
        return HU_ERR_PARSE;
    }
    fclose(fp);
    /* Defensive clamps: a corrupted file shouldn't crash callers. */
    if (tmp.contact_count > HU_IDENTITY_MAX_CONTACTS)
        tmp.contact_count = HU_IDENTITY_MAX_CONTACTS;
    for (size_t i = 0; i < tmp.contact_count; i++) {
        if (tmp.contacts[i].alias_count > HU_IDENTITY_MAX_ALIASES)
            tmp.contacts[i].alias_count = HU_IDENTITY_MAX_ALIASES;
    }
    *out = tmp;
    return HU_OK;
}
