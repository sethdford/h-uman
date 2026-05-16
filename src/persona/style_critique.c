/* US-7.9 — Constitutional style self-critique at generation time.
 *
 * Pure literal pattern matcher.  See header for contract.
 *
 * Design source:  sprints/sprint-7/designs/US-7.9.md
 */

#include "human/persona/style_critique.h"
#include "human/core/allocator.h"
#include "human/core/log.h"
#include "human/observer.h"
#include "human/provider.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define HU_STYLE_MAX_COMPILED_RULES 32

#ifdef HU_IS_TEST
int hu_style_critique_test_unresolved_count = 0;
int hu_style_critique_test_check_invocations = 0;
void hu_style_critique_test_reset(void) {
    hu_style_critique_test_unresolved_count = 0;
    hu_style_critique_test_check_invocations = 0;
}
void hu_style_critique_test_note_unresolved(void) {
    hu_style_critique_test_unresolved_count++;
}
#endif

typedef enum {
    HU_STYLE_MATCH_PREFIX = 0,    /* must NOT start with needle */
    HU_STYLE_MATCH_SUBSTRING = 1, /* must NOT contain needle anywhere */
} hu_style_match_kind_t;

/* A compiled rule references either the original rule string (for
 * needles that are literally embedded in the rule text, e.g. a
 * quoted prefix) or a static alias table entry.  No heap allocation. */
typedef struct {
    const char *needle; /* borrowed */
    size_t needle_len;
    hu_style_match_kind_t kind;
} hu_style_compiled_rule_t;

/* Alias table — maps a human-language phrase (case-insensitive
 * keyword) that appears in a rule string to one or more literal
 * needles that should be banned anywhere in the draft.
 *
 * Order matters: longer/more-specific keys MUST come before any of
 * their substrings ("exclamation marks" before "marks", etc.) so that
 * the first match wins. */
typedef struct {
    const char *keyword; /* searched for in the rule text */
    size_t keyword_len;
    const char *needle; /* literal bytes searched for in the draft */
    size_t needle_len;
} hu_style_alias_t;

#define HU_ALIAS(keyword_lit, needle_lit) \
    {(keyword_lit), sizeof(keyword_lit) - 1, (needle_lit), sizeof(needle_lit) - 1}

/* Curated alias table.  Hand-maintained.  See design doc §3 OQ3. */
static const hu_style_alias_t k_style_aliases[] = {
    /* em-dash family — both the unicode em-dash and the ascii double-dash */
    HU_ALIAS("em-dash", "\xE2\x80\x94"),
    HU_ALIAS("em dash", "\xE2\x80\x94"),
    HU_ALIAS("emdash", "\xE2\x80\x94"),
    /* common emoji needles — substring match.  We keep this small and
     * obvious; the real "no emoji" rule should be enforced by a
     * downstream Unicode-aware pass if the persona truly cares. */
    HU_ALIAS("emoji", "\xF0\x9F"), /* prefix byte of most pictographic
                                      emoji in plane 1F000+ */
    HU_ALIAS("emojis", "\xF0\x9F"),
    /* punctuation patterns */
    HU_ALIAS("exclamation marks", "!"),
    HU_ALIAS("exclamation mark", "!"),
    HU_ALIAS("exclamation point", "!"),
};
static const size_t k_style_alias_count = sizeof(k_style_aliases) / sizeof(k_style_aliases[0]);

/* ---------- string helpers (ASCII case-insensitive, no locale) ---------- */

static char ascii_lower(char c) {
    unsigned char uc = (unsigned char)c;
    if (uc >= 'A' && uc <= 'Z')
        return (char)(uc - 'A' + 'a');
    return (char)uc;
}

/* Case-insensitive (ASCII) needle search inside haystack.  Multi-byte
 * needle bytes >= 0x80 compare unchanged (still byte-for-byte). */
static const char *ascii_casefind(const char *haystack, size_t haystack_len, const char *needle,
                                  size_t needle_len) {
    if (needle_len == 0)
        return haystack;
    if (needle_len > haystack_len)
        return NULL;
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        size_t k = 0;
        while (k < needle_len) {
            char a = haystack[i + k];
            char b = needle[k];
            if (ascii_lower(a) != ascii_lower(b))
                break;
            k++;
        }
        if (k == needle_len)
            return haystack + i;
    }
    return NULL;
}

/* True if c is a word character (letter or digit). */
static bool is_word_char(char c) {
    unsigned char uc = (unsigned char)c;
    if (uc >= 'A' && uc <= 'Z')
        return true;
    if (uc >= 'a' && uc <= 'z')
        return true;
    if (uc >= '0' && uc <= '9')
        return true;
    return false;
}

/* ---------- rule parsing ---------- */

/* Find the last quoted span in `rule[0..rule_len)` delimited by either
 * single or double quotes.  Returns true and writes start/len on
 * success; returns false if no quoted span is found.  The returned
 * pointer is borrowed from the rule string. */
