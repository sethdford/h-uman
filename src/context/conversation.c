#include "human/context/conversation.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/data/loader.h"
#ifdef HU_HAS_PERSONA
#include "human/persona.h"
#endif
#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/emotional_moments.h"
#include <sqlite3.h>
#endif
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
        if (item && item->type == HU_JSON_STRING) {
            result[i] = item->data.string.ptr;
        } else {
            result[i] = "";
        }
    }

    *out_array = result;
    *out_len = arr_len;
    return HU_OK;
}

/* Load contractions from JSON. Expected format: {"contractions": [{"from": "X", "to": "Y"}, ...]} */
static hu_error_t load_contractions(const hu_json_value_t *obj) {
    hu_json_value_t *field = hu_json_object_get(obj, "contractions");
    if (!field || field->type != HU_JSON_ARRAY)
        return HU_ERR_NOT_FOUND;

    size_t arr_len = field->data.array.len;
    if (arr_len == 0)
        return HU_OK;

    hu_conversation_contraction_t *result =
        (hu_conversation_contraction_t *)s_conv_alloc->alloc(s_conv_alloc->ctx, arr_len * sizeof(hu_conversation_contraction_t));
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;

    for (size_t i = 0; i < arr_len; i++) {
        hu_json_value_t *item = field->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;

        const char *from = hu_json_get_string(item, "from");
        const char *to = hu_json_get_string(item, "to");

        if (from && to) {
            result[i].from = from;
            result[i].from_len = strlen(from);
            result[i].to = to;
            result[i].to_len = strlen(to);
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
        hu_error_t err = hu_data_load(alloc, "conversation/ai_disclosure_patterns.json", &json_data, &json_len);
        if (err == HU_OK) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, json_data, json_len, &root);
            if (err == HU_OK && root) {
                load_string_array(root, "patterns", &s_ai_disclosure_patterns, &s_ai_disclosure_patterns_len);
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
        hu_error_t err = hu_data_load(alloc, "conversation/filler_words.json", &json_data, &json_len);
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
        hu_error_t err = hu_data_load(alloc, "conversation/contractions.json", &json_data, &json_len);
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
        hu_error_t err = hu_data_load(alloc, "conversation/conversation_intros.json", &json_data, &json_len);
        if (err == HU_OK) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, json_data, json_len, &root);
            if (err == HU_OK && root) {
                load_string_array(root, "intros", &s_conversation_intros, &s_conversation_intros_len);
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
        hu_error_t err = hu_data_load(alloc, "conversation/backchannel_phrases.json", &json_data, &json_len);
        if (err == HU_OK) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, json_data, json_len, &root);
            if (err == HU_OK && root) {
                load_string_array(root, "phrases", &s_backchannel_phrases, &s_backchannel_phrases_len);
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
        hu_error_t err = hu_data_load(alloc, "conversation/emotional_words.json", &json_data, &json_len);
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
        hu_error_t err = hu_data_load(alloc, "conversation/crisis_keywords.json", &json_data, &json_len);
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
        hu_error_t err = hu_data_load(alloc, "conversation/personal_sharing.json", &json_data, &json_len);
        if (err == HU_OK) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, json_data, json_len, &root);
            if (err == HU_OK && root) {
                load_string_array(root, "phrases", &s_personal_sharing_phrases, &s_personal_sharing_phrases_len);
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
        hu_error_t err = hu_data_load(alloc, "conversation/engage_words.json", &json_data, &json_len);
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

void hu_conversation_data_cleanup(void) {
    if (!s_conv_alloc)
        return;

    if (s_ai_disclosure_patterns) {
        s_conv_alloc->free(s_conv_alloc->ctx, s_ai_disclosure_patterns, s_ai_disclosure_patterns_len * sizeof(const char *));
        s_ai_disclosure_patterns = NULL;
        s_ai_disclosure_patterns_len = 0;
    }

    if (s_filler_words) {
        s_conv_alloc->free(s_conv_alloc->ctx, s_filler_words, s_filler_words_len * sizeof(const char *));
        s_filler_words = NULL;
        s_filler_words_len = 0;
    }

    if (s_contractions) {
        s_conv_alloc->free(s_conv_alloc->ctx, s_contractions, s_contractions_len * sizeof(hu_conversation_contraction_t));
        s_contractions = NULL;
        s_contractions_len = 0;
    }

    if (s_conversation_intros) {
        s_conv_alloc->free(s_conv_alloc->ctx, s_conversation_intros, s_conversation_intros_len * sizeof(const char *));
        s_conversation_intros = NULL;
        s_conversation_intros_len = 0;
    }

    if (s_starters) {
        s_conv_alloc->free(s_conv_alloc->ctx, s_starters, s_starters_len * sizeof(const char *));
        s_starters = NULL;
        s_starters_len = 0;
    }

    if (s_backchannel_phrases) {
        s_conv_alloc->free(s_conv_alloc->ctx, s_backchannel_phrases, s_backchannel_phrases_len * sizeof(const char *));
        s_backchannel_phrases = NULL;
        s_backchannel_phrases_len = 0;
    }

    if (s_emotional_words) {
        s_conv_alloc->free(s_conv_alloc->ctx, s_emotional_words, s_emotional_words_len * sizeof(const char *));
        s_emotional_words = NULL;
        s_emotional_words_len = 0;
    }

    if (s_positive_words) {
        s_conv_alloc->free(s_conv_alloc->ctx, s_positive_words, s_positive_words_len * sizeof(const char *));
        s_positive_words = NULL;
        s_positive_words_len = 0;
    }

    if (s_negative_words) {
        s_conv_alloc->free(s_conv_alloc->ctx, s_negative_words, s_negative_words_len * sizeof(const char *));
        s_negative_words = NULL;
        s_negative_words_len = 0;
    }

    if (s_crisis_keywords) {
        s_conv_alloc->free(s_conv_alloc->ctx, s_crisis_keywords, s_crisis_keywords_len * sizeof(const char *));
        s_crisis_keywords = NULL;
        s_crisis_keywords_len = 0;
    }

    if (s_personal_sharing_phrases) {
        s_conv_alloc->free(s_conv_alloc->ctx, s_personal_sharing_phrases, s_personal_sharing_phrases_len * sizeof(const char *));
        s_personal_sharing_phrases = NULL;
        s_personal_sharing_phrases_len = 0;
    }

    if (s_engage_words) {
        s_conv_alloc->free(s_conv_alloc->ctx, s_engage_words, s_engage_words_len * sizeof(const char *));
        s_engage_words = NULL;
        s_engage_words_len = 0;
    }

    s_conv_alloc = NULL;
}

/* Safe pos advance: snprintf returns the would-be length even when truncated.
 * Clamp to remaining buffer capacity to prevent out-of-bounds writes. */
#define POS_ADVANCE(w, pos, cap)       \
    do {                               \
        if ((w) > 0) {                 \
            size_t _add = (size_t)(w); \
            if (_add > (cap) - (pos))  \
                _add = (cap) - (pos);  \
            (pos) += _add;             \
        }                              \
    } while (0)

static bool str_contains_ci(const char *haystack, size_t hlen, const char *needle)
    __attribute__((unused));
static bool str_contains_ci(const char *haystack, size_t hlen, const char *needle) {
    if (!haystack || !needle)
        return false;
    size_t nlen = strlen(needle);
    if (nlen > hlen)
        return false;
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
            return true;
    }
    return false;
}

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
static size_t extract_value_after(const char *msg, size_t msg_len, const char *after,
                                  char *out, size_t max_len) {
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

int hu_conversation_extract_micro_moments(const char *msg, size_t msg_len,
                                         char facts[][256], char significances[][128],
                                         size_t max_facts) {
    if (!msg || msg_len == 0 || !facts || !significances || max_facts == 0)
        return 0;

    int count = 0;
    char fact_buf[256];

    /* Named entities: pet names */
    static const char *dog_patterns[] = {
        "my dog's name is ", "my dogs name is ", "my dog ", "our dog's name is ",
        NULL
    };
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
    if (count < (int)max_facts && str_contains_ci(msg, msg_len, "my friend ")) {
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
    if (count < (int)max_facts && (str_contains_ci(msg, msg_len, "my kid ") ||
                                  str_contains_ci(msg, msg_len, "my kid's name is "))) {
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
        if (str_contains_ci(msg, msg_len, "got married")) {
            snprintf(facts[count], 256, "They got married");
            snprintf(significances[count], 128, "life_event");
            count++;
        } else if (str_contains_ci(msg, msg_len, "had a baby")) {
            snprintf(facts[count], 256, "They had a baby");
            snprintf(significances[count], 128, "life_event");
            count++;
        } else if (str_contains_ci(msg, msg_len, "new job at ")) {
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
                                      const hu_persona_t *persona, size_t *out_len) {
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
#ifdef HU_HAS_PERSONA
    if (persona && persona->style_rules && persona->style_rules_count > 0) {
        w = snprintf(buf + pos, CTX_BUF_CAP - pos, "\n--- CRITICAL STYLE RULES ---\n");
        POS_ADVANCE(w, pos, CTX_BUF_CAP);
        for (size_t i = 0; i < persona->style_rules_count && pos < CTX_BUF_CAP; i++) {
            if (persona->style_rules[i]) {
                w = snprintf(buf + pos, CTX_BUF_CAP - pos, "%s\n", persona->style_rules[i]);
                POS_ADVANCE(w, pos, CTX_BUF_CAP);
            }
        }
    } else
#endif
    {
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

    /* Brevity (0-25): ratio of response to their average length */
    double ratio = (double)response_len / (double)ref_len;
    if (ratio <= 1.5)
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
    if (str_contains_ci(response, response_len, "as an AI") ||
        str_contains_ci(response, response_len, "as a language model"))
        warmth -= 20;
    else if (str_contains_ci(response, response_len, "I'd be happy to") ||
             str_contains_ci(response, response_len, "let me know if") ||
             str_contains_ci(response, response_len, "feel free") ||
             str_contains_ci(response, response_len, "certainly"))
        warmth -= 18;
    if (warmth < 0)
        warmth = 0;
    if (warmth > 25)
        warmth = 25;
    score.warmth = warmth;

    /* Naturalness (0-25): structural (markdown, lists, AI tells) */
    int nat = 20;
    if (str_contains_ci(response, response_len, "**") ||
        str_contains_ci(response, response_len, "##") ||
        str_contains_ci(response, response_len, "```"))
        nat -= 12;
    else if (str_contains_ci(response, response_len, "- ") ||
             str_contains_ci(response, response_len, "1. "))
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

    /* needs_revision only on gross mismatches (10x length, etc.) */
    bool gross_length = (ratio > 10.0 || (ratio < 0.1 && response_len > 5));
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
    if (str_contains_ci(msg, msg_len, "tomorrow"))
        return now_ts + 86400;
    if (str_contains_ci(msg, msg_len, "next week"))
        return now_ts + 604800;
    if (str_contains_ci(msg, msg_len, "tonight")) {
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
    if (str_contains_ci(msg, msg_len, "this weekend")) {
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
        if (n > 0 && (size_t)n < sizeof(buf) && str_contains_ci(msg, msg_len, buf))
            return now_ts + (int64_t)d * 86400;
        n = snprintf(buf, sizeof(buf), "in %u days", d);
        if (n > 0 && (size_t)n < sizeof(buf) && str_contains_ci(msg, msg_len, buf))
            return now_ts + (int64_t)d * 86400;
    }
    /* "in X hours" */
    for (unsigned int h = 1; h <= 168; h++) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "in %u hour", h);
        if (n > 0 && (size_t)n < sizeof(buf) && str_contains_ci(msg, msg_len, buf))
            return now_ts + (int64_t)h * 3600;
        n = snprintf(buf, sizeof(buf), "in %u hours", h);
        if (n > 0 && (size_t)n < sizeof(buf) && str_contains_ci(msg, msg_len, buf))
            return now_ts + (int64_t)h * 3600;
    }
    return 0;
}

/* Detect if message contains a commitment. Fills description_out with commitment text,
 * who_out with "me" or "them" based on from_me. Returns true if commitment detected. */
bool hu_conversation_detect_commitment(const char *msg, size_t msg_len,
                                      char *description_out, size_t desc_cap,
                                      char *who_out, size_t who_cap, bool from_me) {
    if (!msg || msg_len == 0 || !description_out || desc_cap == 0 || !who_out || who_cap == 0)
        return false;
    description_out[0] = '\0';
    who_out[0] = '\0';

    static const char *KEYWORDS[] = {
        "i'll", "i will", "i'm going to", "gonna", "promise", "let me",
        "i'll call", "i'll text", "i'll send", "i'll check", "we should", NULL
    };
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
                while (end < msg_len && msg[end] != '.' && msg[end] != '!' &&
                       msg[end] != '?' && msg[end] != '\n')
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
    return str_contains_ci(message, message_len, "did you") ||
           str_contains_ci(message, message_len, "have you");
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
        if (str_contains_ci(t, tl, "love") || str_contains_ci(t, tl, "hate") ||
            str_contains_ci(t, tl, "scared") || str_contains_ci(t, tl, "angry") ||
            str_contains_ci(t, tl, "can't believe") || str_contains_ci(t, tl, "so happy") ||
            str_contains_ci(t, tl, "so sad") || str_contains_ci(t, tl, "hurt") ||
            str_contains_ci(t, tl, "need to tell") || str_contains_ci(t, tl, "important"))
            emotional_words++;
    }

    /* Detect closing signals */
    if (count >= 2) {
        const char *last = entries[count - 1].text;
        size_t ll = strlen(last);
        if (!entries[count - 1].from_me &&
            (str_contains_ci(last, ll, "gotta go") || str_contains_ci(last, ll, "talk later") ||
             str_contains_ci(last, ll, "bye") || str_contains_ci(last, ll, "night") ||
             str_contains_ci(last, ll, "ttyl") || str_contains_ci(last, ll, "heading out") ||
             (ll < 10 && (str_contains_ci(last, ll, "ok") || str_contains_ci(last, ll, "k")))))
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

hu_emotional_state_t hu_conversation_detect_emotion(const hu_channel_history_entry_t *entries,
                                                    size_t count) {
    hu_emotional_state_t state = {0.0f, 0.0f, false, "neutral"};
    if (!entries || count == 0)
        return state;

    float positive = 0, negative = 0;
    int samples = 0;
    size_t start = count > 8 ? count - 8 : 0;

    for (size_t i = start; i < count; i++) {
        if (entries[i].from_me)
            continue;
        const char *t = entries[i].text;
        size_t tl = strlen(t);
        samples++;

        /* Positive signals */
        if (str_contains_ci(t, tl, "happy"))
            positive += 1.0f;
        if (str_contains_ci(t, tl, "excited"))
            positive += 1.2f;
        if (str_contains_ci(t, tl, "love"))
            positive += 0.8f;
        if (str_contains_ci(t, tl, "amazing"))
            positive += 1.0f;
        if (str_contains_ci(t, tl, "great"))
            positive += 0.6f;
        if (str_contains_ci(t, tl, "awesome"))
            positive += 0.8f;
        if (str_contains_ci(t, tl, "lol") || str_contains_ci(t, tl, "haha"))
            positive += 0.4f;
        if (str_contains_ci(t, tl, "yay") || str_contains_ci(t, tl, "yes!"))
            positive += 0.7f;

        /* Negative signals */
        if (str_contains_ci(t, tl, "sad"))
            negative += 1.0f;
        if (str_contains_ci(t, tl, "depressed"))
            negative += 1.5f;
        if (str_contains_ci(t, tl, "stressed"))
            negative += 1.0f;
        if (str_contains_ci(t, tl, "worried"))
            negative += 0.8f;
        if (str_contains_ci(t, tl, "anxious"))
            negative += 1.0f;
        if (str_contains_ci(t, tl, "angry"))
            negative += 1.2f;
        if (str_contains_ci(t, tl, "frustrated"))
            negative += 1.0f;
        if (str_contains_ci(t, tl, "hurt"))
            negative += 1.2f;
        if (str_contains_ci(t, tl, "scared"))
            negative += 1.0f;
        if (str_contains_ci(t, tl, "lonely"))
            negative += 1.3f;
        if (str_contains_ci(t, tl, "crying") || str_contains_ci(t, tl, "cry"))
            negative += 1.5f;
        if (str_contains_ci(t, tl, "hate"))
            negative += 1.0f;
        if (str_contains_ci(t, tl, "ugh") || str_contains_ci(t, tl, "damn"))
            negative += 0.5f;
    }

    if (samples == 0)
        return state;

    state.valence = (positive - negative) / (float)samples;
    state.intensity = (positive + negative) / (float)samples;
    if (state.valence < -0.5f)
        state.valence = -1.0f;
    if (state.valence > 1.0f)
        state.valence = 1.0f;
    if (state.intensity > 2.0f)
        state.intensity = 2.0f;

    if (negative > positive * 1.5f) {
        state.concerning = negative >= 2.0f;
        if (str_contains_ci(entries[count - 1].text, strlen(entries[count - 1].text),
                            "depressed") ||
            str_contains_ci(entries[count - 1].text, strlen(entries[count - 1].text), "crying"))
            state.dominant_emotion = "distressed";
        else if (negative >= 1.5f)
            state.dominant_emotion = "upset";
        else
            state.dominant_emotion = "down";
    } else if (positive > negative * 1.5f) {
        if (positive >= 2.0f)
            state.dominant_emotion = "excited";
        else
            state.dominant_emotion = "positive";
    } else if (state.intensity > 0.3f) {
        state.dominant_emotion = "mixed";
    }

    return state;
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
    if (str_contains_ci(msg, msg_len, "ok sounds good") ||
        str_contains_ci(msg, msg_len, "sounds good") ||
        (msg_len <= 4 &&
         (str_contains_ci(msg, msg_len, "ok") || str_contains_ci(msg, msg_len, "k"))))
        return HU_ENERGY_NEUTRAL;

    /* Excited: positive valence, exclamation marks (3+), "omg", "amazing", "love", "so happy" */
    if (str_contains_ci(msg, msg_len, "omg") || str_contains_ci(msg, msg_len, "amazing") ||
        str_contains_ci(msg, msg_len, "love") || str_contains_ci(msg, msg_len, "so happy") ||
        count_exclamations(msg, msg_len) >= 3)
        return HU_ENERGY_EXCITED;

    /* Sad: "sad", "depressed", "hurt", "lonely", "crying", "miss you", "broken" */
    if (str_contains_ci(msg, msg_len, "sad") || str_contains_ci(msg, msg_len, "depressed") ||
        str_contains_ci(msg, msg_len, "hurt") || str_contains_ci(msg, msg_len, "lonely") ||
        str_contains_ci(msg, msg_len, "crying") || str_contains_ci(msg, msg_len, "miss you") ||
        str_contains_ci(msg, msg_len, "broken"))
        return HU_ENERGY_SAD;

    /* Playful: "lol", "haha", "lmao", teasing, "you're ridiculous", "dead 💀" */
    if (str_contains_ci(msg, msg_len, "lol") || str_contains_ci(msg, msg_len, "haha") ||
        str_contains_ci(msg, msg_len, "lmao") ||
        str_contains_ci(msg, msg_len, "you're ridiculous") ||
        str_contains_ci(msg, msg_len, "youre ridiculous") || str_contains_ci(msg, msg_len, "dead"))
        return HU_ENERGY_PLAYFUL;

    /* Anxious: "worried", "stressed", "anxious", "scared", "nervous", "freaking out" */
    if (str_contains_ci(msg, msg_len, "worried") || str_contains_ci(msg, msg_len, "stressed") ||
        str_contains_ci(msg, msg_len, "anxious") || str_contains_ci(msg, msg_len, "scared") ||
        str_contains_ci(msg, msg_len, "nervous") || str_contains_ci(msg, msg_len, "freaking out"))
        return HU_ENERGY_ANXIOUS;

    /* Calm: low intensity, neutral, short messages with no strong keywords */
    if (msg_len < 30 && !str_contains_ci(msg, msg_len, "!") &&
        !str_contains_ci(msg, msg_len, "?") && !str_contains_ci(msg, msg_len, "omg") &&
        !str_contains_ci(msg, msg_len, "love") && !str_contains_ci(msg, msg_len, "hate"))
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
    if (str_contains_ci(msg, msg_len, "remember when") ||
        str_contains_ci(msg, msg_len, "that time we") ||
        str_contains_ci(msg, msg_len, "you always say"))
        return true;

    /* "[X] energy" pattern — e.g. "that's so [name] energy" */
    if (str_contains_ci(msg, msg_len, " energy"))
        return true;

    /* Shared callback phrases */
    if (str_contains_ci(msg, msg_len, "lol that's our thing") ||
        str_contains_ci(msg, msg_len, "that's our thing") ||
        str_contains_ci(msg, msg_len, "classic "))
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
            if (str_contains_ci(msg, msg_len, phrase_buf))
                return true;
        }
    }

    return false;
}

/* ── Avoidance pattern detection (F21) ─────────────────────────────────── */

static const char *const topic_stopwords[] = {
    "i", "the", "a", "is", "was", "that", "this", "it", "to", "and", "but", "so",
    "just", "really", "what", "how", "why", "when", "where", "who", "can", "will",
    "would", "could", "should", "have", "has", "had", "do", "does", "did", "am",
    "are", "were", "be", "been", "being", "of", "in", "on", "at", "for", "with",
    "about", "from", "as", "or", "if", "not", "no", "yes", "oh", "um", "like",
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
        if (wlen >= 2 && !is_stopword(start, wlen)) {
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
    if (str_contains_ci(msg, msg_len, "ok sounds good") ||
        str_contains_ci(msg, msg_len, "sounds good") ||
        (msg_len <= 4 &&
         (str_contains_ci(msg, msg_len, "ok") || str_contains_ci(msg, msg_len, "k"))))
        return "neutral";

    /* Stressed: overwhelmed, burnt out */
    if (str_contains_ci(msg, msg_len, "stressed") ||
        str_contains_ci(msg, msg_len, "overwhelmed") ||
        str_contains_ci(msg, msg_len, "burnt out") ||
        str_contains_ci(msg, msg_len, "burned out"))
        return "stressed";

    /* Excited: omg, amazing, love, so happy, yay, 3+ exclamations */
    if (str_contains_ci(msg, msg_len, "omg") || str_contains_ci(msg, msg_len, "amazing") ||
        str_contains_ci(msg, msg_len, "love") || str_contains_ci(msg, msg_len, "so happy") ||
        str_contains_ci(msg, msg_len, "yay") || str_contains_ci(msg, msg_len, "excited") ||
        count_exclamations_tone(msg, msg_len) >= 3)
        return "excited";

    /* Sad: depressed, hurt, lonely, crying, broken */
    if (str_contains_ci(msg, msg_len, "sad") || str_contains_ci(msg, msg_len, "depressed") ||
        str_contains_ci(msg, msg_len, "hurt") || str_contains_ci(msg, msg_len, "lonely") ||
        str_contains_ci(msg, msg_len, "crying") || str_contains_ci(msg, msg_len, "cry") ||
        str_contains_ci(msg, msg_len, "miss you") || str_contains_ci(msg, msg_len, "broken"))
        return "sad";

    /* Anxious: worried, nervous, scared */
    if (str_contains_ci(msg, msg_len, "anxious") || str_contains_ci(msg, msg_len, "worried") ||
        str_contains_ci(msg, msg_len, "nervous") || str_contains_ci(msg, msg_len, "scared") ||
        str_contains_ci(msg, msg_len, "freaking out"))
        return "anxious";

    /* Frustrated: angry, ugh, damn, hate */
    if (str_contains_ci(msg, msg_len, "frustrated") || str_contains_ci(msg, msg_len, "angry") ||
        str_contains_ci(msg, msg_len, "ugh") || str_contains_ci(msg, msg_len, "damn") ||
        str_contains_ci(msg, msg_len, "hate"))
        return "frustrated";

    /* Happy: great, awesome, lol, haha */
    if (str_contains_ci(msg, msg_len, "happy") || str_contains_ci(msg, msg_len, "great") ||
        str_contains_ci(msg, msg_len, "awesome") || str_contains_ci(msg, msg_len, "lol") ||
        str_contains_ci(msg, msg_len, "haha") || str_contains_ci(msg, msg_len, "lmao"))
        return "happy";

    return "neutral";
}

size_t hu_conversation_extract_topic(const char *msg, size_t msg_len, char *out, size_t cap) {
    return extract_significant_topic(msg, msg_len, out, cap);
}

/* ── Growth celebration detection (F24) ─────────────────────────────────── */

static const char *const growth_phrases[] = {
    "it went great", "i got the job", "nailed it", "crushed it",
    "i passed", "got promoted", "it worked out", "turned out well",
    NULL,
};

bool hu_conversation_detect_growth_opportunity(const char *msg, size_t msg_len,
                                                char *topic_out, size_t topic_cap,
                                                char *after_state_out, size_t after_cap) {
    if (!msg || msg_len == 0 || !topic_out || topic_cap == 0 || !after_state_out || after_cap == 0)
        return false;

    const char *matched = NULL;
    for (const char *const *p = growth_phrases; *p; p++) {
        if (str_contains_ci(msg, msg_len, *p)) {
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
                                         char *topic_before, size_t before_cap,
                                         char *topic_after, size_t after_cap) {
    if (!entries || count < 2 || !topic_before || before_cap == 0 || !topic_after ||
        after_cap == 0)
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
    size_t tb_len = extract_significant_topic(msg_before->text, before_len, t_before, sizeof(t_before));
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
    if (str_contains_ci(text, text_len, "i'm fine") || str_contains_ci(text, text_len, "im fine") ||
        str_contains_ci(text, text_len, "just kidding") || str_contains_ci(text, text_len, "jk") ||
        str_contains_ci(text, text_len, "lol") || str_contains_ci(text, text_len, "haha"))
        return 0.0f; /* neutral for reset purposes */

    float valence = 0.0f;

    /* Negative keywords */
    if (str_contains_ci(text, text_len, "sad") || str_contains_ci(text, text_len, "angry") ||
        str_contains_ci(text, text_len, "frustrated") || str_contains_ci(text, text_len, "upset") ||
        str_contains_ci(text, text_len, "stressed") || str_contains_ci(text, text_len, "worse") ||
        str_contains_ci(text, text_len, "can't deal") ||
        str_contains_ci(text, text_len, "cant deal") || str_contains_ci(text, text_len, "hate") ||
        str_contains_ci(text, text_len, "horrible") || str_contains_ci(text, text_len, "terrible"))
        valence -= 1.0f;

    /* Positive keywords */
    if (str_contains_ci(text, text_len, "happy") || str_contains_ci(text, text_len, "great") ||
        str_contains_ci(text, text_len, "amazing") || str_contains_ci(text, text_len, "love") ||
        str_contains_ci(text, text_len, "thanks") || str_contains_ci(text, text_len, "better"))
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
    if (str_contains_ci(msg, msg_len, "what time") || str_contains_ci(msg, msg_len, "meet at") ||
        str_contains_ci(msg, msg_len, "where are") || str_contains_ci(msg, msg_len, "when are") ||
        str_contains_ci(msg, msg_len, "pick up") || str_contains_ci(msg, msg_len, "pickup"))
        return true;
    /* Quick questions / casual: "what's for dinner", short greetings */
    if (str_contains_ci(msg, msg_len, "what's for dinner") ||
        str_contains_ci(msg, msg_len, "whats for dinner"))
        return true;
    if (msg_len < 25 && (str_contains_ci(msg, msg_len, "hey") ||
                         str_contains_ci(msg, msg_len, "hi ") ||
                         str_contains_ci(msg, msg_len, "lol") ||
                         str_contains_ci(msg, msg_len, "nice")))
        return true;
    return false;
}

hu_call_escalation_t hu_conversation_should_escalate_to_call(
    const char *msg, size_t msg_len,
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
    if (str_contains_ci(msg, msg_len, "i need you") || str_contains_ci(msg, msg_len, "help me") ||
        str_contains_ci(msg, msg_len, "can't deal") || str_contains_ci(msg, msg_len, "cant deal") ||
        str_contains_ci(msg, msg_len, "breaking down") ||
        str_contains_ci(msg, msg_len, "i can't do this") ||
        str_contains_ci(msg, msg_len, "i cant do this") ||
        str_contains_ci(msg, msg_len, "please call") ||
        str_contains_ci(msg, msg_len, "end of my rope"))
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
    if (str_contains_ci(msg, msg_len, "it's complicated") ||
        str_contains_ci(msg, msg_len, "its complicated") ||
        str_contains_ci(msg, msg_len, "really complicated")) {
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
    if (str_contains_ci(msg, msg_len, "crying") || str_contains_ci(msg, msg_len, "freaking out") ||
        str_contains_ci(msg, msg_len, "losing it") ||
        str_contains_ci(msg, msg_len, "don't know what to do"))
        emotion = 0.8f;
    else if (str_contains_ci(msg, msg_len, "sad") || str_contains_ci(msg, msg_len, "overwhelmed") ||
             str_contains_ci(msg, msg_len, "stressed") || str_contains_ci(msg, msg_len, "anxious") ||
             str_contains_ci(msg, msg_len, "scared") || str_contains_ci(msg, msg_len, "hurt") ||
             str_contains_ci(msg, msg_len, "depressed") || str_contains_ci(msg, msg_len, "lonely"))
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

size_t hu_conversation_build_call_directive(const char *msg, size_t msg_len, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    size_t sample_len = msg_len > 80 ? 80 : msg_len;
    int n;
    if (msg && msg_len > 0) {
        n = snprintf(buf, cap,
                     "[CALL: This may be better as a call. Suggest they call when appropriate: \"%.*s%s\"]",
                     (int)sample_len, msg, msg_len > 80 ? "..." : "");
    } else {
        n = snprintf(buf, cap, "[CALL: This may be better as a call. Suggest they call when appropriate.]");
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
    {"job_loss",
     {"fired", "laid off", "lost my job", "getting let go", "downsized", NULL}},
    {"divorce",
     {"divorce", "separating", "custody", "ex-wife", "ex-husband", NULL}},
    {"mental_health",
     {"depression", "therapy", "therapist", "medication", "anxiety disorder", "panic attacks",
      NULL}},
    {"loss",
     {"died", "passed away", "funeral", "lost my", "grieving", NULL}},
    {"family_issue",
     {"my mom", "my dad", "my parents", "my brother", "my sister", NULL}},
    {"illness",
     {"diagnosis", "cancer", "sick", "hospital", "surgery", "chemo", NULL}},
};

static const char *vuln_family_negative[] = {
    "sick", "hospital", "problem", "issue", "fighting", "worried", "died", "passed away",
    "cancer", "diagnosis", "divorce", "custody", "struggling", "stress", NULL,
};

static bool family_issue_has_negative_context(const char *msg, size_t msg_len) {
    for (const char *const *kw = vuln_family_negative; *kw; kw++) {
        if (str_contains_ci(msg, msg_len, *kw))
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
            if (str_contains_ci(msg, msg_len, *kw))
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
bool hu_conversation_is_first_time_topic(hu_memory_t *memory,
                                         const char *contact_id, size_t contact_id_len,
                                         const char *topic, size_t topic_len) {
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

hu_vulnerability_state_t hu_conversation_detect_first_time_vulnerability(
    const char *msg, size_t msg_len,
    hu_memory_t *memory, const char *contact_id, size_t contact_id_len) {
    hu_vulnerability_state_t out = {false, NULL, 0.5f};

    const char *topic = hu_conversation_extract_vulnerability_topic(msg, msg_len);
    if (!topic)
        return out;

    out.topic_category = topic;
    out.intensity = 0.7f;

#ifdef HU_ENABLE_SQLITE
    size_t topic_len = strlen(topic);
    out.first_time = hu_conversation_is_first_time_topic(memory, contact_id, contact_id_len,
                                                        topic, topic_len);
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

#ifdef HU_HAS_PERSONA

static bool detect_heavy_topic(const hu_channel_history_entry_t *entries, size_t count) {
    if (!entries || count == 0)
        return false;
    static const char *DEFAULT_KEYWORDS[] = {
        "died",      "passed away", "passed",  "cancer",      "divorce",  "fired",    "laid off",
        "diagnosis", "funeral",     "breakup", "lost my job", "terminal", "hospital",
    };
    static const size_t DEFAULT_KEYWORDS_LEN = sizeof(DEFAULT_KEYWORDS) / sizeof(DEFAULT_KEYWORDS[0]);

    const char **keywords = s_crisis_keywords_len > 0 ? (const char **)s_crisis_keywords : (const char **)DEFAULT_KEYWORDS;
    size_t keywords_len = s_crisis_keywords_len > 0 ? s_crisis_keywords_len : DEFAULT_KEYWORDS_LEN;

    for (size_t i = 0; i < count; i++) {
        if (entries[i].from_me)
            continue;
        const char *t = entries[i].text;
        size_t tl = strlen(t);
        for (size_t k = 0; k < keywords_len; k++) {
            if (keywords[k] && str_contains_ci(t, tl, keywords[k]))
                return true;
        }
    }
    return false;
}

static bool detect_personal_sharing(const hu_channel_history_entry_t *entries, size_t count) {
    if (!entries || count == 0)
        return false;
    static const char *DEFAULT_PHRASES[] = {
        "i need to tell you",
        "can i be honest",
        "don't judge me",
        "this is hard to say",
        "i never told",
        "first time i'm saying",
        "i've been meaning to tell",
        "dont judge me",
        "first time im saying",
        "ive been meaning to tell",
    };
    static const size_t DEFAULT_PHRASES_LEN = sizeof(DEFAULT_PHRASES) / sizeof(DEFAULT_PHRASES[0]);

    const char **phrases = s_personal_sharing_phrases_len > 0 ? (const char **)s_personal_sharing_phrases : (const char **)DEFAULT_PHRASES;
    size_t phrases_len = s_personal_sharing_phrases_len > 0 ? s_personal_sharing_phrases_len : DEFAULT_PHRASES_LEN;

    for (size_t i = 0; i < count; i++) {
        if (entries[i].from_me)
            continue;
        const char *t = entries[i].text;
        size_t tl = strlen(t);
        for (size_t p = 0; p < phrases_len; p++) {
            if (phrases[p] && str_contains_ci(t, tl, phrases[p]))
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

#endif /* HU_HAS_PERSONA */

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
    static const size_t DEFAULT_STARTERS_LEN = sizeof(DEFAULT_STARTERS) / sizeof(DEFAULT_STARTERS[0]);

    const char **starters = s_starters_len > 0 ? (const char **)s_starters : (const char **)DEFAULT_STARTERS;
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
                                      size_t max_fragments) {
    if (!alloc || !response || response_len == 0 || !fragments || max_fragments == 0)
        return 0;

    /* Short responses stay as one message */
    if (response_len < 40 || max_fragments == 1) {
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
                if (remaining > 10 && is_split_starter(response + i + 1, remaining)) {
                    split_points[split_count++] = i + 1;
                }
            }
        }
    }

    /* If still nothing, split at sentence boundaries for long responses */
    if (split_count == 0 && response_len > 80) {
        for (size_t i = 30; i < response_len - 15 && split_count < 3; i++) {
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

/*
 * Instead of "keep under 50 chars", tell the model WHY a certain length
 * is right. Humans calibrate response length by message type, not by
 * counting characters. This function classifies the incoming message
 * and produces a directive that mimics human instinct.
 */

size_t hu_conversation_calibrate_length(const char *last_msg, size_t last_msg_len,
                                        const hu_channel_history_entry_t *entries, size_t count,
                                        char *buf, size_t cap) {
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
    int max_chars = hu_conversation_max_response_chars(last_msg_len);
    w = snprintf(buf + pos, cap - pos, "Their last message: %zu chars. ", last_msg_len);
    POS_ADVANCE(w, pos, cap);
    if (last_msg_len < 15) {
        w = snprintf(buf + pos, cap - pos, "Very brief — match that brevity. Target: 1-%d chars.\n",
                     max_chars);
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

/* ── Texting style analysis ───────────────────────────────────────────── */

char *hu_conversation_analyze_style(hu_allocator_t *alloc,
                                    const hu_channel_history_entry_t *entries, size_t count,
                                    const hu_persona_t *persona, size_t *out_len) {
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
        if (str_contains_ci(t, tl, "lol") || str_contains_ci(t, tl, "omg") ||
            str_contains_ci(t, tl, "ngl") || str_contains_ci(t, tl, "tbh") ||
            str_contains_ci(t, tl, "rn") || str_contains_ci(t, tl, "idk") ||
            str_contains_ci(t, tl, "imo") || str_contains_ci(t, tl, "nvm") ||
            str_contains_ci(t, tl, "btw") || str_contains_ci(t, tl, "wya") ||
            str_contains_ci(t, tl, "hbu"))
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
#ifdef HU_HAS_PERSONA
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
    } else
#endif
    {
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

size_t hu_conversation_apply_typos(char *buf, size_t len, size_t cap, uint32_t seed) {
    if (!buf || len == 0)
        return len;
    if (len >= cap)
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

typedef enum {
    HU_THINK_DECISION = 0,  /* advice/complex decision */
    HU_THINK_EMOTIONAL = 1, /* emotional support */
    HU_THINK_COMPLEX = 2,   /* philosophical/factual */
} hu_think_type_t;

static hu_think_type_t classify_think_type(const char *msg, size_t msg_len) {
    size_t excl = 0;
    size_t words = 0;
    for (size_t i = 0; i < msg_len; i++) {
        if (msg[i] == '!')
            excl++;
        if (msg[i] == ' ' || msg[i] == '\n')
            words++;
    }
    if (msg_len > 0)
        words++;

    /* Emotional: high exclamation density or short + intense */
    if (excl >= 2 || (excl >= 1 && msg_len < 60))
        return HU_THINK_EMOTIONAL;
    /* Complex: long message, often philosophical or multi-part */
    if (msg_len > 120 && words > 15)
        return HU_THINK_COMPLEX;
    /* Decision: medium-length question (advice-seeking) */
    return HU_THINK_DECISION;
}

bool hu_conversation_classify_thinking(const char *msg, size_t msg_len,
                                       const hu_channel_history_entry_t *entries,
                                       size_t entry_count, hu_thinking_response_t *out,
                                       uint32_t seed) {
    (void)entries;
    (void)entry_count;
    if (!msg || msg_len == 0 || !out)
        return false;

    memset(out, 0, sizeof(*out));

    /* Trigger: message characteristics */
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

    hu_think_type_t t = classify_think_type(msg, msg_len);
    const char *fillers[4];
    size_t filler_count;
    switch (t) {
    case HU_THINK_EMOTIONAL:
        fillers[0] = "hmm";
        fillers[1] = "oh wow";
        fillers[2] = "oh";
        filler_count = 3;
        break;
    case HU_THINK_DECISION:
        fillers[0] = "ooh that's a tough one";
        fillers[1] = "let me think about that for a sec";
        fillers[2] = "hm good question";
        filler_count = 3;
        break;
    case HU_THINK_COMPLEX:
        fillers[0] = "that's a really good question";
        fillers[1] = "okay give me a sec";
        fillers[2] = "let me think about that";
        filler_count = 3;
        break;
    }

    uint32_t state = seed * 1103515245u + 12345u;
    uint32_t idx = (state >> 16) % (uint32_t)filler_count;
    const char *filler = fillers[idx];
    size_t flen = strlen(filler);
    if (flen >= sizeof(out->filler))
        flen = sizeof(out->filler) - 1;
    memcpy(out->filler, filler, flen);
    out->filler[flen] = '\0';
    out->filler_len = flen;

    uint32_t base = 30000;
    uint32_t extra = 0;
    if (msg_len > 150)
        extra = 30000;
    else if (msg_len > 100)
        extra = 15000;
    else if (msg_len > 80)
        extra = 10000;
    out->delay_ms = base + extra;
    if (out->delay_ms > 60000)
        out->delay_ms = 60000;

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
            str_contains_ci(msg, msg_len, "goodnight") ||
            str_contains_ci(msg, msg_len, "good night") ||
            str_contains_ci(msg, msg_len, "gotta go") || str_contains_ci(msg, msg_len, "ttyl") ||
            str_contains_ci(msg, msg_len, "heading out") ||
            str_contains_ci(msg, msg_len, "peace out") || str_contains_ci(msg, msg_len, "see ya") ||
            str_contains_ci(msg, msg_len, "catch you later") ||
            str_contains_ci(msg, msg_len, "i'm out") ||
            (msg_len <= 10 &&
             (str_contains_ci(msg, msg_len, "bye") || str_contains_ci(msg, msg_len, "night") ||
              str_contains_ci(msg, msg_len, "later") || str_contains_ci(msg, msg_len, "gn") ||
              str_contains_ci(msg, msg_len, "cya")));

        if (incoming_is_farewell) {
            /* Check if our last message was also a farewell */
            for (size_t i = entry_count; i > 0; i--) {
                if (entries[i - 1].from_me) {
                    const char *t = entries[i - 1].text;
                    size_t tl = strlen(t);
                    bool our_was_farewell =
                        str_contains_ci(t, tl, "night") || str_contains_ci(t, tl, "bye") ||
                        str_contains_ci(t, tl, "later") || str_contains_ci(t, tl, "ttyl") ||
                        str_contains_ci(t, tl, "see ya") || str_contains_ci(t, tl, "cya") ||
                        str_contains_ci(t, tl, "gn") || str_contains_ci(t, tl, "peace") ||
                        str_contains_ci(t, tl, "take care");
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
        if (str_contains_ci(msg, msg_len, "goodnight") ||
            str_contains_ci(msg, msg_len, "good night") ||
            str_contains_ci(msg, msg_len, "gotta go") || str_contains_ci(msg, msg_len, "ttyl") ||
            str_contains_ci(msg, msg_len, "heading out") ||
            str_contains_ci(msg, msg_len, "peace out") || str_contains_ci(msg, msg_len, "see ya") ||
            str_contains_ci(msg, msg_len, "catch you later") ||
            str_contains_ci(msg, msg_len, "i'm out")) {
            *delay_extra_ms = 1500;
            return HU_RESPONSE_BRIEF;
        }
        if (msg_len <= 10 &&
            (str_contains_ci(msg, msg_len, "bye") || str_contains_ci(msg, msg_len, "night") ||
             str_contains_ci(msg, msg_len, "later") || str_contains_ci(msg, msg_len, "gn") ||
             str_contains_ci(msg, msg_len, "cya"))) {
            *delay_extra_ms = 1500;
            return HU_RESPONSE_BRIEF;
        }
    }

    /* Brief: short acknowledgment by properties (length, word count, no question) */
    if (msg_len < 15 && word_count <= 2 && !has_question)
        return HU_RESPONSE_BRIEF;

    /* Bad news: extended pause — show you're absorbing it */
    if (str_contains_ci(msg, msg_len, "passed away") ||
        str_contains_ci(msg, msg_len, "got fired") || str_contains_ci(msg, msg_len, "broke up") ||
        str_contains_ci(msg, msg_len, "bad news") ||
        str_contains_ci(msg, msg_len, "didn't make it") ||
        str_contains_ci(msg, msg_len, "got rejected") || str_contains_ci(msg, msg_len, "lost my")) {
        *delay_extra_ms = 12000;
        return HU_RESPONSE_DELAY;
    }

    /* Good news: short pause then celebrate */
    if (str_contains_ci(msg, msg_len, "got the job") || str_contains_ci(msg, msg_len, "got in") ||
        str_contains_ci(msg, msg_len, "i passed") || str_contains_ci(msg, msg_len, "got engaged") ||
        str_contains_ci(msg, msg_len, "got promoted") ||
        str_contains_ci(msg, msg_len, "good news") || str_contains_ci(msg, msg_len, "i did it") ||
        str_contains_ci(msg, msg_len, "we did it")) {
        *delay_extra_ms = 3000;
        return HU_RESPONSE_DELAY;
    }

    /* Vulnerability: deliberate pause */
    if (str_contains_ci(msg, msg_len, "i need to tell you") ||
        str_contains_ci(msg, msg_len, "can i be honest") ||
        str_contains_ci(msg, msg_len, "don't judge me") ||
        str_contains_ci(msg, msg_len, "this is hard to say") ||
        str_contains_ci(msg, msg_len, "i never told")) {
        *delay_extra_ms = 8000;
        return HU_RESPONSE_DELAY;
    }

    /* Emotional/heavy messages: full response but delayed (showing you're thinking) */
    static const char *DEFAULT_EMOTIONAL[] = {
        "miss",    "love",     "hurt",   "stress",    "depress",   "lonely",     "scared",
        "worried", "sorry",    "afraid", "giving up", "feel like", "don't know", "can't",
        "help me", "need you", "cry",    "sad",
    };
    static const size_t DEFAULT_EMOTIONAL_LEN = sizeof(DEFAULT_EMOTIONAL) / sizeof(DEFAULT_EMOTIONAL[0]);

    const char **emotional = s_emotional_words_len > 0 ? (const char **)s_emotional_words : (const char **)DEFAULT_EMOTIONAL;
    size_t emotional_len = s_emotional_words_len > 0 ? s_emotional_words_len : DEFAULT_EMOTIONAL_LEN;

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

static bool is_narrative_or_venting(const char *msg, size_t msg_len,
                                    const hu_channel_history_entry_t *entries, size_t count) {
    if (!msg || msg_len < 80)
        return false;
    if (memchr(msg, '?', msg_len) != NULL)
        return false;
    bool has_narrative_phrase =
        str_contains_ci(msg, msg_len, "and then") || str_contains_ci(msg, msg_len, "so i") ||
        str_contains_ci(msg, msg_len, "anyway") || str_contains_ci(msg, msg_len, "long story");
    bool has_first_person = str_contains_ci(msg, msg_len, "i ") ||
                            str_contains_ci(msg, msg_len, "my ") ||
                            str_contains_ci(msg, msg_len, "me ");
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
#define DEFAULT_BACKCHANNEL_COUNT (sizeof(DEFAULT_BACKCHANNEL_PHRASES) / sizeof(DEFAULT_BACKCHANNEL_PHRASES[0]))

size_t hu_conversation_pick_backchannel(uint32_t seed, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    const char *const *phrases = s_backchannel_phrases_len > 0 ? (const char *const *)s_backchannel_phrases : (const char *const *)(const char **)DEFAULT_BACKCHANNEL_PHRASES;
    size_t phrase_count = s_backchannel_phrases_len > 0 ? s_backchannel_phrases_len : DEFAULT_BACKCHANNEL_COUNT;

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
    return str_contains_ci(msg, msg_len, "omg") || str_contains_ci(msg, msg_len, "oh my god") ||
           str_contains_ci(msg, msg_len, "just saw") ||
           str_contains_ci(msg, msg_len, "did you see") ||
           str_contains_ci(msg, msg_len, "holy shit") ||
           str_contains_ci(msg, msg_len, "emergency") ||
           str_contains_ci(msg, msg_len, "are you okay") || count_exclamations(msg, msg_len) >= 3;
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
    return str_contains_ci(t, tl, "night") || str_contains_ci(t, tl, "sleep well") ||
           str_contains_ci(t, tl, "bye") || str_contains_ci(t, tl, "goodnight") ||
           str_contains_ci(t, tl, "good night") || str_contains_ci(t, tl, "later") ||
           str_contains_ci(t, tl, "ttyl") || str_contains_ci(t, tl, "see ya") ||
           str_contains_ci(t, tl, "cya") || str_contains_ci(t, tl, "gn") ||
           str_contains_ci(t, tl, "peace") || str_contains_ci(t, tl, "take care") ||
           str_contains_ci(t, tl, "gotta go") || str_contains_ci(t, tl, "heading out");
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
                                          uint32_t seed) {
    if (!msg || msg_len == 0)
        return false;

    /* Never for direct questions */
    if (memchr(msg, '?', msg_len) != NULL)
        return false;

    /* Never for emotional crisis */
    if (str_contains_ci(msg, msg_len, "help me") || str_contains_ci(msg, msg_len, "i need you"))
        return false;

    /* Never for concerning emotion in recent history */
    if (entries && count > 0) {
        hu_emotional_state_t emo = hu_conversation_detect_emotion(entries, count);
        if (emo.concerning)
            return false;
    }

    /* Never for high emotional content in message itself */
    if (str_contains_ci(msg, msg_len, "emergency") || str_contains_ci(msg, msg_len, "urgent"))
        return false;

    bool trigger = false;

    /* Disagreement */
    if (str_contains_ci(msg, msg_len, "agree to disagree") ||
        str_contains_ci(msg, msg_len, "whatever") || str_contains_ci(msg, msg_len, "fine.") ||
        str_contains_ci(msg, msg_len, "i don't agree") ||
        str_contains_ci(msg, msg_len, "you're wrong") ||
        str_contains_ci(msg, msg_len, "youre wrong"))
        trigger = true;

    /* Space request */
    if (!trigger && (str_contains_ci(msg, msg_len, "i need space") ||
                     str_contains_ci(msg, msg_len, "give me a minute") ||
                     str_contains_ci(msg, msg_len, "can we talk later") ||
                     str_contains_ci(msg, msg_len, "leave me alone")))
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

    /* 2% probability roll */
    return (seed % 100u) < 2u;
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
        if (str_contains_ci(msg, msg_len, patterns[i]))
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
                if (str_contains_ci(prev, prev_len, "share") ||
                    str_contains_ci(prev, prev_len, "link") ||
                    str_contains_ci(prev, prev_len, "check out") ||
                    str_contains_ci(prev, prev_len, "here's") ||
                    str_contains_ci(prev, prev_len, "here is"))
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
        if (str_contains_ci(my_msgs[i], strlen(my_msgs[i]), "haha") ||
            str_contains_ci(my_msgs[i], strlen(my_msgs[i]), "lol"))
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
        if (str_contains_ci(relationship_stage, strlen(relationship_stage), "close") ||
            str_contains_ci(relationship_stage, strlen(relationship_stage), "best") ||
            str_contains_ci(relationship_stage, strlen(relationship_stage), "partner") ||
            str_contains_ci(relationship_stage, strlen(relationship_stage), "intimate")) {
            w = snprintf(buf + pos, cap - pos,
                         "RELATIONSHIP: Close. You can be fully yourself — inside jokes, "
                         "mild roasting, deep honesty, comfortable silence. "
                         "Don't over-explain or be overly polite.\n");
        } else if (str_contains_ci(relationship_stage, strlen(relationship_stage), "friend")) {
            w = snprintf(buf + pos, cap - pos,
                         "RELATIONSHIP: Friend. Warm and genuine but with some boundaries. "
                         "Banter is fine. Personal topics ok if they bring them up first.\n");
        } else if (str_contains_ci(relationship_stage, strlen(relationship_stage), "acquaint") ||
                   str_contains_ci(relationship_stage, strlen(relationship_stage), "new")) {
            w = snprintf(buf + pos, cap - pos,
                         "RELATIONSHIP: Acquaintance/new. Keep it lighter. "
                         "Don't assume familiarity. Match their formality level. "
                         "No inside jokes. No unsolicited deep topics.\n");
        } else if (str_contains_ci(relationship_stage, strlen(relationship_stage),
                                   "professional") ||
                   str_contains_ci(relationship_stage, strlen(relationship_stage), "work")) {
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
        if (str_contains_ci(warmth_level, strlen(warmth_level), "high")) {
            ww = snprintf(buf + pos, cap - pos,
                          "WARMTH: High. Affectionate language is natural with this person.\n");
        } else if (str_contains_ci(warmth_level, strlen(warmth_level), "low")) {
            ww = snprintf(buf + pos, cap - pos,
                          "WARMTH: Low. Keep it cool and understated. No gushing.\n");
        }
        POS_ADVANCE(ww, pos, cap);
    }

    /* Vulnerability level → depth permission */
    if (vulnerability_level) {
        int vw = 0;
        if (str_contains_ci(vulnerability_level, strlen(vulnerability_level), "high") ||
            str_contains_ci(vulnerability_level, strlen(vulnerability_level), "open")) {
            vw = snprintf(buf + pos, cap - pos,
                          "VULNERABILITY: Open. You can share real feelings and be honest "
                          "about struggles. This person can handle depth.\n");
        } else if (str_contains_ci(vulnerability_level, strlen(vulnerability_level), "low") ||
                   str_contains_ci(vulnerability_level, strlen(vulnerability_level), "guarded")) {
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
    if (bot_name && bot_name_len > 0 && str_contains_ci(msg, msg_len, bot_name))
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
    static const size_t DEFAULT_ENGAGE_WORDS_LEN = sizeof(DEFAULT_ENGAGE_WORDS) / sizeof(DEFAULT_ENGAGE_WORDS[0]);

    const char **engage_words = s_engage_words_len > 0 ? (const char **)s_engage_words : (const char **)DEFAULT_ENGAGE_WORDS;
    size_t engage_words_len = s_engage_words_len > 0 ? s_engage_words_len : DEFAULT_ENGAGE_WORDS_LEN;

    for (size_t i = 0; i < engage_words_len; i++) {
        if (engage_words[i] && str_contains_ci(msg, msg_len, engage_words[i]))
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

/* ── Inline reply classifier (iMessage quoted text fallback) ────────────── */

bool hu_conversation_should_inline_reply(const hu_channel_history_entry_t *entries, size_t count,
                                         const char *last_msg, size_t last_msg_len) {
    if (!last_msg || last_msg_len == 0)
        return false;

    /* Heuristic: "you said" / "earlier" / "what about" in their message → inline reply */
    if (str_contains_ci(last_msg, last_msg_len, "you said") ||
        str_contains_ci(last_msg, last_msg_len, "earlier") ||
        str_contains_ci(last_msg, last_msg_len, "what about") ||
        str_contains_ci(last_msg, last_msg_len, "that thing you") ||
        str_contains_ci(last_msg, last_msg_len, "the one you"))
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
    if (str_contains_ci(message, message_len, "passed away") ||
        str_contains_ci(message, message_len, "got fired") ||
        str_contains_ci(message, message_len, "broke up") ||
        str_contains_ci(message, message_len, "bad news") ||
        str_contains_ci(message, message_len, "didn't make it") ||
        str_contains_ci(message, message_len, "stressed") ||
        str_contains_ci(message, message_len, "worried") ||
        str_contains_ci(message, message_len, "sad") ||
        str_contains_ci(message, message_len, "angry"))
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
    if (str_contains_ci(message, message_len, "lol") ||
        str_contains_ci(message, message_len, "haha") ||
        str_contains_ci(message, message_len, "lmao") ||
        str_contains_ci(message, message_len, "😂")) {
        uint32_t roll = tapback_prng_next(&s) % 100u;
        if (roll < 70u)
            return HU_TAPBACK_ONLY;
        return HU_TAPBACK_AND_TEXT;
    }

    /* Agreement/affirmation (yeah, nice, cool, etc.) → TAPBACK_ONLY ~70% */
    if (str_contains_ci(message, message_len, "yeah") ||
        str_contains_ci(message, message_len, "nice") ||
        str_contains_ci(message, message_len, "cool") ||
        str_contains_ci(message, message_len, "sure") ||
        str_contains_ci(message, message_len, "ok") ||
        str_contains_ci(message, message_len, "👍") ||
        (message_len <= 8 && str_contains_ci(message, message_len, "yes"))) {
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

    /* Photos/media placeholders: 50% chance of heart */
    if (str_contains_ci(msg, msg_len, "[image") || str_contains_ci(msg, msg_len, "[attachment")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 50u)
            return HU_REACTION_HEART;
        return HU_REACTION_NONE;
    }

    /* Funny messages → HAHA */
    if (str_contains_ci(msg, msg_len, "lol") || str_contains_ci(msg, msg_len, "lmao") ||
        str_contains_ci(msg, msg_len, "haha") || str_contains_ci(msg, msg_len, "hahaha") ||
        str_contains_ci(msg, msg_len, "😂") || str_contains_ci(msg, msg_len, "hilarious") ||
        str_contains_ci(msg, msg_len, "that's funny") ||
        str_contains_ci(msg, msg_len, "so funny")) {
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
    if (str_contains_ci(msg, msg_len, "love you") || str_contains_ci(msg, msg_len, "miss you") ||
        str_contains_ci(msg, msg_len, "❤") || str_contains_ci(msg, msg_len, "💕") ||
        str_contains_ci(msg, msg_len, "you're amazing") ||
        str_contains_ci(msg, msg_len, "you're the best") ||
        str_contains_ci(msg, msg_len, "proud of you") ||
        str_contains_ci(msg, msg_len, "so sweet")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 30u)
            return HU_REACTION_HEART;
        return HU_REACTION_NONE;
    }

    /* Agreement/affirmation → THUMBS_UP */
    if (str_contains_ci(msg, msg_len, "absolutely") || str_contains_ci(msg, msg_len, "exactly") ||
        str_contains_ci(msg, msg_len, "yes!") || str_contains_ci(msg, msg_len, "👍") ||
        str_contains_ci(msg, msg_len, "for sure") || str_contains_ci(msg, msg_len, "definitely")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 30u)
            return HU_REACTION_THUMBS_UP;
        return HU_REACTION_NONE;
    }

    /* Impressive/exciting news → EMPHASIS */
    if (str_contains_ci(msg, msg_len, "got the job") || str_contains_ci(msg, msg_len, "i did it") ||
        str_contains_ci(msg, msg_len, "we won") || str_contains_ci(msg, msg_len, "we did it") ||
        str_contains_ci(msg, msg_len, "got in") || str_contains_ci(msg, msg_len, "i passed") ||
        str_contains_ci(msg, msg_len, "got engaged") ||
        str_contains_ci(msg, msg_len, "got promoted")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 30u)
            return HU_REACTION_EMPHASIS;
        return HU_REACTION_NONE;
    }

    /* Messages that need a real text response → NONE */
    if (str_contains_ci(msg, msg_len, "what time") || str_contains_ci(msg, msg_len, "where") ||
        str_contains_ci(msg, msg_len, "how do") || str_contains_ci(msg, msg_len, "can you") ||
        str_contains_ci(msg, msg_len, "could you") || str_contains_ci(msg, msg_len, "why") ||
        str_contains_ci(msg, msg_len, "when") || str_contains_ci(msg, msg_len, "?"))
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
    if (str_contains_ci(vision_description, desc_len, "food") ||
        str_contains_ci(vision_description, desc_len, "meal") ||
        str_contains_ci(vision_description, desc_len, "dinner") ||
        str_contains_ci(vision_description, desc_len, "lunch") ||
        str_contains_ci(vision_description, desc_len, "pasta") ||
        str_contains_ci(vision_description, desc_len, "screenshot") ||
        str_contains_ci(vision_description, desc_len, "error") ||
        str_contains_ci(vision_description, desc_len, "code"))
        return HU_REACTION_NONE;

    /* Funny → HAHA (probabilistic) */
    if (str_contains_ci(vision_description, desc_len, "funny") ||
        str_contains_ci(vision_description, desc_len, "meme") ||
        str_contains_ci(vision_description, desc_len, "silly") ||
        str_contains_ci(vision_description, desc_len, "hilarious") ||
        str_contains_ci(vision_description, desc_len, "comic")) {
        uint32_t roll = reaction_prng_next(&s) % 100u;
        if (roll < 85u)
            return HU_REACTION_HAHA;
        return HU_REACTION_NONE;
    }

    /* Sunset/landscape/nature/family/selfie → HEART (probabilistic) */
    if (str_contains_ci(vision_description, desc_len, "sunset") ||
        str_contains_ci(vision_description, desc_len, "landscape") ||
        str_contains_ci(vision_description, desc_len, "nature") ||
        str_contains_ci(vision_description, desc_len, "beach") ||
        str_contains_ci(vision_description, desc_len, "mountain") ||
        str_contains_ci(vision_description, desc_len, "sky") ||
        str_contains_ci(vision_description, desc_len, "beautiful") ||
        str_contains_ci(vision_description, desc_len, "family") ||
        str_contains_ci(vision_description, desc_len, "baby") ||
        str_contains_ci(vision_description, desc_len, "kids") ||
        str_contains_ci(vision_description, desc_len, "selfie") ||
        str_contains_ci(vision_description, desc_len, "portrait") ||
        str_contains_ci(vision_description, desc_len, "couple") ||
        str_contains_ci(vision_description, desc_len, "wedding")) {
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

size_t hu_conversation_apply_disfluency(char *buf, size_t len, size_t cap, uint32_t seed,
                                        float frequency, const struct hu_contact_profile *contact,
                                        const char *formality, size_t formality_len) {
#ifdef HU_HAS_PERSONA
    if (!buf || len == 0 || cap <= len)
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

    /* Type selection: 40% prepend, 30% append, 20% insert actually, 10% self-correction */
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
    } else if (type_roll < 90u) {
        /* 20%: insert "actually " after first space/clause */
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
    } else {
        /* 10%: self-correction " wait no " or " *meant it" */
        if (((seed / 10000u) % 2u) == 0) {
            add = " wait no ";
            add_len = 9;
            for (size_t i = 0; i < len; i++) {
                if (buf[i] == ' ' || buf[i] == ',') {
                    insert_pos = i + 1;
                    break;
                }
            }
            if (insert_pos == (size_t)-1)
                insert_pos = len;
        } else {
            append = true;
            add = " *meant it";
            add_len = 10;
        }
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
#else
    (void)buf;
    (void)len;
    (void)cap;
    (void)seed;
    (void)frequency;
    (void)contact;
    (void)formality;
    (void)formality_len;
    return len;
#endif
}

/* ── Filler word injection ────────────────────────────────────────────── */

static uint32_t filler_lcg(uint32_t *s) {
    *s = *s * 1103515245u + 12345u;
    return (*s >> 16) & 0x7fff;
}

size_t hu_conversation_apply_fillers(char *buf, size_t len, size_t cap, uint32_t seed,
                                     const char *channel_type, size_t channel_type_len) {
    if (!buf || len == 0 || cap <= len)
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

    static const char *DEFAULT_FILLERS[] = {
        "haha ", "lol ", "yeah ", "honestly ", "tbh ",
        "ngl ",  "hmm ", "oh ",   "ah ",       "like "
    };
    static const size_t DEFAULT_FILLER_COUNT = 10;

    const char **fillers = s_filler_words_len > 0 ? (const char **)s_filler_words : (const char **)DEFAULT_FILLERS;
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

/* ── Stylometric variance ─────────────────────────────────────────────── */

size_t hu_conversation_vary_complexity(char *buf, size_t len, uint32_t seed) {
    if (!buf || len == 0)
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
    static const size_t DEFAULT_CONTRACTIONS_LEN = sizeof(DEFAULT_CONTRACTIONS) / sizeof(DEFAULT_CONTRACTIONS[0]);

    const hu_conversation_contraction_t *contractions = s_contractions_len > 0 ? s_contractions : DEFAULT_CONTRACTIONS;
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
        "happy",   "great", "awesome", "love",      "good", "nice",
        "excited", "glad",  "amazing", "wonderful", "lol",  "haha",
        "yay",     "sweet", "perfect", "thanks"
    };
    static const char *DEFAULT_NEG_WORDS[] = {
        "sad",      "angry", "frustrated",   "annoyed", "terrible", "awful", "hate", "worried",
        "stressed", "upset", "disappointed", "ugh",     "sucks",    "rough", "hard", "sorry"
    };
    static const size_t DEFAULT_POS_LEN = sizeof(DEFAULT_POS_WORDS) / sizeof(DEFAULT_POS_WORDS[0]);
    static const size_t DEFAULT_NEG_LEN = sizeof(DEFAULT_NEG_WORDS) / sizeof(DEFAULT_NEG_WORDS[0]);

    const char **pos_words = s_positive_words_len > 0 ? (const char **)s_positive_words : (const char **)DEFAULT_POS_WORDS;
    const char **neg_words = s_negative_words_len > 0 ? (const char **)s_negative_words : (const char **)DEFAULT_NEG_WORDS;
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
            if (pos_words[w] && str_contains_ci(text, tl, pos_words[w]))
                score++;
        }
        for (size_t w = 0; w < n_neg; w++) {
            if (neg_words[w] && str_contains_ci(text, tl, neg_words[w]))
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

    const char **intros = s_conversation_intros_len > 0 ? (const char **)s_conversation_intros : (const char **)DEFAULT_INTROS;
    size_t intros_len = s_conversation_intros_len > 0 ? s_conversation_intros_len : DEFAULT_INTROS_LEN;

    if (intros_len == 0)
        return NULL;

    s = s * 1103515245u + 12345u;
    size_t intro_pick = ((s >> 16) & 0x7fff) % intros_len;

    const char *chosen_intro = intros[intro_pick];
    if (!chosen_intro)
        chosen_intro = intros[0];

    int w =
        snprintf(buf, sizeof(buf),
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
    static const size_t DEFAULT_PATTERNS_LEN = sizeof(DEFAULT_PATTERNS) / sizeof(DEFAULT_PATTERNS[0]);

    const char **patterns = s_ai_disclosure_patterns_len > 0 ? (const char **)s_ai_disclosure_patterns : (const char **)DEFAULT_PATTERNS;
    size_t n = s_ai_disclosure_patterns_len > 0 ? s_ai_disclosure_patterns_len : DEFAULT_PATTERNS_LEN;

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

size_t hu_conversation_strip_ai_phrases(char *buf, size_t len) {
    if (!buf || len == 0)
        return len;

    struct {
        const char *from;
        size_t from_len;
        const char *to;
        size_t to_len;
    } replacements[] = {
        {"Great question! ", 16, "", 0},
        {"Great question. ", 16, "", 0},
        {"That's a great point! ", 22, "", 0},
        {"That's a great point. ", 22, "", 0},
        {"I appreciate you sharing that. ", 31, "", 0},
        {"I appreciate you sharing that! ", 31, "", 0},
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
    };
    size_t n_rep = sizeof(replacements) / sizeof(replacements[0]);

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
            }
            break;
        }
    }

    if (len > 0 && buf[0] >= 'a' && buf[0] <= 'z')
        buf[0] -= 32;

    return len;
}

/* ── iMessage effect classifier (keyword-triggered, client-side) ───────── */

/* Classify if a message contains iMessage effect trigger phrases.
 * Returns effect name (static string) or NULL. Effect is applied automatically
 * on recipient's device when the trigger phrase is sent as plain text.
 * Order: longer phrases first to avoid partial matches. */
const char *hu_conversation_classify_effect(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return NULL;

    if (str_contains_ci(msg, msg_len, "happy birthday"))
        return "confetti";
    if (str_contains_ci(msg, msg_len, "happy new year"))
        return "fireworks";
    if (str_contains_ci(msg, msg_len, "congratulations") ||
        str_contains_ci(msg, msg_len, "congrats"))
        return "balloons";
    if (str_contains_ci(msg, msg_len, "pew pew"))
        return "lasers";

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
            if (str_contains_ci(msg, msg_len, markers[m]))
                return true;
        }
    }

    if (entries && count > 0) {
        const hu_channel_history_entry_t *last = &entries[count - 1];
        if (!last->from_me) {
            const char *t = last->text;
            size_t tl = strlen(t);
            for (size_t m = 0; m < n_markers; m++) {
                if (str_contains_ci(t, tl, markers[m]))
                    return true;
            }
        }
    }
    return false;
}
