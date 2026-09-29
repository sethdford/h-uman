#include "human/core/string.h"
#include "human/daemon/expressive.h"

#include <stdio.h>
#include <string.h>

/* snprintf result -> length written, or 0 (and an empty buffer) if cut off. */
static size_t fitted(char *buf, size_t cap, int n) {
    if (n < 0 || (size_t)n >= cap) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

bool hu_expressive_somber(const char *text, size_t len) {
    static const char *const w[] = {
        "passed away", "died",    "funeral",  "hospital",    "cancer",   "chemo",
        "diagnosed",   "divorce", "broke up", "miscarriage", "laid off", "fired",
        "accident",    "surgery", "grief",    "grieving",    "rip",      "sick",
    };
    if (!text || len == 0)
        return false;
    for (size_t i = 0; i < sizeof(w) / sizeof(w[0]); i++)
        if (hu_str_contains_word_ci_n(text, len, w[i]))
            return true;
    return false;
}

bool hu_expressive_effect_allowed(const char *effect, bool somber, bool is_group,
                                  int64_t secs_since_last) {
    static const char *const ids[] = {"impact",       "loud",     "gentle",
                                      "invisibleink", "confetti", "lasers"};
    if (!effect || somber || is_group)
        return false;
    if (secs_since_last >= 0 && secs_since_last < 7 * 86400)
        return false;
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        if (strcmp(effect, ids[i]) == 0)
            return true;
    return false;
}

bool hu_expressive_gif_allowed(bool somber, bool is_group, const char *relationship,
                               int64_t secs_since_last) {
    /* A GIF to your mother or a business contact reads as a bot, not a person. */
    static const char *const formal[] = {"mother",    "father", "professional", "business",
                                         "colleague", "boss",   "client"};
    if (somber || is_group || !relationship || !relationship[0])
        return false;
    if (secs_since_last >= 0 && secs_since_last < 86400)
        return false;
    for (size_t i = 0; i < sizeof(formal) / sizeof(formal[0]); i++)
        if (strstr(relationship, formal[i]))
            return false;
    return true;
}

size_t hu_expressive_situation(char *buf, size_t cap, bool voice_available, bool bridge_up,
                               bool is_group) {
    if (!buf || cap == 0)
        return 0;
    int n = snprintf(buf, cap, "This turn: voice memo: %s; effects and threaded replies: %s%s.",
                     voice_available ? "available" : "not available",
                     bridge_up ? "available" : "not available", is_group ? "; group chat" : "");
    return fitted(buf, cap, n);
}

size_t hu_expressive_shadow_line(const hu_director_result_t *r, const char *inbound,
                                 size_t inbound_len, bool is_group, const char *relationship,
                                 char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!r)
        return 0;
    bool somber = hu_expressive_somber(inbound, inbound ? inbound_len : 0);
    /* Budgets are not applied here (-1): the shadow line judges the moment. */
    const char *eff_verdict =
        r->effect[0]
            ? (hu_expressive_effect_allowed(r->effect, somber, is_group, -1) ? "(ok)" : "(blocked)")
            : "";
    const char *gif_verdict =
        r->form == HU_DIR_FORM_GIF
            ? (hu_expressive_gif_allowed(somber, is_group, relationship, -1) ? "(ok)" : "(blocked)")
            : "";
    int n = snprintf(buf, cap, "form=%s effect=%s%s gif=\"%s\"%s reply_to=%d somber=%d",
                     hu_director_form_name(r->form), r->effect[0] ? r->effect : "none", eff_verdict,
                     r->gif_query, gif_verdict, r->reply_to ? 1 : 0, somber ? 1 : 0);
    return fitted(buf, cap, n);
}
