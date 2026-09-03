/* semantic_recall.c — gate + wiring for the real semantic retriever. */
#include "human/memory/semantic_recall.h"

#include "human/core/string.h"
#include "human/memory/vector/embedder_http.h"
#include "human/memory/vector/store_sqlite_vec.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

size_t hu_semantic_recall_max_bytes(void) {
    const char *v = getenv("HU_SEMANTIC_RECALL_MAX_BYTES");
    if (!v || !v[0])
        return HU_SEMANTIC_RECALL_DEFAULT_MAX_BYTES;
    char *end = NULL;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (errno != 0 || end == v || (end && *end != '\0') || n <= 0)
        return HU_SEMANTIC_RECALL_DEFAULT_MAX_BYTES; /* fail closed to the default */
    return (size_t)n;
}

size_t hu_semantic_recall_truncate_len(const char *s, size_t len, size_t max_bytes) {
    if (!s || max_bytes == 0)
        return 0;
    if (len <= max_bytes)
        return len;
    /* Prefer the last space in the upper half of the window: s[0, i) is then
     * whole words. A boundary further back would discard too much. */
    for (size_t i = max_bytes; i > max_bytes / 2; i--) {
        if (s[i] == ' ' || s[i] == '\n' || s[i] == '\t') {
            size_t cut = i;
            while (cut > 0 && (s[cut - 1] == ' ' || s[cut - 1] == '\n' || s[cut - 1] == '\t'))
                cut--;
            return cut;
        }
    }
    /* Hard cut: never split a multi-byte UTF-8 sequence. */
    size_t cut = max_bytes;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0u) == 0x80u)
        cut--;
    return cut;
}

/* Shrink entries/scores from `old` to `keep` slots (the dropped tail must
 * already be freed and zeroed). keep == 0 frees both arrays and NULLs them.
 * Both shipped allocators ignore old_size on free, so a failed shrink (kept
 * larger buffer, smaller count) is tolerated rather than fatal — this is
 * what keeps hu_retrieval_result_free's sized frees exact. */
static void result_shrink(hu_allocator_t *alloc, hu_retrieval_result_t *res, size_t old,
                          size_t keep) {
    if (keep == 0) {
        alloc->free(alloc->ctx, res->entries, old * sizeof(hu_memory_entry_t));
        if (res->scores)
            alloc->free(alloc->ctx, res->scores, old * sizeof(double));
        res->entries = NULL;
        res->scores = NULL;
        res->count = 0;
        return;
    }
    hu_memory_entry_t *ne = (hu_memory_entry_t *)alloc->realloc(alloc->ctx, res->entries,
                                                                old * sizeof(hu_memory_entry_t),
                                                                keep * sizeof(hu_memory_entry_t));
    if (ne)
        res->entries = ne;
    if (res->scores) {
        double *ns = (double *)alloc->realloc(alloc->ctx, res->scores, old * sizeof(double),
                                              keep * sizeof(double));
        if (ns)
            res->scores = ns;
    }
    res->count = keep;
}

size_t hu_semantic_recall_clamp_result(hu_allocator_t *alloc, hu_retrieval_result_t *res,
                                       size_t budget_bytes, size_t per_hit_bytes) {
    if (!alloc || !res || !res->entries || res->count == 0)
        return 0;
    size_t used = 0, keep = 0;
    for (size_t i = 0; i < res->count; i++) {
        hu_memory_entry_t *e = &res->entries[i];
        size_t clen = e->content ? e->content_len : 0;
        size_t cut =
            e->content ? hu_semantic_recall_truncate_len(e->content, clen, per_hit_bytes) : 0;
        if (used + cut > budget_bytes)
            break; /* this hit and every lower-ranked one are dropped */
        if (cut < clen) {
            /* Binary-safe copy: content may carry embedded NULs (the
             * 2026-07-13 memory-loader overflow), so hu_strndup — which stops
             * at the first NUL — would allocate fewer than cut+1 bytes while
             * content_len still claims cut. Copy exactly cut bytes. */
            char *nc = (char *)alloc->alloc(alloc->ctx, cut + 1);
            if (!nc)
                break;
            memcpy(nc, e->content, cut);
            nc[cut] = '\0';
            alloc->free(alloc->ctx, (void *)e->content, clen + 1);
            e->content = nc;
            e->content_len = cut;
        }
        used += cut;
        keep++;
    }
    if (keep == res->count)
        return used;
    for (size_t i = keep; i < res->count; i++) {
        hu_memory_entry_free_fields(alloc, &res->entries[i]);
        memset(&res->entries[i], 0, sizeof(res->entries[i])); /* no dangling pointers */
    }
    result_shrink(alloc, res, res->count, keep);
    return used;
}

/* ── Index + recall content policy (see semantic_recall.h) ─────────────── */

#define HU_SEMANTIC_EXPERIENCE_KEY_PREFIX "experience:"

bool hu_semantic_recall_key_is_indexable(const char *key, size_t key_len) {
    if (!key || key_len == 0)
        return false;
    const size_t plen = sizeof(HU_SEMANTIC_EXPERIENCE_KEY_PREFIX) - 1;
    return !(key_len >= plen && memcmp(key, HU_SEMANTIC_EXPERIENCE_KEY_PREFIX, plen) == 0);
}

/* The experience writer's body: "Task: ...\nActions: ..." — both markers,
 * in that order, so "Task force meeting" as prose is not the scaffold. */
static bool content_is_experience_scaffold(const char *c, size_t n) {
    static const char task[] = "Task: ";
    static const char actions[] = "\nActions: ";
    if (n < sizeof(task) - 1 || memcmp(c, task, sizeof(task) - 1) != 0)
        return false;
    const size_t alen = sizeof(actions) - 1;
    for (size_t i = sizeof(task) - 1; i + alen <= n; i++)
        if (memcmp(c + i, actions, alen) == 0)
            return true;
    return false;
}