static bool find_last_quoted(const char *rule, size_t rule_len, const char **out, size_t *out_len) {
    if (!rule || rule_len < 2)
        return false;
    /* Walk from the right.  We want the *innermost* matched pair
     * closest to the end. */
    for (size_t i = rule_len; i-- > 0;) {
        char c = rule[i];
        if (c != '\'' && c != '"')
            continue;
        /* find matching opener earlier in the string */
        for (size_t j = i; j-- > 0;) {
            if (rule[j] == c) {
                const char *start = rule + j + 1;
                size_t len = i - j - 1;
                if (len == 0)
                    return false;
                *out = start;
                *out_len = len;
                return true;
            }
        }
        return false;
    }
    return false;
}

/* Detect the PREFIX-shaped rule.  Recognises any rule that contains
 * the substring "start with" (case-insensitive) AND has a quoted span
 * — we take the last quoted span as the needle. */
static bool parse_prefix_rule(const char *rule, size_t rule_len, hu_style_compiled_rule_t *out) {
    if (!ascii_casefind(rule, rule_len, "start with", strlen("start with")))
        return false;
    const char *needle = NULL;
    size_t needle_len = 0;
    if (!find_last_quoted(rule, rule_len, &needle, &needle_len))
        return false;
    out->needle = needle;
    out->needle_len = needle_len;
    out->kind = HU_STYLE_MATCH_PREFIX;
    return true;
}

/* Compile a rule into one or more compiled rules.  Returns the number
 * of compiled entries appended (0 if the rule is unparseable).  Writes
 * into `out_slots[0..max_slots)`. */
static size_t compile_rule(const char *rule, size_t rule_len, hu_style_compiled_rule_t *out_slots,
                           size_t max_slots) {
    if (!rule || rule_len == 0 || max_slots == 0)
        return 0;

    /* 1) PREFIX form takes precedence. */
    hu_style_compiled_rule_t pfx;
    if (parse_prefix_rule(rule, rule_len, &pfx)) {
        out_slots[0] = pfx;
        return 1;
    }

    /* 2) Alias table:  if any alias keyword appears in the rule text
     *    (case-insensitive), each matching alias becomes a SUBSTRING
     *    compiled rule.  Multiple aliases may apply ("no em-dashes and
     *    no emoji"). */
    size_t produced = 0;
    for (size_t i = 0; i < k_style_alias_count && produced < max_slots; i++) {
        if (ascii_casefind(rule, rule_len, k_style_aliases[i].keyword,
                           k_style_aliases[i].keyword_len)) {
            out_slots[produced].needle = k_style_aliases[i].needle;
            out_slots[produced].needle_len = k_style_aliases[i].needle_len;
            out_slots[produced].kind = HU_STYLE_MATCH_SUBSTRING;
            produced++;
        }
    }
    if (produced > 0)
        return produced;

    /* 3) Fallback:  last quoted span as a SUBSTRING needle. */
    const char *q = NULL;
    size_t qlen = 0;
    if (find_last_quoted(rule, rule_len, &q, &qlen)) {
        out_slots[0].needle = q;
        out_slots[0].needle_len = qlen;
        out_slots[0].kind = HU_STYLE_MATCH_SUBSTRING;
        return 1;
    }

    /* 4) Last resort:  the rule text itself as a SUBSTRING. */
    out_slots[0].needle = rule;
    out_slots[0].needle_len = rule_len;
    out_slots[0].kind = HU_STYLE_MATCH_SUBSTRING;
    return 1;
}

/* ---------- evaluation ---------- */

static bool prefix_matches(const char *draft, size_t draft_len, const char *needle,
                           size_t needle_len) {
    if (needle_len == 0 || needle_len > draft_len)
        return false;
    /* Skip leading whitespace. */
    size_t i = 0;
    while (i < draft_len &&
           (draft[i] == ' ' || draft[i] == '\t' || draft[i] == '\n' || draft[i] == '\r'))
        i++;
    if (i + needle_len > draft_len)
        return false;
    for (size_t k = 0; k < needle_len; k++) {
        if (ascii_lower(draft[i + k]) != ascii_lower(needle[k]))
            return false;
    }
    /* Word-boundary check at end-of-needle:
     *  - if the needle ends in a non-word char (e.g. "Sure!" ends with
     *    '!'), the boundary is satisfied for any following char,
     *    because the needle already "closed" the word.
     *  - if the needle ends in a word char, the next char (or EOS)
     *    must be a non-word char.  This is what prevents
     *    "never start with 'sure'" from matching "surely…".  */
    char last_needle = needle[needle_len - 1];
    if (!is_word_char(last_needle))
        return true;
    size_t after = i + needle_len;
    if (after == draft_len)
        return true;
    return !is_word_char(draft[after]);
}

static bool substring_matches(const char *draft, size_t draft_len, const char *needle,
                              size_t needle_len) {
    return ascii_casefind(draft, draft_len, needle, needle_len) != NULL;
}

/* ---------- public entry ---------- */

