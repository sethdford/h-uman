#include "human/agent/choreography.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdint.h>
#include <string.h>

#define HU_CHOREO_SPAN_MAX 128

typedef struct {
    size_t off;
    size_t len;
} choreo_span_t;

hu_choreography_config_t hu_choreography_config_default(void) {
    hu_choreography_config_t c;
    c.burst_probability = 0.03f;
    c.double_text_probability = 0.08f;
    c.energy_level = 1.f;
    c.max_segments = 4;
    c.ms_per_word = 50;
    c.message_splitting_enabled = true;
    return c;
}

static size_t choreo_word_count(const char *s, size_t n) {
    size_t c = 0;
    bool inw = false;
    for (size_t i = 0; i < n; i++) {
        bool w = !isspace((unsigned char)s[i]);
        if (w && !inw)
            c++;
        inw = w;
    }
    return c > 0 ? c : 1;
}

static bool choreo_prefix_ci(const char *w, size_t wlen, const char *r) {
    for (size_t i = 0; r[i] != '\0'; i++) {
        if (i >= wlen)
            return false;
        if (tolower((unsigned char)w[i]) != tolower((unsigned char)r[i]))
            return false;
    }
    return true;
}

static bool choreo_word_is_reaction(const char *w, size_t wlen) {
    static const char *const rw[] = {"haha", "oh", "wait", "lol", "omg", "hmm"};
    size_t al = 0;
    while (al < wlen && isalpha((unsigned char)w[al]))
        al++;
    if (al == 0)
        return false;
    for (size_t k = 0; k < sizeof rw / sizeof rw[0]; k++) {
        size_t rl = strlen(rw[k]);
        if (al < rl)
            continue;
        if (!choreo_prefix_ci(w, al, rw[k]))
            continue;
        if (strcmp(rw[k], "haha") == 0)
            return true;
        if (al == rl)
            return true;
    }
    return false;
}

static bool choreo_reaction_split(const char *response, size_t response_len, choreo_span_t *out_two) {
    size_t i = 0;
    while (i < response_len && isspace((unsigned char)response[i]))
        i++;
    if (i >= response_len)
        return false;
    size_t w0 = i;
    while (i < response_len && !isspace((unsigned char)response[i]))
        i++;
    size_t wlen = i - w0;
    if (!choreo_word_is_reaction(response + w0, wlen))
        return false;

    while (i < response_len && isspace((unsigned char)response[i]))
        i++;

    out_two[0].off = w0;
    out_two[0].len = wlen;
    out_two[1].off = i;
    out_two[1].len = response_len - i;
    return out_two[1].len > 0;
}

static size_t choreo_count_paragraphs(const char *r, size_t len) {
    size_t n = 1;
    for (size_t j = 0; j + 1 < len; j++) {
        if (r[j] == '\n' && r[j + 1] == '\n')
            n++;
    }
    return n;
}

static void choreo_collect_paragraphs(const char *response, size_t response_len, choreo_span_t *sp,
                                      size_t *nsp) {
    *nsp = 0;
    size_t a = 0;
    for (size_t i = 0; i + 1 < response_len; i++) {
        if (response[i] == '\n' && response[i + 1] == '\n') {
            if (*nsp + 1 >= HU_CHOREO_SPAN_MAX) {
                sp[0].off = 0;
                sp[0].len = response_len;
                *nsp = 1;
                return;
            }
            sp[*nsp].off = a;
            sp[*nsp].len = i - a;
            (*nsp)++;
            a = i + 2;
            i++;
        }
    }
    if (*nsp + 1 >= HU_CHOREO_SPAN_MAX) {
        sp[0].off = 0;
        sp[0].len = response_len;
        *nsp = 1;
        return;
    }
    sp[*nsp].off = a;
    sp[*nsp].len = response_len - a;
    (*nsp)++;
}

static void choreo_merge_last_two(choreo_span_t *sp, size_t *n) {
    if (*n < 2)
        return;
    size_t a = *n - 2;
    sp[a].len = sp[*n - 1].off + sp[*n - 1].len - sp[a].off;
    (*n)--;
}

static bool prob_roll(float p, uint32_t s) {
    if (p <= 0.f)
        return false;
    uint32_t thresh = (uint32_t)(p * 100.f);
    if (thresh >= 100u)
        return true;
    return (s % 100u) < thresh;
}