/* Word-boundary, case-insensitive cues. Every entry is a PHRASE anchored on
 * the sender's identity ("is an ai", "your ai", "are you real"); bare
 * determiner forms ("an ai", "the ai") and single words ("ai", "bot") are
 * not listed on purpose — "started an AI research job" is a memory.
 * MIRROR: scripts/eval_semantic_live_gate.py AI_IDENTITY_CUES. */
static const char *const AI_IDENTITY_CUES[] = {
    "your ai",
    "is an ai",
    "are an ai",
    "be an ai",
    "was an ai",
    "you're an ai",
    "youre an ai",
    "you're ai",
    "you are ai",
    "is the ai",
    "are the ai",
    "ai generated",
    "ai-generated",
    "generated by ai",
    "ai wrote",
    "a bot",
    "chatbot",
    "chat bot",
    "a robot",
    "automated message",
    "automated reply",
    "auto reply",
    "auto-reply",
    "is this really",
    "is this actually",
    "is this you",
    "is that you",
    "is it really you",
    "are you real",
    "are you human",
    "actually you",
    "really you",
    "who am i talking to",
    "am i talking to",
    "talking to a machine",
};

/* "Is this Seth" — a bare identity question: <= 4 words, opening "is this"
 * / "is that". Generic so the memory module never names the persona. */
static bool content_is_bare_identity_question(const char *c, size_t n) {
    size_t i = 0;
    while (i < n && isspace((unsigned char)c[i]))
        i++;
    static const char *const openers[] = {"is this ", "is that "};
    bool opened = false;
    for (size_t k = 0; k < sizeof(openers) / sizeof(openers[0]) && !opened; k++) {
        size_t ol = strlen(openers[k]);
        opened = (n - i >= ol) && strncasecmp(c + i, openers[k], ol) == 0;
    }
    if (!opened)
        return false;
    size_t words = 0;
    bool in_word = false;
    for (; i < n; i++) {
        bool alnum = isalnum((unsigned char)c[i]) != 0;
        if (alnum && !in_word)
            words++;
        in_word = alnum;
    }
    return words <= 4;
}

bool hu_semantic_recall_hit_is_excluded(const char *key, size_t key_len, const char *content,
                                        size_t content_len) {
    if (key && key_len > 0 && !hu_semantic_recall_key_is_indexable(key, key_len))
        return true;
    if (!content || content_len == 0)
        return false;
    if (content_is_experience_scaffold(content, content_len))
        return true;
    for (size_t k = 0; k < sizeof(AI_IDENTITY_CUES) / sizeof(AI_IDENTITY_CUES[0]); k++)
        if (hu_str_contains_word_ci_n(content, content_len, AI_IDENTITY_CUES[k]))
            return true;
    return content_is_bare_identity_question(content, content_len);
}

size_t hu_semantic_recall_filter_result(hu_allocator_t *alloc, hu_retrieval_result_t *res) {
    if (!alloc || !res || !res->entries || res->count == 0)
        return 0;
    size_t old = res->count, keep = 0;
    for (size_t i = 0; i < old; i++) {
        hu_memory_entry_t *e = &res->entries[i];
        if (hu_semantic_recall_hit_is_excluded(e->key, e->key_len, e->content, e->content_len)) {
            hu_memory_entry_free_fields(alloc, e);
            memset(e, 0, sizeof(*e));
            continue;
        }
        if (keep != i) {
            res->entries[keep] = *e; /* compact: rank order preserved */
            memset(e, 0, sizeof(*e));
            if (res->scores)
                res->scores[keep] = res->scores[i];
        }
        keep++;
    }
    if (keep == old)
        return 0;
    result_shrink(alloc, res, old, keep);
    return old - keep;
}

hu_gate_mode_t hu_semantic_recall_mode(void) {
    return hu_gate_mode_from_env("HU_SEMANTIC_RECALL", HU_GATE_OFF);
}

const char *hu_semantic_recall_embed_url(void) {
    const char *u = getenv("HU_SEMANTIC_EMBED_URL");
    return (u && u[0]) ? u : "http://127.0.0.1:8741";
}

hu_error_t hu_semantic_recall_attach(hu_allocator_t *alloc, hu_memory_t *mem,
                                     hu_embedder_t *out_embedder, hu_vector_store_t *out_store) {
    if (!alloc || !mem || !mem->vtable || !out_embedder || !out_store)
        return HU_ERR_INVALID_ARGUMENT;
    out_embedder->ctx = NULL;
    out_store->ctx = NULL;
#ifdef HU_ENABLE_SQLITE
    struct sqlite3 *db = hu_sqlite_memory_get_db(mem);
    if (!db)
        return HU_ERR_NOT_SUPPORTED; /* not the sqlite engine */
    *out_embedder = hu_embedder_http_create(alloc, hu_semantic_recall_embed_url());
    if (!out_embedder->ctx)
        return HU_ERR_INTERNAL;
    *out_store = hu_vector_store_sqlite_vec_create(alloc, db, HU_SEMANTIC_EMBED_DIM);
    if (!out_store->ctx) {
        out_embedder->vtable->deinit(out_embedder->ctx, alloc);
        out_embedder->ctx = NULL;
        return HU_ERR_INTERNAL;
    }
    hu_sqlite_memory_set_semantic_index(mem, out_embedder, out_store);
    return HU_OK;
#else
    return HU_ERR_NOT_SUPPORTED;
#endif
}
