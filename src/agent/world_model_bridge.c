/* W9 wire bridge (FIX 12). See world_model_bridge.h for the rationale.
 *
 * This TU INTENTIONALLY does NOT include `human/agent.h` or `human/memory.h`
 * (legacy). It is the ONE place where W7 + W9 headers are visible. Adding an
 * include of either legacy header here will reintroduce the type collision
 * the bridge exists to dodge. */

#include "human/agent/world_model_bridge.h"
#include "human/agent/world_model.h"
#include "human/memory/memory.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct hu_w7_facade {
    hu_memory_t *m;
};

hu_error_t hu_w7_facade_open(hu_graph_t *graph, hu_allocator_t *alloc, hu_w7_facade_t **out) {
    if (!graph || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    hu_w7_facade_t *f = (hu_w7_facade_t *)alloc->alloc(alloc->ctx, sizeof(*f));
    if (!f)
        return HU_ERR_OUT_OF_MEMORY;
    f->m = NULL;
    hu_error_t e = hu_memory_open(alloc, graph, &f->m);
    if (e != HU_OK) {
        alloc->free(alloc->ctx, f, sizeof(*f));
        return e;
    }
    *out = f;
    return HU_OK;
}

void hu_w7_facade_close(hu_w7_facade_t *facade, hu_allocator_t *alloc) {
    if (!facade)
        return;
    if (facade->m)
        hu_memory_close(facade->m, alloc);
    if (alloc)
        alloc->free(alloc->ctx, facade, sizeof(*facade));
}

/* Append `s` (length `n`) to a growing buffer. Returns false on OOM. */
static bool buf_append(hu_allocator_t *alloc, char **buf, size_t *len, size_t *cap, const char *s,
                       size_t n) {
    if (n == 0)
        return true;
    if (*len + n + 1 > *cap) {
        size_t newcap = *cap == 0 ? 256 : *cap * 2;
        while (newcap < *len + n + 1)
            newcap *= 2;
        char *nb = (char *)alloc->alloc(alloc->ctx, newcap);
        if (!nb)
            return false;
        if (*buf) {
            memcpy(nb, *buf, *len);
            alloc->free(alloc->ctx, *buf, *cap);
        }
        *buf = nb;
        *cap = newcap;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return true;
}

static bool buf_appendf(hu_allocator_t *alloc, char **buf, size_t *len, size_t *cap,
                        const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return false;
    if ((size_t)n >= sizeof(tmp))
        n = (int)sizeof(tmp) - 1;
    return buf_append(alloc, buf, len, cap, tmp, (size_t)n);
}

hu_error_t hu_w7_render_world_model(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                    const char *contact_id, size_t contact_id_len,
                                    int64_t now_ms, char **out_text, size_t *out_len) {
    if (out_text)
        *out_text = NULL;
    if (out_len)
        *out_len = 0;
    if (!facade || !facade->m || !alloc || !contact_id || contact_id_len == 0 || !out_text ||
        !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (now_ms == 0)
        now_ms = (int64_t)time(NULL) * 1000;

    hu_world_model_t *wm = NULL;
    hu_error_t e =
        hu_world_model_load(facade->m, alloc, contact_id, contact_id_len, now_ms, &wm);
    if (e != HU_OK || !wm)
        return e == HU_OK ? HU_OK : e;

    /* If everything is empty, return NULL/0 -- callers skip injection.
     * The W9 builder always stamps `dominant_emotion = "neutral"` as a stub
     * (placeholder until emotional state lands properly), so a literal
     * non-empty `dominant_emotion` is NOT signal. We treat "neutral" with
     * default valence/arousal as "no signal". */
    bool emo_signal = wm->dominant_emotion[0] != '\0' &&
                      strcmp(wm->dominant_emotion, "neutral") != 0;
    bool any = wm->entities_count > 0 || wm->relations_count > 0 || wm->goals_count > 0 ||
               wm->negatives_count > 0 || wm->recent_topics_count > 0 || emo_signal ||
               wm->tom.user_thinks_we_are[0] != '\0' ||
               wm->tom.user_expects_we_can[0] != '\0' ||
               wm->tom.user_expects_we_cannot[0] != '\0';
    if (!any) {
        hu_world_model_free(alloc, wm);
        return HU_OK;
    }

    char *buf = NULL;
    size_t blen = 0, bcap = 0;
    bool ok = true;

    ok = ok && buf_append(alloc, &buf, &blen, &bcap, "## What I know about this conversation\n",
                          strlen("## What I know about this conversation\n"));

    if (wm->goals_count > 0) {
        ok = ok && buf_append(alloc, &buf, &blen, &bcap, "Active goals:\n", 14);
        for (size_t i = 0; i < wm->goals_count && i < 8; i++) {
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- %s\n", wm->goals[i].text);
        }
    }
    if (wm->negatives_count > 0) {
        ok = ok && buf_append(alloc, &buf, &blen, &bcap, "Avoid:\n", 7);
        size_t shown = wm->negatives_count > 6 ? 6 : wm->negatives_count;
        for (size_t i = 0; i < shown; i++) {
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- %s%s%s\n", wm->negatives[i].text,
                                   wm->negatives[i].reason[0] ? " — " : "",
                                   wm->negatives[i].reason[0] ? wm->negatives[i].reason : "");
        }
    }
    if (wm->tom.user_thinks_we_are[0] || wm->tom.user_expects_we_can[0] ||
        wm->tom.user_expects_we_cannot[0]) {
        ok = ok && buf_append(alloc, &buf, &blen, &bcap, "Theory of mind (what they think of me):\n",
                              strlen("Theory of mind (what they think of me):\n"));
        if (wm->tom.user_thinks_we_are[0])
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- They see me as: %s\n",
                                   wm->tom.user_thinks_we_are);
        if (wm->tom.user_expects_we_can[0])
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- They expect I can: %s\n",
                                   wm->tom.user_expects_we_can);
        if (wm->tom.user_expects_we_cannot[0])
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- They expect I cannot: %s\n",
                                   wm->tom.user_expects_we_cannot);
    }
    if (wm->recent_topics_count > 0) {
        ok = ok && buf_append(alloc, &buf, &blen, &bcap, "Recent topics: ", 15);
        for (size_t i = 0; i < wm->recent_topics_count && i < 10; i++) {
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "%s%s", i == 0 ? "" : ", ",
                                   wm->recent_topics[i]);
        }
        ok = ok && buf_append(alloc, &buf, &blen, &bcap, "\n", 1);
    }
    if (emo_signal) {
        ok = ok && buf_appendf(alloc, &buf, &blen, &bcap,
                               "Recent emotional tone: %s (arousal %.2f, valence %.2f)\n",
                               wm->dominant_emotion, (double)wm->arousal, (double)wm->valence);
    }

    hu_world_model_free(alloc, wm);

    if (!ok) {
        if (buf)
            alloc->free(alloc->ctx, buf, bcap);
        return HU_ERR_OUT_OF_MEMORY;
    }

    *out_text = buf;
    *out_len = blen;
    return HU_OK;
}