hu_error_t hu_choreography_plan(hu_allocator_t *alloc, const char *response, size_t response_len,
                                const hu_choreography_config_t *config, uint32_t seed,
                                hu_message_plan_t *out) {
    if (!alloc || !config || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (response_len > 0U && !response)
        return HU_ERR_INVALID_ARGUMENT;

    out->segments = NULL;
    out->segment_count = 0;

    uint32_t max_seg = config->max_segments == 0 ? 1u : config->max_segments;

    choreo_span_t sp[HU_CHOREO_SPAN_MAX];
    size_t nsp = 0;
    bool reaction = false;

    bool use_single = !config->message_splitting_enabled || response_len < 40;

    if (use_single) {
        sp[0].off = 0;
        sp[0].len = response_len;
        nsp = 1;
    } else if (choreo_reaction_split(response, response_len, sp)) {
        nsp = 2;
        reaction = true;
    } else if (choreo_count_paragraphs(response, response_len) > HU_CHOREO_SPAN_MAX) {
        sp[0].off = 0;
        sp[0].len = response_len;
        nsp = 1;
    } else {
        choreo_collect_paragraphs(response, response_len, sp, &nsp);
    }

    if (config->energy_level < 0.3f) {
        sp[0].off = 0;
        sp[0].len = response_len;
        nsp = 1;
        reaction = false;
    }

    while (nsp > max_seg)
        choreo_merge_last_two(sp, &nsp);

    uint32_t s2 = seed ^ 0x9E3779B9u;
    bool burst = prob_roll(config->burst_probability, seed);
    bool dtext = prob_roll(config->double_text_probability, s2);

    if (dtext && nsp == 1 && response_len >= 40 && config->message_splitting_enabled) {
        size_t mid = response_len / 2;
        size_t split = mid;
        while (split > 0 && !isspace((unsigned char)response[split]))
            split--;
        if (split == 0) {
            split = mid;
            while (split < response_len && !isspace((unsigned char)response[split]))
                split++;
        }
        if (split > 0 && split < response_len) {
            size_t right = split;
            while (right < response_len && isspace((unsigned char)response[right]))
                right++;
            if (right < response_len) {
                sp[0].off = 0;
                sp[0].len = split;
                sp[1].off = right;
                sp[1].len = response_len - right;
                if (sp[0].len > 0 && sp[1].len > 0)
                    nsp = 2;
            }
        }
    }

    while (nsp > max_seg)
        choreo_merge_last_two(sp, &nsp);

    hu_message_segment_t *segs =
        (hu_message_segment_t *)alloc->alloc(alloc->ctx, nsp * sizeof(hu_message_segment_t));
    if (!segs)
        return HU_ERR_OUT_OF_MEMORY;
    memset(segs, 0, nsp * sizeof(hu_message_segment_t));

    for (size_t si = 0; si < nsp; si++) {
        char *t = hu_strndup(alloc, response + sp[si].off, sp[si].len);
        if (!t) {
            for (size_t j = 0; j < si; j++)
                hu_str_free(alloc, segs[j].text);
            alloc->free(alloc->ctx, segs, nsp * sizeof(hu_message_segment_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
        size_t tl = strlen(t);
        segs[si].text = t;
        segs[si].text_len = tl;
        if (reaction && si == 0) {
            segs[si].delay_ms = 300;
        } else {
            /* Human typing rhythm: faster when energized, slower and more
             * deliberate when low-energy — and never a metronome. Scale ms/word
             * by energy_level and add seeded ±25% jitter (reproducible from the
             * plan seed, so tests stay deterministic). energy 1.0 -> 1.0x,
             * 0.2 -> ~1.6x slower, 1.6 -> ~0.6x faster. */
            float e = config->energy_level;
            if (e < 0.2f)
                e = 0.2f;
            if (e > 1.6f)
                e = 1.6f;
            float energy_factor = 1.6f - 0.625f * e;
            if (energy_factor < 0.5f)
                energy_factor = 0.5f;
            uint32_t base = (uint32_t)(choreo_word_count(t, tl) * (size_t)config->ms_per_word);
            uint32_t h = seed ^ ((uint32_t)si * 2654435761u);
            h ^= h >> 13;
            h *= 0x5bd1e995u;
            h ^= h >> 15;
            float jitter = 0.75f + ((float)(h % 1000) / 1000.0f) * 0.5f;
            segs[si].delay_ms = (uint32_t)((float)base * energy_factor * jitter);
        }
    }

    if (burst) {
        for (size_t si = 0; si < nsp; si++)
            segs[si].delay_ms = 0;
    }

    for (size_t si = 0; si < nsp; si++)
        segs[si].show_typing_indicator = segs[si].delay_ms > 200U;

    out->segments = segs;
    out->segment_count = nsp;
    return HU_OK;
}

void hu_choreography_plan_free(hu_allocator_t *alloc, hu_message_plan_t *plan) {
    if (!alloc || !plan)
        return;
    if (plan->segments) {
        for (size_t i = 0; i < plan->segment_count; i++)
            hu_str_free(alloc, plan->segments[i].text);
        alloc->free(alloc->ctx, plan->segments, plan->segment_count * sizeof(hu_message_segment_t));
    }
    plan->segments = NULL;
    plan->segment_count = 0;
}
