#include "human/context/conversation.h"
#include "human/channel_class.h"
#include "human/core/allocator.h"
#include "human/core/io_secure.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/data/loader.h"
#include "human/filler_recency.h"
#include "human/persona.h"
#include "human/provider.h"
#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/emotional_moments.h"
#include <sqlite3.h>
#endif
#include <ctype.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define CTX_BUF_CAP 16384

/* Behavior thresholds — updated by hu_conversation_set_thresholds */
static uint32_t g_consecutive_limit = 3;
static uint32_t g_participation_pct = 40;
static uint32_t g_max_response_chars = 300;
static uint32_t g_min_response_chars = 15;

/* Configurable keyword/phrase lists — when NULL/0, DEFAULT_* fallbacks are used */
static const char **s_crisis_keywords = NULL;
static size_t s_crisis_keywords_len = 0;
static const char **s_personal_sharing_phrases = NULL;
static size_t s_personal_sharing_phrases_len = 0;
static const char **s_starters = NULL;
static size_t s_starters_len = 0;
static const char **s_emotional_words = NULL;
static size_t s_emotional_words_len = 0;
static const char **s_backchannel_phrases = NULL;
static size_t s_backchannel_phrases_len = 0;
static const char **s_engage_words = NULL;
static size_t s_engage_words_len = 0;
static const char **s_filler_words = NULL;
static size_t s_filler_words_len = 0;
typedef struct {
    const char *from;
    size_t from_len;
    const char *to;
    size_t to_len;
} hu_conversation_contraction_t;
static hu_conversation_contraction_t *s_contractions = NULL;
static size_t s_contractions_len = 0;
static const char **s_positive_words = NULL;
static size_t s_positive_words_len = 0;
static const char **s_negative_words = NULL;
static size_t s_negative_words_len = 0;
static const char **s_conversation_intros = NULL;
static size_t s_conversation_intros_len = 0;
static const char **s_ai_disclosure_patterns = NULL;
static size_t s_ai_disclosure_patterns_len = 0;

void hu_conversation_set_thresholds(uint32_t consecutive_limit, uint32_t participation_pct,
                                    uint32_t max_response_chars, uint32_t min_response_chars) {
    if (consecutive_limit > 0)
        g_consecutive_limit = consecutive_limit;
    if (participation_pct > 0)
        g_participation_pct = participation_pct;
    if (max_response_chars > 0)
        g_max_response_chars = max_response_chars;
    if (min_response_chars > 0)
        g_min_response_chars = min_response_chars;
}

/* ── Data initialization (word lists from embedded JSON) ─────────────────── */

static hu_allocator_t *s_conv_alloc = NULL;

/* Load a string array from JSON. Field name is expected to be an array of strings. */
static hu_error_t load_string_array(const hu_json_value_t *obj, const char *field_name,
                                    const char ***out_array, size_t *out_len) {
    if (!obj || !field_name || !out_array || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_value_t *field = hu_json_object_get(obj, field_name);
    if (!field || field->type != HU_JSON_ARRAY)
        return HU_ERR_NOT_FOUND;

    size_t arr_len = field->data.array.len;
    if (arr_len == 0)
        return HU_OK;

    const char **result =
        (const char **)s_conv_alloc->alloc(s_conv_alloc->ctx, arr_len * sizeof(const char *));
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;

    for (size_t i = 0; i < arr_len; i++) {
        hu_json_value_t *item = field->data.array.items[i];
        if (item && item->type == HU_JSON_STRING && item->data.string.ptr) {
            size_t slen = strlen(item->data.string.ptr);
            char *copy = (char *)s_conv_alloc->alloc(s_conv_alloc->ctx, slen + 1);
            if (copy) {
                memcpy(copy, item->data.string.ptr, slen + 1);
                result[i] = copy;
            } else {
                result[i] = NULL;
            }
        } else {
            result[i] = NULL;
        }
    }

    *out_array = result;
    *out_len = arr_len;
    return HU_OK;
}

/* Load contractions from JSON. Expected format: {"contractions": [{"from": "X", "to": "Y"}, ...]}
 */
static hu_error_t load_contractions(const hu_json_value_t *obj) {
    hu_json_value_t *field = hu_json_object_get(obj, "contractions");
    if (!field || field->type != HU_JSON_ARRAY)
        return HU_ERR_NOT_FOUND;

    size_t arr_len = field->data.array.len;
    if (arr_len == 0)
        return HU_OK;

    hu_conversation_contraction_t *result = (hu_conversation_contraction_t *)s_conv_alloc->alloc(
        s_conv_alloc->ctx, arr_len * sizeof(hu_conversation_contraction_t));
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;

    for (size_t i = 0; i < arr_len; i++) {
        hu_json_value_t *item = field->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;

        const char *from = hu_json_get_string(item, "from");
        const char *to = hu_json_get_string(item, "to");

        if (from && to) {
            size_t flen = strlen(from);
            size_t tlen = strlen(to);
            char *from_copy = (char *)s_conv_alloc->alloc(s_conv_alloc->ctx, flen + 1);
            char *to_copy = (char *)s_conv_alloc->alloc(s_conv_alloc->ctx, tlen + 1);
            if (from_copy && to_copy) {
                memcpy(from_copy, from, flen + 1);
                memcpy(to_copy, to, tlen + 1);
                result[i].from = from_copy;
                result[i].from_len = flen;
                result[i].to = to_copy;
                result[i].to_len = tlen;
            } else {
                result[i].from = "";
                result[i].from_len = 0;
                result[i].to = "";
                result[i].to_len = 0;
            }
        } else {
            result[i].from = "";
            result[i].from_len = 0;
            result[i].to = "";
            result[i].to_len = 0;
        }
    }

    s_contractions = result;
    s_contractions_len = arr_len;
    return HU_OK;
}

hu_error_t hu_conversation_data_init(hu_allocator_t *alloc) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;

    s_conv_alloc = alloc;

    /* Load AI disclosure patterns */
    {
        char *json_data = NULL;
        size_t json_len = 0;
        hu_error_t err =
            hu_data_load(alloc, "conversation/ai_disclosure_patterns.json", &json_data, &json_len);
        if (err == HU_OK) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, json_data, json_len, &root);
            if (err == HU_OK && root) {
                load_string_array(root, "patterns", &s_ai_disclosure_patterns,
                                  &s_ai_disclosure_patterns_len);
                hu_json_free(alloc, root);
            }
            if (json_data)
                alloc->free(alloc->ctx, json_data, json_len);
        }
    }

    /* Load filler words */
    {
        char *json_data = NULL;
        size_t json_len = 0;
        hu_error_t err =
            hu_data_load(alloc, "conversation/filler_words.json", &json_data, &json_len);
        if (err == HU_OK) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, json_data, json_len, &root);
            if (err == HU_OK && root) {
                load_string_array(root, "fillers", &s_filler_words, &s_filler_words_len);
                hu_json_free(alloc, root);
            }
            if (json_data)
                alloc->free(alloc->ctx, json_data, json_len);
        }
    }

    /* Load contractions */
    {
        char *json_data = NULL;
        size_t json_len = 0;
        hu_error_t err =
            hu_data_load(alloc, "conversation/contractions.json", &json_data, &json_len);
        if (err == HU_OK) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, json_data, json_len, &root);
            if (err == HU_OK && root) {
                load_contractions(root);
                hu_json_free(alloc, root);
            }
            if (json_data)
                alloc->free(alloc->ctx, json_data, json_len);
        }
    }

    /* Load conversation intros */
    {
        char *json_data = NULL;
        size_t json_len = 0;
        hu_error_t err =
            hu_data_load(alloc, "conversation/conversation_intros.json", &json_data, &json_len);
        if (err == HU_OK) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, json_data, json_len, &root);
            if (err == HU_OK && root) {
                load_string_array(root, "intros", &s_conversation_intros,
                                  &s_conversation_intros_len);
                hu_json_free(alloc, root);
            }
            if (json_data)
                alloc->free(alloc->ctx, json_data, json_len);
        }
    }

    /* Load starters */
    {
        char *json_data = NULL;
        size_t json_len = 0;
        hu_error_t err = hu_data_load(alloc, "conversation/starters.json", &json_data, &json_len);
        if (err == HU_OK) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, json_data, json_len, &root);
            if (err == HU_OK && root) {
                load_string_array(root, "starters", &s_starters, &s_starters_len);
                hu_json_free(alloc, root);
            }
            if (json_data)
                alloc->free(alloc->ctx, json_data, json_len);
        }
    }

    /* Load backchannel phrases */
    {
        char *json_data = NULL;
        size_t json_len = 0;
        hu_error_t err =
            hu_data_load(alloc, "conversation/backchannel_phrases.json", &json_data, &json_len);
        if (err == HU_OK) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, json_data, json_len, &root);
            if (err == HU_OK && root) {
                load_string_array(root, "phrases", &s_backchannel_phrases,
                                  &s_backchannel_phrases_len);
                hu_json_free(alloc, root);
            }
            if (json_data)
                alloc->free(alloc->ctx, json_data, json_len);
        }
    }

    /* Load emotional words */
    {
        char *json_data = NULL;
        size_t json_len = 0;
        hu_error_t err =
            hu_data_load(alloc, "conversation/emotional_words.json", &json_data, &json_len);
        if (err == HU_OK) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, json_data, json_len, &root);
            if (err == HU_OK && root) {
                load_string_array(root, "emotional", &s_emotional_words, &s_emotional_words_len);
                load_string_array(root, "positive", &s_positive_words, &s_positive_words_len);
                load_string_array(root, "negative", &s_negative_words, &s_negative_words_len);
                hu_json_free(alloc, root);
            }
            if (json_data)
                alloc->free(alloc->ctx, json_data, json_len);
        }
    }

    /* Load crisis keywords */
    {
        char *json_data = NULL;
        size_t json_len = 0;
        hu_error_t err =
            hu_data_load(alloc, "conversation/crisis_keywords.json", &json_data, &json_len);
        if (err == HU_OK) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, json_data, json_len, &root);
            if (err == HU_OK && root) {
                load_string_array(root, "keywords", &s_crisis_keywords, &s_crisis_keywords_len);
                hu_json_free(alloc, root);
            }
            if (json_data)
                alloc->free(alloc->ctx, json_data, json_len);
        }
    }

    /* Load personal sharing phrases */
    {
        char *json_data = NULL;
        size_t json_len = 0;
        hu_error_t err =
            hu_data_load(alloc, "conversation/personal_sharing.json", &json_data, &json_len);
        if (err == HU_OK) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, json_data, json_len, &root);
            if (err == HU_OK && root) {
                load_string_array(root, "phrases", &s_personal_sharing_phrases,
                                  &s_personal_sharing_phrases_len);
                hu_json_free(alloc, root);
            }
            if (json_data)
                alloc->free(alloc->ctx, json_data, json_len);
        }
    }

    /* Load engage words */
    {
        char *json_data = NULL;
        size_t json_len = 0;
        hu_error_t err =
            hu_data_load(alloc, "conversation/engage_words.json", &json_data, &json_len);
        if (err == HU_OK) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, json_data, json_len, &root);
            if (err == HU_OK && root) {
                load_string_array(root, "words", &s_engage_words, &s_engage_words_len);
                hu_json_free(alloc, root);
            }
            if (json_data)
                alloc->free(alloc->ctx, json_data, json_len);
        }
    }

    return HU_OK;
}

static void free_string_array(const char **arr, size_t count) {
    if (!arr || !s_conv_alloc)
        return;
    for (size_t i = 0; i < count; i++) {
        if (arr[i])
            s_conv_alloc->free(s_conv_alloc->ctx, (void *)arr[i], strlen(arr[i]) + 1);
    }
    s_conv_alloc->free(s_conv_alloc->ctx, (void *)arr, count * sizeof(const char *));
}

void hu_conversation_data_cleanup(void) {
    if (!s_conv_alloc)
        return;

    if (s_ai_disclosure_patterns) {
        free_string_array(s_ai_disclosure_patterns, s_ai_disclosure_patterns_len);
        s_ai_disclosure_patterns = NULL;
        s_ai_disclosure_patterns_len = 0;
    }

    if (s_filler_words) {
        free_string_array(s_filler_words, s_filler_words_len);
        s_filler_words = NULL;
        s_filler_words_len = 0;
    }

    if (s_contractions) {
        /* `load_contractions` heap-allocates per-entry `from` and `to`
         * copies (see lines 149-150) — the contraction struct itself
         * holds borrowed-looking `const char *` but they're owned. We
         * must free both copies before releasing the array, otherwise
         * each contraction reload leaks 158 + 135 bytes (293 b/cycle,
         * 18 cycles in the data-loader integration tests = 5,274 b).
         * The ASan leak summary at b9055/PR55 attributed this to
         * `load_contractions` -> `hu_conversation_data_init`. */
        for (size_t i = 0; i < s_contractions_len; i++) {
            if (s_contractions[i].from && s_contractions[i].from_len > 0)
                s_conv_alloc->free(s_conv_alloc->ctx, (void *)s_contractions[i].from,
                                   s_contractions[i].from_len + 1);
            if (s_contractions[i].to && s_contractions[i].to_len > 0)
                s_conv_alloc->free(s_conv_alloc->ctx, (void *)s_contractions[i].to,
                                   s_contractions[i].to_len + 1);
        }
        s_conv_alloc->free(s_conv_alloc->ctx, s_contractions,
                           s_contractions_len * sizeof(hu_conversation_contraction_t));
        s_contractions = NULL;
        s_contractions_len = 0;
    }

    if (s_conversation_intros) {
        free_string_array(s_conversation_intros, s_conversation_intros_len);
        s_conversation_intros = NULL;
        s_conversation_intros_len = 0;
    }

    if (s_starters) {
        free_string_array(s_starters, s_starters_len);
        s_starters = NULL;
        s_starters_len = 0;
    }

    if (s_backchannel_phrases) {
        free_string_array(s_backchannel_phrases, s_backchannel_phrases_len);
        s_backchannel_phrases = NULL;
        s_backchannel_phrases_len = 0;
    }

    if (s_emotional_words) {
        free_string_array(s_emotional_words, s_emotional_words_len);
        s_emotional_words = NULL;
        s_emotional_words_len = 0;
    }

    if (s_positive_words) {
        free_string_array(s_positive_words, s_positive_words_len);
        s_positive_words = NULL;
        s_positive_words_len = 0;
    }

    if (s_negative_words) {
        free_string_array(s_negative_words, s_negative_words_len);
        s_negative_words = NULL;
        s_negative_words_len = 0;
    }

    if (s_crisis_keywords) {
        free_string_array(s_crisis_keywords, s_crisis_keywords_len);
        s_crisis_keywords = NULL;
        s_crisis_keywords_len = 0;
    }

    if (s_personal_sharing_phrases) {
        free_string_array(s_personal_sharing_phrases, s_personal_sharing_phrases_len);
        s_personal_sharing_phrases = NULL;
        s_personal_sharing_phrases_len = 0;
    }

    if (s_engage_words) {
        free_string_array(s_engage_words, s_engage_words_len);
        s_engage_words = NULL;
        s_engage_words_len = 0;
    }

    s_conv_alloc = NULL;
}

/* Safe pos advance: snprintf returns the would-be length even when truncated.
 * Clamp to remaining buffer capacity to prevent out-of-bounds writes. */
#define POS_ADVANCE(w, pos, cap)          \
    do {                                  \
        if ((w) > 0) {                    \
            size_t _add = (size_t)(w);    \
            if ((pos) >= (cap) - 1)       \
                break;                    \
            if (_add > (cap) - 1 - (pos)) \
                _add = (cap) - 1 - (pos); \
            (pos) += _add;                \
        }                                 \
    } while (0)

/* Return pointer to start of needle match, or NULL. */
static const char *strstr_ci(const char *haystack, size_t hlen, const char *needle) {
    if (!haystack || !needle)
        return NULL;
    size_t nlen = strlen(needle);
    if (nlen > hlen)
        return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (b >= 'A' && b <= 'Z')
                b += 32;
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match)
            return haystack + i;
    }
    return NULL;
}

/* Extract value after pattern: skip spaces, copy until ., !, ?, , or end. Cap at max_len-1. */
static size_t extract_value_after(const char *msg, size_t msg_len, const char *after, char *out,
                                  size_t max_len) {
    if (!msg || !after || !out || max_len == 0)
        return 0;
    const char *p = after;
    const char *end = msg + msg_len;
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    size_t n = 0;
    while (p < end && n < max_len - 1) {
        char c = *p;
        if (c == '.' || c == '!' || c == '?' || c == ',' || c == '\n')
            break;
        out[n++] = c;
        p++;
    }
    out[n] = '\0';
    /* Trim trailing space */
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t'))
        out[--n] = '\0';
    return n;
}

/* ── Micro-moment extraction (F18) ───────────────────────────────────── */

int hu_conversation_extract_micro_moments(const char *msg, size_t msg_len, char facts[][256],
                                          char significances[][128], size_t max_facts) {
    if (!msg || msg_len == 0 || !facts || !significances || max_facts == 0)
        return 0;

    int count = 0;
    char fact_buf[256];

    /* Named entities: pet names */
    static const char *dog_patterns[] = {"my dog's name is ", "my dogs name is ", "my dog ",
                                         "our dog's name is ", NULL};
    for (const char **pat = dog_patterns; *pat && count < (int)max_facts; pat++) {
        const char *m = strstr_ci(msg, msg_len, *pat);
        if (m) {
            size_t vlen = extract_value_after(msg, msg_len, m + strlen(*pat), fact_buf, 256);
            if (vlen > 0 && vlen < 64) {
                int n = snprintf(facts[count], 256, "Their dog's name is %s", fact_buf);
                if (n > 0 && n < 256) {
                    snprintf(significances[count], 128, "pet");
                    count++;
                }
            }
            break;
        }
    }

    /* Named entities: friend, kid */
    if (count < (int)max_facts && hu_str_contains_ci_cstr(msg, msg_len, "my friend ")) {
        const char *m = strstr_ci(msg, msg_len, "my friend ");
        if (m) {
            size_t vlen = extract_value_after(msg, msg_len, m + 10, fact_buf, 256);
            if (vlen > 0 && vlen < 64) {
                int n = snprintf(facts[count], 256, "Their friend is %s", fact_buf);
                if (n > 0 && n < 256) {
                    snprintf(significances[count], 128, "relationship");
                    count++;
                }
            }
        }
    }
    if (count < (int)max_facts && (hu_str_contains_ci_cstr(msg, msg_len, "my kid ") ||
                                   hu_str_contains_ci_cstr(msg, msg_len, "my kid's name is "))) {
        const char *m = strstr_ci(msg, msg_len, "my kid's name is ");
        if (m) {
            size_t vlen = extract_value_after(msg, msg_len, m + 17, fact_buf, 256);
            if (vlen > 0 && vlen < 64) {
                int n = snprintf(facts[count], 256, "Their kid's name is %s", fact_buf);
                if (n > 0 && n < 256) {
                    snprintf(significances[count], 128, "family");
                    count++;
                }
            }
        } else {
            m = strstr_ci(msg, msg_len, "my kid ");
            if (m) {
                size_t vlen = extract_value_after(msg, msg_len, m + 7, fact_buf, 256);
                if (vlen > 0 && vlen < 64) {
                    int n = snprintf(facts[count], 256, "Their kid is %s", fact_buf);
                    if (n > 0 && n < 256) {
                        snprintf(significances[count], 128, "family");
                        count++;
                    }
                }
            }
        }
    }

    /* Places: moved to, live in, from */
    if (count < (int)max_facts) {
        const char *m = strstr_ci(msg, msg_len, "moved to ");
        if (m) {
            size_t vlen = extract_value_after(msg, msg_len, m + 9, fact_buf, 256);
            if (vlen > 0 && vlen < 80) {
                int n = snprintf(facts[count], 256, "They moved to %s", fact_buf);
                if (n > 0 && n < 256) {
                    snprintf(significances[count], 128, "location");
                    count++;
                }
            }
        } else if ((m = strstr_ci(msg, msg_len, "live in ")) != NULL) {
            size_t vlen = extract_value_after(msg, msg_len, m + 8, fact_buf, 256);
            if (vlen > 0 && vlen < 80) {
                int n = snprintf(facts[count], 256, "They live in %s", fact_buf);
                if (n > 0 && n < 256) {
                    snprintf(significances[count], 128, "location");
                    count++;
                }
            }
        } else if ((m = strstr_ci(msg, msg_len, "i'm from ")) != NULL ||
                   (m = strstr_ci(msg, msg_len, "im from ")) != NULL) {
            size_t skip = (m[1] == '\'' || m[1] == 'm') ? 9 : 8;
            size_t vlen = extract_value_after(msg, msg_len, m + skip, fact_buf, 256);
            if (vlen > 0 && vlen < 80) {
                int n = snprintf(facts[count], 256, "They are from %s", fact_buf);
                if (n > 0 && n < 256) {
                    snprintf(significances[count], 128, "location");
                    count++;
                }
            }
        }
    }

    /* Preferences: i love, i hate, my favorite */
    if (count < (int)max_facts) {
        const char *m = strstr_ci(msg, msg_len, "i love ");
        if (m) {
            size_t vlen = extract_value_after(msg, msg_len, m + 7, fact_buf, 256);
            if (vlen > 0 && vlen < 120) {
                int n = snprintf(facts[count], 256, "They love %s", fact_buf);
                if (n > 0 && n < 256) {
                    snprintf(significances[count], 128, "preference");
                    count++;
                }
            }
        } else if ((m = strstr_ci(msg, msg_len, "i hate ")) != NULL) {
            size_t vlen = extract_value_after(msg, msg_len, m + 7, fact_buf, 256);
            if (vlen > 0 && vlen < 120) {
                int n = snprintf(facts[count], 256, "They hate %s", fact_buf);
                if (n > 0 && n < 256) {
                    snprintf(significances[count], 128, "preference");
                    count++;
                }
            }
        } else if ((m = strstr_ci(msg, msg_len, "my favorite ")) != NULL) {
            size_t vlen = extract_value_after(msg, msg_len, m + 11, fact_buf, 256);
            if (vlen > 0 && vlen < 120) {
                int n = snprintf(facts[count], 256, "Their favorite is %s", fact_buf);
                if (n > 0 && n < 256) {
                    snprintf(significances[count], 128, "preference");
                    count++;
                }
            }
        }
    }

    /* Life events */
    if (count < (int)max_facts) {
        if (hu_str_contains_ci_cstr(msg, msg_len, "got married")) {
            snprintf(facts[count], 256, "They got married");
            snprintf(significances[count], 128, "life_event");
            count++;
        } else if (hu_str_contains_ci_cstr(msg, msg_len, "had a baby")) {
            snprintf(facts[count], 256, "They had a baby");
            snprintf(significances[count], 128, "life_event");
            count++;
        } else if (hu_str_contains_ci_cstr(msg, msg_len, "new job at ")) {
            const char *m = strstr_ci(msg, msg_len, "new job at ");
            if (m) {
                size_t vlen = extract_value_after(msg, msg_len, m + 11, fact_buf, 256);
                if (vlen > 0 && vlen < 80) {
                    int n = snprintf(facts[count], 256, "New job at %s", fact_buf);
                    if (n > 0 && n < 256) {
                        snprintf(significances[count], 128, "career");
                        count++;
                    }
                }
            }
        }
    }

    return count;
}

#define HU_CALLBACK_MAX_TOPICS 32
#define HU_CALLBACK_TOPIC_BUF  64
#define HU_CALLBACK_SCORE_MIN  3

typedef struct {
    char phrase[HU_CALLBACK_TOPIC_BUF];
    size_t phrase_len;
    size_t first_idx;
    size_t last_idx;
    int turn_count;
    bool has_unresolved_question;
    int score;
} hu_callback_topic_t;

static void extract_topics_from_text(const char *text, size_t text_len, hu_callback_topic_t *topics,
                                     size_t *topic_count) {
    if (!text || text_len == 0 || !topics || !topic_count || *topic_count >= HU_CALLBACK_MAX_TOPICS)
        return;
    const char *p = text;
    const char *end = text + text_len;
    while (p < end && *topic_count < HU_CALLBACK_MAX_TOPICS) {
        while (p < end && !isalnum((unsigned char)*p) && *p != '"' && *p != '\'')
            p++;
        if (p >= end)
            break;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            const char *start = p;
            while (p < end && *p != q)
                p++;
            if (p > start && p - start < HU_CALLBACK_TOPIC_BUF - 1) {
                size_t len = (size_t)(p - start);
                memcpy(topics[*topic_count].phrase, start, len);
                topics[*topic_count].phrase[len] = '\0';
                topics[*topic_count].phrase_len = len;
                (*topic_count)++;
            }
            if (p < end)
                p++;
            continue;
        }
        if (p + 6 <= end && strncasecmp(p, "about ", 6) == 0) {
            const char *trigger = p + 6;
            /* Skip determiners/stopwords: the, a, an, my, our, your, his, her, its */
            for (;;) {
                while (trigger < end && *trigger == ' ')
                    trigger++;
                static const char *stops[] = {"the ",  "a ",   "an ",  "my ", "our ",
                                              "your ", "his ", "her ", "its "};
                bool skipped = false;
                for (size_t si = 0; si < sizeof(stops) / sizeof(stops[0]); si++) {
                    size_t sl = strlen(stops[si]);
                    if ((size_t)(end - trigger) >= sl && strncasecmp(trigger, stops[si], sl) == 0) {
                        trigger += sl;
                        skipped = true;
                        break;
                    }
                }
                if (!skipped)
                    break;
            }
            p = trigger;
            while (p < end && (isalnum((unsigned char)*p) || *p == '_' || *p == '-'))
                p++;
            if (p > trigger && (size_t)(p - trigger) < HU_CALLBACK_TOPIC_BUF - 1) {
                size_t len = (size_t)(p - trigger);
                memcpy(topics[*topic_count].phrase, trigger, len);
                topics[*topic_count].phrase[len] = '\0';
                topics[*topic_count].phrase_len = len;
                (*topic_count)++;
            }
            continue;
        }
        if (p + 10 <= end && strncasecmp(p, "regarding ", 10) == 0) {
            const char *trigger = p + 10;
            for (;;) {
                while (trigger < end && *trigger == ' ')
                    trigger++;
                static const char *stops[] = {"the ",  "a ",   "an ",  "my ", "our ",
                                              "your ", "his ", "her ", "its "};
                bool skipped = false;
                for (size_t si = 0; si < sizeof(stops) / sizeof(stops[0]); si++) {
                    size_t sl = strlen(stops[si]);
                    if ((size_t)(end - trigger) >= sl && strncasecmp(trigger, stops[si], sl) == 0) {
                        trigger += sl;
                        skipped = true;
                        break;
                    }
                }
                if (!skipped)
                    break;
            }
            p = trigger;
            while (p < end && (isalnum((unsigned char)*p) || *p == '_' || *p == '-'))
                p++;
            if (p > trigger && (size_t)(p - trigger) < HU_CALLBACK_TOPIC_BUF - 1) {
                size_t len = (size_t)(p - trigger);
                memcpy(topics[*topic_count].phrase, trigger, len);
                topics[*topic_count].phrase[len] = '\0';
                topics[*topic_count].phrase_len = len;
                (*topic_count)++;
            }
            continue;
        }
        if (isupper((unsigned char)*p)) {
            const char *start = p;
            while (p < end && (isalnum((unsigned char)*p) || *p == '_' || *p == '-'))
                p++;
            if (p > start && (size_t)(p - start) >= 2 &&
                (size_t)(p - start) < HU_CALLBACK_TOPIC_BUF - 1) {
                size_t len = (size_t)(p - start);
                memcpy(topics[*topic_count].phrase, start, len);
                topics[*topic_count].phrase[len] = '\0';
                topics[*topic_count].phrase_len = len;
                (*topic_count)++;
            }
            continue;
        }
        p++;
    }
}

static bool topic_in_recent(const hu_callback_topic_t *t, const hu_channel_history_entry_t *entries,
                            size_t count, size_t recent_start) {
    for (size_t i = recent_start; i < count; i++) {
        const char *text = entries[i].text;
        size_t len = strlen(text);
        for (size_t j = 0; j + t->phrase_len <= len; j++) {
            if (strncasecmp(text + j, t->phrase, t->phrase_len) != 0)
                continue;
            char before = (j > 0) ? (char)tolower((unsigned char)text[j - 1]) : ' ';
            char after = (j + t->phrase_len < len)
                             ? (char)tolower((unsigned char)text[j + t->phrase_len])
                             : ' ';
            if (!isalnum((unsigned char)before) && !isalnum((unsigned char)after))
                return true;
        }
    }
    return false;
}

char *hu_conversation_build_callback(hu_allocator_t *alloc,
                                     const hu_channel_history_entry_t *entries, size_t count,
                                     size_t *out_len) {
    if (!alloc || !out_len)
        return NULL;
    *out_len = 0;
    if (!entries || count < 6)
        return NULL;

    /* ~20% probability: use hash of last message as seed (avoids count%5 restriction) */
    const hu_channel_history_entry_t *last = &entries[count - 1];
    uint32_t hash = 0;
    for (size_t i = 0; i < 20 && i < strlen(last->text); i++)
        hash = hash * 31u + (uint32_t)(unsigned char)last->text[i];
    if (hash % 5u != 0)
        return NULL;

    size_t half = count / 2;
    size_t recent_start = count > 3 ? count - 3 : 0;

    hu_callback_topic_t all_topics[HU_CALLBACK_MAX_TOPICS];
    size_t num_topics = 0;
    memset(all_topics, 0, sizeof(all_topics));

    for (size_t i = 0; i < half && num_topics < HU_CALLBACK_MAX_TOPICS; i++) {
        const char *text = entries[i].text;
        size_t tl = strlen(text);
        hu_callback_topic_t local[8];
        size_t nlocal = 0;
        memset(local, 0, sizeof(local));
        extract_topics_from_text(text, tl, local, &nlocal);
        for (size_t k = 0; k < nlocal && num_topics < HU_CALLBACK_MAX_TOPICS; k++) {
            bool found = false;
            for (size_t j = 0; j < num_topics; j++) {
                if (all_topics[j].phrase_len == local[k].phrase_len &&
                    strncasecmp(all_topics[j].phrase, local[k].phrase, local[k].phrase_len) == 0) {
                    all_topics[j].last_idx = i;
                    all_topics[j].turn_count++;
                    if (strchr(text, '?'))
                        all_topics[j].has_unresolved_question = true;
                    found = true;
                    break;
                }
            }
            if (!found) {
                all_topics[num_topics] = local[k];
                all_topics[num_topics].first_idx = i;
                all_topics[num_topics].last_idx = i;
                all_topics[num_topics].turn_count = 1;
                all_topics[num_topics].has_unresolved_question = (strchr(text, '?') != NULL);
                num_topics++;
            }
        }
    }

    hu_callback_topic_t *best = NULL;
    int best_score = HU_CALLBACK_SCORE_MIN - 1;
    for (size_t i = 0; i < num_topics; i++) {
        if (topic_in_recent(&all_topics[i], entries, count, recent_start))
            continue;
        if (all_topics[i].phrase_len < 2)
            continue;
        int score = (int)all_topics[i].turn_count * 2;
        if (all_topics[i].has_unresolved_question)
            score += 3;
        size_t msgs_ago = count - 1 - all_topics[i].last_idx;
        if (msgs_ago < 4)
            score -= 2;
        else if (msgs_ago >= 6)
            score += 1;
        all_topics[i].score = score;
        if (score > best_score) {
            best_score = score;
            best = &all_topics[i];
        }
    }

    if (!best || best_score < HU_CALLBACK_SCORE_MIN)
        return NULL;

    size_t msgs_ago = count - 1 - best->last_idx;
    char buf[512];
    int w = snprintf(buf, sizeof(buf),
                     "\n### Thread Callback Opportunity\n"
                     "An earlier topic that could be naturally revisited:\n"
                     "Topic: \"%.*s\"\n"
                     "Last discussed: %zu messages ago\n"
                     "Consider: \"oh wait, going back to %.*s...\" or weave it in naturally\n"
                     "Only use this if it fits the current flow — don't force it.\n",
                     (int)best->phrase_len, best->phrase, msgs_ago, (int)best->phrase_len,
                     best->phrase);
    if (w <= 0 || (size_t)w >= sizeof(buf))
        return NULL;

    char *result = hu_strndup(alloc, buf, (size_t)w);
    if (result)
        *out_len = (size_t)w;
    return result;
}

/* Data-driven conversation metrics (single O(n) pass over entries) */
typedef struct {
    size_t total_msgs;
    size_t their_msgs;
    size_t their_total_chars;
    size_t their_recent_n;
    size_t their_recent_chars;
    size_t exchanges;
    size_t first_half_chars;
    size_t first_half_n;
    size_t second_half_chars;
    size_t second_half_n;
    size_t exclamation_count;
    size_t question_count;
    size_t longest_their_msg;
    size_t msgs_all_lower;
    size_t msgs_no_period_end;
    size_t rapid_exchanges; /* direction changes in last 6 msgs */
    bool has_link;
    bool recent_shorter_than_earlier;
    char repeated_word[32];
    int repeated_word_hits;
} hu_convo_metrics_t;

static void compute_convo_metrics(const hu_channel_history_entry_t *entries, size_t count,
                                  hu_convo_metrics_t *m) {
    memset(m, 0, sizeof(*m));
    if (!entries || count == 0)
        return;
    m->total_msgs = count;
    size_t mid = count / 2;
    size_t recent_start = count > 6 ? count - 6 : 0;
    bool prev_from_me = entries[recent_start].from_me;

    for (size_t i = 0; i < count; i++) {
        const char *t = entries[i].text;
        size_t tl = strlen(t);
        if (i == 0 || entries[i].from_me != prev_from_me) {
            m->exchanges++;
            if (i >= recent_start)
                m->rapid_exchanges++;
        }
        prev_from_me = entries[i].from_me;

        if (!entries[i].from_me && tl > 2) {
            m->their_msgs++;
            m->their_total_chars += tl;
            if (tl > m->longest_their_msg)
                m->longest_their_msg = tl;
            if (i >= mid) {
                m->second_half_chars += tl;
                m->second_half_n++;
            } else {
                m->first_half_chars += tl;
                m->first_half_n++;
            }
            if (i >= count - 3) {
                m->their_recent_n++;
                m->their_recent_chars += tl;
            }
            for (size_t j = 0; j < tl; j++) {
                if (t[j] == '!')
                    m->exclamation_count++;
                if (t[j] == '?')
                    m->question_count++;
            }
            bool has_upper = false;
            for (size_t j = 0; j < tl; j++) {
                if (t[j] >= 'A' && t[j] <= 'Z') {
                    has_upper = true;
                    break;
                }
            }
            if (!has_upper && tl > 0)
                m->msgs_all_lower++;
            char last_c = t[tl - 1];
            if (last_c != '.' && last_c != '!' && last_c != '?')
                m->msgs_no_period_end++;
            if (tl >= 4 && (memcmp(t, "http", 4) == 0 || strstr(t, ".com") != NULL))
                m->has_link = true;
        }
    }
    if (m->first_half_n > 0 && m->second_half_n > 0) {
        size_t avg1 = m->first_half_chars / m->first_half_n;
        size_t avg2 = m->second_half_chars / m->second_half_n;
        m->recent_shorter_than_earlier = (avg1 > 40 && avg2 < avg1 / 2);
    }
    /* Word frequency for repeated theme (words 5+ chars, no stopword list) */
    {
        typedef struct {
            char word[32];
            int hits;
        } wf_t;
        wf_t freq[16];
        size_t fn = 0;
        memset(freq, 0, sizeof(freq));
        for (size_t i = 0; i < count && fn < 16; i++) {
            if (entries[i].from_me)
                continue;
            const char *p = entries[i].text;
            size_t tl = strlen(p);
            size_t wi = 0;
            while (wi < tl) {
                while (wi < tl && (p[wi] == ' ' || p[wi] == '\n'))
                    wi++;
                size_t ws = wi;
                while (wi < tl && p[wi] != ' ' && p[wi] != '\n')
                    wi++;
                size_t wlen = wi - ws;
                if (wlen < 5 || wlen > 30)
                    continue;
                char word[32];
                for (size_t k = 0; k < wlen && k < 31; k++) {
                    word[k] = (char)tolower((unsigned char)p[ws + k]);
                }
                word[wlen < 31 ? wlen : 31] = '\0';
                bool found = false;
                for (size_t f = 0; f < fn; f++) {
                    if (strcmp(freq[f].word, word) == 0) {
                        freq[f].hits++;
                        found = true;
                        break;
                    }
                }
                if (!found && fn < 16) {
                    memcpy(freq[fn].word, word, wlen + 1);
                    freq[fn].hits = 1;
                    fn++;
                }
            }
        }
        for (size_t f = 0; f < fn; f++) {
            if (freq[f].hits >= 3 && freq[f].hits > m->repeated_word_hits) {
                m->repeated_word_hits = freq[f].hits;
                strncpy(m->repeated_word, freq[f].word, sizeof(m->repeated_word) - 1);
                m->repeated_word[sizeof(m->repeated_word) - 1] = '\0';
            }
        }
    }
}

static const char *day_name(int wday) {
    static const char *days[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                 "Thursday", "Friday", "Saturday"};
    return (wday >= 0 && wday < 7) ? days[wday] : "?";
}

char *hu_conversation_build_awareness(hu_allocator_t *alloc,
                                      const hu_channel_history_entry_t *entries, size_t count,
                                      const struct hu_persona *persona, size_t *out_len) {
    if (!alloc || !out_len)
        return NULL;
    *out_len = 0;
    if (!entries || count == 0)
        return NULL;

    char *buf = (char *)alloc->alloc(alloc->ctx, CTX_BUF_CAP);
    if (!buf)
        return NULL;

    size_t pos = 0;
    int w;

    /* ── Conversation thread ─────────────────────────────────────────── */
    w = snprintf(buf + pos, CTX_BUF_CAP - pos, "\n--- Recent conversation thread ---\n");
    POS_ADVANCE(w, pos, CTX_BUF_CAP);

    for (size_t i = 0; i < count; i++) {
        const char *who = entries[i].from_me ? "You" : "Them";
        w = snprintf(buf + pos, CTX_BUF_CAP - pos, "[%s] %s: %s\n", entries[i].timestamp, who,
                     entries[i].text);
        POS_ADVANCE(w, pos, CTX_BUF_CAP);
    }

    w = snprintf(buf + pos, CTX_BUF_CAP - pos, "--- End of recent thread ---\n\n");
    POS_ADVANCE(w, pos, CTX_BUF_CAP);

    /* ── Data-driven conversation awareness ─────────────────────────── */
    {
        hu_convo_metrics_t m;
        compute_convo_metrics(entries, count, &m);

        w = snprintf(buf + pos, CTX_BUF_CAP - pos, "--- Conversation awareness ---\n");
        POS_ADVANCE(w, pos, CTX_BUF_CAP);

        /* Message count and length metrics */
        if (m.their_msgs > 0) {
            size_t avg = m.their_total_chars / m.their_msgs;
            const char *depth = (avg < 30)   ? "brief texting"
                                : (avg < 80) ? "moderate-depth texting"
                                             : "longer-form messages";
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- %zu messages exchanged, averaging %zu chars — %s\n", m.total_msgs, avg,
                         depth);
            POS_ADVANCE(w, pos, CTX_BUF_CAP);

            if (m.their_recent_n > 0) {
                size_t recent_avg = m.their_recent_chars / m.their_recent_n;
                if (recent_avg < avg / 2 && avg > 30) {
                    w = snprintf(
                        buf + pos, CTX_BUF_CAP - pos,
                        "- Their last %zu messages averaged %zu chars — getting more brief\n",
                        m.their_recent_n, recent_avg);
                } else if (recent_avg > avg * 2 && m.their_recent_n >= 2) {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                 "- Their last %zu messages averaged %zu chars — opening up more\n",
                                 m.their_recent_n, recent_avg);
                } else {
                    w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                 "- Their last %zu messages averaged %zu chars\n", m.their_recent_n,
                                 recent_avg);
                }
                POS_ADVANCE(w, pos, CTX_BUF_CAP);
            }
        }

        /* Pace (from exchange density) */
        w = 0;
        if (m.rapid_exchanges >= 4) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Pace: rapid back-and-forth — active, casual flow\n");
        } else if (m.exchanges >= 6) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Pace: %zu exchanges — steady conversation flow\n", m.exchanges);
        } else if (m.exchanges <= 2) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos, "- Pace: just started — %zu exchange(s)\n",
                         m.exchanges);
        }
        POS_ADVANCE(w, pos, CTX_BUF_CAP);

        /* Time (descriptive only, no behavior prescription) */
        {
            time_t now = time(NULL);
            struct tm lt_buf;
            struct tm *lt = localtime_r(&now, &lt_buf);
            if (lt) {
                int hour = lt->tm_hour;
                int min = lt->tm_min;
                const char *ampm = (hour >= 12) ? "PM" : "AM";
                int h12 = (hour % 12) ? (hour % 12) : 12;
                w = snprintf(buf + pos, CTX_BUF_CAP - pos, "- Time: %d:%02d %s %s\n", h12, min,
                             ampm, day_name(lt->tm_wday));
                POS_ADVANCE(w, pos, CTX_BUF_CAP);
            }
        }

        /* Emotional tone from punctuation (data, not keywords) */
        if (m.exclamation_count >= 3 && m.their_msgs > 0) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Their recent messages carry high exclamation density — excited tone\n");
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        } else if (m.recent_shorter_than_earlier) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Their messages are getting shorter — may be winding down\n");
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }
        if (m.question_count > 0 && m.their_msgs > 0) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- They asked %zu question(s) — answer directly\n", m.question_count);
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }

        /* Conversation arc (data-driven description) */
        if (m.exchanges <= 2) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Flow: %zu exchanges in — conversation just started\n", m.exchanges);
        } else if (m.exchanges <= 6) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Flow: %zu exchanges in — energy building, good for deeper engagement\n",
                         m.exchanges);
        } else if (m.recent_shorter_than_earlier) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Flow: %zu exchanges in — winding down, they're getting briefer\n",
                         m.exchanges);
        } else {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Flow: %zu exchanges in — well into the conversation\n", m.exchanges);
        }
        POS_ADVANCE(w, pos, CTX_BUF_CAP);

        /* Verbosity guidance (descriptive) */
        if (m.their_msgs > 0) {
            size_t avg = m.their_total_chars / m.their_msgs;
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- Their messages average %zu characters. Match their brevity.\n", avg);
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }

        /* Repeated theme (data-driven) */
        if (m.repeated_word_hits >= 3) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- They've mentioned \"%s\" %d times — it matters to them\n",
                         m.repeated_word, m.repeated_word_hits);
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }

        /* Link share (structural: contains http/.com) */
        if (m.has_link) {
            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                         "- They shared a link — acknowledge or comment on it\n");
            POS_ADVANCE(w, pos, CTX_BUF_CAP);
        }

        /* Topic deflection (structural pattern: Q from them, A from you, new topic from them) */
        if (count >= 3) {
            for (size_t i = 2; i < count; i++) {
                if (!entries[i - 2].from_me && entries[i - 1].from_me && !entries[i].from_me) {
                    const char *q = entries[i - 2].text;
                    const char *r = entries[i].text;
                    bool had_q = (strchr(q, '?') != NULL);
                    if (had_q && strlen(r) > 10) {
                        bool shares = false;
                        for (size_t j = 0; j + 4 < strlen(q); j++) {
                            char cq[6];
                            for (int k = 0; k < 5 && j + (size_t)k < strlen(q); k++)
                                cq[k] = (char)tolower((unsigned char)q[j + k]);
                            cq[5] = '\0';
                            for (size_t k = 0; k + 4 < strlen(r); k++) {
                                char cr[6];
                                for (int l = 0; l < 5 && k + (size_t)l < strlen(r); l++)
                                    cr[l] = (char)tolower((unsigned char)r[k + l]);
                                cr[5] = '\0';
                                if (strcmp(cq, cr) == 0) {
                                    shares = true;
                                    break;
                                }
                            }
                            if (shares)
                                break;
                        }
                        if (!shares) {
                            w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                                         "- They may have shifted topic — follow their lead\n");
                            POS_ADVANCE(w, pos, CTX_BUF_CAP);
                            break;
                        }
                    }
                }
            }
        }

        w = snprintf(buf + pos, CTX_BUF_CAP - pos,
                     "\nUse this context naturally. Reference specific details they mentioned. "
                     "Do NOT summarize or acknowledge this context aloud.\n");
        POS_ADVANCE(w, pos, CTX_BUF_CAP);
    }

    /* Anti-AI style: prompt-level guidance so LLM produces natural output the first time.
     * Use persona style_rules when available; otherwise fallback to built-in rules. */
    if (persona && persona->style_rules && persona->style_rules_count > 0) {
        w = snprintf(buf + pos, CTX_BUF_CAP - pos, "\n--- CRITICAL STYLE RULES ---\n");
        POS_ADVANCE(w, pos, CTX_BUF_CAP);
        for (size_t i = 0; i < persona->style_rules_count && pos < CTX_BUF_CAP; i++) {
            if (persona->style_rules[i]) {
                w = snprintf(buf + pos, CTX_BUF_CAP - pos, "%s\n", persona->style_rules[i]);
                POS_ADVANCE(w, pos, CTX_BUF_CAP);
            }
        }
    } else {
        w = snprintf(
            buf + pos, CTX_BUF_CAP - pos,
            "\n--- CRITICAL STYLE RULES ---\n"
            "Never start with \"I'd be happy to\", \"Great question!\", \"That's a great "
            "question\", \"Feel free to\", \"Don't hesitate to\", \"I appreciate\", "
            "\"Let me know if\", or similar AI-sounding openers. Never say \"As an AI\" or "
            "\"As a language model\". Just respond naturally like a real person texting.\n");
        POS_ADVANCE(w, pos, CTX_BUF_CAP);
    }

    /* Situational length calibration from the last incoming message */
    if (entries && count > 0) {
        const char *last_their_msg = NULL;
        size_t last_their_len = 0;
        for (size_t i = count; i > 0; i--) {
            if (!entries[i - 1].from_me) {
                last_their_msg = entries[i - 1].text;
                last_their_len = strlen(last_their_msg);
                break;
            }
        }
        if (last_their_msg && last_their_len > 0) {
            size_t cal_len = hu_conversation_calibrate_length(
                last_their_msg, last_their_len, entries, count, buf + pos, CTX_BUF_CAP - pos);
            pos += cal_len;
        }
    }

    buf[pos] = '\0';
    *out_len = pos;
    if (pos + 1 < CTX_BUF_CAP && alloc->realloc) {
        char *shrunk = (char *)alloc->realloc(alloc->ctx, buf, CTX_BUF_CAP, pos + 1);
        if (shrunk)
            buf = shrunk;
    }
    return buf;
}

/* ── Conversation Quality Evaluator (qualitative, context-driven) ─────── */

static void compute_their_avg_len(const hu_channel_history_entry_t *entries, size_t count,
                                  size_t *out_avg, size_t *out_recent_avg) {
    *out_avg = 0;
    *out_recent_avg = 0;
    if (!entries || count == 0)
        return;
    size_t total_chars = 0;
    size_t their_n = 0;
    size_t recent_chars = 0;
    size_t recent_n = 0;
    size_t recent_start = count > 4 ? count - 4 : 0;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].from_me)
            continue;
        size_t tl = strlen(entries[i].text);
        if (tl > 0) {
            total_chars += tl;
            their_n++;
            if (i >= recent_start) {
                recent_chars += tl;
                recent_n++;
            }
        }
    }
    if (their_n > 0)
        *out_avg = total_chars / their_n;
    if (recent_n > 0)
        *out_recent_avg = recent_chars / recent_n;
}

/* Substantive-context floor for the brevity too-short penalty. When their
 * recent messages average below this, the conversation is terse banter and a
 * one-word reply is natural — keep full brevity marks. At/above this, their
 * messages carry real content and a clipped fragment is an under-match. */
#define HU_QUALITY_SUBSTANTIVE_REF_LEN 20u

hu_quality_score_t hu_conversation_evaluate_quality(const char *response, size_t response_len,
                                                    const hu_channel_history_entry_t *entries,
                                                    size_t count, uint32_t max_chars) {
    hu_quality_score_t score = {0, 0, 0, 0, 0, false, {0}};
    if (!response || response_len == 0)
        return score;

    size_t their_avg = 0;
    size_t their_recent_avg = 0;
    compute_their_avg_len(entries, count, &their_avg, &their_recent_avg);
    size_t ref_len = their_recent_avg > 0 ? their_recent_avg : (their_avg > 0 ? their_avg : 50);
    if (ref_len < 10)
        ref_len = 10;

    /* Brevity (0-25): reward MATCHING their energy, not minimizing length.
     * Two-signal too-short penalty: under-matching is as much a mismatch as
     * over-matching, but only fires when their recent messages are
     * substantive (ref_len >= floor) AND the reply is far below their energy
     * (ratio < 0.2). A terse reply to terse banter keeps full marks. Without
     * this, the A/B scorer prefers a clipped fragment over a natural reply. */
    double ratio = (double)response_len / (double)ref_len;
    if (ref_len >= HU_QUALITY_SUBSTANTIVE_REF_LEN && ratio < 0.2)
        score.brevity = 10;
    else if (ratio <= 1.5)
        score.brevity = 25;
    else if (ratio <= 3.0)
        score.brevity = 20;
    else if (ratio <= 6.0)
        score.brevity = 10;
    else
        score.brevity = 0;
    if (max_chars > 0 && response_len > max_chars * 2)
        score.brevity = 0;

    /* Validation (0-25): does response reflect their energy? Use structural cues. */
    if (entries && count > 0) {
        size_t their_excl = 0;
        size_t their_q = 0;
        for (size_t i = count > 3 ? count - 3 : 0; i < count; i++) {
            if (entries[i].from_me)
                continue;
            const char *t = entries[i].text;
            for (size_t j = 0; t[j]; j++) {
                if (t[j] == '!')
                    their_excl++;
                if (t[j] == '?')
                    their_q++;
            }
        }
        int resp_excl = 0;
        for (size_t k = 0; k < response_len; k++) {
            if (response[k] == '!')
                resp_excl++;
        }
        if (their_excl >= 2 && resp_excl > 0)
            score.validation = 25;
        else if (their_q > 0 && response_len > 5)
            score.validation = 20;
        else
            score.validation = 18;
    } else {
        score.validation = 20;
    }

    /* Warmth (0-25): structural (exclamation, personal tone) vs robotic tells */
    int warmth = 15;
    if (strchr(response, '!'))
        warmth += 5;
    if (hu_str_contains_ci_cstr(response, response_len, "as an AI") ||
        hu_str_contains_ci_cstr(response, response_len, "as a language model"))
        warmth -= 20;
    else if (hu_str_contains_ci_cstr(response, response_len, "I'd be happy to") ||
             hu_str_contains_ci_cstr(response, response_len, "let me know if") ||
             hu_str_contains_ci_cstr(response, response_len, "feel free") ||
             hu_str_contains_ci_cstr(response, response_len, "certainly"))
        warmth -= 18;
    if (warmth < 0)
        warmth = 0;
    if (warmth > 25)
        warmth = 25;
    score.warmth = warmth;

    /* Naturalness (0-25): structural (markdown, lists, AI tells) */
    int nat = 20;
    if (hu_str_contains_ci_cstr(response, response_len, "**") ||
        hu_str_contains_ci_cstr(response, response_len, "##") ||
        hu_str_contains_ci_cstr(response, response_len, "```"))
        nat -= 12;
    else if (hu_str_contains_ci_cstr(response, response_len, "- ") ||
             hu_str_contains_ci_cstr(response, response_len, "1. "))
        nat -= 3;
    if (strchr(response, ';'))
        nat -= 5;
    if (strstr(response, " — ") || strstr(response, " - "))
        nat -= 3;
    int excl_count = 0;
    for (size_t k = 0; k < response_len; k++) {
        if (response[k] == '!')
            excl_count++;
    }
    if (excl_count > 2 && response_len < 200)
        nat -= 5;
    if (nat < 0)
        nat = 0;
    if (nat > 25)
        nat = 25;
    score.naturalness = nat;

    score.total = score.brevity + score.validation + score.warmth + score.naturalness;

    /* needs_revision on gross length mismatch. Sprint 39: 5× threshold
     * (was 10×) aligns with G5's 8× guard and post-mortem action item —
     * the 2026-05-12 leak was ~22× rolling avg but still slipped through
     * when quality only fired at 10×. */
    bool gross_length = (ratio > 5.0 || (ratio < 0.1 && response_len > 5));
    bool gross_structural = (score.warmth < 5 || score.naturalness < 5);
    score.needs_revision = gross_length || gross_structural;

    if (score.needs_revision && ratio > 5.0 && their_avg > 0) {
        int n = snprintf(score.guidance, sizeof(score.guidance),
                         "Your response was %zu chars but their last messages averaged %zu chars. "
                         "Tighten up significantly. Match their energy.",
                         response_len, their_avg);
        if (n <= 0 || (size_t)n >= sizeof(score.guidance))
            score.guidance[0] = '\0';
    } else if (score.needs_revision && ratio < 0.2 && response_len > 50) {
        snprintf(
            score.guidance, sizeof(score.guidance),
            "Your response was much shorter than their typical depth. Consider adding a bit more.");
    } else if (score.needs_revision && gross_structural) {
        if (score.warmth < 5 && score.naturalness < 5) {
            snprintf(score.guidance, sizeof(score.guidance),
                     "Your response felt distant and formal. Drop the formality, show you care.");
        } else if (score.warmth < 5) {
            snprintf(score.guidance, sizeof(score.guidance),
                     "Your response felt distant. Show you care.");
        } else if (score.naturalness < 5) {
            snprintf(score.guidance, sizeof(score.guidance),
                     "Your phrasing felt formal. Drop the formality.");
        } else {
            snprintf(score.guidance, sizeof(score.guidance),
                     "Response has AI-sounding or structural tells. Rewrite naturally.");
        }
    } else {
        score.guidance[0] = '\0';
    }
    return score;
}

/* ── Commitment detection with deadline parsing (F20) ───────────────────── */

/* Parse deadline from message. Returns unix timestamp or 0 if no deadline found.
 * Heuristics: tomorrow, next week, in X days/hours, tonight, this weekend. */
int64_t hu_conversation_parse_deadline(const char *msg, size_t msg_len, int64_t now_ts) {
    if (!msg || msg_len == 0)
        return 0;
    if (hu_str_contains_ci_cstr(msg, msg_len, "tomorrow"))
        return now_ts + 86400;
    if (hu_str_contains_ci_cstr(msg, msg_len, "next week"))
        return now_ts + 604800;
    if (hu_str_contains_ci_cstr(msg, msg_len, "tonight")) {
        struct tm tm_buf;
        time_t t = (time_t)now_ts;
#if defined(_WIN32) && !defined(__CYGWIN__)
        struct tm *lt = (localtime_s(&tm_buf, &t) == 0) ? &tm_buf : NULL;
#else
        struct tm *lt = localtime_r(&t, &tm_buf);
#endif
        if (lt && lt->tm_hour < 18)
            return now_ts + (24 - lt->tm_hour) * 3600 - lt->tm_min * 60 - lt->tm_sec;
        return now_ts + 12 * 3600;
    }
    if (hu_str_contains_ci_cstr(msg, msg_len, "this weekend")) {
        struct tm tm_buf;
        time_t t = (time_t)now_ts;
#if defined(_WIN32) && !defined(__CYGWIN__)
        struct tm *lt = (localtime_s(&tm_buf, &t) == 0) ? &tm_buf : NULL;
#else
        struct tm *lt = localtime_r(&t, &tm_buf);
#endif
        if (lt) {
            int days = (6 - lt->tm_wday + 7) % 7;
            return now_ts + (int64_t)days * 86400;
        }
        return now_ts + 86400 * 5; /* fallback: ~5 days */
    }
    /* "in X days" */
    for (unsigned int d = 1; d <= 365; d++) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "in %u day", d);
        if (n > 0 && (size_t)n < sizeof(buf) && hu_str_contains_ci_cstr(msg, msg_len, buf))
            return now_ts + (int64_t)d * 86400;
        n = snprintf(buf, sizeof(buf), "in %u days", d);
        if (n > 0 && (size_t)n < sizeof(buf) && hu_str_contains_ci_cstr(msg, msg_len, buf))
            return now_ts + (int64_t)d * 86400;
    }
    /* "in X hours" */
    for (unsigned int h = 1; h <= 168; h++) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "in %u hour", h);
        if (n > 0 && (size_t)n < sizeof(buf) && hu_str_contains_ci_cstr(msg, msg_len, buf))
            return now_ts + (int64_t)h * 3600;
        n = snprintf(buf, sizeof(buf), "in %u hours", h);
        if (n > 0 && (size_t)n < sizeof(buf) && hu_str_contains_ci_cstr(msg, msg_len, buf))
            return now_ts + (int64_t)h * 3600;
    }
    return 0;
}

/* Detect if message contains a commitment. Fills description_out with commitment text,
 * who_out with "me" or "them" based on from_me. Returns true if commitment detected. */
bool hu_conversation_detect_commitment(const char *msg, size_t msg_len, char *description_out,
                                       size_t desc_cap, char *who_out, size_t who_cap,
                                       bool from_me) {
    if (!msg || msg_len == 0 || !description_out || desc_cap == 0 || !who_out || who_cap == 0)
        return false;
    description_out[0] = '\0';
    who_out[0] = '\0';

    static const char *KEYWORDS[] = {"i'll",      "i will",     "i'm going to", "gonna",
                                     "promise",   "let me",     "i'll call",    "i'll text",
                                     "i'll send", "i'll check", "we should",    NULL};
    bool found = false;
    size_t best_start = msg_len;
    size_t best_len = 0;
    for (const char **kw = KEYWORDS; *kw; kw++) {
        size_t nlen = strlen(*kw);
        if (nlen > msg_len)
            continue;
        for (size_t i = 0; i + nlen <= msg_len; i++) {
            bool wb_before = (i == 0) || isspace((unsigned char)msg[i - 1]);
            if (!wb_before)
                continue;
            if (strncasecmp(msg + i, *kw, nlen) != 0)
                continue;
            found = true;
            if (i < best_start) {
                best_start = i;
                size_t end = i + nlen;
                while (end < msg_len && msg[end] != '.' && msg[end] != '!' && msg[end] != '?' &&
                       msg[end] != '\n')
                    end++;
                best_len = end - best_start;
            }
        }
    }
    if (!found)
        return false;

    size_t copy_len = best_len < desc_cap - 1 ? best_len : desc_cap - 1;
    memcpy(description_out, msg + best_start, copy_len);
    description_out[copy_len] = '\0';
    /* Trim trailing space */
    while (copy_len > 0 && description_out[copy_len - 1] == ' ')
        description_out[--copy_len] = '\0';

    const char *who = from_me ? "me" : "them";
    size_t who_len = strlen(who);
    if (who_len < who_cap) {
        memcpy(who_out, who, who_len + 1);
    } else if (who_cap > 0) {
        memcpy(who_out, who, who_cap - 1);
        who_out[who_cap - 1] = '\0';
    }
    return true;
}

/* ── Honesty Guardrail (pattern-based, contextual output) ───────────────── */

/* Detect if user is asking about a commitment or action: question + action-query pattern.
 * Lightweight: question mark + "did you" or "have you" (covers did you call, have you sent, etc.)
 */
static bool detect_action_commitment_query(const char *message, size_t message_len) {
    bool has_question = false;
    for (size_t i = 0; i < message_len; i++) {
        if (message[i] == '?') {
            has_question = true;
            break;
        }
    }
    if (!has_question)
        return false;
    return hu_str_contains_ci_cstr(message, message_len, "did you") ||
           hu_str_contains_ci_cstr(message, message_len, "have you");
}

char *hu_conversation_honesty_check(hu_allocator_t *alloc, const char *message,
                                    size_t message_len) {
    if (!alloc || !message || message_len == 0)
        return NULL;

    if (!detect_action_commitment_query(message, message_len))
        return NULL;

    char *buf = (char *)alloc->alloc(alloc->ctx, 512);
    if (!buf)
        return NULL;

    int n = snprintf(buf, 512,
                     "HONESTY GUARDRAIL: The user appears to be asking about something you "
                     "committed to or were expected to do. Be honest about what actually happened. "
                     "Don't deflect, don't be vague. If you don't know or haven't done it, say so "
                     "directly. Never fabricate completed actions.");
    if (n <= 0 || (size_t)n >= 512) {
        alloc->free(alloc->ctx, buf, 512);
        return NULL;
    }
    size_t actual_len = (size_t)n;
    if (actual_len + 1 < 512 && alloc->realloc) {
        char *shrunk = (char *)alloc->realloc(alloc->ctx, buf, 512, actual_len + 1);
        if (shrunk)
            buf = shrunk;
    }
    return buf;
}

/* ── Narrative Arc Detection ────────────────────────────────────────── */

hu_narrative_phase_t hu_conversation_detect_narrative(const hu_channel_history_entry_t *entries,
                                                      size_t count) {
    if (!entries || count == 0)
        return HU_NARRATIVE_OPENING;

    /* Count exchanges (direction changes) */
    size_t exchanges = 0;
    bool last_from_me = false;
    for (size_t i = 0; i < count; i++) {
        if (i == 0 || entries[i].from_me != last_from_me)
            exchanges++;
        last_from_me = entries[i].from_me;
    }

    /* Measure emotional intensity in recent messages */
    int emotional_words = 0;
    int question_marks __attribute__((unused)) = 0;
    int exclamation_marks = 0;
    size_t recent_start = count > 5 ? count - 5 : 0;
    for (size_t i = recent_start; i < count; i++) {
        if (entries[i].from_me)
            continue;
        const char *t = entries[i].text;
        size_t tl = strlen(t);
        for (size_t j = 0; j < tl; j++) {
            if (t[j] == '?')
                question_marks++;
            if (t[j] == '!')
                exclamation_marks++;
        }
        if (hu_str_contains_ci_cstr(t, tl, "love") || hu_str_contains_ci_cstr(t, tl, "hate") ||
            hu_str_contains_ci_cstr(t, tl, "scared") || hu_str_contains_ci_cstr(t, tl, "angry") ||
            hu_str_contains_ci_cstr(t, tl, "can't believe") ||
            hu_str_contains_ci_cstr(t, tl, "so happy") ||
            hu_str_contains_ci_cstr(t, tl, "so sad") || hu_str_contains_ci_cstr(t, tl, "hurt") ||
            hu_str_contains_ci_cstr(t, tl, "need to tell") ||
            hu_str_contains_ci_cstr(t, tl, "important"))
            emotional_words++;
    }

    /* Detect closing signals */
    if (count >= 2) {
        const char *last = entries[count - 1].text;
        size_t ll = strlen(last);
        if (!entries[count - 1].from_me && (hu_str_contains_ci_cstr(last, ll, "gotta go") ||
                                            hu_str_contains_ci_cstr(last, ll, "talk later") ||
                                            hu_str_contains_ci_cstr(last, ll, "bye") ||
                                            hu_str_contains_ci_cstr(last, ll, "night") ||
                                            hu_str_contains_ci_cstr(last, ll, "ttyl") ||
                                            hu_str_contains_ci_cstr(last, ll, "heading out") ||
                                            (ll < 10 && (hu_str_contains_ci_cstr(last, ll, "ok") ||
                                                         hu_str_contains_ci_cstr(last, ll, "k")))))
            return HU_NARRATIVE_CLOSING;
    }

    /* Map to phases */
    if (exchanges <= 2)
        return HU_NARRATIVE_OPENING;
    if (exchanges <= 5)
        return HU_NARRATIVE_BUILDING;
    if (emotional_words >= 2 || exclamation_marks >= 3)
        return HU_NARRATIVE_PEAK;
    if (exchanges > 5 && emotional_words >= 1)
        return HU_NARRATIVE_APPROACHING_CLIMAX;
    if (exchanges > 10)
        return HU_NARRATIVE_RELEASE;
    return HU_NARRATIVE_BUILDING;
}

/* ── Engagement Scoring ─────────────────────────────────────────────── */

hu_engagement_level_t hu_conversation_detect_engagement(const hu_channel_history_entry_t *entries,
                                                        size_t count) {
    if (!entries || count == 0)
        return HU_ENGAGEMENT_MODERATE;

    size_t their_msgs = 0;
    size_t total_their_len = 0;
    size_t very_short = 0;
    size_t questions_to_us = 0;
    size_t recent = count > 6 ? count - 6 : 0;

    for (size_t i = recent; i < count; i++) {
        if (entries[i].from_me)
            continue;
        their_msgs++;
        size_t tl = strlen(entries[i].text);
        total_their_len += tl;
        if (tl < 8)
            very_short++;
        for (size_t j = 0; j < tl; j++) {
            if (entries[i].text[j] == '?') {
                questions_to_us++;
                break;
            }
        }
    }

    if (their_msgs == 0)
        return HU_ENGAGEMENT_DISTRACTED;

    size_t avg_len = total_their_len / their_msgs;

    /* High: asking questions, longer messages, engaged */
    if (questions_to_us >= 2 || avg_len > 60)
        return HU_ENGAGEMENT_HIGH;

    /* Low: very short responses, no questions */
    if (avg_len < 15 && very_short >= 2 && questions_to_us == 0)
        return HU_ENGAGEMENT_LOW;

    /* Distracted: single-word or empty responses */
    if (their_msgs >= 2 && very_short == their_msgs)
        return HU_ENGAGEMENT_DISTRACTED;

    return HU_ENGAGEMENT_MODERATE;
}

/* ── Emotional State Detection ──────────────────────────────────────── */

/* Weighted emotion lexicon: each entry carries per-class scores derived from
 * corpus analysis (NRC VAD Lexicon + GoEmotions aggregate weights).
 * Columns: word, joy, sadness, anger, fear, surprise, disgust, positive_valence */
typedef struct {
    const char *word;
    float joy;
    float sadness;
    float anger;
    float fear;
    float surprise;
    float disgust;
    float valence; /* positive = positive, negative = negative valence */
} emotion_lexicon_entry_t;

static const emotion_lexicon_entry_t EMOTION_LEXICON[] = {
    /* High-confidence positive */
    {"happy", 1.8f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.5f},
    {"excited", 1.5f, 0.0f, 0.0f, 0.0f, 0.6f, 0.0f, 1.4f},
    {"love", 1.4f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.3f},
    {"amazing", 1.3f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 1.2f},
    {"great", 0.9f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.8f},
    {"awesome", 1.1f, 0.0f, 0.0f, 0.0f, 0.3f, 0.0f, 1.0f},
    {"wonderful", 1.3f, 0.0f, 0.0f, 0.0f, 0.3f, 0.0f, 1.2f},
    {"fantastic", 1.2f, 0.0f, 0.0f, 0.0f, 0.4f, 0.0f, 1.1f},
    {"beautiful", 1.0f, 0.0f, 0.0f, 0.0f, 0.2f, 0.0f, 1.0f},
    {"grateful", 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.9f},
    {"blessed", 0.9f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.8f},
    {"thrilled", 1.4f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 1.3f},
    {"proud", 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.9f},
    {"yay", 1.0f, 0.0f, 0.0f, 0.0f, 0.3f, 0.0f, 0.9f},

    /* Casual positive / amusement */
    {"lol", 0.5f, 0.0f, 0.0f, 0.0f, 0.2f, 0.0f, 0.4f},
    {"haha", 0.6f, 0.0f, 0.0f, 0.0f, 0.2f, 0.0f, 0.5f},
    {"lmao", 0.7f, 0.0f, 0.0f, 0.0f, 0.3f, 0.0f, 0.5f},
    {"funny", 0.6f, 0.0f, 0.0f, 0.0f, 0.3f, 0.0f, 0.5f},
    {"hilarious", 0.8f, 0.0f, 0.0f, 0.0f, 0.4f, 0.0f, 0.6f},

    /* High-confidence negative: sadness */
    {"sad", 0.0f, 1.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.2f},
    {"depressed", 0.0f, 2.0f, 0.0f, 0.3f, 0.0f, 0.0f, -1.8f},
    {"lonely", 0.0f, 1.6f, 0.0f, 0.2f, 0.0f, 0.0f, -1.4f},
    {"crying", 0.0f, 1.8f, 0.0f, 0.2f, 0.0f, 0.0f, -1.6f},
    {"cry", 0.0f, 1.5f, 0.0f, 0.2f, 0.0f, 0.0f, -1.3f},
    {"heartbroken", 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.8f},
    {"miserable", 0.0f, 1.7f, 0.0f, 0.0f, 0.0f, 0.2f, -1.5f},
    {"devastated", 0.0f, 1.9f, 0.0f, 0.2f, 0.3f, 0.0f, -1.7f},
    {"broken", 0.0f, 1.4f, 0.0f, 0.3f, 0.0f, 0.0f, -1.2f},
    {"miss you", 0.0f, 1.2f, 0.0f, 0.0f, 0.0f, 0.0f, -0.8f},
    {"grief", 0.0f, 1.8f, 0.0f, 0.0f, 0.0f, 0.0f, -1.6f},

    /* Anger */
    {"angry", 0.0f, 0.0f, 1.8f, 0.0f, 0.0f, 0.2f, -1.4f},
    {"furious", 0.0f, 0.0f, 2.0f, 0.0f, 0.2f, 0.2f, -1.6f},
    {"frustrated", 0.0f, 0.2f, 1.3f, 0.0f, 0.0f, 0.2f, -1.0f},
    {"hate", 0.0f, 0.0f, 1.5f, 0.0f, 0.0f, 0.5f, -1.3f},
    {"pissed", 0.0f, 0.0f, 1.6f, 0.0f, 0.0f, 0.3f, -1.2f},
    {"annoyed", 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.3f, -0.8f},
    {"livid", 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.2f, -1.7f},

    /* Fear / anxiety */
    {"scared", 0.0f, 0.2f, 0.0f, 1.6f, 0.2f, 0.0f, -1.2f},
    {"worried", 0.0f, 0.3f, 0.0f, 1.2f, 0.0f, 0.0f, -0.9f},
    {"anxious", 0.0f, 0.3f, 0.0f, 1.4f, 0.0f, 0.0f, -1.1f},
    {"stressed", 0.0f, 0.3f, 0.2f, 1.0f, 0.0f, 0.0f, -0.9f},
    {"nervous", 0.0f, 0.1f, 0.0f, 1.3f, 0.2f, 0.0f, -0.8f},
    {"terrified", 0.0f, 0.2f, 0.0f, 2.0f, 0.3f, 0.0f, -1.6f},
    {"panicking", 0.0f, 0.1f, 0.0f, 1.8f, 0.3f, 0.0f, -1.4f},
    {"freaking out", 0.0f, 0.1f, 0.0f, 1.5f, 0.4f, 0.0f, -1.2f},
    {"overwhelmed", 0.0f, 0.5f, 0.0f, 1.2f, 0.2f, 0.0f, -1.0f},

    /* Disgust */
    {"disgusting", 0.0f, 0.0f, 0.3f, 0.0f, 0.2f, 1.8f, -1.3f},
    {"gross", 0.0f, 0.0f, 0.1f, 0.0f, 0.2f, 1.3f, -0.8f},
    {"sick of", 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 1.0f, -0.7f},

    /* Mild negative signals */
    {"hurt", 0.0f, 1.0f, 0.3f, 0.0f, 0.0f, 0.0f, -1.0f},
    {"ugh", 0.0f, 0.2f, 0.3f, 0.0f, 0.0f, 0.3f, -0.5f},
    {"damn", 0.0f, 0.1f, 0.4f, 0.0f, 0.2f, 0.0f, -0.4f},
    {"sucks", 0.0f, 0.2f, 0.4f, 0.0f, 0.0f, 0.3f, -0.6f},
    {"terrible", 0.0f, 0.5f, 0.3f, 0.2f, 0.0f, 0.3f, -1.0f},
    {"awful", 0.0f, 0.5f, 0.3f, 0.1f, 0.0f, 0.3f, -1.0f},
    {"horrible", 0.0f, 0.5f, 0.3f, 0.2f, 0.0f, 0.4f, -1.1f},
    {"exhausted", 0.0f, 0.6f, 0.0f, 0.0f, 0.0f, 0.0f, -0.7f},
    {"tired", 0.0f, 0.4f, 0.0f, 0.0f, 0.0f, 0.0f, -0.4f},
    {"drained", 0.0f, 0.6f, 0.0f, 0.0f, 0.0f, 0.0f, -0.6f},
};

static const size_t EMOTION_LEXICON_SIZE = sizeof(EMOTION_LEXICON) / sizeof(EMOTION_LEXICON[0]);

/* Negation words that flip polarity */
static bool is_negation_prefix(const char *text, size_t text_len, const char *word_pos) {
    if (word_pos <= text || word_pos >= text + text_len)
        return false;
    const char *p = word_pos - 1;
    while (p >= text && *p == ' ')
        p--;
    if (p < text)
        return false;
    /* Check common negations ending at p */
    const char *negs[] = {"not",    "no",    "don't", "dont", "never", "isn't", "isnt",
                          "wasn't", "wasnt", "can't", "cant", "won't", "wont"};
    for (size_t i = 0; i < sizeof(negs) / sizeof(negs[0]); i++) {
        size_t nlen = strlen(negs[i]);
        if ((size_t)(p - text + 1) >= nlen) {
            const char *start = p - nlen + 1;
            bool match = true;
            for (size_t j = 0; j < nlen; j++) {
                char a = start[j];
                char b = negs[i][j];
                if (a >= 'A' && a <= 'Z')
                    a += 32;
                if (a != b) {
                    match = false;
                    break;
                }
            }
            if (match && (start == text || start[-1] == ' '))
                return true;
        }
    }
    return false;
}

/* Intensifier multiplier: "very", "really", "so", "extremely" */
static float intensifier_multiplier(const char *text, size_t text_len, const char *word_pos) {
    if (word_pos <= text || word_pos >= text + text_len)
        return 1.0f;
    const char *p = word_pos - 1;
    while (p >= text && *p == ' ')
        p--;
    if (p < text)
        return 1.0f;
    const char *ints[] = {"very", "really", "so", "extremely", "super"};
    const float mults[] = {1.4f, 1.4f, 1.3f, 1.6f, 1.3f};
    for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
        size_t ilen = strlen(ints[i]);
        if ((size_t)(p - text + 1) >= ilen) {
            const char *start = p - ilen + 1;
            bool match = true;
            for (size_t j = 0; j < ilen; j++) {
                char a = start[j];
                char b = ints[i][j];
                if (a >= 'A' && a <= 'Z')
                    a += 32;
                if (a != b) {
                    match = false;
                    break;
                }
            }
            if (match && (start == text || start[-1] == ' '))
                return mults[i];
        }
    }
    return 1.0f;
}

/* Case-insensitive strstr that returns pointer to match */
static const char *ci_strstr(const char *haystack, size_t haystack_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > haystack_len)
        return NULL;
    for (size_t i = 0; i <= haystack_len - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (b >= 'A' && b <= 'Z')
                b += 32;
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match)
            return haystack + i;
    }
    return NULL;
}

hu_emotional_state_t hu_conversation_detect_emotion(const hu_channel_history_entry_t *entries,
                                                    size_t count) {
    hu_emotional_state_t state = {0.0f, 0.0f, false, "neutral"};
    if (!entries || count == 0)
        return state;

    float scores[6] = {0}; /* joy, sadness, anger, fear, surprise, disgust */
    float total_valence = 0;
    float total_intensity = 0;
    int samples = 0;
    size_t start = count > 8 ? count - 8 : 0;

    for (size_t i = start; i < count; i++) {
        if (entries[i].from_me)
            continue;
        const char *t = entries[i].text;
        size_t tl = strlen(t);
        samples++;

        /* Scan for each lexicon entry */
        for (size_t li = 0; li < EMOTION_LEXICON_SIZE; li++) {
            const emotion_lexicon_entry_t *e = &EMOTION_LEXICON[li];
            const char *found = ci_strstr(t, tl, e->word);
            if (!found)
                continue;

            float mult = intensifier_multiplier(t, tl, found);
            bool negated = is_negation_prefix(t, tl, found);

            if (negated) {
                /* Flip valence and reduce confidence */
                total_valence -= e->valence * 0.6f * mult;
                scores[0] += e->sadness * 0.3f;
                scores[1] += e->joy * 0.3f;
            } else {
                total_valence += e->valence * mult;
                scores[0] += e->joy * mult;
                scores[1] += e->sadness * mult;
                scores[2] += e->anger * mult;
                scores[3] += e->fear * mult;
                scores[4] += e->surprise * mult;
                scores[5] += e->disgust * mult;
            }
            total_intensity +=
                (e->joy + e->sadness + e->anger + e->fear + e->surprise + e->disgust) * mult;
        }
    }

    if (samples == 0)
        return state;

    state.valence = total_valence / (float)samples;
    state.intensity = total_intensity / (float)samples;
    if (state.valence < -1.0f)
        state.valence = -1.0f;
    if (state.valence > 1.0f)
        state.valence = 1.0f;
    if (state.intensity > 2.0f)
        state.intensity = 2.0f;

    /* Determine dominant emotion from highest-scoring class */
    float max_score = 0;
    int max_class = -1;
    for (int c = 0; c < 6; c++) {
        if (scores[c] > max_score) {
            max_score = scores[c];
            max_class = c;
        }
    }

    float neg_total = scores[1] + scores[2] + scores[3] + scores[5];
    float pos_total = scores[0];

    if (neg_total > pos_total * 1.5f) {
        state.concerning = neg_total >= 3.0f;
        switch (max_class) {
        case 1: /* sadness */
            state.dominant_emotion = max_score >= 3.0f   ? "distressed"
                                     : max_score >= 1.5f ? "upset"
                                                         : "down";
            break;
        case 2: /* anger */
            state.dominant_emotion = max_score >= 2.0f ? "furious" : "frustrated";
            break;
        case 3: /* fear */
            state.dominant_emotion = max_score >= 2.0f ? "terrified" : "anxious";
            break;
        case 5: /* disgust */
            state.dominant_emotion = "repulsed";
            break;
        default:
            state.dominant_emotion = "upset";
            break;
        }
    } else if (pos_total > neg_total * 1.5f) {
        if (scores[4] > pos_total * 0.3f)
            state.dominant_emotion = "amazed";
        else if (pos_total >= 2.0f)
            state.dominant_emotion = "excited";
        else
            state.dominant_emotion = "positive";
    } else if (state.intensity > 0.3f) {
        state.dominant_emotion = "mixed";
    }

    return state;
}

#if !defined(HU_IS_TEST) || !HU_IS_TEST
/* Parse outermost JSON object span (handles ```json fences or leading prose). */
static bool conversation_json_object_span(const char *s, size_t n, size_t *start_out,
                                          size_t *len_out) {
    size_t start = SIZE_MAX;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '{') {
            start = i;
            break;
        }
    }
    if (start == SIZE_MAX)
        return false;
    int depth = 0;
    size_t end = 0;
    for (size_t i = start; i < n; i++) {
        if (s[i] == '{')
            depth++;
        else if (s[i] == '}') {
            depth--;
            if (depth == 0) {
                end = i + 1;
                break;
            }
        }
    }
    if (end <= start)
        return false;
    *start_out = start;
    *len_out = end - start;
    return true;
}

static void conversation_map_llm_emotion_label(const char *emotion, hu_emotional_state_t *base) {
    if (!emotion || !emotion[0])
        return;
    if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "excited") == 0 ||
        strcmp(emotion, "grateful") == 0)
        base->dominant_emotion = "positive";
    else if (strcmp(emotion, "sad") == 0 || strcmp(emotion, "hurt") == 0)
        base->dominant_emotion = "sad";
    else if (strcmp(emotion, "angry") == 0 || strcmp(emotion, "frustrated") == 0)
        base->dominant_emotion = "angry";
    else if (strcmp(emotion, "anxious") == 0)
        base->dominant_emotion = "anxious";
    else if (strcmp(emotion, "loving") == 0)
        base->dominant_emotion = "affectionate";
    else
        base->dominant_emotion = "neutral";
}
#endif

hu_emotional_state_t hu_conversation_detect_emotion_llm(hu_allocator_t *alloc,
                                                        hu_provider_t *provider, const char *model,
                                                        size_t model_len,
                                                        const hu_channel_history_entry_t *entries,
                                                        size_t count) {
    hu_emotional_state_t base = hu_conversation_detect_emotion(entries, count);

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)alloc;
    (void)provider;
    (void)model;
    (void)model_len;
    return base;
#else
    if (!alloc || !provider || !provider->vtable || !provider->vtable->chat_with_system)
        return base;

    char context[2048];
    int cpos = 0;
    size_t hist_start = count > 5 ? count - 5 : 0;
    for (size_t i = hist_start; i < count && cpos < 1800; i++) {
        const char *t = entries[i].text;
        if (!t || t[0] == '\0')
            continue;
        size_t tl = strlen(t);
        if (tl > 200)
            tl = 200;
        const char *role = entries[i].from_me ? "Me" : "Them";
        int added = snprintf(context + (size_t)cpos, sizeof(context) - (size_t)cpos, "%s: %.*s\n",
                             role, (int)tl, t);
        if (added < 0)
            break;
        if ((size_t)added >= sizeof(context) - (size_t)cpos) {
            cpos = (int)(sizeof(context) - 1);
            break;
        }
        cpos += added;
    }
    if (cpos == 0)
        return base;

    static const char sys[] =
        "Analyze the emotional state of the last message in this conversation. "
        "Return ONLY valid JSON: "
        "{\"valence\": -1.0 to 1.0, \"intensity\": 0.0 to 1.0, "
        "\"emotion\": \"happy|sad|angry|anxious|excited|neutral|grateful|frustrated|loving|hurt\", "
        "\"concerning\": true/false, "
        "\"confidence\": 0.0 to 1.0}";

    char *llm_out = NULL;
    size_t llm_out_len = 0;
    const char *use_model = model ? model : "";
    hu_error_t err = provider->vtable->chat_with_system(provider->ctx, alloc, sys, sizeof(sys) - 1,
                                                        context, (size_t)cpos, use_model, model_len,
                                                        0.0, &llm_out, &llm_out_len);

    if (err != HU_OK || !llm_out || llm_out_len == 0) {
        if (llm_out)
            alloc->free(alloc->ctx, llm_out, llm_out_len + 1);
        return base;
    }

    size_t jstart = 0;
    size_t jlen = llm_out_len;
    if (!conversation_json_object_span(llm_out, llm_out_len, &jstart, &jlen)) {
        jstart = 0;
        jlen = llm_out_len;
    }

    hu_json_value_t *json = NULL;
    hu_error_t jerr = hu_json_parse(alloc, llm_out + jstart, jlen, &json);
    alloc->free(alloc->ctx, llm_out, llm_out_len + 1);
    llm_out = NULL;

    if (jerr == HU_OK && json) {
        double valence = hu_json_get_number(json, "valence", (double)base.valence);
        double intensity = hu_json_get_number(json, "intensity", (double)base.intensity);
        const char *emotion = hu_json_get_string(json, "emotion");
        double confidence = hu_json_get_number(json, "confidence", 0.0);

        if (confidence > 0.5) {
            if (valence < -1.0)
                valence = -1.0;
            if (valence > 1.0)
                valence = 1.0;
            if (intensity < 0.0)
                intensity = 0.0;
            if (intensity > 2.0)
                intensity = 2.0;
            base.valence = (float)valence;
            base.intensity = (float)intensity;
            if (emotion && emotion[0])
                conversation_map_llm_emotion_label(emotion, &base);
            base.concerning = hu_json_get_bool(json, "concerning", base.concerning);
        }
        hu_json_free(alloc, json);
    }

    return base;
#endif
}

/* ── Energy level detection (F13) ─────────────────────────────────────── */

static size_t count_exclamations(const char *msg, size_t msg_len) {
    size_t n = 0;
    for (size_t i = 0; i < msg_len; i++)
        if (msg[i] == '!')
            n++;
    return n;
}

hu_energy_level_t hu_conversation_detect_energy(const char *msg, size_t msg_len,
                                                const hu_channel_history_entry_t *entries,
                                                size_t count) {
    (void)entries;
    (void)count;
    if (!msg || msg_len == 0)
        return HU_ENERGY_NEUTRAL;

    /* Common neutral acknowledgments — return NEUTRAL before other heuristics */
    if (hu_str_contains_ci_cstr(msg, msg_len, "ok sounds good") ||
        hu_str_contains_ci_cstr(msg, msg_len, "sounds good") ||
        (msg_len <= 4 && (hu_str_contains_ci_cstr(msg, msg_len, "ok") ||
                          hu_str_contains_ci_cstr(msg, msg_len, "k"))))
        return HU_ENERGY_NEUTRAL;

    /* Excited: positive valence, exclamation marks (3+), "omg", "amazing", "love", "so happy" */
    if (hu_str_contains_ci_cstr(msg, msg_len, "omg") ||
        hu_str_contains_ci_cstr(msg, msg_len, "amazing") ||
        hu_str_contains_ci_cstr(msg, msg_len, "love") ||
        hu_str_contains_ci_cstr(msg, msg_len, "so happy") || count_exclamations(msg, msg_len) >= 3)
        return HU_ENERGY_EXCITED;

    /* Sad: "sad", "depressed", "hurt", "lonely", "crying", "miss you", "broken" */
    if (hu_str_contains_ci_cstr(msg, msg_len, "sad") ||
        hu_str_contains_ci_cstr(msg, msg_len, "depressed") ||
        hu_str_contains_ci_cstr(msg, msg_len, "hurt") ||
        hu_str_contains_ci_cstr(msg, msg_len, "lonely") ||
        hu_str_contains_ci_cstr(msg, msg_len, "crying") ||
        hu_str_contains_ci_cstr(msg, msg_len, "miss you") ||
        hu_str_contains_ci_cstr(msg, msg_len, "broken"))
        return HU_ENERGY_SAD;

    /* Playful: "lol", "haha", "lmao", teasing, "you're ridiculous", "dead 💀" */
    if (hu_str_contains_ci_cstr(msg, msg_len, "lol") ||
        hu_str_contains_ci_cstr(msg, msg_len, "haha") ||
        hu_str_contains_ci_cstr(msg, msg_len, "lmao") ||
        hu_str_contains_ci_cstr(msg, msg_len, "you're ridiculous") ||
        hu_str_contains_ci_cstr(msg, msg_len, "youre ridiculous") ||
        hu_str_contains_ci_cstr(msg, msg_len, "dead"))
        return HU_ENERGY_PLAYFUL;

    /* Anxious: "worried", "stressed", "anxious", "scared", "nervous", "freaking out" */
    if (hu_str_contains_ci_cstr(msg, msg_len, "worried") ||
        hu_str_contains_ci_cstr(msg, msg_len, "stressed") ||
        hu_str_contains_ci_cstr(msg, msg_len, "anxious") ||
        hu_str_contains_ci_cstr(msg, msg_len, "scared") ||
        hu_str_contains_ci_cstr(msg, msg_len, "nervous") ||
        hu_str_contains_ci_cstr(msg, msg_len, "freaking out"))
        return HU_ENERGY_ANXIOUS;

    /* Calm: low intensity, neutral, short messages with no strong keywords */
    if (msg_len < 30 && !hu_str_contains_ci_cstr(msg, msg_len, "!") &&
        !hu_str_contains_ci_cstr(msg, msg_len, "?") &&
        !hu_str_contains_ci_cstr(msg, msg_len, "omg") &&
        !hu_str_contains_ci_cstr(msg, msg_len, "love") &&
        !hu_str_contains_ci_cstr(msg, msg_len, "hate"))
        return HU_ENERGY_CALM;

    return HU_ENERGY_NEUTRAL;
}

size_t hu_conversation_build_energy_directive(hu_energy_level_t energy, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    if (energy == HU_ENERGY_NEUTRAL)
        return 0;

    const char *directive = NULL;
    switch (energy) {
    case HU_ENERGY_EXCITED:
        directive = "[ENERGY: They're excited. Match their energy. Be enthusiastic.]";
        break;
    case HU_ENERGY_SAD:
        directive = "[ENERGY: They're down. Be gentle, shorter, empathetic. No jokes.]";
        break;
    case HU_ENERGY_PLAYFUL:
        directive = "[ENERGY: They're playful. Match the playfulness.]";
        break;
    case HU_ENERGY_ANXIOUS:
        directive = "[ENERGY: They're anxious. Be calm, reassuring, grounding.]";
        break;
    case HU_ENERGY_CALM:
        directive = "[ENERGY: They're calm. Keep it low-key and measured.]";
        break;
    default:
        return 0;
    }
    size_t len = strlen(directive);
    if (len >= cap)
        len = cap - 1;
    memcpy(buf, directive, len);
    buf[len] = '\0';
    return len;
}

/* ── Inside joke detection (F19) ───────────────────────────────────────── */

bool hu_conversation_detect_inside_joke(const char *msg, size_t msg_len,
                                        const hu_channel_history_entry_t *entries, size_t count) {
    if (!msg || msg_len == 0)
        return false;

    /* Keyword heuristics: "remember when", "that time we", "you always say" */
    if (hu_str_contains_ci_cstr(msg, msg_len, "remember when") ||
        hu_str_contains_ci_cstr(msg, msg_len, "that time we") ||
        hu_str_contains_ci_cstr(msg, msg_len, "you always say"))
        return true;

    /* "[X] energy" pattern — e.g. "that's so [name] energy" */
    if (hu_str_contains_ci_cstr(msg, msg_len, " energy"))
        return true;

    /* Shared callback phrases */
    if (hu_str_contains_ci_cstr(msg, msg_len, "lol that's our thing") ||
        hu_str_contains_ci_cstr(msg, msg_len, "that's our thing") ||
        hu_str_contains_ci_cstr(msg, msg_len, "classic "))
        return true;

    /* Shared phrase: if msg contains a significant phrase from history (12+ chars) */
    if (entries && count > 0) {
        char phrase_buf[33];
        for (size_t i = 0; i < count && i < 6; i++) {
            const char *t = entries[i].text;
            size_t tl = strlen(t);
            if (tl < 12)
                continue;
            /* Try last 12-32 chars of entry (often the punchline) */
            size_t start = (tl > 32) ? tl - 32 : 0;
            size_t len = tl - start;
            if (len < 12)
                continue;
            if (len > 32)
                len = 32;
            memcpy(phrase_buf, t + start, len);
            phrase_buf[len] = '\0';
            if (hu_str_contains_ci_cstr(msg, msg_len, phrase_buf))
                return true;
        }
    }

    return false;
}

/* ── Avoidance pattern detection (F21) ─────────────────────────────────── */

/* P2-7 (2026-05-16 incident): expand the stopword list. The old list let
 * "hey" through as a topic ("hey how are you doing" → topic "hey"), which
 * F30's "How is the %s going?" template then rendered as "How is the hey
 * going?" and shipped to family contacts. Also include first-person
 * emotion KEYWORDS (feel/feeling/lost/lonely/sad/ok) because they are
 * surfaced via the emotion channel of the same record — using them as
 * "topic" is a redundant leakage path. */
static const char *const topic_stopwords[] = {
    "i",
    "the",
    "a",
    "is",
    "was",
    "that",
    "this",
    "it",
    "to",
    "and",
    "but",
    "so",
    "just",
    "really",
    "what",
    "how",
    "why",
    "when",
    "where",
    "who",
    "can",
    "will",
    "would",
    "could",
    "should",
    "have",
    "has",
    "had",
    "do",
    "does",
    "did",
    "am",
    "are",
    "were",
    "be",
    "been",
    "being",
    "of",
    "in",
    "on",
    "at",
    "for",
    "with",
    "about",
    "from",
    "as",
    "or",
    "if",
    "not",
    "no",
    "yes",
    "oh",
    "um",
    "like",
    /* P2-7 additions — greetings, fillers, and emotion keywords that must
     * never reach the topic slot. */
    "hey",
    "doing",
    "you",
    "me",
    "my",
    "feel",
    "feeling",
    "lost",
    "lonely",
    "sad",
    "ok",
    "omg",
    "hi",
    "hello",
    "please",
    "thanks",
    "sorry",
    NULL,
};

static bool is_stopword(const char *word, size_t len) {
    for (const char *const *sw = topic_stopwords; *sw; sw++) {
        size_t swlen = strlen(*sw);
        if (len == swlen && strncasecmp(word, *sw, len) == 0)
            return true;
    }
    return false;
}

/* Extract first 2-3 significant words (skip stopwords) into out. Returns length. */
static size_t extract_significant_topic(const char *text, size_t text_len, char *out, size_t cap) {
    if (!text || text_len == 0 || !out || cap == 0)
        return 0;
    size_t pos = 0;
    int word_count = 0;
    const char *p = text;
    const char *end = text + text_len;
    while (p < end && word_count < 3) {
        while (p < end && !isalnum((unsigned char)*p))
            p++;
        if (p >= end)
            break;
        const char *start = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '\'' || *p == '-'))
            p++;
        size_t wlen = (size_t)(p - start);
        /* P2-7 (2026-05-16): raise floor from 2 to 4 chars. Words like
         * "ok", "hi", "yo" are too short to be meaningful topics; they
         * were leaking into proactive prompts. */
        if (wlen >= 4 && !is_stopword(start, wlen)) {
            if (pos > 0 && pos + 1 < cap) {
                out[pos++] = ' ';
            }
            size_t copy = wlen < cap - pos ? wlen : cap - pos - 1;
            if (copy > 0) {
                for (size_t i = 0; i < copy; i++)
                    out[pos + i] = (char)tolower((unsigned char)start[i]);
                pos += copy;
                word_count++;
            }
        }
    }
    if (pos < cap)
        out[pos] = '\0';
    else if (cap > 0)
        out[cap - 1] = '\0';
    return pos;
}

/* ── Emotional tone classification (F22) ───────────────────────────────── */

static size_t count_exclamations_tone(const char *msg, size_t msg_len) {
    size_t n = 0;
    for (size_t i = 0; i < msg_len; i++)
        if (msg[i] == '!')
            n++;
    return n;
}

const char *hu_conversation_classify_emotional_tone(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return "neutral";

    /* Neutral: short acknowledgments */
    if (hu_str_contains_ci_cstr(msg, msg_len, "ok sounds good") ||
        hu_str_contains_ci_cstr(msg, msg_len, "sounds good") ||
        (msg_len <= 4 && (hu_str_contains_ci_cstr(msg, msg_len, "ok") ||
                          hu_str_contains_ci_cstr(msg, msg_len, "k"))))
        return "neutral";

    /* Stressed: overwhelmed, burnt out */
    if (hu_str_contains_ci_cstr(msg, msg_len, "stressed") ||
        hu_str_contains_ci_cstr(msg, msg_len, "overwhelmed") ||
        hu_str_contains_ci_cstr(msg, msg_len, "burnt out") ||
        hu_str_contains_ci_cstr(msg, msg_len, "burned out"))
        return "stressed";

    /* Excited: omg, amazing, love, so happy, yay, 3+ exclamations */
    if (hu_str_contains_ci_cstr(msg, msg_len, "omg") ||
        hu_str_contains_ci_cstr(msg, msg_len, "amazing") ||
        hu_str_contains_ci_cstr(msg, msg_len, "love") ||
        hu_str_contains_ci_cstr(msg, msg_len, "so happy") ||
        hu_str_contains_ci_cstr(msg, msg_len, "yay") ||
        hu_str_contains_ci_cstr(msg, msg_len, "excited") ||
        count_exclamations_tone(msg, msg_len) >= 3)
        return "excited";

    /* Sad: depressed, hurt, lonely, crying, broken */
    if (hu_str_contains_ci_cstr(msg, msg_len, "sad") ||
        hu_str_contains_ci_cstr(msg, msg_len, "depressed") ||
        hu_str_contains_ci_cstr(msg, msg_len, "hurt") ||
        hu_str_contains_ci_cstr(msg, msg_len, "lonely") ||
        hu_str_contains_ci_cstr(msg, msg_len, "crying") ||
        hu_str_contains_ci_cstr(msg, msg_len, "cry") ||
        hu_str_contains_ci_cstr(msg, msg_len, "miss you") ||
        hu_str_contains_ci_cstr(msg, msg_len, "broken"))
        return "sad";

    /* Anxious: worried, nervous, scared */
    if (hu_str_contains_ci_cstr(msg, msg_len, "anxious") ||
        hu_str_contains_ci_cstr(msg, msg_len, "worried") ||
        hu_str_contains_ci_cstr(msg, msg_len, "nervous") ||
        hu_str_contains_ci_cstr(msg, msg_len, "scared") ||
        hu_str_contains_ci_cstr(msg, msg_len, "freaking out"))
        return "anxious";

    /* Frustrated: angry, ugh, damn, hate */
    if (hu_str_contains_ci_cstr(msg, msg_len, "frustrated") ||
        hu_str_contains_ci_cstr(msg, msg_len, "angry") ||
        hu_str_contains_ci_cstr(msg, msg_len, "ugh") ||
        hu_str_contains_ci_cstr(msg, msg_len, "damn") ||
        hu_str_contains_ci_cstr(msg, msg_len, "hate"))
        return "frustrated";

    /* Happy: great, awesome, lol, haha */
    if (hu_str_contains_ci_cstr(msg, msg_len, "happy") ||
        hu_str_contains_ci_cstr(msg, msg_len, "great") ||
        hu_str_contains_ci_cstr(msg, msg_len, "awesome") ||
        hu_str_contains_ci_cstr(msg, msg_len, "lol") ||
        hu_str_contains_ci_cstr(msg, msg_len, "haha") ||
        hu_str_contains_ci_cstr(msg, msg_len, "lmao"))
        return "happy";

    return "neutral";
}

size_t hu_conversation_extract_topic(const char *msg, size_t msg_len, char *out, size_t cap) {
    return extract_significant_topic(msg, msg_len, out, cap);
}

size_t hu_conversation_extract_followup_topic(const char *msg, size_t msg_len, char *topic_out,
                                              size_t cap) {
    return extract_significant_topic(msg, msg_len, topic_out, cap);
}

/* ── Double-text decision (F9) ──────────────────────────────────────────── */

static const char *const farewell_phrases[] = {
    "bye",   "goodbye",    "good night",  "goodnight",   "gn",  "gotta go",
    "ttyl",  "talk later", "see ya",      "see you",     "cya", "later",
    "night", "nite",       "heading out", "heading off", NULL,
};

bool hu_conversation_should_double_text(const char *last_response, size_t resp_len,
                                        const hu_channel_history_entry_t *entries, size_t count,
                                        uint8_t hour_local, uint32_t seed, float probability) {
    if (!last_response || resp_len == 0 || probability <= 0.0f)
        return false;

    if (hour_local >= 23 || hour_local < 5)
        return false;

    for (const char *const *p = farewell_phrases; *p; p++) {
        if (hu_str_contains_ci_cstr(last_response, resp_len, *p))
            return false;
    }

    float adjusted = probability;
    bool high_energy = false;
    for (size_t i = 0; i < resp_len; i++) {
        if (last_response[i] == '!') {
            high_energy = true;
            break;
        }
    }
    if (!high_energy) {
        static const char *const energy_words[] = {
            "lol", "omg", "haha", "lmao", "no way", NULL,
        };
        for (const char *const *w = energy_words; *w; w++) {
            if (hu_str_contains_ci_cstr(last_response, resp_len, *w)) {
                high_energy = true;
                break;
            }
        }
    }
    if (high_energy)
        adjusted *= 1.5f;

    if (count >= 2) {
        size_t recent_from_me = 0;
        size_t check = count < 4 ? count : 4;
        for (size_t i = count - check; i < count; i++) {
            if (entries[i].from_me)
                recent_from_me++;
        }
        if (recent_from_me >= 3)
            return false;
    }

    uint32_t s = seed;
    s = s * 1103515245u + 12345u;
    double roll = (double)(s >> 16) / 65536.0;
    return roll < (double)adjusted;
}

/* ── Growth celebration detection (F24) ─────────────────────────────────── */

static const char *const growth_phrases[] = {
    "it went great", "i got the job", "nailed it",       "crushed it", "i passed",
    "got promoted",  "it worked out", "turned out well", NULL,
};

bool hu_conversation_detect_growth_opportunity(const char *msg, size_t msg_len, char *topic_out,
                                               size_t topic_cap, char *after_state_out,
                                               size_t after_cap) {
    if (!msg || msg_len == 0 || !topic_out || topic_cap == 0 || !after_state_out || after_cap == 0)
        return false;

    const char *matched = NULL;
    for (const char *const *p = growth_phrases; *p; p++) {
        if (hu_str_contains_ci_cstr(msg, msg_len, *p)) {
            matched = *p;
            break;
        }
    }
    if (!matched)
        return false;

    /* Extract topic from message using significant words */
    size_t topic_len = extract_significant_topic(msg, msg_len, topic_out, topic_cap);
    if (topic_len == 0) {
        /* Fallback: use generic topic */
        const char *fallback = "outcome";
        size_t flen = strlen(fallback);
        size_t copy = flen < topic_cap ? flen : topic_cap - 1;
        memcpy(topic_out, fallback, copy);
        topic_out[copy] = '\0';
    }

    /* After state: short success indicator */
    const char *after = "success";
    size_t alen = strlen(after);
    size_t acopy = alen < after_cap ? alen : after_cap - 1;
    memcpy(after_state_out, after, acopy);
    after_state_out[acopy] = '\0';
    return true;
}

bool hu_conversation_detect_topic_change(const hu_channel_history_entry_t *entries, size_t count,
                                         char *topic_before, size_t before_cap, char *topic_after,
                                         size_t after_cap) {
    if (!entries || count < 2 || !topic_before || before_cap == 0 || !topic_after || after_cap == 0)
        return false;

    /* Find last 2 user messages (from_me=false), newest first */
    const hu_channel_history_entry_t *msg_after = NULL;
    const hu_channel_history_entry_t *msg_before = NULL;
    for (size_t i = count; i > 0; i--) {
        if (!entries[i - 1].from_me) {
            if (!msg_after) {
                msg_after = &entries[i - 1];
            } else if (!msg_before) {
                msg_before = &entries[i - 1];
                break;
            }
        }
    }
    if (!msg_after || !msg_before)
        return false;

    size_t after_len = strlen(msg_after->text);
    size_t before_len = strlen(msg_before->text);
    if (after_len == 0 || before_len == 0)
        return false;

    char t_after[64];
    char t_before[64];
    size_t ta_len = extract_significant_topic(msg_after->text, after_len, t_after, sizeof(t_after));
    size_t tb_len =
        extract_significant_topic(msg_before->text, before_len, t_before, sizeof(t_before));
    if (ta_len == 0 || tb_len == 0)
        return false;

    if (strcasecmp(t_after, t_before) == 0)
        return false;

    size_t copy_after = ta_len < after_cap ? ta_len : after_cap - 1;
    size_t copy_before = tb_len < before_cap ? tb_len : before_cap - 1;
    memcpy(topic_after, t_after, copy_after);
    topic_after[copy_after] = '\0';
    memcpy(topic_before, t_before, copy_before);
    topic_before[copy_before] = '\0';
    return true;
}

/* ── Emotional escalation detection (F14) ───────────────────────────────── */

#define ESCALATION_LOOKBACK 8

static float compute_valence(const char *text, size_t text_len) {
    if (!text || text_len == 0)
        return 0.0f;

    /* Reset signals: "i'm fine", "just kidding", "lol", "haha" → reset consecutive */
    if (hu_str_contains_ci_cstr(text, text_len, "i'm fine") ||
        hu_str_contains_ci_cstr(text, text_len, "im fine") ||
        hu_str_contains_ci_cstr(text, text_len, "just kidding") ||
        hu_str_contains_ci_cstr(text, text_len, "jk") ||
        hu_str_contains_ci_cstr(text, text_len, "lol") ||
        hu_str_contains_ci_cstr(text, text_len, "haha"))
        return 0.0f; /* neutral for reset purposes */

    float valence = 0.0f;

    /* Negative keywords */
    if (hu_str_contains_ci_cstr(text, text_len, "sad") ||
        hu_str_contains_ci_cstr(text, text_len, "angry") ||
        hu_str_contains_ci_cstr(text, text_len, "frustrated") ||
        hu_str_contains_ci_cstr(text, text_len, "upset") ||
        hu_str_contains_ci_cstr(text, text_len, "stressed") ||
        hu_str_contains_ci_cstr(text, text_len, "worse") ||
        hu_str_contains_ci_cstr(text, text_len, "can't deal") ||
        hu_str_contains_ci_cstr(text, text_len, "cant deal") ||
        hu_str_contains_ci_cstr(text, text_len, "hate") ||
        hu_str_contains_ci_cstr(text, text_len, "horrible") ||
        hu_str_contains_ci_cstr(text, text_len, "terrible"))
        valence -= 1.0f;

    /* Positive keywords */
    if (hu_str_contains_ci_cstr(text, text_len, "happy") ||
        hu_str_contains_ci_cstr(text, text_len, "great") ||
        hu_str_contains_ci_cstr(text, text_len, "amazing") ||
        hu_str_contains_ci_cstr(text, text_len, "love") ||
        hu_str_contains_ci_cstr(text, text_len, "thanks") ||
        hu_str_contains_ci_cstr(text, text_len, "better"))
        valence += 1.0f;

    return valence;
}

hu_escalation_state_t hu_conversation_detect_escalation(const hu_channel_history_entry_t *entries,
                                                        size_t count) {
    hu_escalation_state_t out = {false, 0, 0.0f};
    if (!entries || count == 0)
        return out;

    /* Collect last 6–8 their messages (from_me=false), newest first */
    const hu_channel_history_entry_t *their[ESCALATION_LOOKBACK];
    size_t their_count = 0;
    for (size_t i = count; i > 0 && their_count < ESCALATION_LOOKBACK; i--) {
        if (!entries[i - 1].from_me) {
            their[their_count++] = &entries[i - 1];
        }
    }

    if (their_count == 0)
        return out;

    /* Process oldest to newest (reverse order) */
    int consecutive = 0;
    float traj_sum = 0.0f;

    for (size_t i = their_count; i > 0; i--) {
        const hu_channel_history_entry_t *e = their[i - 1];
        size_t tl = strlen(e->text);
        float v = compute_valence(e->text, tl);

        if (v < 0.0f) {
            consecutive++;
        } else {
            consecutive = 0;
        }

        /* trajectory: sum of valences of last 3 messages (most recent = i <= 3) */
        if (i <= 3)
            traj_sum += v;
    }

    /* trajectory: sum of valences of last 3 messages (negative = worsening) */
    out.consecutive_negative = consecutive;
    out.trajectory = traj_sum;
    out.escalating = (consecutive >= 3);

    return out;
}

size_t hu_conversation_build_deescalation_directive(char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    const char *directive =
        "[DE-ESCALATION: Their mood is escalating negatively. Be shorter, more empathetic, fewer "
        "jokes. \"hey you okay?\" energy.]";
    size_t len = strlen(directive);
    if (len >= cap)
        len = cap - 1;
    memcpy(buf, directive, len);
    buf[len] = '\0';
    return len;
}

/* ── Call escalation (F49) ─────────────────────────────────────────────── */

#define HU_CALL_ESCALATION_THRESHOLD 0.6f

static bool is_logistics_or_casual(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return true;
    /* Logistics: quick scheduling, where, when, etc. */
    if (hu_str_contains_ci_cstr(msg, msg_len, "what time") ||
        hu_str_contains_ci_cstr(msg, msg_len, "meet at") ||
        hu_str_contains_ci_cstr(msg, msg_len, "where are") ||
        hu_str_contains_ci_cstr(msg, msg_len, "when are") ||
        hu_str_contains_ci_cstr(msg, msg_len, "pick up") ||
        hu_str_contains_ci_cstr(msg, msg_len, "pickup"))
        return true;
    /* Quick questions / casual: "what's for dinner", short greetings */
    if (hu_str_contains_ci_cstr(msg, msg_len, "what's for dinner") ||
        hu_str_contains_ci_cstr(msg, msg_len, "whats for dinner"))
        return true;
    if (msg_len < 25 && (hu_str_contains_ci_cstr(msg, msg_len, "hey") ||
                         hu_str_contains_ci_cstr(msg, msg_len, "hi ") ||
                         hu_str_contains_ci_cstr(msg, msg_len, "lol") ||
                         hu_str_contains_ci_cstr(msg, msg_len, "nice")))
        return true;
    return false;
}

hu_call_escalation_t
hu_conversation_should_escalate_to_call(const char *msg, size_t msg_len,
                                        const hu_channel_history_entry_t *entries, size_t count) {
    hu_call_escalation_t out = {false, 0.0f};
    (void)entries;
    (void)count;

    if (!msg)
        return out;
    if (msg_len == 0)
        return out;

    /* Never escalate for logistics, quick questions, casual chat. Prefer false negatives. */
    if (is_logistics_or_casual(msg, msg_len))
        return out;

    /* Crisis keywords (weight 0.4): → 1.0 if any match */
    float crisis = 0.0f;
    if (hu_str_contains_ci_cstr(msg, msg_len, "i need you") ||
        hu_str_contains_ci_cstr(msg, msg_len, "help me") ||
        hu_str_contains_ci_cstr(msg, msg_len, "can't deal") ||
        hu_str_contains_ci_cstr(msg, msg_len, "cant deal") ||
        hu_str_contains_ci_cstr(msg, msg_len, "breaking down") ||
        hu_str_contains_ci_cstr(msg, msg_len, "i can't do this") ||
        hu_str_contains_ci_cstr(msg, msg_len, "i cant do this") ||
        hu_str_contains_ci_cstr(msg, msg_len, "please call") ||
        hu_str_contains_ci_cstr(msg, msg_len, "end of my rope"))
        crisis = 1.0f;

    /* Complexity (weight 0.3): length > 200 → 0.6, > 400 → 1.0;
     * multiple questions → 0.3 per question; "it's complicated" → 0.5 */
    float complexity = 0.0f;
    if (msg_len > 400)
        complexity = 1.0f;
    else if (msg_len > 200)
        complexity = 0.6f;
    size_t qcount = 0;
    for (size_t i = 0; i < msg_len; i++)
        if (msg[i] == '?')
            qcount++;
    float q_score = (float)qcount * 0.3f;
    if (q_score > complexity)
        complexity = q_score;
    if (hu_str_contains_ci_cstr(msg, msg_len, "it's complicated") ||
        hu_str_contains_ci_cstr(msg, msg_len, "its complicated") ||
        hu_str_contains_ci_cstr(msg, msg_len, "really complicated")) {
        if (0.5f > complexity)
            complexity = 0.5f;
    }
    if (complexity > 1.0f)
        complexity = 1.0f;

    /* Emotional intensity (weight 0.3): emotional words + exclamation marks */
    float emotion = 0.0f;
    size_t excl = 0;
    for (size_t i = 0; i < msg_len; i++)
        if (msg[i] == '!')
            excl++;
    if (hu_str_contains_ci_cstr(msg, msg_len, "crying") ||
        hu_str_contains_ci_cstr(msg, msg_len, "freaking out") ||
        hu_str_contains_ci_cstr(msg, msg_len, "losing it") ||
        hu_str_contains_ci_cstr(msg, msg_len, "don't know what to do"))
        emotion = 0.8f;
    else if (hu_str_contains_ci_cstr(msg, msg_len, "sad") ||
             hu_str_contains_ci_cstr(msg, msg_len, "overwhelmed") ||
             hu_str_contains_ci_cstr(msg, msg_len, "stressed") ||
             hu_str_contains_ci_cstr(msg, msg_len, "anxious") ||
             hu_str_contains_ci_cstr(msg, msg_len, "scared") ||
             hu_str_contains_ci_cstr(msg, msg_len, "hurt") ||
             hu_str_contains_ci_cstr(msg, msg_len, "depressed") ||
             hu_str_contains_ci_cstr(msg, msg_len, "lonely"))
        emotion = 0.5f;
    if (excl >= 2)
        emotion = (emotion > 0.6f) ? emotion : 0.6f;
    else if (excl >= 1 && emotion < 0.3f)
        emotion = 0.3f;
    if (emotion > 1.0f)
        emotion = 1.0f;

    float score = crisis * 0.4f + complexity * 0.3f + emotion * 0.3f;
    if (crisis >= 1.0f && score < 0.6f)
        score = 0.6f;
    out.score = score;
    out.should_suggest = (score >= HU_CALL_ESCALATION_THRESHOLD);
    return out;
}

size_t hu_conversation_build_call_directive(const char *msg, size_t msg_len, char *buf,
                                            size_t cap) {
    if (!buf || cap == 0)
        return 0;
    size_t sample_len = msg_len > 80 ? 80 : msg_len;
    int n;
    if (msg && msg_len > 0) {
        n = snprintf(
            buf, cap,
            "[CALL: This may be better as a call. Suggest they call when appropriate: \"%.*s%s\"]",
            (int)sample_len, msg, msg_len > 80 ? "..." : "");
    } else {
        n = snprintf(buf, cap,
                     "[CALL: This may be better as a call. Suggest they call when appropriate.]");
    }
    if (n <= 0 || (size_t)n >= cap)
        return 0;
    return (size_t)n;
}

/* ── Comfort pattern directive (F27) ───────────────────────────────────── */

size_t hu_conversation_build_comfort_directive(const char *response_type, size_t type_len,
                                               const char *emotion, size_t emotion_len, char *buf,
                                               size_t cap) {
    if (!buf || cap == 0 || !response_type || type_len == 0 || !emotion || emotion_len == 0)
        return 0;
    int n = snprintf(buf, cap, "[COMFORT: This contact responds well to %.*s when %.*s.]",
                     (int)type_len, response_type, (int)emotion_len, emotion);
    if (n <= 0 || (size_t)n >= cap)
        return 0;
    return (size_t)n;
}

/* ── First-time vulnerability detection (F17) ─────────────────────────── */

typedef struct {
    const char *category;
    const char *keywords[16];
} hu_vuln_category_t;

/* Order matters: loss before family_issue (so "my dad passed away" → loss);
 * family_issue before illness (so "my mom is sick" → family_issue). */
static const hu_vuln_category_t vuln_categories[] = {
    {"job_loss", {"fired", "laid off", "lost my job", "getting let go", "downsized", NULL}},
    {"divorce", {"divorce", "separating", "custody", "ex-wife", "ex-husband", NULL}},
    {"mental_health",
     {"depression", "therapy", "therapist", "medication", "anxiety disorder", "panic attacks",
      NULL}},
    {"loss", {"died", "passed away", "funeral", "lost my", "grieving", NULL}},
    {"family_issue", {"my mom", "my dad", "my parents", "my brother", "my sister", NULL}},
    {"illness", {"diagnosis", "cancer", "sick", "hospital", "surgery", "chemo", NULL}},
};

static const char *vuln_family_negative[] = {
    "sick",   "hospital",  "problem", "issue",   "fighting",   "worried", "died", "passed away",
    "cancer", "diagnosis", "divorce", "custody", "struggling", "stress",  NULL,
};

static bool family_issue_has_negative_context(const char *msg, size_t msg_len) {
    for (const char *const *kw = vuln_family_negative; *kw; kw++) {
        if (hu_str_contains_ci_cstr(msg, msg_len, *kw))
            return true;
    }
    return false;
}

const char *hu_conversation_extract_vulnerability_topic(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return NULL;

    for (size_t c = 0; c < sizeof(vuln_categories) / sizeof(vuln_categories[0]); c++) {
        const hu_vuln_category_t *cat = &vuln_categories[c];
        bool found = false;
        for (const char *const *kw = cat->keywords; *kw && !found; kw++) {
            if (hu_str_contains_ci_cstr(msg, msg_len, *kw))
                found = true;
        }
        if (found) {
            if (strcmp(cat->category, "family_issue") == 0 &&
                !family_issue_has_negative_context(msg, msg_len))
                continue;
            return cat->category;
        }
    }
    return NULL;
}

#ifdef HU_ENABLE_SQLITE
bool hu_conversation_is_first_time_topic(hu_memory_t *memory, const char *contact_id,
                                         size_t contact_id_len, const char *topic,
                                         size_t topic_len) {
    if (!memory || !contact_id || contact_id_len == 0 || !topic || topic_len == 0)
        return true;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return true;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT COUNT(*) FROM emotional_moments "
                                "WHERE contact_id=? AND topic LIKE ?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (stmt)
            sqlite3_finalize(stmt);
        return true;
    }

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    char pattern[272];
    int pn = snprintf(pattern, sizeof(pattern), "%%%.*s%%", (int)topic_len, topic);
    if (pn <= 0 || (size_t)pn >= sizeof(pattern)) {
        sqlite3_finalize(stmt);
        return true;
    }
    sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    return count == 0;
}
#endif

hu_vulnerability_state_t hu_conversation_detect_first_time_vulnerability(const char *msg,
                                                                         size_t msg_len,
                                                                         hu_memory_t *memory,
                                                                         const char *contact_id,
                                                                         size_t contact_id_len) {
    hu_vulnerability_state_t out = {false, NULL, 0.5f};

    const char *topic = hu_conversation_extract_vulnerability_topic(msg, msg_len);
    if (!topic)
        return out;

    out.topic_category = topic;
    out.intensity = 0.7f;

#ifdef HU_ENABLE_SQLITE
    size_t topic_len = strlen(topic);
    out.first_time =
        hu_conversation_is_first_time_topic(memory, contact_id, contact_id_len, topic, topic_len);
#else
    (void)memory;
    (void)contact_id;
    (void)contact_id_len;
    out.first_time = true;
#endif

    return out;
}

size_t hu_conversation_build_vulnerability_directive(const hu_vulnerability_state_t *state,
                                                     char *buf, size_t cap) {
    if (!state || !buf || cap == 0)
        return 0;
    if (!state->first_time || !state->topic_category)
        return 0;

    int n = snprintf(buf, cap,
                     "[VULNERABILITY: First time they've shared about %s. Extra care. "
                     "Acknowledge weight. Don't pivot to advice. "
                     "\"that's huge. thanks for telling me.\" energy.]",
                     state->topic_category);
    if (n <= 0 || (size_t)n >= cap)
        return 0;
    return (size_t)n;
}

/* ── Context modifiers (F16) ─────────────────────────────────────────── */

static bool detect_heavy_topic(const hu_channel_history_entry_t *entries, size_t count) {
    if (!entries || count == 0)
        return false;
    static const char *DEFAULT_KEYWORDS[] = {
        "died",      "passed away", "passed",  "cancer",      "divorce",  "fired",    "laid off",
        "diagnosis", "funeral",     "breakup", "lost my job", "terminal", "hospital",
    };
    static const size_t DEFAULT_KEYWORDS_LEN =
        sizeof(DEFAULT_KEYWORDS) / sizeof(DEFAULT_KEYWORDS[0]);

    const char **keywords = s_crisis_keywords_len > 0 ? (const char **)s_crisis_keywords
                                                      : (const char **)DEFAULT_KEYWORDS;
    size_t keywords_len = s_crisis_keywords_len > 0 ? s_crisis_keywords_len : DEFAULT_KEYWORDS_LEN;

    for (size_t i = 0; i < count; i++) {
        if (entries[i].from_me)
            continue;
        const char *t = entries[i].text;
        size_t tl = strlen(t);
        for (size_t k = 0; k < keywords_len; k++) {
            if (keywords[k] && hu_str_contains_ci_cstr(t, tl, keywords[k]))
                return true;
        }
    }
    return false;
}

static bool detect_personal_sharing(const hu_channel_history_entry_t *entries, size_t count) {
    if (!entries || count == 0)
        return false;
    static const char *DEFAULT_PHRASES[] = {
        "i need to tell you",        "can i be honest", "don't judge me",
        "this is hard to say",       "i never told",    "first time i'm saying",
        "i've been meaning to tell", "dont judge me",   "first time im saying",
        "ive been meaning to tell",
    };
    static const size_t DEFAULT_PHRASES_LEN = sizeof(DEFAULT_PHRASES) / sizeof(DEFAULT_PHRASES[0]);

    const char **phrases = s_personal_sharing_phrases_len > 0
                               ? (const char **)s_personal_sharing_phrases
                               : (const char **)DEFAULT_PHRASES;
    size_t phrases_len =
        s_personal_sharing_phrases_len > 0 ? s_personal_sharing_phrases_len : DEFAULT_PHRASES_LEN;

    for (size_t i = 0; i < count; i++) {
        if (entries[i].from_me)
            continue;
        const char *t = entries[i].text;
        size_t tl = strlen(t);
        for (size_t p = 0; p < phrases_len; p++) {
            if (phrases[p] && hu_str_contains_ci_cstr(t, tl, phrases[p]))
                return true;
        }
    }
    return false;
}

static bool detect_early_turn(const hu_channel_history_entry_t *entries, size_t count) {
    if (!entries || count == 0)
        return false;
    size_t from_me_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].from_me)
            from_me_count++;
    }
    return count <= 6 && from_me_count <= 3;
}

size_t hu_conversation_build_context_modifiers(const hu_channel_history_entry_t *entries,
                                               size_t count, const hu_emotional_state_t *emo,
                                               const hu_context_modifiers_t *mods, char *buf,
                                               size_t cap) {
    if (!buf || cap == 0)
        return 0;

    float reduction = 0.4f;
    float warmth_boost = 1.6f;
    float breathing_boost = 1.5f;
    float humanization_boost = 1.4f;
    if (mods) {
        reduction = mods->serious_topics_reduction > 0.0f ? mods->serious_topics_reduction : 0.4f;
        warmth_boost =
            mods->personal_sharing_warmth_boost > 0.0f ? mods->personal_sharing_warmth_boost : 1.6f;
        breathing_boost =
            mods->high_emotion_breathing_boost > 0.0f ? mods->high_emotion_breathing_boost : 1.5f;
        humanization_boost =
            mods->early_turn_humanization_boost > 0.0f ? mods->early_turn_humanization_boost : 1.4f;
    }
    (void)breathing_boost;

    size_t pos = 0;
    int w;

    if (detect_heavy_topic(entries, count)) {
        int pct = (int)(reduction * 100.0f);
        if (pct < 0)
            pct = 0;
        if (pct > 100)
            pct = 100;
        w = snprintf(buf + pos, cap - pos,
                     "[CONTEXT: Heavy topic detected. Reduce humor by %d%%.]\n", pct);
        if (w > 0 && (size_t)w < cap - pos)
            pos += (size_t)w;
    }

    if (detect_personal_sharing(entries, count)) {
        int pct = (int)((warmth_boost - 1.0f) * 100.0f);
        if (pct < 0)
            pct = 0;
        if (pct > 200)
            pct = 200;
        w = snprintf(buf + pos, cap - pos,
                     "[CONTEXT: They're sharing something personal. Boost warmth %d%%.]\n", pct);
        if (w > 0 && (size_t)w < cap - pos)
            pos += (size_t)w;
    }

    if (emo && emo->intensity > 0.8f) {
        w = snprintf(buf + pos, cap - pos,
                     "[CONTEXT: High emotion. Use shorter sentences, more line breaks.]\n");
        if (w > 0 && (size_t)w < cap - pos)
            pos += (size_t)w;
    }

    if (detect_early_turn(entries, count)) {
        (void)humanization_boost; /* used for future percentage if needed */
        w = snprintf(buf + pos, cap - pos,
                     "[CONTEXT: Early in conversation. Be warmer, more human.]\n");
        if (w > 0 && (size_t)w < cap - pos)
            pos += (size_t)w;
    }

    if (pos > 0 && buf[pos - 1] == '\n')
        pos--; /* trim trailing newline for cleaner output */
    buf[pos] = '\0';
    return pos;
}

/* ── Typo correction fragment (*meant) ─────────────────────────────────── */

size_t hu_conversation_generate_correction(const char *original, size_t original_len,
                                           const char *typo_applied, size_t typo_applied_len,
                                           char *out_buf, size_t out_cap, uint32_t seed,
                                           uint32_t correction_chance) {
    if (!original || !typo_applied || !out_buf || out_cap < 4)
        return 0;

    /* Find first difference */
    size_t diff_pos = 0;
    size_t lim = original_len < typo_applied_len ? original_len : typo_applied_len;
    while (diff_pos < lim && original[diff_pos] == typo_applied[diff_pos])
        diff_pos++;

    /* No difference: identical strings */
    if (diff_pos >= lim && original_len == typo_applied_len)
        return 0;

    /* Find word boundaries in original: word = run of non-space chars bounded by space or start/end
     */
    size_t word_start = diff_pos;
    while (word_start > 0 && original[word_start - 1] != ' ')
        word_start--;
    size_t word_end = diff_pos;
    while (word_end < original_len && original[word_end] != ' ')
        word_end++;

    size_t word_len = word_end - word_start;
    if (word_len == 0)
        return 0;

    /* Seed-based PRNG: LCG for deterministic chance check */
    uint32_t state = seed * 1103515245u + 12345u;
    uint32_t roll = (state >> 16) % 100u;
    if (roll >= correction_chance)
        return 0;

    /* Format "*<correct_word>" — need 1 + word_len + 1 for null */
    if (out_cap < 2 + word_len)
        word_len = out_cap > 2 ? out_cap - 2 : 0;
    if (word_len == 0)
        return 0;

    int n = snprintf(out_buf, out_cap, "*%.*s", (int)word_len, original + word_start);
    if (n < 0)
        return 0;
    return ((size_t)n >= out_cap) ? out_cap - 1 : (size_t)n;
}

/* ── Multi-message splitting ──────────────────────────────────────────── */

/*
 * Split at natural breakpoints that mimic how humans fragment thoughts
 * across multiple iMessage bubbles. Priorities:
 * 1. Explicit newlines in the response
 * 2. Sentence boundaries followed by conjunctions/interjections
 * 3. Sentence boundaries when response is long enough
 */

static bool is_split_starter(const char *s, size_t len) {
    if (len < 2)
        return false;
    /* Starters that signal a new thought bubble (loaded from JSON or fallback) */
    static const char *DEFAULT_STARTERS[] = {
        "oh ",  "but ", "and ", "like ",   "also ", "wait ", "haha", "lol", "omg",
        "ngl ", "tbh ", "btw ", "anyway ", "ok ",   "so ",   "yeah", "nah",
    };
    static const size_t DEFAULT_STARTERS_LEN =
        sizeof(DEFAULT_STARTERS) / sizeof(DEFAULT_STARTERS[0]);

    const char **starters =
        s_starters_len > 0 ? (const char **)s_starters : (const char **)DEFAULT_STARTERS;
    size_t starters_len = s_starters_len > 0 ? s_starters_len : DEFAULT_STARTERS_LEN;

    for (size_t i = 0; i < starters_len; i++) {
        if (!starters[i])
            continue;
        size_t sl = strlen(starters[i]);
        if (len >= sl) {
            bool match = true;
            for (size_t j = 0; j < sl; j++) {
                char a = s[j];
                char b = starters[i][j];
                if (a >= 'A' && a <= 'Z')
                    a += 32;
                if (a != b) {
                    match = false;
                    break;
                }
            }
            if (match)
                return true;
        }
    }
    return false;
}

size_t hu_conversation_split_response(hu_allocator_t *alloc, const char *response,
                                      size_t response_len, hu_message_fragment_t *fragments,
                                      size_t max_fragments, uint32_t max_chars) {
    if (!alloc || !response || response_len == 0 || !fragments || max_fragments == 0)
        return 0;

    /* Length thresholds scale with max_chars; 0 => ~300-char reference (legacy default). */
    const uint32_t limit = max_chars != 0 ? max_chars : 300u;
    const uint64_t base = 300u;
    const size_t thr_short = (size_t)((40ull * (uint64_t)limit + base - 1ull) / base);
    const size_t thr_long = (size_t)((80ull * (uint64_t)limit + base - 1ull) / base);
    const size_t thr_scan = (size_t)((30ull * (uint64_t)limit + base - 1ull) / base);
    const size_t thr_tail = (size_t)((15ull * (uint64_t)limit + base - 1ull) / base);
    const size_t thr_rem = (size_t)((10ull * (uint64_t)limit + base - 1ull) / base);

    /* Short responses stay as one message */
    if (response_len < thr_short || max_fragments == 1) {
        char *copy = (char *)alloc->alloc(alloc->ctx, response_len + 1);
        if (!copy)
            return 0;
        memcpy(copy, response, response_len);
        copy[response_len] = '\0';
        fragments[0].text = copy;
        fragments[0].text_len = response_len;
        fragments[0].delay_ms = 0;
        return 1;
    }

    /* First pass: find split points */
    size_t split_points[8];
    size_t split_count = 0;

    /* Check for explicit newlines first */
    for (size_t i = 1; i < response_len - 1 && split_count < 7; i++) {
        if (response[i] == '\n') {
            /* Skip consecutive newlines */
            size_t next = i + 1;
            while (next < response_len && response[next] == '\n')
                next++;
            if (next < response_len && next - i > 0) {
                split_points[split_count++] = next;
                i = next - 1;
            }
        }
    }

    /* If no newlines, split at sentence boundaries before conjunctions */
    if (split_count == 0) {
        for (size_t i = 2; i < response_len - 2 && split_count < 7; i++) {
            char prev = response[i - 1];
            if ((prev == '.' || prev == '!' || prev == '?') && response[i] == ' ') {
                size_t remaining = response_len - (i + 1);
                if (remaining > thr_rem && is_split_starter(response + i + 1, remaining)) {
                    split_points[split_count++] = i + 1;
                }
            }
        }
    }

    /* If still nothing, split at sentence boundaries for long responses */
    if (split_count == 0 && response_len > thr_long) {
        for (size_t i = thr_scan; i + thr_tail < response_len && split_count < 3; i++) {
            char prev = response[i - 1];
            if ((prev == '.' || prev == '!' || prev == '?') && response[i] == ' ') {
                split_points[split_count++] = i + 1;
            }
        }
    }

    if (split_count == 0) {
        char *copy = (char *)alloc->alloc(alloc->ctx, response_len + 1);
        if (!copy)
            return 0;
        memcpy(copy, response, response_len);
        copy[response_len] = '\0';
        fragments[0].text = copy;
        fragments[0].text_len = response_len;
        fragments[0].delay_ms = 0;
        return 1;
    }

    /* Cap at max_fragments - 1 split points */
    if (split_count >= max_fragments)
        split_count = max_fragments - 1;

    /* Build fragments */
    size_t frag_count = 0;
    size_t start = 0;
    for (size_t s = 0; s < split_count && frag_count < max_fragments - 1; s++) {
        size_t end = split_points[s];
        /* Trim trailing whitespace/newlines */
        size_t trim_end = end;
        while (trim_end > start &&
               (response[trim_end - 1] == ' ' || response[trim_end - 1] == '\n'))
            trim_end--;
        size_t flen = trim_end - start;
        if (flen < 2) {
            start = end;
            continue;
        }
        char *frag = (char *)alloc->alloc(alloc->ctx, flen + 1);
        if (!frag) {
            for (size_t k = 0; k < frag_count; k++)
                alloc->free(alloc->ctx, fragments[k].text, fragments[k].text_len + 1);
            return 0;
        }
        memcpy(frag, response + start, flen);
        frag[flen] = '\0';
        fragments[frag_count].text = frag;
        fragments[frag_count].text_len = flen;
        fragments[frag_count].delay_ms = frag_count == 0 ? 0 : (uint32_t)(500 + flen * 15);
        if (fragments[frag_count].delay_ms > 3000)
            fragments[frag_count].delay_ms = 3000;
        frag_count++;
        start = end;
    }

    /* Final fragment */
    if (start < response_len) {
        /* Trim leading whitespace */
        while (start < response_len && response[start] == ' ')
            start++;
        size_t flen = response_len - start;
        if (flen > 0) {
            char *frag = (char *)alloc->alloc(alloc->ctx, flen + 1);
            if (!frag) {
                for (size_t k = 0; k < frag_count; k++)
                    alloc->free(alloc->ctx, fragments[k].text, fragments[k].text_len + 1);
                return 0;
            }
            memcpy(frag, response + start, flen);
            frag[flen] = '\0';
            fragments[frag_count].text = frag;
            fragments[frag_count].text_len = flen;
            fragments[frag_count].delay_ms = frag_count == 0 ? 0 : (uint32_t)(500 + flen * 15);
            if (fragments[frag_count].delay_ms > 3000)
                fragments[frag_count].delay_ms = 3000;
            frag_count++;
        }
    }

    return frag_count;
}

/* ── Situational length calibration ───────────────────────────────────── */

int hu_conversation_max_response_chars(size_t incoming_len) {
    if (incoming_len == 0)
        return (int)g_min_response_chars;
    double scaled = (double)incoming_len * 2.0;
    int result = (int)scaled;
    if (result < (int)g_min_response_chars)
        result = (int)g_min_response_chars;
    if (result > (int)g_max_response_chars)
        result = (int)g_max_response_chars;
    return result;
}

int hu_conversation_max_response_chars_relational(size_t incoming_len,
                                                  const hu_contact_profile_t *contact,
                                                  hu_relationship_stage_t session_stage) {
    if (incoming_len == 0)
        return (int)g_min_response_chars;
    double mult = 2.0;
    if (session_stage >= HU_REL_DEEP)
        mult = 3.25;
    else if (session_stage >= HU_REL_TRUSTED)
        mult = 3.0;
    else if (session_stage >= HU_REL_FAMILIAR)
        mult = 2.55;

    if (contact) {
        if (contact->prefers_short_texts)
            mult = 2.0;
        if (contact->relationship_type && contact->relationship_type[0]) {
            size_t tl = strlen(contact->relationship_type);
            if (hu_str_contains_ci_cstr(contact->relationship_type, tl, "family") ||
                hu_str_contains_ci_cstr(contact->relationship_type, tl, "friend")) {
                if (mult < 2.75)
                    mult = 2.75;
            }
        }
        if (contact->relationship_stage && contact->relationship_stage[0]) {
            const char *rs = contact->relationship_stage;
            size_t rsl = strlen(rs);
            if (hu_str_contains_ci_cstr(rs, rsl, "close") ||
                hu_str_contains_ci_cstr(rs, rsl, "best") ||
                hu_str_contains_ci_cstr(rs, rsl, "intimate") ||
                hu_str_contains_ci_cstr(rs, rsl, "friend")) {
                if (mult < 2.7)
                    mult = 2.7;
            }
        }
        if (contact->warmth_level && contact->warmth_level[0]) {
            const char *w = contact->warmth_level;
            size_t wl = strlen(w);
            if (hu_str_contains_ci_cstr(w, wl, "high") || hu_str_contains_ci_cstr(w, wl, "warm")) {
                if (mult < 2.65)
                    mult = 2.65;
            }
        }
    }

    double scaled = (double)incoming_len * mult;
    int result = (int)scaled;
    if (result < (int)g_min_response_chars)
        result = (int)g_min_response_chars;
    if (result > (int)g_max_response_chars)
        result = (int)g_max_response_chars;
    return result;
}

uint32_t hu_conversation_brief_char_cap(bool is_group, const hu_contact_profile_t *contact,
                                        hu_relationship_stage_t session_stage) {
    if (is_group)
        return 50u;

    if (contact && contact->prefers_short_texts)
        return 72u;

    if (session_stage >= HU_REL_DEEP)
        return 260u;
    if (session_stage >= HU_REL_TRUSTED)
        return 220u;
    if (session_stage >= HU_REL_FAMILIAR)
        return 160u;

    if (contact) {
        if (contact->relationship_type && contact->relationship_type[0]) {
            size_t tl = strlen(contact->relationship_type);
            if (hu_str_contains_ci_cstr(contact->relationship_type, tl, "family") ||
                hu_str_contains_ci_cstr(contact->relationship_type, tl, "friend"))
                return 185u;
        }
        if (contact->relationship_stage && contact->relationship_stage[0]) {
            const char *rs = contact->relationship_stage;
            size_t rsl = strlen(rs);
            if (hu_str_contains_ci_cstr(rs, rsl, "close") ||
                hu_str_contains_ci_cstr(rs, rsl, "best") ||
                hu_str_contains_ci_cstr(rs, rsl, "intimate") ||
                hu_str_contains_ci_cstr(rs, rsl, "friend"))
                return 195u;
            if (hu_str_contains_ci_cstr(rs, rsl, "work") ||
                hu_str_contains_ci_cstr(rs, rsl, "acquaint") ||
                hu_str_contains_ci_cstr(rs, rsl, "new"))
                return 85u;
        }
        if (contact->warmth_level && contact->warmth_level[0]) {
            const char *w = contact->warmth_level;
            size_t wl = strlen(w);
            if (hu_str_contains_ci_cstr(w, wl, "high") || hu_str_contains_ci_cstr(w, wl, "warm"))
                return 175u;
            if (hu_str_contains_ci_cstr(w, wl, "low") || hu_str_contains_ci_cstr(w, wl, "cold"))
                return 72u;
        }
    }

    return 96u;
}

/*
 * Instead of "keep under 50 chars", tell the model WHY a certain length
 * is right. Humans calibrate response length by message type, not by
 * counting characters. This function classifies the incoming message
 * and produces a directive that mimics human instinct.
 */

static size_t calibrate_length_impl(const char *last_msg, size_t last_msg_len,
                                    const hu_channel_history_entry_t *entries, size_t count,
                                    bool is_group, const hu_contact_profile_t *contact,
                                    hu_relationship_stage_t session_stage, char *buf, size_t cap) {
    if (!last_msg || last_msg_len == 0 || !buf || cap < 64)
        return 0;

    size_t pos = 0;
    int w;

    hu_convo_metrics_t m;
    compute_convo_metrics(entries, count, &m);

    w = snprintf(buf + pos, cap - pos, "\n--- Response calibration ---\n");
    POS_ADVANCE(w, pos, cap);

    /* Style calibration from actual conversation data */
    if (m.their_msgs > 0) {
        size_t avg = m.their_total_chars / m.their_msgs;
        w = snprintf(buf + pos, cap - pos,
                     "Their average message: %zu chars across %zu messages.\n", avg, m.their_msgs);
        POS_ADVANCE(w, pos, cap);

        if (m.longest_their_msg > 0) {
            w = snprintf(buf + pos, cap - pos, "Their longest recent message: %zu chars.\n",
                         m.longest_their_msg);
            POS_ADVANCE(w, pos, cap);
        }

        /* Style: capitalization, punctuation */
        if (m.their_msgs >= 2) {
            if (m.msgs_all_lower > m.their_msgs * 2 / 3) {
                w = snprintf(buf + pos, cap - pos,
                             "Their style: mostly lowercase, minimal punctuation.\n");
            } else if (m.msgs_no_period_end > m.their_msgs * 2 / 3) {
                w = snprintf(buf + pos, cap - pos,
                             "Their style: rarely ends with periods, casual punctuation.\n");
            } else {
                w = snprintf(buf + pos, cap - pos,
                             "Their style: mixed capitalization and punctuation.\n");
            }
            POS_ADVANCE(w, pos, cap);
        }
    }

    /* Last message length (structural) + numeric char limit for prompt */
    int max_chars = is_group ? hu_conversation_max_response_chars(last_msg_len)
                             : hu_conversation_max_response_chars_relational(last_msg_len, contact,
                                                                             session_stage);
    w = snprintf(buf + pos, cap - pos, "Their last message: %zu chars. ", last_msg_len);
    POS_ADVANCE(w, pos, cap);
    if (last_msg_len < 15) {
        /* Cold caps stay telegraphic; warm relationships get permission for a human sentence. */
        if (!is_group && max_chars >= 30) {
            w = snprintf(buf + pos, cap - pos,
                         "Short line — still sound like a real friend, not a bot. Natural voice, "
                         "up to %d chars.\n",
                         max_chars);
        } else {
            w = snprintf(buf + pos, cap - pos,
                         "Very brief — match that brevity. Target: 1-%d chars.\n", max_chars);
        }
    } else if (last_msg_len > 100) {
        w = snprintf(buf + pos, cap - pos,
                     "Substantial — you can respond with more depth, but don't over-match. "
                     "Max %d chars.\n",
                     max_chars);
    } else {
        w = snprintf(buf + pos, cap - pos,
                     "Moderate length — match their energy and length. Target: ~%d chars max.\n",
                     max_chars);
    }
    POS_ADVANCE(w, pos, cap);

    /* Question (structural: contains ?) */
    if (strchr(last_msg, '?') != NULL) {
        w = snprintf(buf + pos, cap - pos, "They asked a question — answer it directly.\n");
        POS_ADVANCE(w, pos, cap);
    }

    /* Link (structural: contains http or .com) */
    {
        bool last_has_http = (last_msg_len >= 4 && (strncmp(last_msg, "http", 4) == 0 ||
                                                    strstr(last_msg, "http") != NULL ||
                                                    strstr(last_msg, ".com") != NULL));
        if (m.has_link || last_has_http) {
            w = snprintf(buf + pos, cap - pos,
                         "They shared a link — acknowledge or comment on it.\n");
            POS_ADVANCE(w, pos, cap);
        }
    }

    /* Momentum from exchange density */
    if (m.rapid_exchanges >= 4) {
        w = snprintf(
            buf + pos, cap - pos,
            "Rapid-fire exchange — keep responses extra short, one thought per message.\n");
        POS_ADVANCE(w, pos, cap);
    }

    /* Partial engagement: multi-topic messages get selective response.
     * Count sentence-ending punctuation as a proxy for distinct topics. */
    {
        size_t sentence_ends = 0;
        for (size_t si = 0; si < last_msg_len; si++) {
            if (last_msg[si] == '.' || last_msg[si] == '!' || last_msg[si] == '?')
                sentence_ends++;
        }
        if (sentence_ends >= 2 || last_msg_len > 200) {
            w = snprintf(buf + pos, cap - pos,
                         "Multiple topics detected. Don't address every point. "
                         "Respond to the most important thing only.\n");
            POS_ADVANCE(w, pos, cap);
        }
        /* Brief-style caps: reinforce single-thread answer when they packed several ideas in. */
        if (max_chars <= 90 && sentence_ends >= 2) {
            w = snprintf(buf + pos, cap - pos,
                         "You have a very tight reply budget and they raised several threads — "
                         "answer only the single most important one.\n");
            POS_ADVANCE(w, pos, cap);
        }
    }

    /* Generic guidance (data-driven, not prescriptive) */
    w = snprintf(buf + pos, cap - pos,
                 "Match their energy and length. If they're brief, be brief. If they open up, "
                 "you can too.\n");
    POS_ADVANCE(w, pos, cap);

    w = snprintf(buf + pos, cap - pos, "--- End calibration ---\n");
    POS_ADVANCE(w, pos, cap);

    buf[pos] = '\0';
    return pos;
}

size_t hu_conversation_calibrate_length(const char *last_msg, size_t last_msg_len,
                                        const hu_channel_history_entry_t *entries, size_t count,
                                        char *buf, size_t cap) {
    return calibrate_length_impl(last_msg, last_msg_len, entries, count, false, NULL, HU_REL_NEW,
                                 buf, cap);
}

size_t hu_conversation_calibrate_length_for_contact(const char *last_msg, size_t last_msg_len,
                                                    const hu_channel_history_entry_t *entries,
                                                    size_t count, bool is_group,
                                                    const hu_contact_profile_t *contact,
                                                    hu_relationship_stage_t session_stage,
                                                    char *buf, size_t cap) {
    return calibrate_length_impl(last_msg, last_msg_len, entries, count, is_group, contact,
                                 session_stage, buf, cap);
}

/* ── Texting style analysis ───────────────────────────────────────────── */

char *hu_conversation_analyze_style(hu_allocator_t *alloc,
                                    const hu_channel_history_entry_t *entries, size_t count,
                                    const struct hu_persona *persona, size_t *out_len) {
    if (!alloc || !entries || count == 0 || !out_len)
        return NULL;
    *out_len = 0;

    /* Analyze their (non-self) messages */
    size_t their_count = 0;
    size_t total_chars = 0;
    size_t msgs_with_caps_start = 0;
    size_t msgs_no_period_end = 0;
    size_t msgs_all_lower = 0;
    size_t msgs_with_abbrev = 0;
    size_t fragment_count = 0; /* messages under 25 chars (rapid-fire fragments) */

    for (size_t i = 0; i < count; i++) {
        if (entries[i].from_me)
            continue;
        const char *t = entries[i].text;
        size_t tl = strlen(t);
        if (tl < 2)
            continue;
        their_count++;
        total_chars += tl;

        if (tl < 25)
            fragment_count++;

        bool has_upper = false;
        bool has_lower = false;
        bool starts_cap = (t[0] >= 'A' && t[0] <= 'Z');
        if (starts_cap)
            msgs_with_caps_start++;

        char last_alpha = 0;
        for (size_t j = 0; j < tl; j++) {
            if (t[j] >= 'A' && t[j] <= 'Z')
                has_upper = true;
            else if (t[j] >= 'a' && t[j] <= 'z')
                has_lower = true;
            if ((t[j] >= 'a' && t[j] <= 'z') || (t[j] >= 'A' && t[j] <= 'Z'))
                last_alpha = t[j];
        }

        if (has_lower && !has_upper)
            msgs_all_lower++;
        if (last_alpha && last_alpha != '.' && t[tl - 1] != '.' && t[tl - 1] != '!' &&
            t[tl - 1] != '?')
            msgs_no_period_end++;

        /* Abbreviation detection */
        if (hu_str_contains_ci_cstr(t, tl, "lol") || hu_str_contains_ci_cstr(t, tl, "omg") ||
            hu_str_contains_ci_cstr(t, tl, "ngl") || hu_str_contains_ci_cstr(t, tl, "tbh") ||
            hu_str_contains_ci_cstr(t, tl, "rn") || hu_str_contains_ci_cstr(t, tl, "idk") ||
            hu_str_contains_ci_cstr(t, tl, "imo") || hu_str_contains_ci_cstr(t, tl, "nvm") ||
            hu_str_contains_ci_cstr(t, tl, "btw") || hu_str_contains_ci_cstr(t, tl, "wya") ||
            hu_str_contains_ci_cstr(t, tl, "hbu"))
            msgs_with_abbrev++;
    }

    if (their_count < 3) {
        return NULL;
    }

#define STYLE_BUF_CAP 2048
    char *buf = (char *)alloc->alloc(alloc->ctx, STYLE_BUF_CAP);
    if (!buf)
        return NULL;
    size_t pos = 0;
    int w;

    w = snprintf(buf + pos, STYLE_BUF_CAP - pos, "--- Their texting style (mirror this) ---\n");
    POS_ADVANCE(w, pos, STYLE_BUF_CAP);

    /* Capitalization pattern */
    if (msgs_all_lower > their_count * 2 / 3) {
        w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                     "CAPS: They almost never capitalize. Write in all lowercase. "
                     "No capital letters at start of sentences.\n");
    } else if (msgs_with_caps_start < their_count / 3) {
        w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                     "CAPS: They rarely capitalize sentence starts. "
                     "Skip capital letters most of the time.\n");
    } else {
        w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                     "CAPS: They use normal capitalization. Match that.\n");
    }
    POS_ADVANCE(w, pos, STYLE_BUF_CAP);

    /* Punctuation pattern */
    if (msgs_no_period_end > their_count * 2 / 3) {
        w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                     "PUNCTUATION: They almost never end with periods. "
                     "Drop periods at end of messages. Questions marks are ok.\n");
    } else {
        w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                     "PUNCTUATION: They use normal punctuation. Match that.\n");
    }
    POS_ADVANCE(w, pos, STYLE_BUF_CAP);

    /* Fragmentation pattern */
    if (fragment_count > their_count / 2) {
        w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                     "STYLE: They send short rapid-fire messages. Keep yours very short too. "
                     "One thought per message.\n");
    } else {
        w = 0;
    }
    POS_ADVANCE(w, pos, STYLE_BUF_CAP);

    /* Abbreviation pattern */
    if (msgs_with_abbrev > their_count / 3) {
        w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                     "SLANG: They use texting abbreviations (lol, omg, ngl, tbh, idk). "
                     "Use these naturally.\n");
        POS_ADVANCE(w, pos, STYLE_BUF_CAP);
    }

    /* Average message length */
    if (their_count > 0) {
        size_t avg = total_chars / their_count;
        if (avg < 25) {
            w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                         "LENGTH: Their average message is ~%zu chars. "
                         "Keep yours under 30 chars per message.\n",
                         avg);
        } else if (avg < 60) {
            w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                         "LENGTH: Their average message is ~%zu chars. "
                         "Keep yours around that length.\n",
                         avg);
        } else {
            w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                         "LENGTH: Their average message is ~%zu chars. "
                         "You can write a bit more but stay concise.\n",
                         avg);
        }
        POS_ADVANCE(w, pos, STYLE_BUF_CAP);
    }

    /* Anti-AI warnings: use persona anti_patterns when available; otherwise fallback. */
    if (persona && persona->anti_patterns && persona->anti_patterns_count > 0) {
        w = snprintf(buf + pos, STYLE_BUF_CAP - pos,
                     "\n--- Anti-patterns (NEVER do these in texts) ---\n");
        POS_ADVANCE(w, pos, STYLE_BUF_CAP);
        for (size_t i = 0; i < persona->anti_patterns_count && pos < STYLE_BUF_CAP; i++) {
            if (persona->anti_patterns[i]) {
                w = snprintf(buf + pos, STYLE_BUF_CAP - pos, "- %s\n", persona->anti_patterns[i]);
                POS_ADVANCE(w, pos, STYLE_BUF_CAP);
            }
        }
    } else {
        w = snprintf(
            buf + pos, STYLE_BUF_CAP - pos,
            "\n--- Anti-patterns (NEVER do these in texts) ---\n"
            "- Never use semicolons or em-dashes in texts\n"
            "- Never use \"certainly\", \"absolutely\", \"I'd be happy to\", "
            "\"let me know if\", \"feel free\"\n"
            "- Never start with their name (siblings don't address each other by name)\n"
            "- Never use perfect grammar if they don't\n"
            "- Never write more than 2x their average message length\n"
            "- Never use numbered lists or bullet points\n"
            "- Never use \"!\" on every message — save it for when you mean it\n"
            "- Never give unsolicited advice unless they explicitly ask\n"
            "- It's ok to be blunt, sarcastic, or tease — that's how siblings talk\n"
            "- It's ok to just say \"lol\" or \"yeah\" — not everything needs a real response\n");
        POS_ADVANCE(w, pos, STYLE_BUF_CAP);
    }

    buf[pos] = '\0';
    *out_len = pos;
    return buf;
}

/* ── Typing quirk post-processing ─────────────────────────────────────── */

static bool quirk_enabled(const char *const *quirks, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (quirks[i] && strcmp(quirks[i], name) == 0)
            return true;
    }
    return false;
}

size_t hu_conversation_apply_typing_quirks(char *buf, size_t len, const char *const *quirks,
                                           size_t quirks_count) {
    if (!buf || len == 0 || !quirks || quirks_count == 0)
        return len;

    bool do_lowercase = quirk_enabled(quirks, quirks_count, "lowercase");
    bool do_no_periods = quirk_enabled(quirks, quirks_count, "no_periods");
    bool do_no_commas = quirk_enabled(quirks, quirks_count, "no_commas");
    bool do_no_apostrophes = quirk_enabled(quirks, quirks_count, "no_apostrophes");
    bool do_double_space_to_newline =
        quirk_enabled(quirks, quirks_count, "double_space_to_newline");
    bool do_variable_punctuation = quirk_enabled(quirks, quirks_count, "variable_punctuation");

    if (do_lowercase) {
        for (size_t i = 0; i < len; i++) {
            if (buf[i] >= 'A' && buf[i] <= 'Z')
                buf[i] += 32;
        }
    }

    if (do_no_periods || do_no_commas || do_no_apostrophes) {
        size_t out = 0;
        for (size_t i = 0; i < len; i++) {
            bool strip = false;
            if (do_no_periods && buf[i] == '.') {
                bool is_end = (i + 1 == len) || (buf[i + 1] == ' ' && i + 2 < len &&
                                                 buf[i + 2] >= 'A' && buf[i + 2] <= 'z');
                bool in_ellipsis = (i + 2 < len && buf[i + 1] == '.' && buf[i + 2] == '.') ||
                                   (i > 0 && buf[i - 1] == '.');
                if (is_end && !in_ellipsis)
                    strip = true;
            }
            if (do_no_commas && buf[i] == ',')
                strip = true;
            if (do_no_apostrophes && buf[i] == '\'')
                strip = true;
            if (!strip)
                buf[out++] = buf[i];
        }
        buf[out] = '\0';
        len = out;
    }

    if (do_double_space_to_newline) {
        size_t out = 0;
        for (size_t i = 0; i < len; i++) {
            if (i + 1 < len && buf[i] == ' ' && buf[i + 1] == ' ') {
                buf[out++] = '\n';
                i++; /* skip second space */
            } else {
                buf[out++] = buf[i];
            }
        }
        buf[out] = '\0';
        len = out;
    }

    /* Variable punctuation pass: randomly drop/modify sentence-ending periods */
    if (do_variable_punctuation) {
        uint32_t vp_seed = (uint32_t)len;
        for (size_t i = 0; i < len; i++) {
            if (buf[i] == '.' &&
                (i + 1 >= len || buf[i + 1] == ' ' || buf[i + 1] == '\n' || buf[i + 1] == '\0')) {
                vp_seed = vp_seed * 1103515245u + 12345u;
                uint32_t r = (vp_seed >> 16) & 0x7fff;
                if (r % 100 < 40) {
                    /* Drop the period entirely */
                    memmove(buf + i, buf + i + 1, len - i);
                    len--;
                    i--;
                }
            }
        }
        buf[len] = '\0';
    }

    /* Strip trailing whitespace that may result from punctuation removal */
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\n')) {
        len--;
        buf[len] = '\0';
    }

    return len;
}

/* ── Typo simulation post-processor ──────────────────────────────────── */

/* QWERTY adjacency: for each lowercase letter, neighbors on keyboard.
 * Used for adjacent-key-swap typo. Index by (c - 'a'). */
static const char *const QWERTY_NEIGHBORS[] = {
    "sqwz",   /* a */
    "vghn",   /* b */
    "xdfv",   /* c */
    "serfcx", /* d */
    "wrds",   /* e */
    "drtgvc", /* f */
    "ftyhbv", /* g */
    "gyujnb", /* h */
    "uokj",   /* i */
    "huikmn", /* j */
    "jolm",   /* k */
    "kop",    /* l */
    "njk",    /* m */
    "bhjm",   /* n */
    "iplk",   /* o */
    "ol",     /* p */
    "wa",     /* q */
    "etfd",   /* r */
    "awedxz", /* s */
    "ryfg",   /* t */
    "yihj",   /* u */
    "cfgxb",  /* v */
    "qeas",   /* w */
    "zsdc",   /* x */
    "tugh",   /* y */
    "asx",    /* z */
};

static uint32_t prng_next(uint32_t *seed) {
    *seed = *seed * 1103515245u + 12345u;
    return (*seed >> 16u) & 0x7fffu;
}

static bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Typo-injector activation gate (off | shadow | live). Default OFF: injecting
 * character corruptions into clean output is an unmeasured send-mutator, so it
 * must not shape real output until a measurement promotes it
 * (~/.claude/rules/feature-gate-requires-measurement.md). */
#if defined(HU_IS_TEST) && HU_IS_TEST
static int s_typos_mode_override = -1; /* -1 = defer to env HU_TYPOS */
void hu_conversation_typos_set_mode_for_test(int mode) {
    s_typos_mode_override = mode;
}
#endif

hu_gate_mode_t hu_conversation_typos_mode(void) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    if (s_typos_mode_override >= 0)
        return (hu_gate_mode_t)s_typos_mode_override;
#endif
    return hu_gate_mode_from_env("HU_TYPOS", HU_GATE_OFF);
}

size_t hu_conversation_apply_typos(char *buf, size_t len, size_t cap, uint32_t seed) {
    if (!buf || len == 0)
        return len;
    if (len >= cap)
        return len;

    /* Gated OFF by default: injecting character corruptions into clean output
     * is an unmeasured send-mutator, so only LIVE mutates. SHADOW currently
     * emits nothing (no would-corrupt logging wired yet). */
    if (hu_conversation_typos_mode() != HU_GATE_LIVE)
        return len;

    uint32_t s = seed;
    uint32_t val = prng_next(&s);
    if (val % 100u >= 15u)
        return len;

    /* Collect eligible (word_start, word_len) where word_len >= 3 and
     * we have at least one middle position (indices 1..len-2). */
    typedef struct {
        size_t start;
        size_t word_len;
    } word_t;
    word_t words[64];
    size_t word_count = 0;

    size_t i = 0;
    while (i < len && word_count < 64) {
        while (i < len && !is_word_char(buf[i]))
            i++;
        if (i >= len)
            break;
        size_t start = i;
        while (i < len && is_word_char(buf[i]))
            i++;
        size_t word_len = i - start;
        if (word_len >= 3 && word_len > 2) {
            words[word_count].start = start;
            words[word_count].word_len = word_len;
            word_count++;
        }
    }

    if (word_count == 0)
        return len;

    val = prng_next(&s);
    size_t word_idx = (size_t)(val % (uint32_t)word_count);
    size_t wstart = words[word_idx].start;
    size_t wlen = words[word_idx].word_len;
    size_t middle_count = wlen - 2;
    if (middle_count == 0)
        return len;

    val = prng_next(&s);
    size_t pos_in_word = 1u + (size_t)(val % (uint32_t)middle_count);
    size_t abs_pos = wstart + pos_in_word;

    val = prng_next(&s);
    uint32_t typo_type = val % 100u;

    char c = buf[abs_pos];
    char c_lower = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    if (c_lower < 'a' || c_lower > 'z')
        return len;

    if (typo_type < 40u) {
        /* Adjacent key swap */
        const char *neighbors = QWERTY_NEIGHBORS[(unsigned)(c_lower - 'a')];
        size_t ncount = 0;
        while (neighbors[ncount] != '\0')
            ncount++;
        if (ncount == 0)
            return len;
        val = prng_next(&s);
        char repl = neighbors[val % (uint32_t)ncount];
        if (c >= 'A' && c <= 'Z')
            repl = (char)(repl - 32);
        buf[abs_pos] = repl;
    } else if (typo_type < 70u) {
        /* Letter transposition: swap with next char (if letter) */
        if (abs_pos + 1 >= len || !is_word_char(buf[abs_pos + 1]))
            return len;
        char tmp = buf[abs_pos];
        buf[abs_pos] = buf[abs_pos + 1];
        buf[abs_pos + 1] = tmp;
    } else if (typo_type < 90u) {
        /* Dropped letter */
        memmove(buf + abs_pos, buf + abs_pos + 1, len - abs_pos - 1);
        buf[len - 1] = '\0';
        return len - 1;
    } else {
        /* Double letter: requires cap > len + 1 (len+1 chars + null) */
        if (cap <= len + 1)
            return len;
        memmove(buf + abs_pos + 1, buf + abs_pos, len - abs_pos);
        buf[abs_pos + 1] = buf[abs_pos];
        buf[len + 1] = '\0';
        return len + 1;
    }
    return len;
}

/* ── Two-phase "let me think" thinking response classifier ─────────────── */

bool hu_conversation_classify_thinking(const hu_thinking_context_t *ctx, const char *msg,
                                       size_t msg_len, const hu_channel_history_entry_t *entries,
                                       size_t entry_count, hu_thinking_response_t *out) {
    (void)entries;
    (void)entry_count;
    if (!ctx || !msg || msg_len == 0 || !out)
        return false;
    memset(out, 0, sizeof(*out));

    /* Channel class shapes the delay AFTER the filler, not whether to use one.
     * Earlier versions excluded TEXT_FAST channels (iMessage/SMS) entirely
     * because they reused the TEXT_ASYNC 30–60 s delay, which feels broken on
     * iMessage. Real human texting cadence sends "hm" instantly and follows
     * with the substantive reply within 1–2 s. We now keep the filler bubble
     * on TEXT_FAST and use a 400–1500 ms delay so the pause reads as natural
     * thinking rather than dispreference. See the delay formula below. */
    hu_channel_class_t cls = hu_channel_class_for_name(ctx->channel_name);

    /* Persona and filler bank are required */
    if (!ctx->persona || !ctx->channel_name)
        return false;
    const hu_persona_overlay_t *ov =
        hu_persona_find_overlay(ctx->persona, ctx->channel_name, strlen(ctx->channel_name));
    if (!ov || ov->filler_bank_count == 0)
        return false;

    /* Trigger heuristic — preserved from original */
    bool has_question = false;
    size_t words = 0;
    for (size_t i = 0; i < msg_len; i++) {
        if (msg[i] == '?')
            has_question = true;
        if (msg[i] == ' ' || msg[i] == '\n')
            words++;
    }
    if (msg_len > 0)
        words++;
    bool triggered = false;
    if (has_question && msg_len > 35 && words > 6)
        triggered = true;
    if (!triggered && msg_len > 80 && words > 10)
        triggered = true;
    if (!triggered)
        return false;

    /* Select index, excluding recency_last when bank has >=2 entries */
    uint32_t state = ctx->seed * 1103515245u + 12345u;
    uint32_t n = (uint32_t)ov->filler_bank_count;
    int32_t last =
        ctx->recency ? hu_filler_recency_last(ctx->recency, ctx->chat_id, ctx->chat_id_len) : -1;
    uint32_t idx;
    if (n >= 2 && last >= 0 && (uint32_t)last < n) {
        /* pick from n-1 alternatives, skipping the excluded slot */
        uint32_t pick = (state >> 16) % (n - 1);
        idx = (pick >= (uint32_t)last) ? pick + 1 : pick;
    } else {
        idx = (state >> 16) % n;
    }

    if (ctx->recency) {
        hu_filler_recency_record(ctx->recency, ctx->chat_id, ctx->chat_id_len, (uint16_t)idx);
    }

    const char *filler = ov->filler_bank[idx];
    if (!filler)
        return false;
    size_t flen = strlen(filler);
    if (flen >= sizeof(out->filler))
        flen = sizeof(out->filler) - 1;
    memcpy(out->filler, filler, flen);
    out->filler[flen] = '\0';
    out->filler_len = flen;

    /* Channel-aware delay between the filler bubble and the substantive reply. */
    if (cls == HU_CHANNEL_CLASS_VOICE) {
#ifdef HU_ENABLE_VOICE_VAD_TIMING
        out->delay_ms = 200 + ((state >> 8) % 301); /* 200-500ms uniform */
#else
        out->delay_ms = 0; /* voice channel without VAD timing: no delay */
#endif
    } else if (cls == HU_CHANNEL_CLASS_TEXT_FAST) {
        /* iMessage / SMS: 400-1500 ms uniform. Matches the real-human cadence
         * of sending "hm" and following with the substantive reply within a
         * second or two. Long enough to feel like genuine thought; short
         * enough that the recipient doesn't read it as disengagement. */
        out->delay_ms = 400 + ((state >> 8) % 1101);
    } else {
        /* TEXT_ASYNC / UNKNOWN: existing dynamic formula */
        uint32_t base = 30000, extra = 0;
        if (msg_len > 150)
            extra = 30000;
        else if (msg_len > 100)
            extra = 15000;
        else if (msg_len > 80)
            extra = 10000;
        out->delay_ms = base + extra;
        if (out->delay_ms > 60000)
            out->delay_ms = 60000;
    }
    return true;
}

/* ── Enhanced response action classification (message-property driven) ─── */

static size_t count_words(const char *msg, size_t msg_len) {
    size_t n = 0;
    bool in_word = false;
    for (size_t i = 0; i < msg_len; i++) {
        char c = msg[i];
        bool w = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 (c == '\'');
        if (w && !in_word) {
            in_word = true;
            n++;
        } else if (!w) {
            in_word = false;
        }
    }
    return n;
}

/* Tapbacks are system-generated (channel protocol), not natural language — minimal set. */
static bool is_tapback_reaction(const char *norm, size_t ni) {
    if (ni < 4)
        return false;
    if (ni <= 12 &&
        ((ni == 5 && memcmp(norm, "liked", 5) == 0) || (ni == 5 && memcmp(norm, "loved", 5) == 0) ||
         (ni == 7 && memcmp(norm, "laughed", 7) == 0) ||
         (ni == 10 && memcmp(norm, "emphasized", 10) == 0) ||
         (ni == 8 && memcmp(norm, "disliked", 8) == 0) ||
         (ni == 9 && memcmp(norm, "questioned", 9) == 0)))
        return true;
    /* "Loved an image", "Liked a message" etc. */
    if (ni >= 7 && (memcmp(norm, "lovedan", 7) == 0 || memcmp(norm, "likedan", 7) == 0))
        return true;
    if (ni >= 9 && memcmp(norm, "laughedat", 9) == 0)
        return true;
    return false;
}

hu_response_action_t hu_conversation_classify_response(const char *msg, size_t msg_len,
                                                       const hu_channel_history_entry_t *entries,
                                                       size_t entry_count,
                                                       uint32_t *delay_extra_ms) {
    if (!delay_extra_ms)
        return HU_RESPONSE_FULL;
    *delay_extra_ms = 0;

    if (!msg || msg_len == 0)
        return HU_RESPONSE_SKIP;

    /* Normalize for comparison */
    char norm[128];
    size_t ni = 0;
    for (size_t i = 0; i < msg_len && ni < sizeof(norm) - 1; i++) {
        char c = msg[i];
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (c != ' ' && c != '\n' && c != '\r')
            norm[ni++] = c;
    }
    norm[ni] = '\0';

    size_t word_count = count_words(msg, msg_len);
    bool has_question = (memchr(msg, '?', msg_len) != NULL);

    /* Skip: tapbacks (system vocabulary) */
    if (is_tapback_reaction(norm, ni))
        return HU_RESPONSE_SKIP;

    /* Greetings: always respond even if short */
    if (ni >= 2 && (memcmp(norm, "hi", 2) == 0 || (ni >= 3 && memcmp(norm, "hey", 3) == 0) ||
                    memcmp(norm, "yo", 2) == 0 || (ni >= 3 && memcmp(norm, "sup", 3) == 0) ||
                    (ni >= 5 && memcmp(norm, "hello", 5) == 0) ||
                    (ni >= 5 && memcmp(norm, "howdy", 5) == 0))) {
        *delay_extra_ms = 2000;
        return HU_RESPONSE_BRIEF;
    }

    /* Skip: very short non-greeting (single char, emoji reactions) */
    if (msg_len <= 1)
        return HU_RESPONSE_SKIP;

    /* ok/k/okay: skip unless answering our question */
    if ((ni == 1 && norm[0] == 'k') || (ni == 2 && memcmp(norm, "ok", 2) == 0) ||
        (ni == 4 && memcmp(norm, "okay", 4) == 0)) {
        if (entries && entry_count > 0) {
            size_t checked = 0;
            for (size_t i = entry_count; i > 0 && checked < 3; i--) {
                if (entries[i - 1].from_me) {
                    checked++;
                    const char *t = entries[i - 1].text;
                    size_t tl = strlen(t);
                    for (size_t j = 0; j < tl; j++) {
                        if (t[j] == '?')
                            return HU_RESPONSE_SKIP;
                    }
                }
            }
        }
        return HU_RESPONSE_SKIP;
    }

    /* ── Keep Silent: conversation fade-out detection ────────────────── */

    /* Mutual farewell = SKIP: if our last message was a farewell and they
     * reply with another farewell, don't have the last word */
    if (entries && entry_count > 0) {
        bool incoming_is_farewell =
            hu_str_contains_ci_cstr(msg, msg_len, "goodnight") ||
            hu_str_contains_ci_cstr(msg, msg_len, "good night") ||
            hu_str_contains_ci_cstr(msg, msg_len, "gotta go") ||
            hu_str_contains_ci_cstr(msg, msg_len, "ttyl") ||
            hu_str_contains_ci_cstr(msg, msg_len, "heading out") ||
            hu_str_contains_ci_cstr(msg, msg_len, "peace out") ||
            hu_str_contains_ci_cstr(msg, msg_len, "see ya") ||
            hu_str_contains_ci_cstr(msg, msg_len, "catch you later") ||
            hu_str_contains_ci_cstr(msg, msg_len, "i'm out") ||
            (msg_len <= 10 && (hu_str_contains_ci_cstr(msg, msg_len, "bye") ||
                               hu_str_contains_ci_cstr(msg, msg_len, "night") ||
                               hu_str_contains_ci_cstr(msg, msg_len, "later") ||
                               hu_str_contains_ci_cstr(msg, msg_len, "gn") ||
                               hu_str_contains_ci_cstr(msg, msg_len, "cya")));

        if (incoming_is_farewell) {
            /* Check if our last message was also a farewell */
            for (size_t i = entry_count; i > 0; i--) {
                if (entries[i - 1].from_me) {
                    const char *t = entries[i - 1].text;
                    size_t tl = strlen(t);
                    bool our_was_farewell = hu_str_contains_ci_cstr(t, tl, "night") ||
                                            hu_str_contains_ci_cstr(t, tl, "bye") ||
                                            hu_str_contains_ci_cstr(t, tl, "later") ||
                                            hu_str_contains_ci_cstr(t, tl, "ttyl") ||
                                            hu_str_contains_ci_cstr(t, tl, "see ya") ||
                                            hu_str_contains_ci_cstr(t, tl, "cya") ||
                                            hu_str_contains_ci_cstr(t, tl, "gn") ||
                                            hu_str_contains_ci_cstr(t, tl, "peace") ||
                                            hu_str_contains_ci_cstr(t, tl, "take care");
                    if (our_was_farewell)
                        return HU_RESPONSE_SKIP;
                    break;
                }
            }
            /* Not mutual — respond briefly */
            *delay_extra_ms = 1500;
            return HU_RESPONSE_BRIEF;
        }

        /* Trailing off = SKIP: if last 2-3 exchanges are all brief acks,
         * the conversation is fading — don't prolong it */
        if (entry_count >= 3 && msg_len < 15 && word_count <= 2 && !has_question) {
            size_t brief_streak = 0;
            for (size_t i = entry_count; i > 0 && brief_streak < 4; i--) {
                const char *t = entries[i - 1].text;
                size_t tl = strlen(t);
                size_t twc = count_words(t, tl);
                if (tl < 15 && twc <= 2)
                    brief_streak++;
                else
                    break;
            }
            if (brief_streak >= 2)
                return HU_RESPONSE_SKIP;
        }

        /* Last-word avoidance: if our last message was a statement (not a question)
         * and they reply with a minimal acknowledgment, don't pile on */
        if (msg_len < 15 && word_count <= 2 && !has_question) {
            for (size_t i = entry_count; i > 0; i--) {
                if (entries[i - 1].from_me) {
                    const char *t = entries[i - 1].text;
                    size_t tl = strlen(t);
                    bool our_was_question = false;
                    for (size_t j = 0; j < tl; j++) {
                        if (t[j] == '?') {
                            our_was_question = true;
                            break;
                        }
                    }
                    if (!our_was_question)
                        return HU_RESPONSE_SKIP;
                    break;
                }
            }
        }
    } else {
        /* No history — use original farewell logic */
        if (hu_str_contains_ci_cstr(msg, msg_len, "goodnight") ||
            hu_str_contains_ci_cstr(msg, msg_len, "good night") ||
            hu_str_contains_ci_cstr(msg, msg_len, "gotta go") ||
            hu_str_contains_ci_cstr(msg, msg_len, "ttyl") ||
            hu_str_contains_ci_cstr(msg, msg_len, "heading out") ||
            hu_str_contains_ci_cstr(msg, msg_len, "peace out") ||
            hu_str_contains_ci_cstr(msg, msg_len, "see ya") ||
            hu_str_contains_ci_cstr(msg, msg_len, "catch you later") ||
            hu_str_contains_ci_cstr(msg, msg_len, "i'm out")) {
            *delay_extra_ms = 1500;
            return HU_RESPONSE_BRIEF;
        }
        if (msg_len <= 10 && (hu_str_contains_ci_cstr(msg, msg_len, "bye") ||
                              hu_str_contains_ci_cstr(msg, msg_len, "night") ||
                              hu_str_contains_ci_cstr(msg, msg_len, "later") ||
                              hu_str_contains_ci_cstr(msg, msg_len, "gn") ||
                              hu_str_contains_ci_cstr(msg, msg_len, "cya"))) {
            *delay_extra_ms = 1500;
            return HU_RESPONSE_BRIEF;
        }
    }

    /* Brief: short acknowledgment by properties (length, word count, no question) */
    if (msg_len < 15 && word_count <= 2 && !has_question)
        return HU_RESPONSE_BRIEF;

    /* Bad news: extended pause — show you're absorbing it */
    if (hu_str_contains_ci_cstr(msg, msg_len, "passed away") ||
        hu_str_contains_ci_cstr(msg, msg_len, "got fired") ||
        hu_str_contains_ci_cstr(msg, msg_len, "broke up") ||
        hu_str_contains_ci_cstr(msg, msg_len, "bad news") ||
        hu_str_contains_ci_cstr(msg, msg_len, "didn't make it") ||
        hu_str_contains_ci_cstr(msg, msg_len, "got rejected") ||
        hu_str_contains_ci_cstr(msg, msg_len, "lost my")) {
        *delay_extra_ms = 12000;
        return HU_RESPONSE_DELAY;
    }

    /* Good news: short pause then celebrate */
    if (hu_str_contains_ci_cstr(msg, msg_len, "got the job") ||
        hu_str_contains_ci_cstr(msg, msg_len, "got in") ||
        hu_str_contains_ci_cstr(msg, msg_len, "i passed") ||
        hu_str_contains_ci_cstr(msg, msg_len, "got engaged") ||
        hu_str_contains_ci_cstr(msg, msg_len, "got promoted") ||
        hu_str_contains_ci_cstr(msg, msg_len, "good news") ||
        hu_str_contains_ci_cstr(msg, msg_len, "i did it") ||
        hu_str_contains_ci_cstr(msg, msg_len, "we did it")) {
        *delay_extra_ms = 3000;
        return HU_RESPONSE_DELAY;
    }

    /* Vulnerability: deliberate pause */
    if (hu_str_contains_ci_cstr(msg, msg_len, "i need to tell you") ||
        hu_str_contains_ci_cstr(msg, msg_len, "can i be honest") ||
        hu_str_contains_ci_cstr(msg, msg_len, "don't judge me") ||
        hu_str_contains_ci_cstr(msg, msg_len, "this is hard to say") ||
        hu_str_contains_ci_cstr(msg, msg_len, "i never told")) {
        *delay_extra_ms = 8000;
        return HU_RESPONSE_DELAY;
    }

    /* Emotional/heavy messages: full response but delayed (showing you're thinking) */
    static const char *DEFAULT_EMOTIONAL[] = {
        "miss",       "love",    "hurt",    "stress",   "depress",   "lonely",
        "scared",     "worried", "sorry",   "afraid",   "giving up", "feel like",
        "don't know", "can't",   "help me", "need you", "cry",       "sad",
    };
    static const size_t DEFAULT_EMOTIONAL_LEN =
        sizeof(DEFAULT_EMOTIONAL) / sizeof(DEFAULT_EMOTIONAL[0]);

    const char **emotional = s_emotional_words_len > 0 ? (const char **)s_emotional_words
                                                       : (const char **)DEFAULT_EMOTIONAL;
    size_t emotional_len =
        s_emotional_words_len > 0 ? s_emotional_words_len : DEFAULT_EMOTIONAL_LEN;

    for (size_t i = 0; i < emotional_len; i++) {
        if (!emotional[i])
            continue;
        size_t elen = strlen(emotional[i]);
        for (size_t j = 0; j + elen <= msg_len; j++) {
            bool match = true;
            for (size_t k = 0; k < elen; k++) {
                char a = msg[j + k];
                if (a >= 'A' && a <= 'Z')
                    a += 32;
                if (a != emotional[i][k]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                *delay_extra_ms = 8000;
                return HU_RESPONSE_DELAY;
            }
        }
    }

    /* Statement without question: consider if the conversation is winding down.
     * If their last 3 messages were getting shorter and this one has no question,
     * they may not expect a response. Respond BRIEF. */
    if (!has_question && entries && entry_count >= 3) {
        size_t their_recent = 0;
        size_t getting_shorter = 0;
        size_t prev_len = 999;
        for (size_t i = entry_count; i > 0 && their_recent < 3; i--) {
            if (!entries[i - 1].from_me) {
                size_t tl = strlen(entries[i - 1].text);
                if (tl < prev_len && prev_len != 999)
                    getting_shorter++;
                prev_len = tl;
                their_recent++;
            }
        }
        if (getting_shorter >= 2 && msg_len < 30) {
            return HU_RESPONSE_BRIEF;
        }
    }

    /* Question: normal response, moderate thinking delay */
    if (has_question) {
        *delay_extra_ms = 2000;
        return HU_RESPONSE_FULL;
    }

    /* Consecutive response limit: if we have responded to the last 3+
     * messages in a row with no interleaving real-user messages, the
     * conversation is running away — skip to let the real user step in. */
    if (entries && entry_count >= 3) {
        size_t consecutive_ours = 0;
        for (size_t i = entry_count; i > 0; i--) {
            if (entries[i - 1].from_me)
                consecutive_ours++;
            else
                break;
        }
        if (consecutive_ours >= 3)
            return HU_RESPONSE_SKIP;
    }

    /* Narrative statement: no question, not short, not emotional — the sender
     * is sharing something but not necessarily expecting a reply.  Respond
     * briefly rather than generating a full AI-essay response. */
    if (msg_len > 20 && word_count >= 4)
        return HU_RESPONSE_BRIEF;

    return HU_RESPONSE_FULL;
}

/* ── Active listening backchannels (F29) ───────────────────────────────── */

static bool has_negative_sentiment(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return false;
    static const char *neg[] = {"frustrated",  "angry",        "ugh",          "hate",
                                "not working", "doesn't work", "annoyed",      "upset",
                                "stressed",    "awful",        "terrible",     "disappointed",
                                "broken",      "failing",      "can't believe"};
    for (size_t i = 0; i < sizeof(neg) / sizeof(neg[0]); i++) {
        if (hu_str_contains_ci_cstr(msg, msg_len, neg[i]))
            return true;
    }
    return false;
}

static bool is_narrative_or_venting(const char *msg, size_t msg_len,
                                    const hu_channel_history_entry_t *entries, size_t count) {
    if (!msg || msg_len < 80)
        return false;
    if (has_negative_sentiment(msg, msg_len))
        return false;
    if (memchr(msg, '?', msg_len) != NULL)
        return false;
    bool has_narrative_phrase = hu_str_contains_ci_cstr(msg, msg_len, "and then") ||
                                hu_str_contains_ci_cstr(msg, msg_len, "so i") ||
                                hu_str_contains_ci_cstr(msg, msg_len, "anyway") ||
                                hu_str_contains_ci_cstr(msg, msg_len, "long story");
    bool has_first_person = hu_str_contains_ci_cstr(msg, msg_len, "i ") ||
                            hu_str_contains_ci_cstr(msg, msg_len, "my ") ||
                            hu_str_contains_ci_cstr(msg, msg_len, "me ");
    if (has_narrative_phrase || has_first_person)
        return true;
    /* Last 2–3 their messages are long and we haven't replied yet (they're in flow) */
    if (entries && count >= 2) {
        size_t their_long = 0;
        bool saw_ours = false;
        for (size_t i = count; i > 0 && their_long < 3; i--) {
            const hu_channel_history_entry_t *e = &entries[i - 1];
            size_t tl = strlen(e->text);
            if (e->from_me) {
                saw_ours = true;
                break;
            }
            if (tl >= 60)
                their_long++;
            else
                break;
        }
        if (their_long >= 2 && !saw_ours)
            return true;
    }
    return false;
}

bool hu_conversation_should_backchannel(const char *msg, size_t msg_len,
                                        const hu_channel_history_entry_t *entries, size_t count,
                                        uint32_t seed, float probability) {
    if (!msg || msg_len == 0)
        return false;
    if (!is_narrative_or_venting(msg, msg_len, entries, count))
        return false;
    if (probability <= 0.0f)
        return false;
    if (probability >= 1.0f)
        return true;
    uint32_t roll = (seed % 100u);
    return roll < (uint32_t)(probability * 100.0f);
}

static const char *const DEFAULT_BACKCHANNEL_PHRASES[] = {
    "yeah", "totally", "right", "100%", "for real", "mmhm", "mhm", "oh wow", "damn",
};
#define DEFAULT_BACKCHANNEL_COUNT \
    (sizeof(DEFAULT_BACKCHANNEL_PHRASES) / sizeof(DEFAULT_BACKCHANNEL_PHRASES[0]))

size_t hu_conversation_pick_backchannel(uint32_t seed, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    const char *const *phrases =
        s_backchannel_phrases_len > 0
            ? (const char *const *)s_backchannel_phrases
            : (const char *const *)(const char **)DEFAULT_BACKCHANNEL_PHRASES;
    size_t phrase_count =
        s_backchannel_phrases_len > 0 ? s_backchannel_phrases_len : DEFAULT_BACKCHANNEL_COUNT;

    if (phrase_count == 0)
        return 0;
    size_t idx = (size_t)(seed % (uint32_t)phrase_count);
    const char *phrase = phrases[idx];
    if (!phrase)
        phrase = phrases[0];
    size_t len = strlen(phrase);
    if (len >= cap)
        len = cap - 1;
    memcpy(buf, phrase, len);
    buf[len] = '\0';
    return len;
}

/* ── Burst messaging (F45) ────────────────────────────────────────────── */

static bool has_urgency_keywords(const char *msg, size_t msg_len) {
    return hu_str_contains_ci_cstr(msg, msg_len, "omg") ||
           hu_str_contains_ci_cstr(msg, msg_len, "oh my god") ||
           hu_str_contains_ci_cstr(msg, msg_len, "just saw") ||
           hu_str_contains_ci_cstr(msg, msg_len, "did you see") ||
           hu_str_contains_ci_cstr(msg, msg_len, "holy shit") ||
           hu_str_contains_ci_cstr(msg, msg_len, "emergency") ||
           hu_str_contains_ci_cstr(msg, msg_len, "are you okay") ||
           count_exclamations(msg, msg_len) >= 3;
}

bool hu_conversation_should_burst(const char *msg, size_t msg_len,
                                  const hu_channel_history_entry_t *entries, size_t count,
                                  uint32_t seed, float probability) {
    if (!msg || msg_len == 0)
        return false;
    if (!has_urgency_keywords(msg, msg_len))
        return false;
    hu_energy_level_t energy = hu_conversation_detect_energy(msg, msg_len, entries, count);
    if (energy != HU_ENERGY_EXCITED && energy != HU_ENERGY_ANXIOUS)
        return false;
    if (probability <= 0.0f)
        return false;
    if (probability >= 1.0f)
        return true;
    uint32_t roll = seed % 100u;
    return roll < (uint32_t)(probability * 100.0f);
}

size_t hu_conversation_build_burst_prompt(char *buf, size_t cap) {
    static const char BURST_DIRECTIVE[] =
        "[BURST MODE: Generate 3-4 SHORT independent messages as separate thoughts. "
        "Output ONLY a JSON array: [\"msg1\", \"msg2\", \"msg3\"]. "
        "Each 2-10 words. Rapid-fire thoughts, not one split message.]";
    size_t len = sizeof(BURST_DIRECTIVE) - 1;
    if (!buf || cap == 0)
        return 0;
    if (len >= cap)
        len = cap - 1;
    memcpy(buf, BURST_DIRECTIVE, len);
    buf[len] = '\0';
    return len;
}

int hu_conversation_parse_burst_response(const char *response, size_t resp_len,
                                         char messages[][256], size_t max_messages) {
    if (!response || !messages || max_messages == 0)
        return 0;
    const char *p = response;
    const char *end = response + resp_len;
    while (p < end && *p != '[')
        p++;
    if (p >= end)
        return 0;
    p++; /* skip '[' */
    int count = 0;
    while (p < end && count < (int)max_messages) {
        while (p < end && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r'))
            p++;
        if (p >= end || *p != '"')
            break;
        p++; /* skip opening quote */
        const char *start = p;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end)
                p++;
            p++;
        }
        if (p >= end)
            break;
        size_t len = (size_t)(p - start);
        if (len >= 256)
            len = 255;
        memcpy(messages[count], start, len);
        messages[count][len] = '\0';
        count++;
        p++; /* skip closing quote */
    }
    return count;
}

/* ── Natural conversation drop-off classifier (F11) ──────────────────── */

static bool is_farewell_text(const char *t, size_t tl) {
    return hu_str_contains_ci_cstr(t, tl, "night") ||
           hu_str_contains_ci_cstr(t, tl, "sleep well") || hu_str_contains_ci_cstr(t, tl, "bye") ||
           hu_str_contains_ci_cstr(t, tl, "goodnight") ||
           hu_str_contains_ci_cstr(t, tl, "good night") ||
           hu_str_contains_ci_cstr(t, tl, "later") || hu_str_contains_ci_cstr(t, tl, "ttyl") ||
           hu_str_contains_ci_cstr(t, tl, "see ya") || hu_str_contains_ci_cstr(t, tl, "cya") ||
           hu_str_contains_ci_cstr(t, tl, "gn") || hu_str_contains_ci_cstr(t, tl, "peace") ||
           hu_str_contains_ci_cstr(t, tl, "take care") ||
           hu_str_contains_ci_cstr(t, tl, "gotta go") ||
           hu_str_contains_ci_cstr(t, tl, "heading out");
}

static bool is_low_energy_ack(const char *t, size_t tl) {
    if (tl > 8)
        return false;
    char norm[16];
    size_t ni = 0;
    for (size_t i = 0; i < tl && ni < sizeof(norm) - 1; i++) {
        char c = t[i];
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (c != ' ' && c != '\n' && c != '\r')
            norm[ni++] = c;
    }
    norm[ni] = '\0';
    return (ni == 1 && norm[0] == 'k') || (ni == 2 && memcmp(norm, "ok", 2) == 0) ||
           (ni == 4 && memcmp(norm, "okay", 4) == 0) || (ni == 4 && memcmp(norm, "yeah", 4) == 0) ||
           (ni == 4 && memcmp(norm, "cool", 4) == 0) || (ni == 3 && memcmp(norm, "yep", 3) == 0) ||
           (ni == 4 && memcmp(norm, "nope", 4) == 0) || (ni == 2 && memcmp(norm, "ya", 2) == 0) ||
           (ni == 4 && memcmp(norm, "sure", 4) == 0) || (ni == 4 && memcmp(norm, "nice", 4) == 0);
}

static bool is_emoji_only(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return false;
    bool has_alpha = false;
    for (size_t i = 0; i < msg_len; i++) {
        unsigned char c = (unsigned char)msg[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            has_alpha = true;
    }
    return !has_alpha && msg_len > 0;
}

int hu_conversation_classify_dropoff(const char *message, size_t message_len,
                                     const hu_channel_history_entry_t *entries, size_t entry_count,
                                     uint32_t seed) {
    (void)seed; /* Caller uses seed for roll: (seed % 100) < prob → SKIP */
    if (!message || message_len == 0)
        return 0;

    if (!entries || entry_count == 0) {
        /* No history: only emoji-only applies */
        if (is_emoji_only(message, message_len))
            return 70;
        return 0;
    }

    /* Find our last message */
    const char *our_last = NULL;
    size_t our_last_len = 0;
    for (size_t i = entry_count; i > 0; i--) {
        if (entries[i - 1].from_me) {
            our_last = entries[i - 1].text;
            our_last_len = strlen(our_last);
            break;
        }
    }

    /* Our farewell, they gave minimal reply (k, ok, yeah) → 100% SKIP */
    if (our_last && our_last_len > 0 && is_farewell_text(our_last, our_last_len) &&
        is_low_energy_ack(message, message_len))
        return 100;

    /* Mutual farewell: both said goodbye → 90% SKIP */
    if (our_last && our_last_len > 0 && is_farewell_text(our_last, our_last_len) &&
        is_farewell_text(message, message_len))
        return 90;

    /* Emoji-only from them → 70% SKIP */
    if (is_emoji_only(message, message_len))
        return 70;

    /* Low-energy: yeah, cool, ok from them → 60% SKIP */
    if (is_low_energy_ack(message, message_len))
        return 60;

    return 0;
}

/* ── Leave-on-read classifier (F46) ────────────────────────────────────── */

bool hu_conversation_should_leave_on_read(const char *msg, size_t msg_len,
                                          const hu_channel_history_entry_t *entries, size_t count,
                                          uint32_t seed, uint8_t threshold_pct) {
    if (!msg || msg_len == 0)
        return false;

    /* Never for direct questions */
    if (memchr(msg, '?', msg_len) != NULL)
        return false;

    /* Never for emotional crisis */
    if (hu_str_contains_ci_cstr(msg, msg_len, "help me") ||
        hu_str_contains_ci_cstr(msg, msg_len, "i need you"))
        return false;

    /* Never for concerning emotion in recent history */
    if (entries && count > 0) {
        hu_emotional_state_t emo = hu_conversation_detect_emotion(entries, count);
        if (emo.concerning)
            return false;
    }

    /* Never for high emotional content in message itself */
    if (hu_str_contains_ci_cstr(msg, msg_len, "emergency") ||
        hu_str_contains_ci_cstr(msg, msg_len, "urgent"))
        return false;

    bool trigger = false;

    /* Disagreement */
    if (hu_str_contains_ci_cstr(msg, msg_len, "agree to disagree") ||
        hu_str_contains_ci_cstr(msg, msg_len, "whatever") ||
        hu_str_contains_ci_cstr(msg, msg_len, "fine.") ||
        hu_str_contains_ci_cstr(msg, msg_len, "i don't agree") ||
        hu_str_contains_ci_cstr(msg, msg_len, "you're wrong") ||
        hu_str_contains_ci_cstr(msg, msg_len, "youre wrong"))
        trigger = true;

    /* Space request */
    if (!trigger && (hu_str_contains_ci_cstr(msg, msg_len, "i need space") ||
                     hu_str_contains_ci_cstr(msg, msg_len, "give me a minute") ||
                     hu_str_contains_ci_cstr(msg, msg_len, "can we talk later") ||
                     hu_str_contains_ci_cstr(msg, msg_len, "leave me alone")))
        trigger = true;

    /* Low-content: ok, cool, sure, k (1-4 chars, no question) — exact match */
    if (!trigger && msg_len >= 1 && msg_len <= 4) {
        if (msg_len == 1 && (msg[0] == 'k' || msg[0] == 'K'))
            trigger = true;
        else if (msg_len == 2 && strncasecmp(msg, "ok", 2) == 0)
            trigger = true;
        else if (msg_len == 4 &&
                 (strncasecmp(msg, "cool", 4) == 0 || strncasecmp(msg, "sure", 4) == 0))
            trigger = true;
    }

    if (!trigger)
        return false;

    uint8_t pct = threshold_pct > 0 ? threshold_pct : 10;
    return (seed % 100u) < pct;
}

hu_leave_on_read_decision_t hu_leave_on_read_decide(bool is_group_chat,
                                                    bool already_in_active_period,
                                                    bool helper_says_should_leave) {
    if (is_group_chat)
        return HU_LOR_RESPOND;
    if (already_in_active_period)
        return HU_LOR_ALREADY_IN_PERIOD;
    if (helper_says_should_leave)
        return HU_LOR_TRIGGER_NEW;
    return HU_LOR_RESPOND;
}

/* ── URL extraction ──────────────────────────────────────────────────── */

/* Utility for future use. Not currently wired into production; link-sharing
 * logic uses hu_conversation_should_share_link with pattern matching instead. */
size_t hu_conversation_extract_urls(const char *text, size_t text_len, hu_url_extract_t *urls,
                                    size_t max_urls) {
    if (!text || !urls || max_urls == 0)
        return 0;

    size_t count = 0;
    const char *p = text;
    const char *end = text + text_len;

    while (p < end && count < max_urls) {
        const char *start = NULL;
        size_t url_len = 0;

        if (p + 8 <= end && strncmp(p, "https://", 8) == 0) {
            start = p;
            p += 8;
            while (p < end) {
                char c = *p;
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '>' || c == ')' ||
                    c == ']' || c == '"' || c == '\'')
                    break;
                p++;
            }
            url_len = (size_t)(p - start);
        } else if (p + 7 <= end && strncmp(p, "http://", 7) == 0) {
            start = p;
            p += 7;
            while (p < end) {
                char c = *p;
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '>' || c == ')' ||
                    c == ']' || c == '"' || c == '\'')
                    break;
                p++;
            }
            url_len = (size_t)(p - start);
        } else {
            p++;
        }

        if (start && url_len > 0 && url_len <= 2048) {
            urls[count].start = start;
            urls[count].len = url_len;
            count++;
        }
    }
    return count;
}

/* ── Link-sharing detection ───────────────────────────────────────────── */

static bool msg_contains_recommendation_pattern(const char *msg, size_t msg_len) {
    static const char *patterns[] = {
        "check this out", "have you seen", "you should try", "recommend",      "link",
        "article",        "look at this",  "here's a link",  "here is a link", NULL,
    };
    for (int i = 0; patterns[i]; i++) {
        if (hu_str_contains_ci_cstr(msg, msg_len, patterns[i]))
            return true;
    }
    return false;
}

bool hu_conversation_should_share_link(const char *msg, size_t msg_len,
                                       const hu_channel_history_entry_t *entries,
                                       size_t entry_count) {
    if (!msg || msg_len == 0)
        return false;

    if (msg_contains_recommendation_pattern(msg, msg_len))
        return true;

    /* Check if our previous response (last from_me) mentions wanting to share something */
    if (entries && entry_count > 0) {
        for (size_t i = entry_count; i > 0; i--) {
            if (entries[i - 1].from_me) {
                const char *prev = entries[i - 1].text;
                size_t prev_len = strlen(prev);
                if (hu_str_contains_ci_cstr(prev, prev_len, "share") ||
                    hu_str_contains_ci_cstr(prev, prev_len, "link") ||
                    hu_str_contains_ci_cstr(prev, prev_len, "check out") ||
                    hu_str_contains_ci_cstr(prev, prev_len, "here's") ||
                    hu_str_contains_ci_cstr(prev, prev_len, "here is"))
                    return true;
                break;
            }
        }
    }
    return false;
}

/* ── Attachment context for prompts ──────────────────────────────────── */

static bool is_attachment_placeholder(const char *text, size_t len) {
    if (!text || len < 10)
        return false;
    if (strstr(text, "[image or attachment]"))
        return true;
    if (strstr(text, "[Photo shared]"))
        return true;
    if (strstr(text, "[Video shared]"))
        return true;
    if (strstr(text, "[Audio message]"))
        return true;
    if (strstr(text, "[Attachment shared]"))
        return true;
    if (strstr(text, "[Document:"))
        return true;
    return false;
}

char *hu_conversation_attachment_context(hu_allocator_t *alloc,
                                         const hu_channel_history_entry_t *entries, size_t count,
                                         size_t *out_len) {
    if (!alloc || !out_len || !entries || count == 0)
        return NULL;
    *out_len = 0;

    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].from_me)
            continue;
        const char *t = entries[i].text;
        size_t tl = strlen(t);
        if (is_attachment_placeholder(t, tl)) {
            found = true;
            break;
        }
    }
    if (!found)
        return NULL;

    const char *ctx =
        "The user shared a photo/attachment. Acknowledge it naturally — "
        "\"love that!\", \"that looks great\", etc. Don't say \"I can see the image\" "
        "if you can't actually analyze it.";
    size_t ctx_len = strlen(ctx);
    char *result = hu_strndup(alloc, ctx, ctx_len);
    if (result)
        *out_len = ctx_len;
    return result;
}

/* ── Anti-repetition detection ────────────────────────────────────────── */

size_t hu_conversation_detect_repetition(const hu_channel_history_entry_t *entries, size_t count,
                                         char *buf, size_t cap) {
    if (!entries || count < 4 || !buf || cap < 64)
        return 0;

    /* Collect last N "from_me" messages */
    const char *my_msgs[8];
    size_t my_count = 0;
    for (size_t i = count; i > 0 && my_count < 8; i--) {
        if (entries[i - 1].from_me) {
            my_msgs[my_count++] = entries[i - 1].text;
        }
    }
    if (my_count < 3)
        return 0;

    size_t pos = 0;
    int w;
    bool found = false;

    /* Detect repeated openers (first word of each message) */
    char openers[8][16];
    for (size_t i = 0; i < my_count; i++) {
        size_t j = 0;
        const char *m = my_msgs[i];
        while (m[j] && m[j] != ' ' && j < 15)
            j++;
        if (j > 0 && j < 15) {
            for (size_t k = 0; k < j; k++) {
                char c = m[k];
                if (c >= 'A' && c <= 'Z')
                    c += 32;
                openers[i][k] = c;
            }
            openers[i][j] = '\0';
        } else {
            openers[i][0] = '\0';
        }
    }

    /* Check if same opener used 3+ times in last 5 messages */
    for (size_t i = 0; i < my_count && i < 5; i++) {
        if (openers[i][0] == '\0')
            continue;
        size_t matches = 0;
        for (size_t j = 0; j < my_count && j < 5; j++) {
            if (strcmp(openers[i], openers[j]) == 0)
                matches++;
        }
        if (matches >= 3) {
            if (!found) {
                w = snprintf(buf + pos, cap - pos, "\n--- Anti-repetition ---\n");
                POS_ADVANCE(w, pos, cap);
                found = true;
            }
            w = snprintf(buf + pos, cap - pos,
                         "WARNING: You've started %zu of your last messages with '%s'. "
                         "Vary your openers. Start differently this time.\n",
                         matches, openers[i]);
            POS_ADVANCE(w, pos, cap);
            break;
        }
    }

    /* Check if always ending with a question */
    size_t questions = 0;
    size_t check = my_count < 5 ? my_count : 5;
    for (size_t i = 0; i < check; i++) {
        const char *m = my_msgs[i];
        size_t ml = strlen(m);
        if (ml > 0 && m[ml - 1] == '?')
            questions++;
    }
    if (questions >= 3) {
        if (!found) {
            w = snprintf(buf + pos, cap - pos, "\n--- Anti-repetition ---\n");
            POS_ADVANCE(w, pos, cap);
            found = true;
        }
        w = snprintf(buf + pos, cap - pos,
                     "WARNING: You've ended %zu of your last %zu messages with a question. "
                     "Not every message needs a follow-up question. "
                     "Make a statement, react, or just let it sit.\n",
                     questions, check);
        POS_ADVANCE(w, pos, cap);
    }

    /* Check if always using "haha"/"lol" as filler */
    size_t laughs = 0;
    for (size_t i = 0; i < check; i++) {
        if (hu_str_contains_ci_cstr(my_msgs[i], strlen(my_msgs[i]), "haha") ||
            hu_str_contains_ci_cstr(my_msgs[i], strlen(my_msgs[i]), "lol"))
            laughs++;
    }
    if (laughs >= 3) {
        if (!found) {
            w = snprintf(buf + pos, cap - pos, "\n--- Anti-repetition ---\n");
            POS_ADVANCE(w, pos, cap);
            found = true;
        }
        w = snprintf(buf + pos, cap - pos,
                     "WARNING: You've used 'haha'/'lol' in %zu of your last %zu messages. "
                     "Drop the nervous laughter. Not everything needs softening.\n",
                     laughs, check);
        POS_ADVANCE(w, pos, cap);
    }

    if (found) {
        w = snprintf(buf + pos, cap - pos, "--- End anti-repetition ---\n");
        POS_ADVANCE(w, pos, cap);
    }

    return pos;
}

/* ── Relationship-tier calibration ────────────────────────────────────── */

size_t hu_conversation_calibrate_relationship(const char *relationship_stage,
                                              const char *warmth_level,
                                              const char *vulnerability_level, char *buf,
                                              size_t cap) {
    if (!buf || cap < 64)
        return 0;

    size_t pos = 0;
    int w = snprintf(buf, cap, "\n--- Relationship context ---\n");
    POS_ADVANCE(w, pos, cap);

    /* Relationship stage → engagement depth */
    if (relationship_stage) {
        if (hu_str_contains_ci_cstr(relationship_stage, strlen(relationship_stage), "close") ||
            hu_str_contains_ci_cstr(relationship_stage, strlen(relationship_stage), "best") ||
            hu_str_contains_ci_cstr(relationship_stage, strlen(relationship_stage), "partner") ||
            hu_str_contains_ci_cstr(relationship_stage, strlen(relationship_stage), "intimate")) {
            w = snprintf(buf + pos, cap - pos,
                         "RELATIONSHIP: Close. You can be fully yourself — inside jokes, "
                         "mild roasting, deep honesty, comfortable silence. "
                         "Don't over-explain or be overly polite.\n");
        } else if (hu_str_contains_ci_cstr(relationship_stage, strlen(relationship_stage),
                                           "friend")) {
            w = snprintf(buf + pos, cap - pos,
                         "RELATIONSHIP: Friend. Warm and genuine but with some boundaries. "
                         "Banter is fine. Personal topics ok if they bring them up first.\n");
        } else if (hu_str_contains_ci_cstr(relationship_stage, strlen(relationship_stage),
                                           "acquaint") ||
                   hu_str_contains_ci_cstr(relationship_stage, strlen(relationship_stage), "new")) {
            w = snprintf(buf + pos, cap - pos,
                         "RELATIONSHIP: Acquaintance/new. Keep it lighter. "
                         "Don't assume familiarity. Match their formality level. "
                         "No inside jokes. No unsolicited deep topics.\n");
        } else if (hu_str_contains_ci_cstr(relationship_stage, strlen(relationship_stage),
                                           "professional") ||
                   hu_str_contains_ci_cstr(relationship_stage, strlen(relationship_stage),
                                           "work")) {
            w = snprintf(buf + pos, cap - pos,
                         "RELATIONSHIP: Professional. Stay on topic. "
                         "Friendly but not casual. No slang, minimal emoji.\n");
        } else {
            w = snprintf(buf + pos, cap - pos, "RELATIONSHIP: %s. Calibrate formality to match.\n",
                         relationship_stage);
        }
        POS_ADVANCE(w, pos, cap);
    }

    /* Warmth level → emotional expression */
    if (warmth_level) {
        int ww = 0;
        if (hu_str_contains_ci_cstr(warmth_level, strlen(warmth_level), "high")) {
            ww = snprintf(buf + pos, cap - pos,
                          "WARMTH: High. Affectionate language is natural with this person.\n");
        } else if (hu_str_contains_ci_cstr(warmth_level, strlen(warmth_level), "low")) {
            ww = snprintf(buf + pos, cap - pos,
                          "WARMTH: Low. Keep it cool and understated. No gushing.\n");
        }
        POS_ADVANCE(ww, pos, cap);
    }

    /* Vulnerability level → depth permission */
    if (vulnerability_level) {
        int vw = 0;
        if (hu_str_contains_ci_cstr(vulnerability_level, strlen(vulnerability_level), "high") ||
            hu_str_contains_ci_cstr(vulnerability_level, strlen(vulnerability_level), "open")) {
            vw = snprintf(buf + pos, cap - pos,
                          "VULNERABILITY: Open. You can share real feelings and be honest "
                          "about struggles. This person can handle depth.\n");
        } else if (hu_str_contains_ci_cstr(vulnerability_level, strlen(vulnerability_level),
                                           "low") ||
                   hu_str_contains_ci_cstr(vulnerability_level, strlen(vulnerability_level),
                                           "guarded")) {
            vw = snprintf(buf + pos, cap - pos,
                          "VULNERABILITY: Guarded. Keep emotional sharing surface-level. "
                          "Don't dump feelings. Stay light.\n");
        }
        POS_ADVANCE(vw, pos, cap);
    }

    w = snprintf(buf + pos, cap - pos, "--- End relationship ---\n");
    POS_ADVANCE(w, pos, cap);

    return pos;
}

/* ── Group chat classifier ────────────────────────────────────────────── */

hu_group_response_t hu_conversation_classify_group(const char *msg, size_t msg_len,
                                                   const char *bot_name, size_t bot_name_len,
                                                   const hu_channel_history_entry_t *entries,
                                                   size_t count) {
    if (!msg || msg_len == 0)
        return HU_GROUP_SKIP;

    /* Always respond if directly addressed */
    if (bot_name && bot_name_len > 0 && hu_str_contains_ci_cstr(msg, msg_len, bot_name))
        return HU_GROUP_RESPOND;

    /* Always respond to direct questions (contains "?" and is short) */
    bool has_question = false;
    for (size_t i = 0; i < msg_len; i++) {
        if (msg[i] == '?') {
            has_question = true;
            break;
        }
    }
    if (has_question && msg_len < 100)
        return HU_GROUP_RESPOND;

    /* Skip tapbacks and reactions */
    if (msg_len <= 3)
        return HU_GROUP_SKIP;

    /* Skip if we responded to the last N messages already (don't dominate).
     * Threshold: g_consecutive_limit (default 3) */
    if (entries && count >= 3) {
        size_t consecutive_mine = 0;
        for (size_t i = count; i > 0 && consecutive_mine < g_consecutive_limit; i--) {
            if (entries[i - 1].from_me)
                consecutive_mine++;
            else
                break;
        }
        if (consecutive_mine >= g_consecutive_limit - 1)
            return HU_GROUP_SKIP;
    }

    /* Count how much of the recent conversation we've participated in.
     * If we've responded to more than g_participation_pct% of the last 10 messages, dial back.
     * Default threshold: 40% */
    if (entries && count >= 6) {
        size_t window = count < 10 ? count : 10;
        size_t my_msgs = 0;
        for (size_t i = count - window; i < count; i++) {
            if (entries[i].from_me)
                my_msgs++;
        }
        if (my_msgs * 100 / window > g_participation_pct)
            return HU_GROUP_SKIP;
    }

    /* Emotional content or someone asking for help → respond */
    static const char *DEFAULT_ENGAGE_WORDS[] = {
        "help", "anyone", "thoughts?", "what do you", "need", "advice", "opinion",
    };
    static const size_t DEFAULT_ENGAGE_WORDS_LEN =
        sizeof(DEFAULT_ENGAGE_WORDS) / sizeof(DEFAULT_ENGAGE_WORDS[0]);

    const char **engage_words = s_engage_words_len > 0 ? (const char **)s_engage_words
                                                       : (const char **)DEFAULT_ENGAGE_WORDS;
    size_t engage_words_len =
        s_engage_words_len > 0 ? s_engage_words_len : DEFAULT_ENGAGE_WORDS_LEN;

    for (size_t i = 0; i < engage_words_len; i++) {
        if (engage_words[i] && hu_str_contains_ci_cstr(msg, msg_len, engage_words[i]))
            return HU_GROUP_RESPOND;
    }

    /* Short message with no clear prompt → skip */
    if (msg_len < 30 && !has_question)
        return HU_GROUP_SKIP;

    /* Default: brief acknowledgment for medium messages, skip for long ones
     * (long messages in group chats are usually directed at specific people) */
    if (msg_len > 100)
        return HU_GROUP_SKIP;

    return HU_GROUP_BRIEF;
}

/* ── Group chat @ mentions (F56) ───────────────────────────────────────── */

size_t hu_conversation_build_group_member_directive(const char *const *members, size_t member_count,
                                                    char *buf, size_t cap) {
    if (!buf || cap == 0 || !members || member_count == 0)
        return 0;

    size_t pos = 0;
    int n = snprintf(buf, cap, "[GROUP: Members present: ");
    if (n <= 0 || (size_t)n >= cap)
        return 0;
    pos += (size_t)n;

    bool added_any = false;
    for (size_t i = 0; i < member_count && pos < cap; i++) {
        const char *name = members[i];
        if (!name || name[0] == '\0')
            continue;
        size_t name_len = strlen(name);
        if (name_len > 64)
            name_len = 64;
        if (added_any) {
            if (pos + 2 >= cap)
                break;
            memcpy(buf + pos, ", ", 2);
            pos += 2;
        }
        if (pos + name_len >= cap)
            break;
        memcpy(buf + pos, name, name_len);
        pos += name_len;
        added_any = true;
    }

    if (!added_any)
        return 0;

    const char *suffix = ". You can address them by name.]";
    size_t suffix_len = strlen(suffix);
    if (pos + suffix_len >= cap)
        return 0;
    memcpy(buf + pos, suffix, suffix_len + 1);
    return pos + suffix_len;
}

/* ── Inline reply classifier (iMessage quoted text fallback) ────────────── */

bool hu_conversation_should_inline_reply(const hu_channel_history_entry_t *entries, size_t count,
                                         const char *last_msg, size_t last_msg_len) {
    if (!last_msg || last_msg_len == 0)
        return false;

    /* Heuristic: "you said" / "earlier" / "what about" in their message → inline reply */
    if (hu_str_contains_ci_cstr(last_msg, last_msg_len, "you said") ||
        hu_str_contains_ci_cstr(last_msg, last_msg_len, "earlier") ||
        hu_str_contains_ci_cstr(last_msg, last_msg_len, "what about") ||
        hu_str_contains_ci_cstr(last_msg, last_msg_len, "that thing you") ||
        hu_str_contains_ci_cstr(last_msg, last_msg_len, "the one you"))
        return true;

    /* Heuristic: multiple questions pending in recent history */
    if (entries && count > 0) {
        int question_count = 0;
        size_t recent = count < 8 ? count : 8;
        for (size_t i = count - recent; i < count; i++) {
            if (entries[i].from_me)
                continue;
            const char *t = entries[i].text;
            size_t tl = strlen(t);
            for (size_t j = 0; j < tl; j++) {
                if (t[j] == '?') {
                    question_count++;
                    break;
                }
            }
        }
        if (question_count > 1)
            return true;
    }

    /* Single-topic conversation: no inline reply */
    return false;
}

/* ── Tapback-vs-text decision engine ────────────────────────────────────── */

static uint32_t tapback_prng_next(uint32_t *s) {
    *s = *s * 1103515245u + 12345u;
    return (*s >> 16u) & 0x7fffu;
}

/* Count recent from_me entries that look like tapback reactions (Loved, Liked, etc.) */
static size_t count_recent_tapbacks_from_me(const hu_channel_history_entry_t *entries,
                                            size_t entry_count) {
    if (!entries || entry_count == 0)
        return 0;
    size_t n = 0;
    for (size_t i = entry_count; i > 0 && n < 5; i--) {
        const hu_channel_history_entry_t *e = &entries[i - 1];
        if (!e->from_me)
            continue;
        char norm[64];
        size_t ni = 0;
        const char *t = e->text;
        size_t tl = strlen(t);
        for (size_t j = 0; j < tl && ni < sizeof(norm) - 1; j++) {
            char c = t[j];
            if (c >= 'A' && c <= 'Z')
                c += 32;
            if (c != ' ' && c != '\n' && c != '\r')
                norm[ni++] = c;
        }
        norm[ni] = '\0';
        if (is_tapback_reaction(norm, ni))
            n++;
    }
    return n;
}

hu_tapback_decision_t hu_conversation_classify_tapback_decision(
    const char *message, size_t message_len, const hu_channel_history_entry_t *entries,
    size_t entry_count, const struct hu_contact_profile *contact, uint32_t seed) {
    (void)contact; /* tapback_style.frequency future; use defaults for now */

    if (!message || message_len == 0)
        return HU_NO_RESPONSE;

    uint32_t s = seed;

    /* Normalize for comparison */
    char norm[128];
    size_t ni = 0;
    for (size_t i = 0; i < message_len && ni < sizeof(norm) - 1; i++) {
        char c = message[i];
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (c != ' ' && c != '\n' && c != '\r')
            norm[ni++] = c;
    }
    norm[ni] = '\0';

    size_t word_count = count_words(message, message_len);
    bool has_question = (memchr(message, '?', message_len) != NULL);

    /* Skip: tapbacks (system vocabulary) — never respond */
    if (is_tapback_reaction(norm, ni))
        return HU_NO_RESPONSE;

    /* Question → text preferred (need to answer) */
    if (has_question)
        return HU_TEXT_ONLY;

    /* Emotional/heavy content → text preferred */
    if (hu_str_contains_ci_cstr(message, message_len, "passed away") ||
        hu_str_contains_ci_cstr(message, message_len, "got fired") ||
        hu_str_contains_ci_cstr(message, message_len, "broke up") ||
        hu_str_contains_ci_cstr(message, message_len, "bad news") ||
        hu_str_contains_ci_cstr(message, message_len, "didn't make it") ||
        hu_str_contains_ci_cstr(message, message_len, "stressed") ||
        hu_str_contains_ci_cstr(message, message_len, "worried") ||
        hu_str_contains_ci_cstr(message, message_len, "sad") ||
        hu_str_contains_ci_cstr(message, message_len, "angry"))
        return HU_TEXT_ONLY;

    /* k/ok/okay: NO_RESPONSE with ~60% prob, else brief TEXT_ONLY */
    if ((ni == 1 && norm[0] == 'k') || (ni == 2 && memcmp(norm, "ok", 2) == 0) ||
        (ni == 4 && memcmp(norm, "okay", 4) == 0)) {
        uint32_t roll = tapback_prng_next(&s) % 100u;
        if (roll < 60u)
            return HU_NO_RESPONSE;
        return HU_TEXT_ONLY;
    }

    /* Humor (lol, haha, lmao) → TAPBACK_ONLY ~70%, else TAPBACK_AND_TEXT */
    if (hu_str_contains_ci_cstr(message, message_len, "lol") ||
        hu_str_contains_ci_cstr(message, message_len, "haha") ||
        hu_str_contains_ci_cstr(message, message_len, "lmao") ||
        hu_str_contains_ci_cstr(message, message_len, "😂")) {
        uint32_t roll = tapback_prng_next(&s) % 100u;
        if (roll < 70u)
            return HU_TAPBACK_ONLY;
        return HU_TAPBACK_AND_TEXT;
    }

    /* Agreement/affirmation (yeah, nice, cool, etc.) → TAPBACK_ONLY ~70% */
    if (hu_str_contains_ci_cstr(message, message_len, "yeah") ||
        hu_str_contains_ci_cstr(message, message_len, "nice") ||
        hu_str_contains_ci_cstr(message, message_len, "cool") ||
        hu_str_contains_ci_cstr(message, message_len, "sure") ||
        hu_str_contains_ci_cstr(message, message_len, "ok") ||
        hu_str_contains_ci_cstr(message, message_len, "👍") ||
        (message_len <= 8 && hu_str_contains_ci_cstr(message, message_len, "yes"))) {
        uint32_t roll = tapback_prng_next(&s) % 100u;
        if (roll < 70u)
            return HU_TAPBACK_ONLY;
        return HU_TAPBACK_AND_TEXT;
    }

    /* Recent tapbacks from us → reduce tapback probability, prefer text */
    size_t recent_tapbacks = count_recent_tapbacks_from_me(entries, entry_count);
    if (recent_tapbacks >= 2) {
        uint32_t roll = tapback_prng_next(&s) % 100u;
        if (roll < 60u)
            return HU_TEXT_ONLY;
    }

    /* Short message (<15 chars, no question) → tapback more likely */
    if (message_len < 15 && word_count <= 2) {
        uint32_t roll = tapback_prng_next(&s) % 100u;
        if (roll < 50u)
            return HU_TAPBACK_ONLY;
        if (roll < 80u)
            return HU_TAPBACK_AND_TEXT;
        return HU_TEXT_ONLY;
    }

    /* Default: substantive message → text */
    return HU_TEXT_ONLY;
}

/* ── Reaction classifier ───────────────────────────────────────────────── */

static uint32_t reaction_prng_next(uint32_t *s) {
    *s = *s * 1103515245u + 12345u;
    return (*s >> 16u) & 0x7fffu;
}

hu_reaction_type_t hu_conversation_classify_reaction(const char *msg, size_t msg_len, bool from_me,
                                                     const hu_channel_history_entry_t *entries,
                                                     size_t entry_count, uint32_t seed) {
    (void)entries;
    (void)entry_count;

    if (!msg || msg_len == 0)
        return HU_REACTION_NONE;

    /* Only react to messages from others, not our own */
    if (from_me)
        return HU_REACTION_NONE;

    uint32_t s = seed;

    /* GIFs: usually haha (they're sent to be funny), sometimes no reaction */
    if (hu_str_contains_ci_cstr(msg, msg_len, "[GIF]") ||
        hu_str_contains_ci_cstr(msg, msg_len, ".gif")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 40u)
            return HU_REACTION_HAHA;
        if (roll < 55u)
            return HU_REACTION_HEART;
        return HU_REACTION_NONE;
    }

    /* Voice messages: usually no reaction needed (will respond with text) */
    if (hu_str_contains_ci_cstr(msg, msg_len, "[Voice Message]"))
        return HU_REACTION_NONE;

    /* Photos/media placeholders: 50% chance of heart */
    if (hu_str_contains_ci_cstr(msg, msg_len, "[image") ||
        hu_str_contains_ci_cstr(msg, msg_len, "[attachment")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 50u)
            return HU_REACTION_HEART;
        return HU_REACTION_NONE;
    }

    /* Funny messages → HAHA */
    if (hu_str_contains_ci_cstr(msg, msg_len, "lol") ||
        hu_str_contains_ci_cstr(msg, msg_len, "lmao") ||
        hu_str_contains_ci_cstr(msg, msg_len, "haha") ||
        hu_str_contains_ci_cstr(msg, msg_len, "hahaha") ||
        hu_str_contains_ci_cstr(msg, msg_len, "😂") ||
        hu_str_contains_ci_cstr(msg, msg_len, "hilarious") ||
        hu_str_contains_ci_cstr(msg, msg_len, "that's funny") ||
        hu_str_contains_ci_cstr(msg, msg_len, "so funny")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 30u)
            return HU_REACTION_HAHA;
        return HU_REACTION_NONE;
    }

    /* Short message with exclamations (e.g. "omg!!", "yes!!!") → HAHA or EMPHASIS */
    if (msg_len <= 20) {
        bool has_excl = false;
        for (size_t i = 0; i < msg_len; i++) {
            if (msg[i] == '!') {
                has_excl = true;
                break;
            }
        }
        if (has_excl) {
            uint32_t roll = reaction_prng_next(&s) % 100u;
            if (roll < 30u)
                return HU_REACTION_HAHA;
        }
    }

    /* Loving/sweet messages → HEART */
    if (hu_str_contains_ci_cstr(msg, msg_len, "love you") ||
        hu_str_contains_ci_cstr(msg, msg_len, "miss you") ||
        hu_str_contains_ci_cstr(msg, msg_len, "❤") || hu_str_contains_ci_cstr(msg, msg_len, "💕") ||
        hu_str_contains_ci_cstr(msg, msg_len, "you're amazing") ||
        hu_str_contains_ci_cstr(msg, msg_len, "you're the best") ||
        hu_str_contains_ci_cstr(msg, msg_len, "proud of you") ||
        hu_str_contains_ci_cstr(msg, msg_len, "so sweet")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 30u)
            return HU_REACTION_HEART;
        return HU_REACTION_NONE;
    }

    /* Agreement/affirmation → THUMBS_UP */
    if (hu_str_contains_ci_cstr(msg, msg_len, "absolutely") ||
        hu_str_contains_ci_cstr(msg, msg_len, "exactly") ||
        hu_str_contains_ci_cstr(msg, msg_len, "yes!") ||
        hu_str_contains_ci_cstr(msg, msg_len, "👍") ||
        hu_str_contains_ci_cstr(msg, msg_len, "for sure") ||
        hu_str_contains_ci_cstr(msg, msg_len, "definitely")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 30u)
            return HU_REACTION_THUMBS_UP;
        return HU_REACTION_NONE;
    }

    /* Impressive/exciting news → EMPHASIS */
    if (hu_str_contains_ci_cstr(msg, msg_len, "got the job") ||
        hu_str_contains_ci_cstr(msg, msg_len, "i did it") ||
        hu_str_contains_ci_cstr(msg, msg_len, "we won") ||
        hu_str_contains_ci_cstr(msg, msg_len, "we did it") ||
        hu_str_contains_ci_cstr(msg, msg_len, "got in") ||
        hu_str_contains_ci_cstr(msg, msg_len, "i passed") ||
        hu_str_contains_ci_cstr(msg, msg_len, "got engaged") ||
        hu_str_contains_ci_cstr(msg, msg_len, "got promoted")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 30u)
            return HU_REACTION_EMPHASIS;
        return HU_REACTION_NONE;
    }

    /* Messages that need a real text response → NONE */
    if (hu_str_contains_ci_cstr(msg, msg_len, "what time") ||
        hu_str_contains_ci_cstr(msg, msg_len, "where") ||
        hu_str_contains_ci_cstr(msg, msg_len, "how do") ||
        hu_str_contains_ci_cstr(msg, msg_len, "can you") ||
        hu_str_contains_ci_cstr(msg, msg_len, "could you") ||
        hu_str_contains_ci_cstr(msg, msg_len, "why") ||
        hu_str_contains_ci_cstr(msg, msg_len, "when") || hu_str_contains_ci_cstr(msg, msg_len, "?"))
        return HU_REACTION_NONE;

    return HU_REACTION_NONE;
}

/* ── Photo reaction classifier (vision-based) ──────────────────────────── */

bool hu_conversation_extract_vision_description(const char *combined, size_t combined_len,
                                                const char **out_start, size_t *out_len) {
    if (!combined || combined_len == 0 || !out_start || !out_len) {
        if (out_start)
            *out_start = NULL;
        if (out_len)
            *out_len = 0;
        return false;
    }
    const char *prefix = "[They sent a photo: ";
    const size_t prefix_len = 20;
    if (combined_len < prefix_len + 1)
        return false;
    for (size_t i = 0; i + prefix_len <= combined_len; i++) {
        if (memcmp(combined + i, prefix, prefix_len) != 0)
            continue;
        const char *desc_start = combined + i + prefix_len;
        const char *end =
            (const char *)memchr(desc_start, ']', combined_len - (size_t)(desc_start - combined));
        if (!end || end <= desc_start) {
            if (out_start)
                *out_start = NULL;
            if (out_len)
                *out_len = 0;
            return false;
        }
        *out_start = desc_start;
        *out_len = (size_t)(end - desc_start);
        return true;
    }
    *out_start = NULL;
    *out_len = 0;
    return false;
}

hu_reaction_type_t hu_conversation_classify_photo_reaction(const char *vision_description,
                                                           size_t desc_len,
                                                           const struct hu_contact_profile *contact,
                                                           uint32_t seed) {
    (void)contact;

    if (!vision_description || desc_len == 0)
        return HU_REACTION_NONE;

    uint32_t s = seed;

    /* Text-preferred: food, screenshot, error, code → NONE */
    if (hu_str_contains_ci_cstr(vision_description, desc_len, "food") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "meal") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "dinner") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "lunch") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "pasta") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "screenshot") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "error") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "code"))
        return HU_REACTION_NONE;

    /* Funny → HAHA (probabilistic) */
    if (hu_str_contains_ci_cstr(vision_description, desc_len, "funny") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "meme") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "silly") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "hilarious") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "comic")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 85u)
            return HU_REACTION_HAHA;
        return HU_REACTION_NONE;
    }

    /* Sunset/landscape/nature/family/selfie → HEART (probabilistic) */
    if (hu_str_contains_ci_cstr(vision_description, desc_len, "sunset") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "landscape") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "nature") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "beach") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "mountain") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "sky") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "beautiful") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "family") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "baby") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "kids") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "selfie") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "portrait") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "couple") ||
        hu_str_contains_ci_cstr(vision_description, desc_len, "wedding")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 85u)
            return HU_REACTION_HEART;
        return HU_REACTION_NONE;
    }

    return HU_REACTION_NONE;
}

/* ── Text disfluency (F33) ──────────────────────────────────────────── */

static bool str_eq_ci(const char *a, size_t a_len, const char *b) {
    size_t b_len = strlen(b);
    if (a_len != b_len)
        return false;
    for (size_t i = 0; i < a_len; i++) {
        char ca =
            (char)((unsigned char)a[i] >= 'A' && (unsigned char)a[i] <= 'Z' ? a[i] + 32
                                                                            : (unsigned char)a[i]);
        char cb =
            (char)((unsigned char)b[i] >= 'A' && (unsigned char)b[i] <= 'Z' ? b[i] + 32
                                                                            : (unsigned char)b[i]);
        if (ca != cb)
            return false;
    }
    return true;
}

static atomic_int s_disfluency_mode = -1; /* -1 = unresolved */

hu_disfluency_mode_t hu_conversation_disfluency_mode(void) {
    int m = atomic_load_explicit(&s_disfluency_mode, memory_order_relaxed);
    if (m >= 0)
        return (hu_disfluency_mode_t)m;
    const char *env = getenv("HU_DISFLUENCY");
    hu_disfluency_mode_t resolved = HU_DISFLUENCY_OFF;
    if (env && env[0]) {
        if (strcmp(env, "live") == 0)
            resolved = HU_DISFLUENCY_LIVE;
        else if (strcmp(env, "shadow") == 0)
            resolved = HU_DISFLUENCY_SHADOW;
    }
    atomic_store_explicit(&s_disfluency_mode, (int)resolved, memory_order_relaxed);
    return resolved;
}

#if HU_IS_TEST
void hu_conversation_disfluency_set_mode_for_test(int mode) {
    atomic_store_explicit(&s_disfluency_mode, mode, memory_order_relaxed);
}
#endif

size_t hu_conversation_apply_disfluency(char *buf, size_t len, size_t cap, uint32_t seed,
                                        float frequency, const struct hu_contact_profile *contact,
                                        const char *formality, size_t formality_len) {
    if (!buf || len == 0 || cap <= len)
        return len;

    /* Send-mutating humanness feature: OFF unless explicitly promoted to
     * LIVE via HU_DISFLUENCY (feature-gate-requires-measurement). SHADOW
     * and OFF are both no-ops here — there is no would-do artifact worth
     * logging per message, and never mutating the send is the safe state. */
    if (hu_conversation_disfluency_mode() != HU_DISFLUENCY_LIVE)
        return len;

    /* Skip for formal contexts: coworker or formality "formal" */
    if (contact && contact->relationship_type) {
        size_t rt_len = strlen(contact->relationship_type);
        if (str_eq_ci(contact->relationship_type, rt_len, "coworker"))
            return len;
    }
    if (formality && formality_len > 0 && str_eq_ci(formality, formality_len, "formal"))
        return len;

    /* Probability roll: (seed % 100) < frequency*100 */
    uint32_t roll = seed % 100u;
    uint32_t threshold = (uint32_t)(frequency * 100.0f);
    if (threshold > 100u)
        threshold = 100u;
    if (roll >= threshold)
        return len;

    /* Type selection: 40% prepend, 30% append, 30% insert "actually".
     * The former 10% self-correction (" wait no " inserted mid-clause,
     * " *meant it") was REMOVED 2026-07-12 — it produced ungrammatical
     * output ("Wish wait no I could lol") that reads as broken, not human,
     * and was sent to a real contact. Every remaining variant is
     * grammatical wherever it lands. */
    uint32_t type_roll = (seed / 100u) % 100u;
    size_t add_len = 0;
    const char *add = NULL;
    bool prepend = false;
    bool append = false;
    size_t insert_pos = (size_t)-1;

    if (type_roll < 40u) {
        /* 40%: prepend "i mean " or "like " */
        prepend = true;
        if (((seed / 10000u) % 2u) == 0) {
            add = "i mean ";
            add_len = 7;
        } else {
            add = "like ";
            add_len = 5;
        }
    } else if (type_roll < 70u) {
        /* 30%: append "..." or " you know" (before final period) */
        append = true;
        if (((seed / 10000u) % 2u) == 0) {
            add = "...";
            add_len = 3;
        } else {
            add = " you know";
            add_len = 9;
        }
    } else {
        /* 30%: insert "actually " after first space/clause */
        add = "actually ";
        add_len = 9;
        for (size_t i = 0; i < len; i++) {
            if (buf[i] == ' ' || buf[i] == ',' || buf[i] == '.') {
                insert_pos = i + 1;
                break;
            }
        }
        if (insert_pos == (size_t)-1)
            insert_pos = len;
    }

    if (len + add_len >= cap)
        return len;

    if (prepend) {
        memmove(buf + add_len, buf, len);
        memcpy(buf, add, add_len);
        if (add_len < len + add_len && buf[add_len] >= 'A' && buf[add_len] <= 'Z')
            buf[add_len] += 32;
        len += add_len;
    } else if (append) {
        /* Insert before final period if present */
        size_t period_pos = len;
        for (size_t i = len; i > 0; i--) {
            if (buf[i - 1] == '.') {
                period_pos = i - 1;
                break;
            }
        }
        memmove(buf + period_pos + add_len, buf + period_pos, len - period_pos);
        memcpy(buf + period_pos, add, add_len);
        len += add_len;
    } else if (insert_pos != (size_t)-1) {
        memmove(buf + insert_pos + add_len, buf + insert_pos, len - insert_pos);
        memcpy(buf + insert_pos, add, add_len);
        len += add_len;
    }
    buf[len] = '\0';
    return len;
}

/* ── Filler word injection ────────────────────────────────────────────── */

static uint32_t filler_lcg(uint32_t *s) {
    *s = *s * 1103515245u + 12345u;
    return (*s >> 16) & 0x7fff;
}

/* Filler-injector activation gate (off | shadow | live). Default OFF: the
 * hardcoded fillers are an unmeasured send-mutator, so they must not shape
 * real output until a measurement promotes them
 * (~/.claude/rules/feature-gate-requires-measurement.md). */
#if defined(HU_IS_TEST) && HU_IS_TEST
static int s_fillers_mode_override = -1; /* -1 = defer to env HU_FILLERS */
void hu_conversation_fillers_set_mode_for_test(int mode) {
    s_fillers_mode_override = mode;
}
#endif

hu_gate_mode_t hu_conversation_fillers_mode(void) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    if (s_fillers_mode_override >= 0)
        return (hu_gate_mode_t)s_fillers_mode_override;
#endif
    return hu_gate_mode_from_env("HU_FILLERS", HU_GATE_OFF);
}

size_t hu_conversation_apply_fillers(char *buf, size_t len, size_t cap, uint32_t seed,
                                     const char *channel_type, size_t channel_type_len) {
    if (!buf || len == 0 || cap <= len)
        return len;

    /* Gated OFF by default: unmeasured hardcoded fillers must not mutate real
     * output until a measurement promotes them to LIVE. SHADOW currently emits
     * nothing (no would-inject logging wired yet), so only LIVE mutates. */
    if (hu_conversation_fillers_mode() != HU_GATE_LIVE)
        return len;

    /* Skip fillers for formal channels */
    if (channel_type && channel_type_len > 0) {
        if ((channel_type_len == 5 && memcmp(channel_type, "email", 5) == 0) ||
            (channel_type_len == 5 && memcmp(channel_type, "slack", 5) == 0))
            return len;
    }

    /* ~20% chance of injecting a filler per response */
    uint32_t s = seed;
    if (filler_lcg(&s) % 5 != 0)
        return len;

    static const char *DEFAULT_FILLERS[] = {"haha ", "lol ", "yeah ", "honestly ", "tbh ",
                                            "ngl ",  "hmm ", "oh ",   "ah ",       "like "};
    static const size_t DEFAULT_FILLER_COUNT = 10;

    const char **fillers =
        s_filler_words_len > 0 ? (const char **)s_filler_words : (const char **)DEFAULT_FILLERS;
    size_t filler_count = s_filler_words_len > 0 ? s_filler_words_len : DEFAULT_FILLER_COUNT;

    if (filler_count == 0)
        return len;

    size_t pick = filler_lcg(&s) % filler_count;
    const char *filler = fillers[pick];
    if (!filler)
        filler = fillers[0];

    size_t filler_len = strlen(filler);

    if (len + filler_len >= cap)
        return len;

    /* Placement: start of response (most natural for casual messaging) */
    memmove(buf + filler_len, buf, len);
    memcpy(buf, filler, filler_len);
    /* Lowercase the first char of original text after filler */
    if (filler_len < len + filler_len && buf[filler_len] >= 'A' && buf[filler_len] <= 'Z')
        buf[filler_len] += 32;
    len += filler_len;
    buf[len] = '\0';
    return len;
}

/* ── Nonverbal sound injection (F39) ─────────────────────────────────── */

size_t hu_conversation_inject_nonverbals(char *buf, size_t len, size_t cap, uint32_t seed,
                                         bool enabled) {
    if (!buf || len == 0 || !enabled)
        return len;
    if (cap <= len)
        return len;
    /* 15% probability */
    if ((seed % 100u) >= 15u)
        return len;

    /* 50% [laughter], 30% Hmm..., 20% ... */
    uint32_t t = (seed / 100u) % 10u;
    uint32_t type = (t < 5u) ? 0u : (t < 8u) ? 1u : 2u;
    const char *insert = NULL;
    size_t insert_len = 0;
    size_t pos = 0;
    bool insert_after = true;

    if (type == 0) {
        /* [laughter] after first sentence-ending or before lol/haha */
        insert = "[laughter] ";
        insert_len = 11;
        for (size_t i = 0; i < len; i++) {
            if (buf[i] == '.' || buf[i] == '!' || buf[i] == '?') {
                pos = i + 1;
                insert_after = true;
                break;
            }
            if (i + 3 <= len && strncasecmp(buf + i, "lol", 3) == 0) {
                pos = i;
                insert_after = false;
                break;
            }
            if (i + 4 <= len && strncasecmp(buf + i, "haha", 4) == 0) {
                pos = i;
                insert_after = false;
                break;
            }
            if (i + 4 <= len && strncasecmp(buf + i, "lmao", 4) == 0) {
                pos = i;
                insert_after = false;
                break;
            }
        }
        if (pos == 0 && len > 0) {
            pos = len;
            insert_after = true;
        }
    } else if (type == 1) {
        /* Prepend "Hmm... " at start */
        insert = "Hmm... ";
        insert_len = 7;
        pos = 0;
        insert_after = false;
    } else {
        /* type == 2: insert "... " after first comma or period */
        insert = "... ";
        insert_len = 4;
        for (size_t i = 0; i < len; i++) {
            if (buf[i] == ',' || buf[i] == '.') {
                pos = i + 1;
                insert_after = true;
                break;
            }
        }
        if (pos == 0) {
            pos = len;
            insert_after = true;
        }
    }

    if (len + insert_len >= cap)
        return len;

    if (insert_after) {
        memmove(buf + pos + insert_len, buf + pos, len - pos);
        memcpy(buf + pos, insert, insert_len);
    } else {
        memmove(buf + insert_len, buf, len);
        memcpy(buf, insert, insert_len);
    }
    len += insert_len;
    buf[len] = '\0';
    return len;
}

/* ── Stylometric variance ─────────────────────────────────────────────── */

#if defined(HU_IS_TEST) && HU_IS_TEST
static int s_vary_complexity_mode_override = -1; /* -1 = defer to env */
void hu_conversation_vary_complexity_set_mode_for_test(int mode) {
    s_vary_complexity_mode_override = mode;
}
#endif

hu_gate_mode_t hu_conversation_vary_complexity_mode(void) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    if (s_vary_complexity_mode_override >= 0)
        return (hu_gate_mode_t)s_vary_complexity_mode_override;
#endif
    return hu_gate_mode_from_env("HU_VARY_COMPLEXITY", HU_GATE_OFF);
}

size_t hu_conversation_vary_complexity(char *buf, size_t len, uint32_t seed) {
    if (!buf || len == 0)
        return len;

    /* Egress single-funnel Phase 2 (2026-07-18): the last ungated
     * send-mutator. Contraction rewriting mutates model output unmeasured —
     * same class as fillers/disfluency — so it is gated OFF by default and
     * only a measurement promotes it to LIVE
     * (.claude/rules/feature-gate-requires-measurement.md). SHADOW currently
     * behaves like OFF (no would-mutate logging wired yet). */
    if (hu_conversation_vary_complexity_mode() != HU_GATE_LIVE)
        return len;

    /* Apply common contractions with ~40% probability each */
    uint32_t s = seed;

    /* Default contractions (fallback) */
    static const hu_conversation_contraction_t DEFAULT_CONTRACTIONS[] = {
        {"I am ", 5, "I'm ", 4},
        {"it is ", 6, "it's ", 5},
        {"do not ", 7, "don't ", 6},
        {"does not ", 9, "doesn't ", 8},
        {"did not ", 8, "didn't ", 7},
        {"is not ", 7, "isn't ", 6},
        {"are not ", 8, "aren't ", 7},
        {"would not ", 10, "wouldn't ", 9},
        {"could not ", 10, "couldn't ", 9},
        {"I will ", 7, "I'll ", 5},
        {"I would ", 8, "I'd ", 4},
        {"that is ", 8, "that's ", 7},
        {"there is ", 9, "there's ", 8},
        {"I have ", 7, "I've ", 5},
        {"you are ", 8, "you're ", 7},
        {"they are ", 9, "they're ", 8},
        {"we are ", 7, "we're ", 6},
        {"cannot ", 7, "can't ", 6},
    };
    static const size_t DEFAULT_CONTRACTIONS_LEN =
        sizeof(DEFAULT_CONTRACTIONS) / sizeof(DEFAULT_CONTRACTIONS[0]);

    const hu_conversation_contraction_t *contractions =
        s_contractions_len > 0 ? s_contractions : DEFAULT_CONTRACTIONS;
    size_t n_contractions = s_contractions_len > 0 ? s_contractions_len : DEFAULT_CONTRACTIONS_LEN;

    for (size_t c = 0; c < n_contractions && len > 0; c++) {
        s = s * 1103515245u + 12345u;
        if (((s >> 16) & 0x7fff) % 100 >= 40)
            continue;

        if (contractions[c].from_len == 0)
            continue;

        /* Case-insensitive search for the contraction source */
        for (size_t i = 0; i + contractions[c].from_len <= len; i++) {
            bool match = true;
            for (size_t j = 0; j < contractions[c].from_len; j++) {
                char a = buf[i + j];
                char b = contractions[c].from[j];
                if (a >= 'A' && a <= 'Z')
                    a += 32;
                if (a != b) {
                    match = false;
                    break;
                }
            }
            if (!match)
                continue;

            /* Preserve original case of first char */
            bool was_upper = (buf[i] >= 'A' && buf[i] <= 'Z');
            size_t diff = contractions[c].from_len - contractions[c].to_len;
            memcpy(buf + i, contractions[c].to, contractions[c].to_len);
            if (was_upper && buf[i] >= 'a' && buf[i] <= 'z')
                buf[i] -= 32;
            if (diff > 0) {
                memmove(buf + i + contractions[c].to_len, buf + i + contractions[c].from_len,
                        len - (i + contractions[c].from_len));
                len -= diff;
                buf[len] = '\0';
            }
            break;
        }
    }
    return len;
}

/* ── Bidirectional sentiment momentum ─────────────────────────────────── */

char *hu_conversation_build_sentiment_momentum(hu_allocator_t *alloc,
                                               const hu_channel_history_entry_t *entries,
                                               size_t count, size_t *out_len) {
    *out_len = 0;
    if (!alloc || !out_len || !entries || count < 3)
        return NULL;

    static const char *DEFAULT_POS_WORDS[] = {
        "happy",   "great",     "awesome", "love", "good", "nice",  "excited", "glad",
        "amazing", "wonderful", "lol",     "haha", "yay",  "sweet", "perfect", "thanks"};
    static const char *DEFAULT_NEG_WORDS[] = {
        "sad",      "angry", "frustrated",   "annoyed", "terrible", "awful", "hate", "worried",
        "stressed", "upset", "disappointed", "ugh",     "sucks",    "rough", "hard", "sorry"};
    static const size_t DEFAULT_POS_LEN = sizeof(DEFAULT_POS_WORDS) / sizeof(DEFAULT_POS_WORDS[0]);
    static const size_t DEFAULT_NEG_LEN = sizeof(DEFAULT_NEG_WORDS) / sizeof(DEFAULT_NEG_WORDS[0]);

    const char **pos_words = s_positive_words_len > 0 ? (const char **)s_positive_words
                                                      : (const char **)DEFAULT_POS_WORDS;
    const char **neg_words = s_negative_words_len > 0 ? (const char **)s_negative_words
                                                      : (const char **)DEFAULT_NEG_WORDS;
    size_t n_pos = s_positive_words_len > 0 ? s_positive_words_len : DEFAULT_POS_LEN;
    size_t n_neg = s_negative_words_len > 0 ? s_negative_words_len : DEFAULT_NEG_LEN;

    float momentum = 0.0f;
    size_t user_msgs = 0;
    size_t window = count > 6 ? 6 : count;

    for (size_t i = count - window; i < count; i++) {
        if (entries[i].from_me)
            continue;
        const char *text = entries[i].text;
        size_t tl = strlen(text);
        if (tl == 0)
            continue;
        user_msgs++;
        int score = 0;
        for (size_t w = 0; w < n_pos; w++) {
            if (pos_words[w] && hu_str_contains_ci_cstr(text, tl, pos_words[w]))
                score++;
        }
        for (size_t w = 0; w < n_neg; w++) {
            if (neg_words[w] && hu_str_contains_ci_cstr(text, tl, neg_words[w]))
                score--;
        }
        float weight = (float)(i - (count - window) + 1) / (float)window;
        momentum += (float)score * weight;
    }

    if (user_msgs < 2)
        return NULL;

    momentum /= (float)user_msgs;

    char buf[256];
    int w;
    if (momentum > 1.0f) {
        w = snprintf(buf, sizeof(buf),
                     "\nSENTIMENT: The conversation mood is trending positive and upbeat. "
                     "Match their energy — be warm, enthusiastic, and engaged.\n");
    } else if (momentum < -1.0f) {
        w = snprintf(buf, sizeof(buf),
                     "\nSENTIMENT: The conversation mood is trending heavy or negative. "
                     "Match their energy — be empathetic, gentle, and present. Don't try to "
                     "force positivity.\n");
    } else if (momentum < -0.3f) {
        w = snprintf(buf, sizeof(buf),
                     "\nSENTIMENT: The conversation mood is slightly low. Be supportive and "
                     "attentive without being overly cheerful.\n");
    } else if (momentum > 0.3f) {
        w = snprintf(buf, sizeof(buf),
                     "\nSENTIMENT: The conversation mood is light and positive. Keep the vibe "
                     "going naturally.\n");
    } else {
        return NULL;
    }

    if (w <= 0 || (size_t)w >= sizeof(buf))
        return NULL;

    char *result = (char *)alloc->alloc(alloc->ctx, (size_t)w + 1);
    if (!result)
        return NULL;
    memcpy(result, buf, (size_t)w + 1);
    *out_len = (size_t)w;
    return result;
}

/* ── Conversation depth signal ────────────────────────────────────────── */

char *hu_conversation_build_depth_signal(hu_allocator_t *alloc,
                                         const hu_channel_history_entry_t *entries, size_t count,
                                         size_t *out_len) {
    *out_len = 0;
    if (!alloc || !out_len || !entries || count < 5)
        return NULL;

    size_t user_turns = 0;
    for (size_t i = 0; i < count; i++) {
        if (!entries[i].from_me)
            user_turns++;
    }

    if (user_turns < 5)
        return NULL;

    char buf[512];
    int w;
    if (user_turns >= 15) {
        w = snprintf(buf, sizeof(buf),
                     "\nDEPTH: This is a deep conversation (%zu exchanges). Stay deeply in "
                     "character. Reference earlier parts of THIS conversation naturally. Your "
                     "consistency matters more than ever — any break in persona will be noticed. "
                     "Vary your sentence structure and vocabulary to avoid repetitive patterns.\n",
                     user_turns);
    } else if (user_turns >= 10) {
        w = snprintf(buf, sizeof(buf),
                     "\nDEPTH: This is a sustained conversation (%zu exchanges). Maintain strong "
                     "persona consistency. Mix up your response patterns — vary openers, vary "
                     "length, reference earlier context.\n",
                     user_turns);
    } else {
        w = snprintf(buf, sizeof(buf),
                     "\nDEPTH: Conversation is building (%zu exchanges). Keep your voice steady "
                     "and natural. Avoid falling into a pattern.\n",
                     user_turns);
    }

    if (w <= 0 || (size_t)w >= sizeof(buf))
        return NULL;

    char *result = (char *)alloc->alloc(alloc->ctx, (size_t)w + 1);
    if (!result)
        return NULL;
    memcpy(result, buf, (size_t)w + 1);
    *out_len = (size_t)w;
    return result;
}

/* ── Topic tangent/callback engine ────────────────────────────────────── */

char *hu_conversation_build_tangent_callback(hu_allocator_t *alloc,
                                             const hu_channel_history_entry_t *entries,
                                             size_t count, uint32_t seed, size_t *out_len) {
    *out_len = 0;
    if (!alloc || !out_len || !entries || count < 6)
        return NULL;

    /* ~6% probability per turn */
    uint32_t s = seed * 1103515245u + 12345u;
    if (((s >> 16) & 0x7fff) % 100 >= 6)
        return NULL;

    /* Find topics from the EARLIER part of conversation (not last 3 messages) */
    hu_callback_topic_t topics[HU_CALLBACK_MAX_TOPICS];
    size_t topic_count = 0;
    memset(topics, 0, sizeof(topics));

    for (size_t i = 0; i + 3 < count; i++) {
        const char *text = entries[i].text;
        size_t tl = strlen(text);
        if (tl == 0)
            continue;
        extract_topics_from_text(text, tl, topics, &topic_count);
    }

    if (topic_count == 0)
        return NULL;

    /* Pick a random earlier topic */
    s = s * 1103515245u + 12345u;
    size_t pick = ((s >> 16) & 0x7fff) % topic_count;
    hu_callback_topic_t *t = &topics[pick];

    /* Skip if topic is too short or too generic */
    if (t->phrase_len < 3)
        return NULL;

    char buf[256];
    static const char *DEFAULT_INTROS[] = {
        "oh btw, about ", "that reminds me of when we talked about ",
        "going back to ", "hey also re: ",
        "oh and about ",
    };
    static const size_t DEFAULT_INTROS_LEN = sizeof(DEFAULT_INTROS) / sizeof(DEFAULT_INTROS[0]);

    const char **intros = s_conversation_intros_len > 0 ? (const char **)s_conversation_intros
                                                        : (const char **)DEFAULT_INTROS;
    size_t intros_len =
        s_conversation_intros_len > 0 ? s_conversation_intros_len : DEFAULT_INTROS_LEN;

    if (intros_len == 0)
        return NULL;

    s = s * 1103515245u + 12345u;
    size_t intro_pick = ((s >> 16) & 0x7fff) % intros_len;

    const char *chosen_intro = intros[intro_pick];
    if (!chosen_intro)
        chosen_intro = intros[0];

    int w = snprintf(buf, sizeof(buf),
                     "\nTANGENT: Consider naturally circling back to \"%.*s\" from earlier. "
                     "You could use something like: \"%s%.*s...\" — only if it fits naturally. "
                     "Don't force it.\n",
                     (int)t->phrase_len, t->phrase, chosen_intro, (int)t->phrase_len, t->phrase);
    if (w <= 0 || (size_t)w >= sizeof(buf))
        return NULL;

    char *result = (char *)alloc->alloc(alloc->ctx, (size_t)w + 1);
    if (!result)
        return NULL;
    memcpy(result, buf, (size_t)w + 1);
    *out_len = (size_t)w;
    return result;
}

/* ── AI disclosure detection ──────────────────────────────────────────── */

bool hu_conversation_check_ai_disclosure(const char *response, size_t response_len) {
    if (!response || response_len == 0)
        return false;

    static const char *DEFAULT_PATTERNS[] = {
        "i'm an ai",
        "i am an ai",
        "as an ai",
        "i'm a language",
        "i am a language",
        "language model",
        "as a chatbot",
        "i'm a chatbot",
        "i am a chatbot",
        "i'm not human",
        "i am not human",
        "i don't have feelings",
        "i do not have feelings",
        "i can't feel",
        "as an assistant",
        "i'm an assistant",
        "i am an assistant",
        "i was programmed",
        "my training data",
        "i don't have personal",
        "i do not have personal",
        "i'm not a real person",
        "i am not a real person",
        "i'm just a bot",
        "i am just a bot",
    };
    static const size_t DEFAULT_PATTERNS_LEN =
        sizeof(DEFAULT_PATTERNS) / sizeof(DEFAULT_PATTERNS[0]);

    const char **patterns = s_ai_disclosure_patterns_len > 0
                                ? (const char **)s_ai_disclosure_patterns
                                : (const char **)DEFAULT_PATTERNS;
    size_t n =
        s_ai_disclosure_patterns_len > 0 ? s_ai_disclosure_patterns_len : DEFAULT_PATTERNS_LEN;

    for (size_t p = 0; p < n; p++) {
        if (!patterns[p])
            continue;
        size_t plen = strlen(patterns[p]);
        if (plen > response_len)
            continue;
        for (size_t i = 0; i + plen <= response_len; i++) {
            bool match = true;
            for (size_t j = 0; j < plen; j++) {
                char a = response[i + j];
                char b = patterns[p][j];
                if (a >= 'A' && a <= 'Z')
                    a += 32;
                if (a != b) {
                    match = false;
                    break;
                }
            }
            if (match)
                return true;
        }
    }
    return false;
}

/* ── Banned AI phrases post-processor ─────────────────────────────────── */

typedef struct {
    const char *from;
    size_t from_len;
    const char *to;
    size_t to_len;
} hu_ai_phrase_replacement_t;

/* Hardcoded fallback when conversation/ai_phrases.json cannot be loaded */
static const hu_ai_phrase_replacement_t s_ai_phrases_fallback[] = {
    {"Let me know if you need anything", 32, "", 0},
    {"That's a great question! ", 24, "", 0},
    {"That's a great question", 23, "", 0},
    {"Here's what I think: ", 21, "", 0},
    {"I understand your ", 18, "", 0},
    {"I hope this helps", 17, "", 0},
    {"Let me know if ", 15, "", 0},
    {"Great question! ", 16, "", 0},
    {"Great question. ", 16, "", 0},
    {"That's a great point! ", 22, "", 0},
    {"That's a great point. ", 22, "", 0},
    {"I appreciate you sharing that. ", 31, "", 0},
    {"I appreciate you sharing that! ", 31, "", 0},
    {"I appreciate ", 13, "", 0},
    {"Let me break this down. ", 24, "", 0},
    {"Let me break this down: ", 24, "", 0},
    {"Here's the thing: ", 18, "", 0},
    {"Here's the thing, ", 18, "", 0},
    {"That's a fantastic ", 19, "That's a good ", 14},
    {"I think that's a great ", 23, "", 0},
    {"I think that's great! ", 22, "", 0},
    {"crucial", 7, "important", 9},
    {"comprehensive", 13, "thorough", 8},
    {"pivotal", 7, "key", 3},
    {"delve", 5, "dig", 3},
    {"facilitate", 10, "help", 4},
    {"leverage", 8, "use", 3},
    {"utilize", 7, "use", 3},
    {"I completely understand", 23, "I get it", 8},
    {"Absolutely! ", 12, "", 0},
    {"Certainly! ", 11, "", 0},
    {"Certainly!", 10, "", 0},
    {"Of course! ", 11, "", 0},
    {"Feel free to ", 13, "", 0},
    {"Don't hesitate to ", 18, "", 0},
    {"I'd be happy to ", 16, "", 0},
    {"I'm here to help", 16, "I'm here", 8},
    {"I'm here for you! ", 18, "I'm here ", 9},
    {"That said, ", 11, "", 0},
    {"That being said, ", 17, "", 0},
    {"It's worth noting ", 18, "", 0},
    {"It's important to note ", 23, "", 0},
    {"In any case, ", 13, "", 0},
    {"At the end of the day, ", 23, "", 0},
    {"To be honest, ", 14, "", 0},
    {"I want you to know ", 19, "", 0},
    {"navigating", 10, "handling", 8},
    {"resonate", 8, "hit home", 8},
    {"boundaries", 10, "limits", 6},
    {"self-care", 9, "rest", 4},
    {"impactful", 9, "big", 3},
    {"!! ", 3, "! ", 2},
    {"As an AI, ", 10, "", 0},
    {"As an AI ", 9, "", 0},
    {"As a language model, ", 21, "", 0},
    {"As a language model ", 20, "", 0},
};

static hu_ai_phrase_replacement_t *s_ai_phrases_cache = NULL;
static size_t s_ai_phrases_cache_len = 0;
static bool s_ai_phrases_loaded = false;

static void load_ai_phrases_once(void) {
    if (s_ai_phrases_loaded)
        return;
    s_ai_phrases_loaded = true;

    hu_allocator_t alloc_val = hu_system_allocator();
    hu_allocator_t *alloc = &alloc_val;
    char *json_data = NULL;
    size_t json_len = 0;
    hu_error_t err = hu_data_load(alloc, "conversation/ai_phrases.json", &json_data, &json_len);
    if (err != HU_OK || !json_data || json_len == 0)
        goto use_fallback;

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, json_data, json_len, &root);
    alloc->free(alloc->ctx, json_data, json_len);
    if (err != HU_OK || !root || root->type != HU_JSON_ARRAY)
        goto use_fallback;

    size_t arr_len = root->data.array.len;
    if (arr_len == 0)
        goto use_fallback;

    hu_ai_phrase_replacement_t *cache = (hu_ai_phrase_replacement_t *)alloc->alloc(
        alloc->ctx, arr_len * sizeof(hu_ai_phrase_replacement_t));
    if (!cache) {
        hu_json_free(alloc, root);
        goto use_fallback;
    }

    size_t valid = 0;
    for (size_t i = 0; i < arr_len; i++) {
        hu_json_value_t *item = root->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;
        const char *from = hu_json_get_string(item, "from");
        const char *to = hu_json_get_string(item, "to");
        if (!from)
            continue;
        size_t flen = strlen(from);
        char *from_copy = (char *)alloc->alloc(alloc->ctx, flen + 1);
        char *to_copy = to ? (char *)alloc->alloc(alloc->ctx, strlen(to) + 1) : NULL;
        if (!from_copy)
            continue;
        memcpy(from_copy, from, flen + 1);
        if (to && to_copy)
            memcpy(to_copy, to, strlen(to) + 1);
        cache[valid].from = from_copy;
        cache[valid].from_len = flen;
        cache[valid].to = (to && to_copy) ? to_copy : "";
        cache[valid].to_len = (to && to_copy) ? strlen(to) : 0;
        valid++;
    }
    hu_json_free(alloc, root);

    if (valid > 0) {
        s_ai_phrases_cache = cache;
        s_ai_phrases_cache_len = valid;
        return;
    }
    alloc->free(alloc->ctx, cache, arr_len * sizeof(hu_ai_phrase_replacement_t));

use_fallback:
    s_ai_phrases_cache = (hu_ai_phrase_replacement_t *)s_ai_phrases_fallback;
    s_ai_phrases_cache_len = sizeof(s_ai_phrases_fallback) / sizeof(s_ai_phrases_fallback[0]);
}

size_t hu_conversation_strip_ai_phrases(char *buf, size_t len) {
    if (!buf || len == 0)
        return len;

    load_ai_phrases_once();
    const hu_ai_phrase_replacement_t *replacements = s_ai_phrases_cache;
    size_t n_rep = s_ai_phrases_cache_len;

    for (size_t r = 0; r < n_rep; r++) {
        for (size_t i = 0; i + replacements[r].from_len <= len; i++) {
            bool ci_match = true;
            for (size_t j = 0; j < replacements[r].from_len; j++) {
                char a = buf[i + j];
                char b = replacements[r].from[j];
                if (a >= 'A' && a <= 'Z')
                    a += 32;
                if (b >= 'A' && b <= 'Z')
                    b += 32;
                if (a != b) {
                    ci_match = false;
                    break;
                }
            }
            if (!ci_match)
                continue;

            if (replacements[r].to_len <= replacements[r].from_len) {
                size_t diff = replacements[r].from_len - replacements[r].to_len;
                if (replacements[r].to_len > 0)
                    memcpy(buf + i, replacements[r].to, replacements[r].to_len);
                memmove(buf + i + replacements[r].to_len, buf + i + replacements[r].from_len,
                        len - (i + replacements[r].from_len));
                len -= diff;
                buf[len] = '\0';
                i += replacements[r].to_len;
                if (i > 0)
                    i--;
            }
        }
    }

    return len;
}

/* ── Hallucinated tag stripper ─────────────────────────────────────────── */

static bool strip_match_lit(const char *buf, size_t len, size_t pos, const char *lit,
                            size_t lit_len) {
    return pos + lit_len <= len && memcmp(buf + pos, lit, lit_len) == 0;
}

static bool strip_find_xml_close(const char *buf, size_t len, size_t start, const char *tag,
                                 size_t tag_len, size_t *end_out) {
    for (size_t k = start; k + tag_len + 3 <= len; k++) {
        if (buf[k] == '<' && buf[k + 1] == '/' && memcmp(buf + k + 2, tag, tag_len) == 0 &&
            buf[k + 2 + tag_len] == '>') {
            *end_out = k + 3 + tag_len;
            return true;
        }
    }
    return false;
}

size_t hu_conversation_strip_channel_tags(char *buf, size_t len) {
    if (!buf || len < 3)
        return len;

    /* Known artifact tags to strip entirely (paired XML-style) */
    static const char *const xml_tags[] = {"thinking",   "answer",   "analysis",
                                           "reflection", "internal", "scratchpad"};
    static const size_t xml_tag_count = sizeof(xml_tags) / sizeof(xml_tags[0]);

    /* Known standalone tokens to strip */
    static const struct {
        const char *token;
        size_t len;
    } standalone[] = {
        {"</s>", 4},
        {"<s>", 3},
        {"<|endoftext|>", 14},
        {"<|im_start|>", 12},
        /* Split literal so "\\r" in "redacted\\r..." is not parsed as a carriage return. */
        {"<|redacted"
         "_im_end|>",
         19},
        {"[INST]", 6},
        {"[/INST]", 7},
        {"<<SYS>>", 7},
        {"<</SYS>>", 8},
    };
    static const size_t standalone_count = sizeof(standalone) / sizeof(standalone[0]);

    size_t w = 0;
    size_t i = 0;
    while (i < len) {
        /* 1. Standalone known tokens */
        bool matched = false;
        for (size_t s = 0; s < standalone_count; s++) {
            if (strip_match_lit(buf, len, i, standalone[s].token, standalone[s].len)) {
                i += standalone[s].len;
                matched = true;
                break;
            }
        }
        if (matched)
            continue;

        /* 2. ChatML-style <|word|> and paired <|word>...<word|> */
        if (buf[i] == '<' && i + 1 < len && buf[i + 1] == '|') {
            size_t j = i + 2;
            while (j < len && buf[j] != '>' && buf[j] != '|' && buf[j] != '<' && buf[j] != ' ')
                j++;
            if (j < len && buf[j] == '|' && j + 1 < len && buf[j + 1] == '>') {
                i = j + 2;
                continue;
            }
            if (j < len && buf[j] == '>') {
                size_t word_start = i + 2;
                size_t word_len = j - word_start;
                bool found_close = false;
                for (size_t k = j + 1; k + word_len + 2 <= len; k++) {
                    if (buf[k] == '<' && memcmp(buf + k + 1, buf + word_start, word_len) == 0 &&
                        k + 1 + word_len < len && buf[k + 1 + word_len] == '|' &&
                        k + 2 + word_len < len && buf[k + 2 + word_len] == '>') {
                        i = k + word_len + 3;
                        found_close = true;
                        break;
                    }
                }
                if (found_close)
                    continue;
            }
        }

        /* 3. XML-style paired tags: <thinking>...</thinking> etc. */
        if (buf[i] == '<' && i + 1 < len && buf[i + 1] != '/') {
            for (size_t t = 0; t < xml_tag_count; t++) {
                size_t tl = strlen(xml_tags[t]);
                if (strip_match_lit(buf, len, i + 1, xml_tags[t], tl) && i + 1 + tl < len &&
                    buf[i + 1 + tl] == '>') {
                    size_t end;
                    if (strip_find_xml_close(buf, len, i + 2 + tl, xml_tags[t], tl, &end)) {
                        i = end;
                        matched = true;
                        break;
                    }
                }
            }
            if (matched)
                continue;
        }

        buf[w++] = buf[i++];
    }
    buf[w] = '\0';

    /* Trim leading whitespace left behind after stripping */
    size_t trim = 0;
    while (trim < w && (buf[trim] == ' ' || buf[trim] == '\n' || buf[trim] == '\r'))
        trim++;
    if (trim > 0 && trim < w) {
        memmove(buf, buf + trim, w - trim);
        w -= trim;
        buf[w] = '\0';
    }

    return w;
}

/* ── Formal structure stripper (casual channels) ──────────────────────── */

size_t hu_conversation_strip_formal_structure(char *buf, size_t len) {
    if (!buf || len < 3)
        return len;

    size_t w = 0;
    size_t i = 0;
    while (i < len) {
        bool at_line_start = (w == 0 || buf[w - 1] == '\n');

        /* Strip numbered list markers at line start: "1. ", "2) ", etc. */
        if (at_line_start && i + 2 < len && buf[i] >= '1' && buf[i] <= '9') {
            size_t j = i + 1;
            while (j < len && buf[j] >= '0' && buf[j] <= '9')
                j++;
            if (j < len && (buf[j] == '.' || buf[j] == ')') && j + 1 < len && buf[j + 1] == ' ') {
                i = j + 2;
                continue;
            }
        }

        /* Strip bullet markers at line start: "- ", "* " */
        if (at_line_start && i + 1 < len && (buf[i] == '-' || buf[i] == '*') && buf[i + 1] == ' ') {
            i += 2;
            continue;
        }

        /* Strip "Topic: " colon-phrase at line start (capitalized word followed by colon+space).
         * Keeps the text after the colon. */
        if (at_line_start && buf[i] >= 'A' && buf[i] <= 'Z') {
            size_t j = i + 1;
            while (j < len &&
                   ((buf[j] >= 'a' && buf[j] <= 'z') || (buf[j] >= 'A' && buf[j] <= 'Z')))
                j++;
            if (j < len && buf[j] == ':' && j + 1 < len && buf[j + 1] == ' ' && j - i >= 2 &&
                j - i <= 20) {
                i = j + 2;
                continue;
            }
        }

        /* Replace em-dash (UTF-8: E2 80 94) with comma-space */
        if (i + 2 < len && (unsigned char)buf[i] == 0xE2 && (unsigned char)buf[i + 1] == 0x80 &&
            (unsigned char)buf[i + 2] == 0x94) {
            buf[w++] = ',';
            buf[w++] = ' ';
            i += 3;
            while (i < len && buf[i] == ' ')
                i++;
            continue;
        }

        /* Replace en-dash (UTF-8: E2 80 93) with a regular dash */
        if (i + 2 < len && (unsigned char)buf[i] == 0xE2 && (unsigned char)buf[i + 1] == 0x80 &&
            (unsigned char)buf[i + 2] == 0x93) {
            buf[w++] = '-';
            i += 3;
            continue;
        }

        buf[w++] = buf[i++];
    }
    buf[w] = '\0';
    return w;
}

/* ── iMessage effect classifier (keyword-triggered, client-side) ───────── */

/* Classify if a message contains iMessage effect trigger phrases.
 * Returns effect name (static string) or NULL. Effect is applied automatically
 * on recipient's device when the trigger phrase is sent as plain text.
 * Order: longer phrases first to avoid partial matches. */
const char *hu_conversation_classify_effect(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return NULL;

    /* Longer phrases first to avoid partial matches */
    if (hu_str_contains_ci_cstr(msg, msg_len, "happy birthday"))
        return "confetti";
    if (hu_str_contains_ci_cstr(msg, msg_len, "happy new year"))
        return "fireworks";
    if (hu_str_contains_ci_cstr(msg, msg_len, "happy lunar new year"))
        return "fireworks";
    if (hu_str_contains_ci_cstr(msg, msg_len, "happy fourth of july"))
        return "fireworks";
    if (hu_str_contains_ci_cstr(msg, msg_len, "happy 4th of july"))
        return "fireworks";
    if (hu_str_contains_ci_cstr(msg, msg_len, "congratulations") ||
        hu_str_contains_ci_cstr(msg, msg_len, "congrats"))
        return "balloons";
    if (hu_str_contains_ci_cstr(msg, msg_len, "happy halloween"))
        return "echo";
    if (hu_str_contains_ci_cstr(msg, msg_len, "happy thanksgiving"))
        return "confetti";
    if (hu_str_contains_ci_cstr(msg, msg_len, "happy valentine"))
        return "heart";
    if (hu_str_contains_ci_cstr(msg, msg_len, "merry christmas") ||
        hu_str_contains_ci_cstr(msg, msg_len, "happy christmas"))
        return "confetti";
    if (hu_str_contains_ci_cstr(msg, msg_len, "happy hanukkah") ||
        hu_str_contains_ci_cstr(msg, msg_len, "happy chanukah"))
        return "confetti";
    if (hu_str_contains_ci_cstr(msg, msg_len, "happy anniversary"))
        return "heart";
    if (hu_str_contains_ci_cstr(msg, msg_len, "pew pew"))
        return "lasers";
    if (hu_str_contains_ci_cstr(msg, msg_len, "happy graduation") ||
        hu_str_contains_ci_cstr(msg, msg_len, "you graduated"))
        return "confetti";
    if (hu_str_contains_ci_cstr(msg, msg_len, "best wishes"))
        return "confetti";
    if (hu_str_contains_ci_cstr(msg, msg_len, "happy easter"))
        return "confetti";
    if (hu_str_contains_ci_cstr(msg, msg_len, "selamat") ||
        hu_str_contains_ci_cstr(msg, msg_len, "felicidades") ||
        hu_str_contains_ci_cstr(msg, msg_len, "joyeux"))
        return "confetti";

    return NULL;
}

/* ── Media-type awareness ─────────────────────────────────────────────── */

bool hu_conversation_is_media_message(const char *msg, size_t msg_len,
                                      const hu_channel_history_entry_t *entries, size_t count) {
    static const char *markers[] = {
        "[image or attachment]",
        "[Photo shared]",
        "[Attachment shared]",
        "[image]",
        "[photo]",
        "[video]",
        "[attachment]",
        "[Voice Message]",
    };
    size_t n_markers = sizeof(markers) / sizeof(markers[0]);

    if (msg && msg_len > 0) {
        for (size_t m = 0; m < n_markers; m++) {
            if (hu_str_contains_ci_cstr(msg, msg_len, markers[m]))
                return true;
        }
    }

    if (entries && count > 0) {
        const hu_channel_history_entry_t *last = &entries[count - 1];
        if (!last->from_me) {
            const char *t = last->text;
            size_t tl = strlen(t);
            for (size_t m = 0; m < n_markers; m++) {
                if (hu_str_contains_ci_cstr(t, tl, markers[m]))
                    return true;
            }
        }
    }
    return false;
}

/* ── F57: Multi-thread energy management ─────────────────────────────── */

void hu_thread_energy_init(hu_thread_energy_tracker_t *tracker) {
    if (!tracker)
        return;
    memset(tracker, 0, sizeof(*tracker));
}

void hu_thread_energy_update(hu_thread_energy_tracker_t *tracker, const char *contact_id,
                             size_t cid_len, hu_energy_level_t energy, uint64_t now_ms) {
    if (!tracker || !contact_id || cid_len == 0)
        return;
    if (cid_len >= sizeof(tracker->entries[0].contact_id))
        cid_len = sizeof(tracker->entries[0].contact_id) - 1;
    for (size_t i = 0; i < tracker->count; i++) {
        if (strncmp(tracker->entries[i].contact_id, contact_id, cid_len) == 0 &&
            tracker->entries[i].contact_id[cid_len] == '\0') {
            tracker->entries[i].energy = energy;
            tracker->entries[i].last_updated_ms = now_ms;
            return;
        }
    }
    if (tracker->count < HU_MAX_CONCURRENT_CHATS) {
        hu_thread_energy_entry_t *e = &tracker->entries[tracker->count];
        memcpy(e->contact_id, contact_id, cid_len);
        e->contact_id[cid_len] = '\0';
        e->energy = energy;
        e->last_updated_ms = now_ms;
        tracker->count++;
    } else {
        size_t oldest = 0;
        for (size_t i = 1; i < tracker->count; i++) {
            if (tracker->entries[i].last_updated_ms < tracker->entries[oldest].last_updated_ms)
                oldest = i;
        }
        hu_thread_energy_entry_t *e = &tracker->entries[oldest];
        memset(e->contact_id, 0, sizeof(e->contact_id));
        memcpy(e->contact_id, contact_id, cid_len);
        e->contact_id[cid_len] = '\0';
        e->energy = energy;
        e->last_updated_ms = now_ms;
    }
}

hu_energy_level_t hu_thread_energy_get(const hu_thread_energy_tracker_t *tracker,
                                       const char *contact_id, size_t cid_len) {
    if (!tracker || !contact_id || cid_len == 0)
        return HU_ENERGY_NEUTRAL;
    for (size_t i = 0; i < tracker->count; i++) {
        if (strncmp(tracker->entries[i].contact_id, contact_id, cid_len) == 0 &&
            tracker->entries[i].contact_id[cid_len] == '\0')
            return tracker->entries[i].energy;
    }
    return HU_ENERGY_NEUTRAL;
}

size_t hu_thread_energy_build_isolation_hint(const hu_thread_energy_tracker_t *tracker,
                                             const char *contact_id, size_t cid_len, char *buf,
                                             size_t cap) {
    if (!tracker || !contact_id || !buf || cap < 128)
        return 0;
    hu_energy_level_t current = hu_thread_energy_get(tracker, contact_id, cid_len);
    bool has_conflicting = false;
    for (size_t i = 0; i < tracker->count; i++) {
        if (strncmp(tracker->entries[i].contact_id, contact_id, cid_len) == 0 &&
            tracker->entries[i].contact_id[cid_len] == '\0')
            continue;
        if (tracker->entries[i].energy != current &&
            tracker->entries[i].energy != HU_ENERGY_NEUTRAL) {
            has_conflicting = true;
            break;
        }
    }
    if (!has_conflicting)
        return 0;
    int n = snprintf(buf, cap,
                     "[ENERGY ISOLATION]: You have other active conversations at different "
                     "energy levels. Stay in THIS conversation's tone — don't leak.");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* ── Cold restart detection ──────────────────────────────────────────── */

size_t hu_conversation_build_cold_restart_hint(const hu_channel_history_entry_t *entries,
                                               size_t count, char *buf, size_t cap) {
    if (!entries || count < 2 || !buf || cap < 64)
        return 0;

    /* Find last two messages: the most recent inbound and the one before it.
     * If timestamp gap > 4 hours, this is a cold restart. */
    int last_their = -1;
    int prev_any = -1;
    for (int i = (int)count - 1; i >= 0; i--) {
        if (!entries[i].from_me && last_their < 0) {
            last_their = i;
        } else if (last_their >= 0 && prev_any < 0) {
            prev_any = i;
            break;
        }
    }
    if (last_their < 0 || prev_any < 0)
        return 0;

    /* Parse "YYYY-MM-DD HH:MM[:SS]" into epoch seconds and take a real delta.
     * The previous (d2 - d1) day-of-month math broke across month/year
     * boundaries: e.g. 05-31 -> 06-01 gave d2-d1 = -30, a large negative gap
     * that the single "+1440 wrapped past midnight" correction could not
     * recover, so the cold-restart hint was wrongly suppressed on the 1st of
     * every month. timegm on both timestamps yields a tz-independent delta. */
    const char *t1 = entries[prev_any].timestamp;
    const char *t2 = entries[last_their].timestamp;
    struct tm tm1, tm2;
    memset(&tm1, 0, sizeof(tm1));
    memset(&tm2, 0, sizeof(tm2));
    if (sscanf(t1, "%d-%d-%d %d:%d", &tm1.tm_year, &tm1.tm_mon, &tm1.tm_mday, &tm1.tm_hour,
               &tm1.tm_min) != 5 ||
        sscanf(t2, "%d-%d-%d %d:%d", &tm2.tm_year, &tm2.tm_mon, &tm2.tm_mday, &tm2.tm_hour,
               &tm2.tm_min) != 5) {
        return 0;
    }
    tm1.tm_year -= 1900;
    tm1.tm_mon -= 1;
    tm2.tm_year -= 1900;
    tm2.tm_mon -= 1;
    time_t e1 = timegm(&tm1);
    time_t e2 = timegm(&tm2);
    if (e1 == (time_t)-1 || e2 == (time_t)-1)
        return 0;
    long gap_minutes = (long)((e2 - e1) / 60);
    if (gap_minutes < 240)
        return 0; /* less than 4 hours (or out of order), not a cold restart */

    int n;
    if (gap_minutes >= 1440) {
        n = snprintf(buf, cap,
                     "[COLD RESTART] It's been over a day since your last exchange. "
                     "Start fresh — don't reference old topics unless they bring it up. "
                     "Open naturally: \"hey\" or respond to what they just said.");
    } else {
        n = snprintf(buf, cap,
                     "[COLD RESTART] Several hours since your last messages. "
                     "Don't pick up mid-thought from before. Respond to what they just said.");
    }
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* ── Self-reaction on own messages ───────────────────────────────────── */

hu_reaction_type_t hu_conversation_classify_self_reaction(const char *msg, size_t msg_len,
                                                          uint32_t seed) {
    if (!msg || msg_len == 0)
        return HU_REACTION_NONE;

    uint32_t s = seed;
    uint32_t roll = reaction_prng_next(&s) % 1000u;

    /* ~2% chance of self-reacting at all */
    if (roll >= 20u)
        return HU_REACTION_NONE;

    /* Self-deprecating humor: haha on own jokes / awkward messages */
    if (hu_str_contains_ci_cstr(msg, msg_len, "lol") ||
        hu_str_contains_ci_cstr(msg, msg_len, "haha") ||
        hu_str_contains_ci_cstr(msg, msg_len, "sorry") ||
        hu_str_contains_ci_cstr(msg, msg_len, "oops") ||
        hu_str_contains_ci_cstr(msg, msg_len, "my bad") ||
        hu_str_contains_ci_cstr(msg, msg_len, "whoops") ||
        hu_str_contains_ci_cstr(msg, msg_len, "nvm") ||
        hu_str_contains_ci_cstr(msg, msg_len, "jk")) {
        return HU_REACTION_HAHA;
    }

    /* Emphasis on own strong statements */
    if (hu_str_contains_ci_cstr(msg, msg_len, "!!!") ||
        hu_str_contains_ci_cstr(msg, msg_len, "i swear") ||
        hu_str_contains_ci_cstr(msg, msg_len, "no way") ||
        hu_str_contains_ci_cstr(msg, msg_len, "deadass")) {
        return HU_REACTION_EMPHASIS;
    }

    return HU_REACTION_NONE;
}

/* ── Group chat participant mention ──────────────────────────────────── */

size_t hu_conversation_build_group_mention_hint(const char *first_name, size_t first_name_len,
                                                bool is_group, char *buf, size_t cap) {
    if (!is_group || !first_name || first_name_len == 0 || !buf || cap < 64)
        return 0;

    int n = snprintf(buf, cap,
                     "[GROUP] The last message was from %.*s. "
                     "You can address them by name occasionally for a natural feel, "
                     "but don't start every message with their name.",
                     (int)(first_name_len > 32 ? 32 : first_name_len), first_name);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* ── Link content awareness ──────────────────────────────────────────── */

size_t hu_conversation_build_link_context(const char *msg, size_t msg_len, char *buf, size_t cap) {
    if (!msg || msg_len == 0 || !buf || cap < 64)
        return 0;

    hu_url_extract_t urls[3];
    size_t url_count = hu_conversation_extract_urls(msg, msg_len, urls, 3);
    if (url_count == 0)
        return 0;

    int n = snprintf(buf, cap,
                     "[LINK SHARED] They sent %s%.*s%s. "
                     "React to the link naturally — comment on the content, not the URL itself. "
                     "If you can't tell what it is, say something like \"ooh what's this\" or "
                     "\"let me check that out\".",
                     url_count == 1 ? "a link: " : "links including: ",
                     (int)(urls[0].len > 60 ? 60 : urls[0].len), urls[0].start,
                     urls[0].len > 60 ? "..." : "");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* ── GIF decision engine ─────────────────────────────────────────────── */

bool hu_conversation_should_send_gif(const char *msg, size_t msg_len,
                                     const hu_channel_history_entry_t *entries, size_t count,
                                     uint32_t seed, float gif_probability) {
    if (!msg || msg_len == 0)
        return false;

    /* Never send GIFs for questions, emotional/serious content, or short greetings */
    if (hu_str_contains_ci_cstr(msg, msg_len, "?"))
        return false;
    if (hu_str_contains_ci_cstr(msg, msg_len, "sad") ||
        hu_str_contains_ci_cstr(msg, msg_len, "sorry") ||
        hu_str_contains_ci_cstr(msg, msg_len, "died") ||
        hu_str_contains_ci_cstr(msg, msg_len, "funeral") ||
        hu_str_contains_ci_cstr(msg, msg_len, "depressed") ||
        hu_str_contains_ci_cstr(msg, msg_len, "anxious") ||
        hu_str_contains_ci_cstr(msg, msg_len, "help me") ||
        hu_str_contains_ci_cstr(msg, msg_len, "crying"))
        return false;

    /* GIF-worthy triggers: humor, excitement, reactions */
    bool gif_worthy = false;
    if (hu_str_contains_ci_cstr(msg, msg_len, "lol") ||
        hu_str_contains_ci_cstr(msg, msg_len, "lmao") ||
        hu_str_contains_ci_cstr(msg, msg_len, "haha") ||
        hu_str_contains_ci_cstr(msg, msg_len, "omg") ||
        hu_str_contains_ci_cstr(msg, msg_len, "no way") ||
        hu_str_contains_ci_cstr(msg, msg_len, "bruh") ||
        hu_str_contains_ci_cstr(msg, msg_len, "i can't") ||
        hu_str_contains_ci_cstr(msg, msg_len, "dead") ||
        hu_str_contains_ci_cstr(msg, msg_len, "mood") ||
        hu_str_contains_ci_cstr(msg, msg_len, "same") ||
        hu_str_contains_ci_cstr(msg, msg_len, "😂") ||
        hu_str_contains_ci_cstr(msg, msg_len, "💀") ||
        hu_str_contains_ci_cstr(msg, msg_len, "exactly") ||
        hu_str_contains_ci_cstr(msg, msg_len, "yesss"))
        gif_worthy = true;

    /* Celebration triggers */
    if (hu_str_contains_ci_cstr(msg, msg_len, "let's go") ||
        hu_str_contains_ci_cstr(msg, msg_len, "wooo") ||
        hu_str_contains_ci_cstr(msg, msg_len, "hell yeah") ||
        hu_str_contains_ci_cstr(msg, msg_len, "we did it") ||
        hu_str_contains_ci_cstr(msg, msg_len, "finally"))
        gif_worthy = true;

    if (!gif_worthy)
        return false;

    /* Don't send GIFs too often — check recent history for our GIF sends */
    if (entries && count > 0) {
        size_t recent_gifs = 0;
        size_t check = count > 6 ? 6 : count;
        for (size_t i = count - check; i < count; i++) {
            if (entries[i].from_me &&
                hu_str_contains_ci_cstr(entries[i].text, strlen(entries[i].text), "[GIF]"))
                recent_gifs++;
        }
        if (recent_gifs >= 1)
            return false;
    }

    /* Probability roll */
    uint32_t s = seed;
    uint32_t roll = reaction_prng_next(&s) % 100u;
    uint32_t threshold = (uint32_t)(gif_probability * 100.0f);
    if (threshold > 100)
        threshold = 100;
    return roll < threshold;
}

size_t hu_conversation_build_gif_search_prompt(const char *msg, size_t msg_len, char *buf,
                                               size_t cap) {
    if (!msg || msg_len == 0 || !buf || cap < 64)
        return 0;

    int n = snprintf(buf, cap,
                     "Pick a short, funny GIF search query (2-4 words) that would be a perfect "
                     "reaction to: \"%.*s\". Return ONLY the search query, nothing else.",
                     (int)(msg_len > 120 ? 120 : msg_len), msg);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* ── Contact-aware GIF probability (relationship-based) ──────────────── */

float hu_conversation_adjust_gif_probability(float base_probability, const char *relationship_type,
                                             size_t rel_len) {
    if (base_probability < 0.0f)
        base_probability = 0.0f;
    if (base_probability > 1.0f)
        base_probability = 1.0f;
    if (!relationship_type || rel_len == 0)
        return base_probability;

    float result = base_probability;

    if (hu_str_contains_ci_cstr(relationship_type, rel_len, "friend") ||
        hu_str_contains_ci_cstr(relationship_type, rel_len, "bestie") ||
        hu_str_contains_ci_cstr(relationship_type, rel_len, "sibling"))
        result = base_probability * 1.5f;
    else if (hu_str_contains_ci_cstr(relationship_type, rel_len, "partner") ||
             hu_str_contains_ci_cstr(relationship_type, rel_len, "spouse"))
        result = base_probability * 1.3f;
    else if (hu_str_contains_ci_cstr(relationship_type, rel_len, "coworker") ||
             hu_str_contains_ci_cstr(relationship_type, rel_len, "colleague") ||
             hu_str_contains_ci_cstr(relationship_type, rel_len, "boss") ||
             hu_str_contains_ci_cstr(relationship_type, rel_len, "professional"))
        result = base_probability * 0.3f;
    else if (hu_str_contains_ci_cstr(relationship_type, rel_len, "parent") ||
             hu_str_contains_ci_cstr(relationship_type, rel_len, "family"))
        result = base_probability * 0.5f;
    else if (hu_str_contains_ci_cstr(relationship_type, rel_len, "acquaintance"))
        result = base_probability * 0.2f;

    if (result > 1.0f)
        result = 1.0f;
    if (result < 0.0f)
        result = 0.0f;
    return result;
}

size_t hu_conversation_build_gif_style_hint(const char *relationship_type, size_t rel_len,
                                            char *buf, size_t cap) {
    if (!buf || cap < 32)
        return 0;

    const char *style = "funny reaction";
    if (relationship_type && rel_len > 0) {
        if (hu_str_contains_ci_cstr(relationship_type, rel_len, "friend") ||
            hu_str_contains_ci_cstr(relationship_type, rel_len, "bestie"))
            style = "absurd meme or chaotic reaction";
        else if (hu_str_contains_ci_cstr(relationship_type, rel_len, "partner") ||
                 hu_str_contains_ci_cstr(relationship_type, rel_len, "spouse"))
            style = "cute or flirty reaction";
        else if (hu_str_contains_ci_cstr(relationship_type, rel_len, "parent") ||
                 hu_str_contains_ci_cstr(relationship_type, rel_len, "family"))
            style = "wholesome or warm reaction";
        else if (hu_str_contains_ci_cstr(relationship_type, rel_len, "coworker"))
            style = "professional but lighthearted";
    }

    int n = snprintf(buf, cap, "GIF style: %s", style);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* ── GIF rate limiter (per-contact, in-memory ring) ──────────────────── */

#define HU_GIF_RATE_MAX_CONTACTS 32

static struct {
    char contact_id[128];
    uint64_t last_gif_ms;
    uint32_t gif_count_today;
    uint64_t day_start_ms;
} gif_rate_slots[HU_GIF_RATE_MAX_CONTACTS];
static size_t gif_rate_count;

bool hu_conversation_gif_rate_allow(const char *contact_id, size_t cid_len, uint64_t now_ms,
                                    uint32_t max_per_day, uint64_t min_gap_ms) {
    if (!contact_id || cid_len == 0)
        return true;
    if (max_per_day == 0)
        max_per_day = 5;
    if (min_gap_ms == 0)
        min_gap_ms = 600000; /* 10 minutes */

    size_t slot = SIZE_MAX;
    for (size_t i = 0; i < gif_rate_count; i++) {
        if (strncmp(gif_rate_slots[i].contact_id, contact_id, cid_len < 127 ? cid_len : 127) == 0 &&
            gif_rate_slots[i].contact_id[cid_len < 127 ? cid_len : 127] == '\0') {
            slot = i;
            break;
        }
    }

    if (slot == SIZE_MAX)
        return true; /* first GIF to this contact is always allowed */

    /* Reset daily count if new day (86400000 ms = 24h) */
    if (now_ms - gif_rate_slots[slot].day_start_ms > 86400000) {
        gif_rate_slots[slot].gif_count_today = 0;
        gif_rate_slots[slot].day_start_ms = now_ms;
    }

    if (gif_rate_slots[slot].gif_count_today >= max_per_day)
        return false;

    if (now_ms - gif_rate_slots[slot].last_gif_ms < min_gap_ms)
        return false;

    return true;
}

void hu_conversation_gif_rate_record(const char *contact_id, size_t cid_len, uint64_t now_ms) {
    if (!contact_id || cid_len == 0)
        return;

    size_t slot = SIZE_MAX;
    for (size_t i = 0; i < gif_rate_count; i++) {
        if (strncmp(gif_rate_slots[i].contact_id, contact_id, cid_len < 127 ? cid_len : 127) == 0 &&
            gif_rate_slots[i].contact_id[cid_len < 127 ? cid_len : 127] == '\0') {
            slot = i;
            break;
        }
    }

    if (slot == SIZE_MAX) {
        if (gif_rate_count >= HU_GIF_RATE_MAX_CONTACTS) {
            slot = 0;
            for (size_t i = 1; i < gif_rate_count; i++) {
                if (gif_rate_slots[i].last_gif_ms < gif_rate_slots[slot].last_gif_ms)
                    slot = i;
            }
        } else
            slot = gif_rate_count++;
        size_t copy = cid_len < 127 ? cid_len : 127;
        memcpy(gif_rate_slots[slot].contact_id, contact_id, copy);
        gif_rate_slots[slot].contact_id[copy] = '\0';
        gif_rate_slots[slot].day_start_ms = now_ms;
        gif_rate_slots[slot].gif_count_today = 0;
    }

    gif_rate_slots[slot].last_gif_ms = now_ms;
    gif_rate_slots[slot].gif_count_today++;
}

/* ── Seen behavior modeling (read receipt delay) ─────────────────────── */

hu_seen_action_t hu_conversation_classify_seen_behavior(const char *msg, size_t msg_len,
                                                        uint8_t hour_local, uint32_t seed,
                                                        uint32_t *out_delay_ms) {
    if (!msg || msg_len == 0) {
        if (out_delay_ms)
            *out_delay_ms = 0;
        return HU_SEEN_RESPOND_NOW;
    }
    if (out_delay_ms)
        *out_delay_ms = 0;

    uint32_t s = seed;
    uint32_t roll = reaction_prng_next(&s) % 100u;

    /* During busy hours (9-17), higher chance of "seen but respond later" */
    bool busy_hours = (hour_local >= 9 && hour_local <= 17);

    /* Low-priority messages: "ok", "k", "sounds good", thumbs up only */
    bool low_priority = (msg_len <= 3) || hu_str_contains_ci_cstr(msg, msg_len, "sounds good") ||
                        hu_str_contains_ci_cstr(msg, msg_len, "👍");

    if (low_priority && busy_hours && roll < 15) {
        if (out_delay_ms)
            *out_delay_ms = 300000 + (reaction_prng_next(&s) % 600000); /* 5-15 min */
        return HU_SEEN_DELAY_THEN_RESPOND;
    }

    /* Non-urgent messages during busy hours: 5% chance of delay */
    if (busy_hours && !hu_str_contains_ci_cstr(msg, msg_len, "?") &&
        !hu_str_contains_ci_cstr(msg, msg_len, "urgent") &&
        !hu_str_contains_ci_cstr(msg, msg_len, "asap") && roll < 5) {
        if (out_delay_ms)
            *out_delay_ms = 180000 + (reaction_prng_next(&s) % 420000); /* 3-10 min */
        return HU_SEEN_DELAY_THEN_RESPOND;
    }

    return HU_SEEN_RESPOND_NOW;
}

/* ── GIF humor calibration (per-contact feedback loop) ───────────────── */

#define HU_GIF_CAL_MAX_CONTACTS 32
#define HU_GIF_CAL_HISTORY      8

static struct {
    char contact_id[128];
    uint8_t sent_count;
    uint8_t reacted_count;
    char last_queries[HU_GIF_CAL_HISTORY][64];
    bool last_got_reaction[HU_GIF_CAL_HISTORY];
    uint8_t history_pos;
} gif_cal_slots[HU_GIF_CAL_MAX_CONTACTS];
static size_t gif_cal_count;

void hu_conversation_gif_cal_record_send(const char *contact_id, size_t cid_len, const char *query,
                                         size_t query_len) {
    if (!contact_id || cid_len == 0)
        return;

    size_t slot = SIZE_MAX;
    for (size_t i = 0; i < gif_cal_count; i++) {
        size_t n = cid_len < 127 ? cid_len : 127;
        if (strncmp(gif_cal_slots[i].contact_id, contact_id, n) == 0 &&
            gif_cal_slots[i].contact_id[n] == '\0') {
            slot = i;
            break;
        }
    }
    if (slot == SIZE_MAX) {
        if (gif_cal_count >= HU_GIF_CAL_MAX_CONTACTS) {
            /* Evict the contact with the fewest sends (least calibration data) */
            slot = 0;
            for (size_t i = 1; i < HU_GIF_CAL_MAX_CONTACTS; i++) {
                if (gif_cal_slots[i].sent_count < gif_cal_slots[slot].sent_count)
                    slot = i;
            }
        } else {
            slot = gif_cal_count++;
        }
        size_t n = cid_len < 127 ? cid_len : 127;
        memcpy(gif_cal_slots[slot].contact_id, contact_id, n);
        gif_cal_slots[slot].contact_id[n] = '\0';
        gif_cal_slots[slot].sent_count = 0;
        gif_cal_slots[slot].reacted_count = 0;
        gif_cal_slots[slot].history_pos = 0;
    }

    uint8_t pos = gif_cal_slots[slot].history_pos % HU_GIF_CAL_HISTORY;
    size_t qn = query_len < 63 ? query_len : 63;
    if (query && qn > 0)
        memcpy(gif_cal_slots[slot].last_queries[pos], query, qn);
    gif_cal_slots[slot].last_queries[pos][qn] = '\0';
    gif_cal_slots[slot].last_got_reaction[pos] = false;
    gif_cal_slots[slot].history_pos++;
    gif_cal_slots[slot].sent_count++;
}

void hu_conversation_gif_cal_record_reaction(const char *contact_id, size_t cid_len) {
    if (!contact_id || cid_len == 0)
        return;

    for (size_t i = 0; i < gif_cal_count; i++) {
        size_t n = cid_len < 127 ? cid_len : 127;
        if (strncmp(gif_cal_slots[i].contact_id, contact_id, n) == 0 &&
            gif_cal_slots[i].contact_id[n] == '\0') {
            gif_cal_slots[i].reacted_count++;
            if (gif_cal_slots[i].history_pos > 0) {
                uint8_t prev = (gif_cal_slots[i].history_pos - 1) % HU_GIF_CAL_HISTORY;
                gif_cal_slots[i].last_got_reaction[prev] = true;
            }
            return;
        }
    }
}

float hu_conversation_gif_cal_hit_rate(const char *contact_id, size_t cid_len) {
    if (!contact_id || cid_len == 0)
        return 0.5f;

    for (size_t i = 0; i < gif_cal_count; i++) {
        size_t n = cid_len < 127 ? cid_len : 127;
        if (strncmp(gif_cal_slots[i].contact_id, contact_id, n) == 0 &&
            gif_cal_slots[i].contact_id[n] == '\0') {
            if (gif_cal_slots[i].sent_count < 3)
                return 0.5f;
            return (float)gif_cal_slots[i].reacted_count / (float)gif_cal_slots[i].sent_count;
        }
    }
    return 0.5f;
}

/* ── Reaction-to-their-reaction awareness ────────────────────────────── */

size_t hu_conversation_build_reaction_received_hint(const hu_channel_history_entry_t *entries,
                                                    size_t count, char *buf, size_t cap) {
    if (!entries || count < 2 || !buf || cap < 32)
        return 0;

    size_t reactions_on_ours = 0;
    for (size_t i = 0; i < count; i++) {
        if (!entries[i].from_me &&
            (hu_str_contains_ci_cstr(entries[i].text, strlen(entries[i].text), "Loved") ||
             hu_str_contains_ci_cstr(entries[i].text, strlen(entries[i].text), "Liked") ||
             hu_str_contains_ci_cstr(entries[i].text, strlen(entries[i].text), "Laughed at") ||
             hu_str_contains_ci_cstr(entries[i].text, strlen(entries[i].text), "Emphasized") ||
             hu_str_contains_ci_cstr(entries[i].text, strlen(entries[i].text), "Questioned")))
            reactions_on_ours++;
    }

    if (reactions_on_ours == 0)
        return 0;

    int n = snprintf(buf, cap,
                     "[TAPBACK RECEIVED] They reacted to %zu of your recent messages. "
                     "Don't explicitly acknowledge tapbacks — they're passive acknowledgment. "
                     "But take it as a positive signal that the conversation is going well.",
                     reactions_on_ours);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* ── Emoji frequency matching directive builder ──────────────────────── */

size_t hu_conversation_build_emoji_mirror_hint(const hu_channel_history_entry_t *entries,
                                               size_t count, char *buf, size_t cap) {
    if (!entries || count == 0 || !buf || cap < 32)
        return 0;

    size_t their_msgs = 0;
    size_t their_emoji_msgs = 0;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].from_me)
            continue;
        their_msgs++;
        size_t tlen = strlen(entries[i].text);
        for (size_t j = 0; j < tlen; j++) {
            if ((unsigned char)entries[i].text[j] > 127) {
                their_emoji_msgs++;
                break;
            }
        }
    }

    if (their_msgs < 2)
        return 0;

    float emoji_rate = (float)their_emoji_msgs / (float)their_msgs;
    const char *directive;
    if (emoji_rate > 0.6f)
        directive = "[EMOJI MATCH] They use emoji frequently — feel free to use emoji naturally. "
                    "Match their energy level.";
    else if (emoji_rate < 0.1f)
        directive = "[EMOJI MATCH] They rarely use emoji — keep your responses mostly text-based. "
                    "An occasional emoji is fine but don't overdo it.";
    else
        return 0; /* middle ground — no special directive needed */

    int n = snprintf(buf, cap, "%s", directive);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* ── Message edit/unsend awareness ───────────────────────────────────── */

size_t hu_conversation_build_edit_awareness_hint(bool message_was_edited, bool message_was_unsent,
                                                 char *buf, size_t cap) {
    if (!buf || cap < 32)
        return 0;

    if (message_was_unsent) {
        int n = snprintf(buf, cap,
                         "[MESSAGE UNSENT] They deleted a message. DO NOT mention it or ask what "
                         "they said. Pretend you never saw it — this is crucial social etiquette.");
        return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
    }

    if (message_was_edited) {
        int n = snprintf(buf, cap,
                         "[MESSAGE EDITED] They corrected a message. Respond to the corrected "
                         "version only. Don't point out the edit.");
        return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
    }

    return 0;
}

/* ── Sticker-like reaction images ────────────────────────────────────── */

typedef struct {
    const char *category;
    const char *filename;
    const char *const *triggers;
} hu_sticker_def_t;

static const char *const sticker_celebration[] = {"congrats",    "amazing",  "proud",     "won",
                                                  "got the job", "accepted", "graduated", NULL};
static const char *const sticker_laughing[] = {
    "lmao", "lol", "dying", "dead", "hilarious", "haha", "rofl", "crying laughing", NULL};
static const char *const sticker_heart[] = {"love you",        "miss you",        "ily", "i love",
                                            "thinking of you", "you're the best", NULL};
static const char *const sticker_mind_blown[] = {
    "no way", "what the", "mind blown", "insane", "unbelievable", "can't believe", "omg", NULL};
static const char *const sticker_thumbs_up[] = {"sounds good", "perfect", "deal",  "got it",
                                                "bet",         "will do", "on it", NULL};
static const char *const sticker_sad[] = {"sorry to hear", "that sucks", "feel better", "rough day",
                                          "ugh",           "worst",      NULL};
static const char *const sticker_excited[] = {"can't wait", "so excited", "let's go", "pumped",
                                              "yesss",      "finally",    NULL};

static const hu_sticker_def_t sticker_defs[] = {
    {"celebration", "celebration.png", sticker_celebration},
    {"laughing", "laughing.png", sticker_laughing},
    {"heart", "heart.png", sticker_heart},
    {"mind-blown", "mind-blown.png", sticker_mind_blown},
    {"thumbs-up", "thumbs-up.png", sticker_thumbs_up},
    {"sad", "sad.png", sticker_sad},
    {"excited", "excited.png", sticker_excited},
};

#define HU_STICKER_DEF_COUNT (sizeof(sticker_defs) / sizeof(sticker_defs[0]))

bool hu_conversation_should_send_sticker(const char *msg, size_t msg_len, const char *last_response,
                                         size_t resp_len, uint32_t seed, float probability) {
    if (!msg || msg_len == 0 || probability <= 0.0f)
        return false;

    /* Only send stickers in response to emotionally expressive messages */
    bool has_trigger = false;
    for (size_t d = 0; d < HU_STICKER_DEF_COUNT && !has_trigger; d++) {
        for (const char *const *t = sticker_defs[d].triggers; *t && !has_trigger; t++) {
            if (hu_str_contains_ci_cstr(msg, msg_len, *t))
                has_trigger = true;
        }
    }
    if (!has_trigger)
        return false;

    /* Don't double up: skip if we just sent a GIF or sticker */
    if (last_response && resp_len > 0) {
        if (hu_str_contains_ci_cstr(last_response, resp_len, "[GIF]") ||
            hu_str_contains_ci_cstr(last_response, resp_len, "[Sticker]"))
            return false;
    }

    uint32_t s = seed;
    uint32_t roll = reaction_prng_next(&s) % 100u;
    return roll < (uint32_t)(probability * 100.0f);
}

size_t hu_conversation_select_sticker(const char *msg, size_t msg_len, uint32_t seed,
                                      const char *sticker_dir, size_t dir_len, char *out_path,
                                      size_t out_cap) {
    if (!msg || msg_len == 0 || !sticker_dir || dir_len == 0 || !out_path || out_cap < 32)
        return 0;

    /* Find all matching categories */
    size_t matches[HU_STICKER_DEF_COUNT];
    size_t match_count = 0;
    for (size_t d = 0; d < HU_STICKER_DEF_COUNT; d++) {
        for (const char *const *t = sticker_defs[d].triggers; *t; t++) {
            if (hu_str_contains_ci_cstr(msg, msg_len, *t)) {
                matches[match_count++] = d;
                break;
            }
        }
    }
    if (match_count == 0)
        return 0;

    /* Pick one at random */
    uint32_t s = seed;
    size_t idx = reaction_prng_next(&s) % match_count;
    const hu_sticker_def_t *pick = &sticker_defs[matches[idx]];

    int n = snprintf(out_path, out_cap, "%.*s/%s", (int)dir_len, sticker_dir, pick->filename);
    if (n > 0 && (size_t)n < out_cap)
        return (size_t)n;
    return 0;
}

/* ── Music teaser (iMessage link previews) ─────────────────────────── */

static bool history_has_recent_music_url(const hu_channel_history_entry_t *entries, size_t count) {
    if (!entries || count == 0)
        return false;
    size_t start = count > 10 ? count - 10 : 0;
    for (size_t i = start; i < count; i++) {
        const char *t = entries[i].text;
        size_t tl = strlen(t);
        if (hu_str_contains_ci_cstr(t, tl, "music.apple.com") ||
            hu_str_contains_ci_cstr(t, tl, "open.spotify.com") ||
            hu_str_contains_ci_cstr(t, tl, "spotify.link"))
            return true;
    }
    return false;
}

bool hu_conversation_should_send_music(const char *incoming, size_t incoming_len,
                                       const hu_channel_history_entry_t *history,
                                       size_t history_count, uint32_t seed, float probability) {
    if (!incoming || incoming_len == 0 || probability <= 0.0f)
        return false;

    /* Skip crisis-heavy moments (music shares would feel tone-deaf). */
    if (hu_str_contains_ci_cstr(incoming, incoming_len, "died") ||
        hu_str_contains_ci_cstr(incoming, incoming_len, "funeral") ||
        hu_str_contains_ci_cstr(incoming, incoming_len, "depressed") ||
        hu_str_contains_ci_cstr(incoming, incoming_len, "help me") ||
        hu_str_contains_ci_cstr(incoming, incoming_len, "crying") ||
        hu_str_contains_ci_cstr(incoming, incoming_len, "suicide"))
        return false;

    if (history_has_recent_music_url(history, history_count))
        return false;

    bool keyword_hit = hu_str_contains_ci_cstr(incoming, incoming_len, "music") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "song") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "songs") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "listen") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "listening") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "playlist") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "album") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "artist") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "tune") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "jam") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "vibe") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "vibes") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "mood") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "spotify") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "apple music") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "track") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "lyrics") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "concert") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "band") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "sing") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "karaoke") ||
                       hu_str_contains_ci_cstr(incoming, incoming_len, "what are you listening") ||
                       /* YouTube / TikTok cues — keep in sync with the cue lists in
                        * hu_inspiration_pick_medium (src/inspiration.c). Word-boundary
                        * matched so "trendy" != "trend", "watchdog" != "watch", etc.
                        * (see ~/.claude/rules/substring-classifier-pitfalls.md). */
                       hu_str_contains_word_ci_n(incoming, incoming_len, "tiktok") ||
                       hu_str_contains_word_ci_n(incoming, incoming_len, "fyp") ||
                       hu_str_contains_word_ci_n(incoming, incoming_len, "for you") ||
                       hu_str_contains_word_ci_n(incoming, incoming_len, "trend") ||
                       hu_str_contains_word_ci_n(incoming, incoming_len, "trending") ||
                       hu_str_contains_word_ci_n(incoming, incoming_len, "reel") ||
                       hu_str_contains_word_ci_n(incoming, incoming_len, "video") ||
                       hu_str_contains_word_ci_n(incoming, incoming_len, "watch") ||
                       hu_str_contains_word_ci_n(incoming, incoming_len, "clip") ||
                       hu_str_contains_word_ci_n(incoming, incoming_len, "youtube") ||
                       hu_str_contains_word_ci_n(incoming, incoming_len, "tutorial") ||
                       hu_str_contains_word_ci_n(incoming, incoming_len, "trailer") ||
                       hu_str_contains_word_ci_n(incoming, incoming_len, "funny");

    float effective = probability;
    if (keyword_hit && effective < 1.0f) {
        effective *= 2.5f;
        if (effective > 1.0f)
            effective = 1.0f;
    }

    uint32_t s = seed;
    uint32_t roll = reaction_prng_next(&s) % 10000u;
    uint32_t threshold = (uint32_t)(effective * 10000.0f);
    if (threshold > 10000u)
        threshold = 10000u;
    return roll < threshold;
}

size_t hu_conversation_build_music_prompt(const char *incoming, size_t incoming_len, char *out,
                                          size_t out_cap) {
    if (!incoming || incoming_len == 0 || !out || out_cap < 128)
        return 0;

    size_t clip = incoming_len > 200 ? 200 : incoming_len;
    int n = snprintf(out, out_cap,
                     "Based on this conversation, suggest ONE song that fits the mood. "
                     "Return ONLY in this format: ARTIST - TITLE | brief casual message\n"
                     "The casual message should feel like a natural text — not a recommendation.\n"
                     "Recent message context: \"%.*s\"",
                     (int)clip, incoming);
    return (n > 0 && (size_t)n < out_cap) ? (size_t)n : 0;
}

/* ── Inline reply context builder ────────────────────────────────────── */

size_t hu_conversation_build_inline_reply_hint(const char *original_text, size_t original_len,
                                               char *buf, size_t cap) {
    if (!original_text || original_len == 0 || !buf || cap < 64)
        return 0;

    /* Truncate the original message if too long */
    size_t show_len = original_len;
    if (show_len > 120)
        show_len = 120;

    int n = snprintf(buf, cap,
                     "[INLINE REPLY] Their message is a reply to: \"%.*s%s\"\n"
                     "Respond in the context of that original message. "
                     "You can reference it naturally but don't quote it back verbatim.",
                     (int)show_len, original_text, (original_len > 120) ? "..." : "");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* ── GIF calibration persistence ─────────────────────────────────────── */

static void fprint_json_escaped(FILE *f, const char *s) {
    if (!f || !s)
        return;
    for (; *s; s++) {
        if (*s == '"')
            fputs("\\\"", f);
        else if (*s == '\\')
            fputs("\\\\", f);
        else if ((unsigned char)*s < 0x20) {
            /* Escape control characters as \uXXXX for valid JSON */
            fprintf(f, "\\u%04x", (unsigned)*s);
        } else {
            fputc(*s, f);
        }
    }
}

hu_error_t hu_conversation_gif_cal_save(const char *path, size_t path_len) {
    if (!path || path_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (gif_cal_count == 0)
        return HU_OK;

    char path_buf[512];
    if (path_len >= sizeof(path_buf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(path_buf, path, path_len);
    path_buf[path_len] = '\0';

    /* GIF calibration data (per-contact preferences). Caller-supplied
     * path, typically ~/.human/conversation_data/gif_calibrations.json;
     * user data, not secret. */
    FILE *f = NULL;
    if (hu_io_secure_open(path_buf, HU_IO_PERM_USER, "w", &f) != HU_OK || !f)
        return HU_ERR_IO;

    fprintf(f, "[\n");
    for (size_t i = 0; i < gif_cal_count; i++) {
        fprintf(f, "  {\"contact\":\"");
        fprint_json_escaped(f, gif_cal_slots[i].contact_id);
        fprintf(f, "\",\"sent\":%u,\"reacted\":%u}%s\n", (unsigned)gif_cal_slots[i].sent_count,
                (unsigned)gif_cal_slots[i].reacted_count, (i + 1 < gif_cal_count) ? "," : "");
    }
    fprintf(f, "]\n");
    fclose(f);
    return HU_OK;
}

hu_error_t hu_conversation_gif_cal_load(const char *path, size_t path_len) {
    if (!path || path_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    char path_buf[512];
    if (path_len >= sizeof(path_buf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(path_buf, path, path_len);
    path_buf[path_len] = '\0';

    FILE *f = fopen(path_buf, "r");
    if (!f)
        return HU_ERR_IO;

    gif_cal_count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f) && gif_cal_count < HU_GIF_CAL_MAX_CONTACTS) {
        const char *cstart = strstr(line, "\"contact\":\"");
        if (!cstart)
            continue;
        cstart += 11;
        /* Find the unescaped closing quote */
        const char *cend = cstart;
        while (*cend && !(*cend == '"' && (cend == cstart || *(cend - 1) != '\\')))
            cend++;
        if (!*cend || cend == cstart)
            continue;
        size_t raw_clen = (size_t)(cend - cstart);

        const char *sstart = strstr(line, "\"sent\":");
        const char *rstart = strstr(line, "\"reacted\":");
        if (!sstart || !rstart)
            continue;

        unsigned sent = 0, reacted = 0;
        if (sscanf(sstart + 7, "%u", &sent) != 1)
            continue;
        if (sscanf(rstart + 10, "%u", &reacted) != 1)
            continue;
        if (sent > 255)
            sent = 255;
        if (reacted > 255)
            reacted = 255;

        /* Unescape contact_id: \" -> ", \\ -> \ */
        size_t idx = gif_cal_count++;
        size_t di = 0;
        for (size_t si = 0; si < raw_clen && di < 126; si++) {
            if (cstart[si] == '\\' && si + 1 < raw_clen &&
                (cstart[si + 1] == '"' || cstart[si + 1] == '\\')) {
                gif_cal_slots[idx].contact_id[di++] = cstart[si + 1];
                si++;
            } else {
                gif_cal_slots[idx].contact_id[di++] = cstart[si];
            }
        }
        gif_cal_slots[idx].contact_id[di] = '\0';
        gif_cal_slots[idx].sent_count = (uint8_t)sent;
        gif_cal_slots[idx].reacted_count = (uint8_t)reacted;
        gif_cal_slots[idx].history_pos = 0;
    }
    fclose(f);
    return HU_OK;
}

/* ── Multi-message splitting (simple chunk-based) ────────────────────── */

/* True when [buf, buf+len) is empty or contains only ASCII whitespace.
 * Used to suppress blank/whitespace-only message bubbles. */
static bool conv_is_blank(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v')
            return false;
    }
    return true;
}

/* Largest prefix length of buf[0..want) that ends on a complete UTF-8
 * codepoint (never splits a multi-byte sequence). Mirrors the contract of
 * hu_mlx_utf8_safe_emit_len without crossing the providers layer boundary.
 * For a prefix already ending on a boundary (or malformed input), returns
 * want unchanged. */
static size_t conv_utf8_safe_len(const char *buf, size_t want) {
    if (want == 0)
        return 0;
    /* Walk back over trailing continuation bytes (0x80..0xBF). */
    size_t i = want;
    size_t cont = 0;
    while (i > 0 && ((unsigned char)buf[i - 1] & 0xC0) == 0x80) {
        i--;
        cont++;
    }
    if (i == 0)
        return want; /* all continuation bytes — malformed, leave as-is */
    unsigned char lead = (unsigned char)buf[i - 1];
    size_t need;
    if ((lead & 0x80) == 0x00)
        need = 1; /* ASCII */
    else if ((lead & 0xE0) == 0xC0)
        need = 2;
    else if ((lead & 0xF0) == 0xE0)
        need = 3;
    else if ((lead & 0xF8) == 0xF0)
        need = 4;
    else
        return want; /* malformed lead byte — leave as-is */
    /* Bytes present for the tail codepoint: the lead plus its continuations. */
    if (1 + cont >= need)
        return want; /* complete codepoint at the tail */
    return i - 1;    /* incomplete — cut before the lead byte */
}

size_t hu_conversation_split_into_texts(const char *response, size_t resp_len, size_t max_chunk,
                                        char chunks[][512], size_t max_chunks) {
    if (!response || resp_len == 0 || !chunks || max_chunks == 0 || max_chunk == 0)
        return 0;
    if (max_chunk > 511)
        max_chunk = 511;

    /* Short messages don't need splitting */
    if (resp_len <= max_chunk) {
        size_t n = resp_len > 511 ? 511 : resp_len;
        n = conv_utf8_safe_len(response, n);
        if (n == 0 || conv_is_blank(response, n))
            return 0; /* blank/whitespace-only — never emit an empty bubble */
        memcpy(chunks[0], response, n);
        chunks[0][n] = '\0';
        return 1;
    }

    size_t count = 0;
    size_t pos = 0;
    while (pos < resp_len && count < max_chunks) {
        size_t remaining = resp_len - pos;
        if (remaining <= max_chunk) {
            size_t n = remaining > 511 ? 511 : remaining;
            memcpy(chunks[count], response + pos, n);
            chunks[count][n] = '\0';
            count++;
            break;
        }

        /* Find sentence boundary near max_chunk (stay within bounds) */
        size_t cut = max_chunk;
        if (pos + cut > resp_len)
            cut = resp_len - pos;
        size_t search = cut;
        while (search > max_chunk / 2) {
            search--;
            char ch = response[pos + search];
            if (ch == '.' || ch == '!' || ch == '?') {
                cut = search + 1; /* include the punctuation */
                break;
            }
        }
        /* Fallback: split at last space */
        if (cut == max_chunk || (pos + cut > resp_len)) {
            if (pos + cut > resp_len)
                cut = resp_len - pos;
            size_t sp = cut;
            while (sp > max_chunk / 2 && response[pos + sp - 1] != ' ')
                sp--;
            if (sp > max_chunk / 2)
                cut = sp;
        }

        size_t n = cut > 511 ? 511 : cut;
        /* Never sever a multi-byte UTF-8 codepoint at the cut point. */
        n = conv_utf8_safe_len(response + pos, n);
        if (n > 0 && !conv_is_blank(response + pos, n)) {
            memcpy(chunks[count], response + pos, n);
            chunks[count][n] = '\0';
            count++;
        }

        /* Skip leading whitespace in next chunk */
        pos += cut;
        while (pos < resp_len && response[pos] == ' ')
            pos++;
    }
    return count;
}

/* ── Channel-class-aware cadence splitter ────────────────────────────── */

/* Counts sentence-ending punctuation (.!?) in [text, text+len).
 * Used by the cadence splitter to decide whether short multi-sentence text
 * is best burst as multiple bubbles. */
static size_t count_sentence_ends(const char *text, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '.' || c == '!' || c == '?')
            n++;
    }
    return n;
}

/* Splits text at sentence boundaries (.!? followed by whitespace or EOS).
 * Trims surrounding whitespace per chunk. Skips empty fragments. Caller
 * supplies the chunks[][512] backing store. Returns chunk count. */
static size_t split_by_sentence_boundaries(const char *text, size_t len, char chunks[][512],
                                           size_t max_chunks) {
    size_t count = 0;
    size_t pos = 0;
    while (pos < len && count < max_chunks) {
        /* Skip leading whitespace */
        while (pos < len && (text[pos] == ' ' || text[pos] == '\t'))
            pos++;
        if (pos >= len)
            break;

        size_t start = pos;
        size_t end = pos;
        while (end < len) {
            char c = text[end];
            if (c == '.' || c == '!' || c == '?') {
                /* Include this punctuation; consume any run of the same kind. */
                end++;
                while (end < len && (text[end] == '.' || text[end] == '!' || text[end] == '?'))
                    end++;
                break;
            }
            end++;
        }

        size_t chunk_len = end - start;
        if (chunk_len > 0) {
            if (chunk_len > 511)
                chunk_len = 511;
            /* Never sever a multi-byte UTF-8 codepoint at the 511 cap. */
            chunk_len = conv_utf8_safe_len(text + start, chunk_len);
            /* Trim trailing whitespace (rare here, but safe). */
            while (chunk_len > 0 &&
                   (text[start + chunk_len - 1] == ' ' || text[start + chunk_len - 1] == '\t'))
                chunk_len--;
            if (chunk_len > 0) {
                memcpy(chunks[count], text + start, chunk_len);
                chunks[count][chunk_len] = '\0';
                count++;
            }
        }
        pos = end;
    }
    return count;
}

size_t hu_conversation_split_for_cadence(const char *text, size_t text_len, hu_channel_class_t cls,
                                         char chunks[][512], size_t max_chunks) {
    if (!text || text_len == 0 || !chunks || max_chunks == 0)
        return 0;

    /* Explicit newlines from the LLM are intentional formatting; do not
     * second-guess them by re-chunking. The caller sends the text as-is
     * (one bubble) in this case. */
    if (memchr(text, '\n', text_len))
        return 0;

    /* Long prose → chunk-based splitter (preserves existing >120-char
     * behavior for every channel class, including TEXT_ASYNC). */
    if (text_len > 120)
        return hu_conversation_split_into_texts(text, text_len, 100, chunks, max_chunks);

    /* TEXT_FAST burst cadence: short multi-sentence replies are bubbled
     * one sentence per bubble. The 40-char floor avoids splitting one-line
     * acknowledgements like "ok cool. see ya." into two trivial bubbles;
     * the ≥2 sentence-ends rule avoids splitting single-statement replies
     * like "sure thing." that happen to contain a period. */
    if (cls == HU_CHANNEL_CLASS_TEXT_FAST && text_len >= 40) {
        if (count_sentence_ends(text, text_len) >= 2) {
            size_t n = split_by_sentence_boundaries(text, text_len, chunks, max_chunks);
            return n >= 2 ? n : 0; /* one-bubble result is no-op */
        }
    }

    /* Otherwise: single bubble. */
    return 0;
}

/* ── Scheduled message queue ─────────────────────────────────────────── */

static hu_sched_slot_t sched_queue[HU_SCHED_MAX];

hu_sched_slot_t *hu_conversation_sched_slot(size_t index) {
    if (index >= HU_SCHED_MAX)
        return NULL;
    return &sched_queue[index];
}

hu_error_t hu_conversation_schedule_message(const char *contact_id, size_t cid_len,
                                            const char *message, size_t msg_len,
                                            uint64_t deliver_at_ms) {
    return hu_conversation_schedule_message_on(contact_id, cid_len, NULL, 0, message, msg_len,
                                               deliver_at_ms);
}

hu_error_t hu_conversation_schedule_message_on(const char *contact_id, size_t cid_len,
                                               const char *channel_name, size_t ch_len,
                                               const char *message, size_t msg_len,
                                               uint64_t deliver_at_ms) {
    if (!contact_id || cid_len == 0 || !message || msg_len == 0 || deliver_at_ms == 0)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < HU_SCHED_MAX; i++) {
        if (!sched_queue[i].active) {
            size_t cn = cid_len < 127 ? cid_len : 127;
            memcpy(sched_queue[i].contact_id, contact_id, cn);
            sched_queue[i].contact_id[cn] = '\0';
            if (channel_name && ch_len > 0) {
                size_t chn = ch_len < 31 ? ch_len : 31;
                memcpy(sched_queue[i].channel_name, channel_name, chn);
                sched_queue[i].channel_name[chn] = '\0';
            } else {
                sched_queue[i].channel_name[0] = '\0';
            }
            size_t mn = msg_len < 511 ? msg_len : 511;
            memcpy(sched_queue[i].message, message, mn);
            sched_queue[i].message[mn] = '\0';
            sched_queue[i].msg_len = mn;
            sched_queue[i].deliver_at_ms = deliver_at_ms;
            sched_queue[i].active = true;
            return HU_OK;
        }
    }
    return HU_ERR_OUT_OF_MEMORY;
}

size_t hu_conversation_flush_scheduled(uint64_t now_ms, char *out_contact, size_t contact_cap,
                                       char *out_message, size_t message_cap) {
    return hu_conversation_flush_scheduled_on(now_ms, out_contact, contact_cap, NULL, 0,
                                              out_message, message_cap);
}

size_t hu_conversation_flush_scheduled_on(uint64_t now_ms, char *out_contact, size_t contact_cap,
                                          char *out_channel, size_t channel_cap, char *out_message,
                                          size_t message_cap) {
    return hu_conversation_flush_scheduled_for(now_ms, NULL, 0, out_contact, contact_cap,
                                               out_channel, channel_cap, out_message, message_cap);
}

size_t hu_conversation_flush_scheduled_for(uint64_t now_ms, const char *channel_filter,
                                           size_t filter_len, char *out_contact, size_t contact_cap,
                                           char *out_channel, size_t channel_cap, char *out_message,
                                           size_t message_cap) {
    if (!out_contact || !out_message || contact_cap == 0 || message_cap == 0)
        return 0;

    for (size_t i = 0; i < HU_SCHED_MAX; i++) {
        if (!sched_queue[i].active || now_ms < sched_queue[i].deliver_at_ms)
            continue;

        /* Channel routing: skip if filter specified and doesn't match */
        if (channel_filter && filter_len > 0 && sched_queue[i].channel_name[0]) {
            size_t stored_len = strlen(sched_queue[i].channel_name);
            if (stored_len != filter_len ||
                memcmp(sched_queue[i].channel_name, channel_filter, filter_len) != 0)
                continue;
        }

        size_t cn = strlen(sched_queue[i].contact_id);
        if (cn >= contact_cap)
            cn = contact_cap - 1;
        memcpy(out_contact, sched_queue[i].contact_id, cn);
        out_contact[cn] = '\0';
        if (out_channel && channel_cap > 0) {
            size_t chn = strlen(sched_queue[i].channel_name);
            if (chn >= channel_cap)
                chn = channel_cap - 1;
            memcpy(out_channel, sched_queue[i].channel_name, chn);
            out_channel[chn] = '\0';
        }
        size_t mn = sched_queue[i].msg_len;
        if (mn >= message_cap)
            mn = message_cap - 1;
        memcpy(out_message, sched_queue[i].message, mn);
        out_message[mn] = '\0';
        sched_queue[i].active = false;
        return mn;
    }
    return 0;
}

/* ── Scheduled message persistence ───────────────────────────────────── */

hu_error_t hu_conversation_sched_save(const char *path, size_t path_len) {
    if (!path || path_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    char path_buf[512];
    if (path_len >= sizeof(path_buf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(path_buf, path, path_len);
    path_buf[path_len] = '\0';

    size_t active_count = 0;
    for (size_t i = 0; i < HU_SCHED_MAX; i++)
        if (sched_queue[i].active)
            active_count++;

    if (active_count == 0) {
        (void)remove(path_buf);
        return HU_OK;
    }

    /* Scheduled messages queue at ~/.human/conversation_data/
     * scheduled.json. User data, not secret. */
    FILE *f = NULL;
    if (hu_io_secure_open(path_buf, HU_IO_PERM_USER, "w", &f) != HU_OK || !f)
        return HU_ERR_IO;

    fprintf(f, "[\n");
    size_t written = 0;
    for (size_t i = 0; i < HU_SCHED_MAX; i++) {
        if (!sched_queue[i].active)
            continue;
        fprintf(f, "  {\"contact\":\"");
        fprint_json_escaped(f, sched_queue[i].contact_id);
        fprintf(f, "\",\"channel\":\"");
        fprint_json_escaped(f, sched_queue[i].channel_name);
        fprintf(f, "\",\"message\":\"");
        fprint_json_escaped(f, sched_queue[i].message);
        fprintf(f, "\",\"deliver_at\":%llu}%s\n", (unsigned long long)sched_queue[i].deliver_at_ms,
                (written + 1 < active_count) ? "," : "");
        written++;
    }
    fprintf(f, "]\n");
    fclose(f);
    return HU_OK;
}

hu_error_t hu_conversation_sched_load(const char *path, size_t path_len) {
    if (!path || path_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    char path_buf[512];
    if (path_len >= sizeof(path_buf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(path_buf, path, path_len);
    path_buf[path_len] = '\0';

    FILE *f = fopen(path_buf, "r");
    if (!f)
        return HU_ERR_IO;

    for (size_t i = 0; i < HU_SCHED_MAX; i++)
        sched_queue[i].active = false;

    char line[768];
    size_t slot = 0;
    while (fgets(line, sizeof(line), f) && slot < HU_SCHED_MAX) {
        const char *cstart = strstr(line, "\"contact\":\"");
        if (!cstart)
            continue;
        cstart += 11;
        const char *cend = strchr(cstart, '"');
        if (!cend)
            continue;

        const char *chstart = strstr(line, "\"channel\":\"");
        const char *mstart = strstr(line, "\"message\":\"");
        const char *dstart = strstr(line, "\"deliver_at\":");
        if (!mstart || !dstart)
            continue;

        size_t cn = (size_t)(cend - cstart);
        if (cn >= 127)
            cn = 127;
        memcpy(sched_queue[slot].contact_id, cstart, cn);
        sched_queue[slot].contact_id[cn] = '\0';

        sched_queue[slot].channel_name[0] = '\0';
        if (chstart) {
            chstart += 11;
            const char *chend = strchr(chstart, '"');
            if (chend) {
                size_t chn = (size_t)(chend - chstart);
                if (chn >= 31)
                    chn = 31;
                memcpy(sched_queue[slot].channel_name, chstart, chn);
                sched_queue[slot].channel_name[chn] = '\0';
            }
        }

        mstart += 11;
        const char *mend = strstr(mstart, "\",\"deliver_at\"");
        if (!mend)
            mend = strchr(mstart, '"');
        if (!mend || mend == mstart)
            continue;

        /* Unescape JSON sequences: \" -> ", \\ -> \, \n -> newline, \uXXXX -> char */
        size_t raw_len = (size_t)(mend - mstart);
        char *dst = sched_queue[slot].message;
        size_t di = 0;
        for (size_t si = 0; si < raw_len && di < 511; si++) {
            if (mstart[si] == '\\' && si + 1 < raw_len) {
                char next = mstart[si + 1];
                if (next == '"' || next == '\\' || next == '/') {
                    dst[di++] = next;
                    si++;
                } else if (next == 'n') {
                    dst[di++] = '\n';
                    si++;
                } else if (next == 't') {
                    dst[di++] = '\t';
                    si++;
                } else if (next == 'r') {
                    dst[di++] = '\r';
                    si++;
                } else if (next == 'u' && si + 5 < raw_len) {
                    /* \uXXXX with UTF-16 surrogate pair support */
                    unsigned cp = 0;
                    bool valid = true;
                    for (int d = 0; d < 4 && valid; d++) {
                        char h = mstart[si + 2 + d];
                        if (h >= '0' && h <= '9')
                            cp = cp * 16 + (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f')
                            cp = cp * 16 + (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F')
                            cp = cp * 16 + (unsigned)(h - 'A' + 10);
                        else
                            valid = false;
                    }
                    if (!valid) {
                        dst[di++] = mstart[si];
                        continue;
                    }
                    size_t consumed = 5;
                    /* Handle UTF-16 surrogate pairs: \uD800-\uDBFF + \uDC00-\uDFFF */
                    if (cp >= 0xD800 && cp <= 0xDBFF && si + 11 < raw_len &&
                        mstart[si + 6] == '\\' && mstart[si + 7] == 'u') {
                        unsigned low = 0;
                        bool low_valid = true;
                        for (int d = 0; d < 4 && low_valid; d++) {
                            char h = mstart[si + 8 + d];
                            if (h >= '0' && h <= '9')
                                low = low * 16 + (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f')
                                low = low * 16 + (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F')
                                low = low * 16 + (unsigned)(h - 'A' + 10);
                            else
                                low_valid = false;
                        }
                        if (low_valid && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            consumed = 11;
                        }
                    }
                    /* Skip lone surrogates (invalid Unicode) */
                    if (cp >= 0xD800 && cp <= 0xDFFF) {
                        si += consumed;
                        continue;
                    }
                    /* Encode to UTF-8, only advance si if write succeeds */
                    if (cp < 0x80 && di < 511) {
                        dst[di++] = (char)cp;
                        si += consumed;
                    } else if (cp < 0x800 && di + 1 <= 510) {
                        dst[di++] = (char)(0xC0 | (cp >> 6));
                        dst[di++] = (char)(0x80 | (cp & 0x3F));
                        si += consumed;
                    } else if (cp < 0x10000 && di + 2 <= 510) {
                        dst[di++] = (char)(0xE0 | (cp >> 12));
                        dst[di++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        dst[di++] = (char)(0x80 | (cp & 0x3F));
                        si += consumed;
                    } else if (cp >= 0x10000 && di + 3 <= 510) {
                        dst[di++] = (char)(0xF0 | (cp >> 18));
                        dst[di++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        dst[di++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        dst[di++] = (char)(0x80 | (cp & 0x3F));
                        si += consumed;
                    } else {
                        break;
                    }
                } else {
                    dst[di++] = mstart[si];
                }
            } else {
                dst[di++] = mstart[si];
            }
        }
        dst[di] = '\0';
        sched_queue[slot].msg_len = di;

        unsigned long long deliver = 0;
        if (sscanf(dstart + 13, "%llu", &deliver) != 1 || deliver == 0)
            continue;
        sched_queue[slot].deliver_at_ms = (uint64_t)deliver;
        sched_queue[slot].active = true;
        slot++;
    }
    fclose(f);
    return HU_OK;
}

/* ── Contact photo resolver (macOS AddressBook SQLite) ────────────────── */

size_t hu_conversation_contact_photo_path(const char *contact_id, size_t cid_len, char *out_path,
                                          size_t out_cap) {
    if (!contact_id || cid_len == 0 || !out_path || out_cap < 64)
        return 0;

#ifdef HU_IS_TEST
    (void)cid_len;
    out_path[0] = '\0';
    return 0;
#elif !defined(HU_ENABLE_SQLITE)
    (void)cid_len;
    out_path[0] = '\0';
    return 0;
#else
    const char *home = getenv("HOME");
    if (!home)
        return 0;

    char db_path[512];
    int dp = snprintf(db_path, sizeof(db_path),
                      "%s/Library/Application Support/AddressBook/AddressBook-v22.abcddb", home);
    if (dp < 0 || (size_t)dp >= sizeof(db_path))
        return 0;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        out_path[0] = '\0';
        return 0;
    }

    /* Join: phone/email -> ZABCDRECORD -> ZABCDLIKENESS to find the image.
     * Contact photos are stored as blobs in ZABCDLIKENESS.ZDATA, but we want the
     * tiff/jpeg file path. macOS also writes contact images to:
     * ~/Library/Application Support/AddressBook/Sources/<uuid>/Images/<ZOWNER hash>
     * We query for the ZOWNER (record ROWID) then construct the path. */
    const char *sql = "SELECT r.Z_PK FROM ZABCDRECORD r "
                      "LEFT JOIN ZABCDPHONENUMBER p ON p.ZOWNER = r.Z_PK "
                      "LEFT JOIN ZABCDEMAILADDRESS e ON e.ZOWNER = r.Z_PK "
                      "WHERE p.ZFULLNUMBER LIKE ? OR e.ZADDRESS LIKE ? "
                      "LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        out_path[0] = '\0';
        return 0;
    }

    char like_pat[140];
    size_t safe_len = cid_len > 128 ? 128 : cid_len;
    int lp = snprintf(like_pat, sizeof(like_pat), "%%%.*s%%", (int)safe_len, contact_id);
    if (lp < 0 || (size_t)lp >= sizeof(like_pat)) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        out_path[0] = '\0';
        return 0;
    }
    sqlite3_bind_text(stmt, 1, like_pat, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, like_pat, -1, SQLITE_STATIC);

    int64_t record_pk = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        record_pk = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    if (record_pk < 0) {
        sqlite3_close(db);
        out_path[0] = '\0';
        return 0;
    }

    /* Try legacy path first: ~/Library/Application Support/AddressBook/Images/ABImage-<PK> */
    int n = snprintf(out_path, out_cap,
                     "%s/Library/Application Support/AddressBook/Images/ABImage-%lld", home,
                     (long long)record_pk);
    if (n > 0 && (size_t)n < out_cap && access(out_path, R_OK) == 0) {
        sqlite3_close(db);
        return (size_t)n;
    }

    /* Modern macOS: try Sources/<uuid>/Images/<PK> for each source directory */
    {
        char sources_dir[512];
        int sd = snprintf(sources_dir, sizeof(sources_dir),
                          "%s/Library/Application Support/AddressBook/Sources", home);
        if (sd > 0 && (size_t)sd < sizeof(sources_dir)) {
            DIR *dir = opendir(sources_dir);
            if (dir) {
                struct dirent *ent;
                while ((ent = readdir(dir)) != NULL) {
                    if (ent->d_name[0] == '.')
                        continue;
                    n = snprintf(out_path, out_cap, "%s/%s/Images/%lld", sources_dir, ent->d_name,
                                 (long long)record_pk);
                    if (n > 0 && (size_t)n < out_cap && access(out_path, R_OK) == 0) {
                        closedir(dir);
                        sqlite3_close(db);
                        return (size_t)n;
                    }
                }
                closedir(dir);
            }
        }
    }

    /* Fallback: extract ZDATA blob from ZABCDLIKENESS to a cache file */
    {
        const char *blob_sql = "SELECT ZDATA FROM ZABCDLIKENESS WHERE ZOWNER = ? "
                               "AND ZDATA IS NOT NULL LIMIT 1";
        sqlite3_stmt *blob_stmt = NULL;
        if (sqlite3_prepare_v2(db, blob_sql, -1, &blob_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(blob_stmt, 1, record_pk);
            if (sqlite3_step(blob_stmt) == SQLITE_ROW) {
                const void *blob = sqlite3_column_blob(blob_stmt, 0);
                int blob_len = sqlite3_column_bytes(blob_stmt, 0);
                if (blob && blob_len > 0) {
                    char cache_dir[512];
                    int cd = snprintf(cache_dir, sizeof(cache_dir),
                                      "%s/.human/cache/contact-photos", home);
                    if (cd > 0 && (size_t)cd < sizeof(cache_dir)) {
                        (void)mkdir(cache_dir, 0700);
                        n = snprintf(out_path, out_cap, "%s/%lld.jpg", cache_dir,
                                     (long long)record_pk);
                        if (n > 0 && (size_t)n < out_cap) {
                            /* Contact photo cache at
                             * ~/.human/cache/contact-photos/{pk}.jpg.
                             * Photos are personal data; 0600. */
                            FILE *cf = NULL;
                            if (hu_io_secure_open(out_path, HU_IO_PERM_SECRET, "wb", &cf) ==
                                    HU_OK &&
                                cf) {
                                fwrite(blob, 1, (size_t)blob_len, cf);
                                fclose(cf);
                                sqlite3_finalize(blob_stmt);
                                sqlite3_close(db);
                                return (size_t)n;
                            }
                        }
                    }
                }
            }
            sqlite3_finalize(blob_stmt);
        }
    }

    sqlite3_close(db);
    out_path[0] = '\0';
    return 0;
#endif
}
