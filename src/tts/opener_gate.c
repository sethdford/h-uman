#include "human/tts/opener_gate.h"

#include <ctype.h>
#include <string.h>

static bool tail_is(const char *w, size_t from, size_t n, char c) {
    for (size_t i = from; i < n; i++)
        if (w[i] != c)
            return false;
    return true;
}

/* w is one whole lowercase letter run, so matching it entirely is the same as
 * voiceai's /^(?:oh+|ugh+|ha(?:ha)*|hah|yeah|yep|hmm+|mm+|ah+|aw+|wow|whoa)\b/i. */
static bool is_reaction_word(const char *w, size_t n) {
    if (n >= 2 && w[0] == 'o' && tail_is(w, 1, n, 'h'))
        return true;
    if (n >= 3 && w[0] == 'u' && w[1] == 'g' && tail_is(w, 2, n, 'h'))
        return true;
    if (n >= 2 && n % 2 == 0) {
        bool ha = true;
        for (size_t i = 0; ha && i < n; i += 2)
            ha = w[i] == 'h' && w[i + 1] == 'a';
        if (ha)
            return true;
    }
    if (n >= 3 && w[0] == 'h' && tail_is(w, 1, n, 'm'))
        return true;
    if (n >= 2 && tail_is(w, 0, n, 'm'))
        return true;
    if (n >= 2 && w[0] == 'a' && (tail_is(w, 1, n, 'h') || tail_is(w, 1, n, 'w')))
        return true;
    static const char *const exact[] = {"hah", "yeah", "yep", "wow", "whoa"};
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++)
        if (strlen(exact[i]) == n && memcmp(exact[i], w, n) == 0)
            return true;
    return false;
}

/* Length of one reaction word plus the punctuation and space after it, or 0. */
static size_t reaction_at(const char *s, size_t n) {
    char w[24];
    size_t k = 0;
    while (k < n && isalpha((unsigned char)s[k])) {
        if (k >= sizeof(w))
            return 0;
        w[k] = (char)tolower((unsigned char)s[k]);
        k++;
    }
    if (k == 0 || (k < n && (isdigit((unsigned char)s[k]) || s[k] == '_')) ||
        !is_reaction_word(w, k))
        return 0;
    for (;;) {
        if (k < n && (isspace((unsigned char)s[k]) || strchr(",.!-", s[k])))
            k++;
        else if (k + 2 < n && memcmp(s + k, "\xE2\x80\xA6", 3) == 0) /* … */
            k += 3;
        else
            return k;
    }
}

/* Leading markup kept in front: whitespace, <tags/> and [bracket] cues. */
static size_t markup_prefix(const char *s, size_t n) {
    size_t i = 0;
    for (;;) {
        while (i < n && isspace((unsigned char)s[i]))
            i++;
        if (i < n && (s[i] == '<' || s[i] == '[')) {
            const char *close = memchr(s + i, s[i] == '<' ? '>' : ']', n - i);
            if (!close)
                return i;
            i = (size_t)(close - s) + 1;
        } else {
            return i;
        }
    }
}

size_t hu_opener_strip(const char *in, size_t len, char *out, size_t cap) {
    if (!in || !out || cap == 0)
        return 0;
    size_t pre = markup_prefix(in, len);
    size_t at = pre;
    for (size_t m = reaction_at(in + at, len - at); m > 0; m = reaction_at(in + at, len - at))
        at += m;
    if (at == pre || at >= len || !isalpha((unsigned char)in[at]))
        return 0;
    size_t total = pre + (len - at);
    if (total + 1 > cap)
        return 0;
    memcpy(out, in, pre);
    memcpy(out + pre, in + at, len - at);
    out[pre] = (char)toupper((unsigned char)out[pre]);
    out[total] = '\0';
    return total;
}

void hu_opener_gate_init(hu_opener_gate_t *g, uint8_t every) {
    if (!g)
        return;
    memset(g, 0, sizeof(*g));
    g->every = every;
}

bool hu_opener_gate_keep(hu_opener_gate_t *g, const char *key, size_t key_len) {
    if (!g || !key || key_len == 0)
        return true; /* no one to remember: let it through */
    if (key_len > sizeof(g->slot[0].key))
        key_len = sizeof(g->slot[0].key);
    size_t hit = HU_OPENER_GATE_SLOTS, oldest = 0;
    for (size_t i = 0; i < HU_OPENER_GATE_SLOTS; i++) {
        if (g->slot[i].key_len == key_len && memcmp(g->slot[i].key, key, key_len) == 0) {
            hit = i;
            break;
        }
        if (g->slot[i].last_use < g->slot[oldest].last_use)
            oldest = i;
    }
    if (hit == HU_OPENER_GATE_SLOTS) { /* new recipient: its first memo may keep one */
        hit = oldest;
        memcpy(g->slot[hit].key, key, key_len);
        g->slot[hit].key_len = (uint8_t)key_len;
        g->slot[hit].since_kept = g->every;
    }
    g->slot[hit].last_use = ++g->tick;
    if (g->slot[hit].since_kept >= g->every) {
        g->slot[hit].since_kept = 0;
        return true;
    }
    g->slot[hit].since_kept++;
    return false;
}