hu_error_t hu_style_critique_check(const char *draft, size_t draft_len, char *const *style_rules,
                                   size_t style_rules_count, const char **violated_rule_out,
                                   size_t *violated_rule_len_out) {
    if (!draft || !violated_rule_out || !violated_rule_len_out)
        return HU_ERR_INVALID_ARGUMENT;

    *violated_rule_out = NULL;
    *violated_rule_len_out = 0;

#ifdef HU_IS_TEST
    hu_style_critique_test_check_invocations++;
#endif

    if (!style_rules || style_rules_count == 0)
        return HU_OK;

    bool warned_drop = false;
    (void)warned_drop;

    for (size_t r = 0; r < style_rules_count; r++) {
        const char *rule = style_rules[r];
        if (!rule)
            continue;
        size_t rule_len = strlen(rule);
        if (rule_len == 0)
            continue;

        hu_style_compiled_rule_t slots[8];
        size_t n = compile_rule(rule, rule_len, slots, sizeof(slots) / sizeof(slots[0]));
        if (n == 0)
            continue;

        for (size_t s = 0; s < n; s++) {
            bool hit = false;
            if (slots[s].kind == HU_STYLE_MATCH_PREFIX)
                hit = prefix_matches(draft, draft_len, slots[s].needle, slots[s].needle_len);
            else
                hit = substring_matches(draft, draft_len, slots[s].needle, slots[s].needle_len);
            if (hit) {
                *violated_rule_out = rule;
                *violated_rule_len_out = rule_len;
                return HU_OK;
            }
        }
    }

    /* Compile-time upper bound only matters when many rules co-exist;
     * silence the unused warning in non-debug builds. */
    (void)HU_STYLE_MAX_COMPILED_RULES;
    return HU_OK;
}

/* ---------- orchestration ---------- */

hu_error_t hu_style_critique_run(struct hu_allocator *alloc, struct hu_provider *provider,
                                 struct hu_observer *observer, const char *system_prompt,
                                 size_t system_prompt_len, const char *user_message,
                                 size_t user_message_len, const char *model_name,
                                 size_t model_name_len, const char *draft, size_t draft_len,
                                 char *const *style_rules, size_t style_rules_count,
                                 char **out_response, size_t *out_response_len) {
    if (!alloc || !provider || !provider->vtable || !provider->vtable->chat_with_system || !draft ||
        !out_response || !out_response_len)
        return HU_ERR_INVALID_ARGUMENT;

    *out_response = NULL;
    *out_response_len = 0;

    if (!style_rules || style_rules_count == 0)
        return HU_OK;

    const char *violated_rule = NULL;
    size_t violated_rule_len = 0;
    hu_error_t cerr = hu_style_critique_check(draft, draft_len, style_rules, style_rules_count,
                                              &violated_rule, &violated_rule_len);
    if (cerr != HU_OK)
        return cerr;
    if (!violated_rule || violated_rule_len == 0)
        return HU_OK;

    static const char k_suffix_pre[] = "\n\nIMPORTANT: rewrite the previous answer. Constraint: ";
    static const char k_suffix_post[] = ". Keep the same meaning.";
    size_t pre_len = sizeof(k_suffix_pre) - 1;
    size_t post_len = sizeof(k_suffix_post) - 1;
    size_t base_len = system_prompt ? system_prompt_len : 0;
    size_t aug_len = base_len + pre_len + violated_rule_len + post_len;
    char *aug = (char *)alloc->alloc(alloc->ctx, aug_len + 1);
    if (!aug)
        return HU_ERR_OUT_OF_MEMORY;
    size_t off = 0;
    if (system_prompt && base_len > 0) {
        memcpy(aug + off, system_prompt, base_len);
        off += base_len;
    }
    memcpy(aug + off, k_suffix_pre, pre_len);
    off += pre_len;
    memcpy(aug + off, violated_rule, violated_rule_len);
    off += violated_rule_len;
    memcpy(aug + off, k_suffix_post, post_len);
    off += post_len;
    aug[off] = '\0';

    char *regen_out = NULL;
    size_t regen_out_len = 0;
    hu_error_t rerr = provider->vtable->chat_with_system(
        provider->ctx, alloc, aug, aug_len, user_message, user_message_len, model_name,
        model_name_len, 0.0, &regen_out, &regen_out_len);
    alloc->free(alloc->ctx, aug, aug_len + 1);

    if (rerr != HU_OK || !regen_out || regen_out_len == 0) {
        if (regen_out)
            alloc->free(alloc->ctx, regen_out, regen_out_len + 1);
        return HU_OK; /* best-effort:  no regen, caller keeps original */
    }

    /* Re-check the regen output.  We accept it unconditionally
     * (best-effort, AC-7.9.3) but emit the unresolved event if the
     * second draft also violates a rule. */
    const char *vr2 = NULL;
    size_t vr2_len = 0;
    (void)hu_style_critique_check(regen_out, regen_out_len, style_rules, style_rules_count, &vr2,
                                  &vr2_len);
    if (vr2 && vr2_len > 0) {
        hu_log_info("style_critique", observer, "style_rule_violation_unresolved rule=\"%.*s\"",
                    (int)vr2_len, vr2);
#ifdef HU_IS_TEST
        hu_style_critique_test_note_unresolved();
#endif
    }

    *out_response = regen_out;
    *out_response_len = regen_out_len;
    return HU_OK;
}
