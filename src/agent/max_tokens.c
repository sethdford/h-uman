#include "human/max_tokens.h"
#include "human/config_types.h"
#include <ctype.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const char *key;
    uint32_t tokens;
} max_entry_t;

static const max_entry_t MODEL_MAX[] = {
    {"claude-opus-4-6", 8192},
    {"claude-opus-4.6", 8192},
    {"claude-sonnet-4-6", 8192},
    {"claude-sonnet-4.6", 8192},
    {"claude-haiku-4-5", 8192},
    {"gpt-5.2", 8192},
    {"gpt-5.2-codex", 8192},
    {"gpt-4.5-preview", 8192},
    {"gpt-4.1", 8192},
    {"gpt-4.1-mini", 8192},
    {"gpt-4o", 8192},
    {"gpt-4o-mini", 8192},
    {"o3-mini", 8192},
    {"gemini-2.5-pro", 8192},
    {"gemini-2.5-flash", 8192},
    {"gemini-2.0-flash", 8192},
    {"deepseek-v3.2", 8192},
    {"deepseek-chat", 8192},
    {"deepseek-reasoner", 8192},
    {"llama-4-70b-instruct", 8192},
    {"k2p5", 32768},
};

static const max_entry_t PROVIDER_MAX[] = {
    {"anthropic", 8192},      {"openai", 8192},
    {"google", 8192},         {"gemini", 8192},
    {"openrouter", 8192},     {"minimax", 8192},
    {"xiaomi", 8192},         {"moonshot", 8192},
    {"kimi", 8192},           {"kimi-coding", 32768},
    {"qwen", 8192},           {"qwen-portal", 8192},
    {"ollama", 8192},         {"vllm", 8192},
    {"github-copilot", 8192}, {"qianfan", 32768},
    {"nvidia", 4096},         {"byteplus", 4096},
    {"doubao", 4096},         {"cloudflare-ai-gateway", 64000},
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static int strncasecmpx(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = a[i] ? (int)tolower((unsigned char)a[i]) : 0;
        int cb = b[i] ? (int)tolower((unsigned char)b[i]) : 0;
        if (ca != cb)
            return ca - cb;
        if (ca == 0)
            return 0;
    }
    return 0;
}

static int str_eql(const char *a, size_t a_len, const char *b) {
    size_t bl = strlen(b);
    if (a_len != bl)
        return 0;
    return strncasecmpx(a, b, a_len) == 0;
}

static int starts_with_ic(const char *hay, size_t hay_len, const char *prefix) {
    size_t pl = strlen(prefix);
    if (hay_len < pl)
        return 0;
    return strncasecmpx(hay, prefix, pl) == 0;
}

static int ends_with_ic(const char *hay, size_t hay_len, const char *suffix) {
    size_t sl = strlen(suffix);
    if (hay_len < sl)
        return 0;
    return strncasecmpx(hay + hay_len - sl, suffix, sl) == 0;
}

static int is_all_digits(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    return len > 0;
}

static void trim(const char **s, size_t *len) {
    while (*len > 0 && (*s)[0] <= ' ') {
        (*s)++;
        (*len)--;
    }
    while (*len > 0 && (*s)[*len - 1] <= ' ')
        (*len)--;
}

static const char *find_last_char(const char *s, size_t len, char c) {
    for (size_t i = len; i > 0; i--) {
        if (s[i - 1] == c)
            return s + (i - 1);
    }
    return NULL;
}

static void strip_date_suffix(const char *model_id, size_t len, size_t *out_len) {
    *out_len = len;
    const char *last = find_last_char(model_id, len, '-');
    if (!last || last + 1 >= model_id + len)
        return;
    size_t suffix_len = (model_id + len) - (last + 1);
    if (suffix_len == 8 && is_all_digits(last + 1, suffix_len))
        *out_len = (size_t)(last - model_id);
}

static uint32_t lookup_table(const max_entry_t *tbl, size_t tbl_len, const char *key,
                             size_t key_len) {
    for (size_t i = 0; i < tbl_len; i++) {
        if (str_eql(key, key_len, tbl[i].key))
            return tbl[i].tokens;
    }
    return 0;
}

static int contains_str(const char *hay, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (hay_len < nlen)
        return 0;
    for (size_t i = 0; i <= hay_len - nlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

static uint32_t infer_from_pattern(const char *model_id, size_t len) {
    if (len >= 4 && contains_str(model_id, len, "k2p5"))
        return 32768;
    if (starts_with_ic(model_id, len, "kimi-coding") || starts_with_ic(model_id, len, "kimi-k2"))
        return 32768;
    if (starts_with_ic(model_id, len, "nvidia/"))
        return 4096;
    if (starts_with_ic(model_id, len, "claude-") || starts_with_ic(model_id, len, "gpt-") ||
        starts_with_ic(model_id, len, "o1") || starts_with_ic(model_id, len, "o3") ||
        starts_with_ic(model_id, len, "gemini-") || starts_with_ic(model_id, len, "deepseek-"))
        return 8192;
    return 0;
}

static uint32_t lookup_model_candidates(const char *model_id_raw, size_t len) {
    const char *no_latest = model_id_raw;
    size_t no_latest_len = len;
    if (ends_with_ic(model_id_raw, len, "-latest"))
        no_latest_len = len - 7;

    size_t no_date_len = no_latest_len;
    strip_date_suffix(no_latest, no_latest_len, &no_date_len);

    uint32_t v = lookup_table(MODEL_MAX, ARRAY_LEN(MODEL_MAX), model_id_raw, len);
    if (v)
        return v;
    if (no_latest_len != len) {
        v = lookup_table(MODEL_MAX, ARRAY_LEN(MODEL_MAX), no_latest, no_latest_len);
        if (v)
            return v;
    }
    v = infer_from_pattern(no_latest, no_date_len);
    if (v)
        return v;
    v = infer_from_pattern(no_latest, no_latest_len);
    if (v)
        return v;
    return infer_from_pattern(model_id_raw, len);
}

uint32_t hu_max_tokens_default(void) {
    return HU_DEFAULT_MODEL_MAX_TOKENS;
}

uint32_t hu_max_tokens_lookup(const char *model_ref, size_t len) {
    if (!model_ref || len == 0)
        return 0;
    const char *p = model_ref;
    trim(&p, &len);
    if (len == 0)
        return 0;

    uint32_t v = lookup_model_candidates(p, len);
    if (v)
        return v;

    const char *slash = memchr(p, '/', len);
    if (slash && slash > p && slash + 1 < p + len) {
        const char *model = slash + 1;
        size_t model_len = (p + len) - model;
        v = lookup_model_candidates(model, model_len);
        if (v)
            return v;
        const char *nested = memchr(model, '/', model_len);
        if (nested && nested + 1 < model + model_len) {
            size_t nested_prov_len = nested - model;
            size_t nested_mod_len = (model + model_len) - (nested + 1);
            v = lookup_model_candidates(nested + 1, nested_mod_len);
            if (v)
                return v;
            v = lookup_table(PROVIDER_MAX, ARRAY_LEN(PROVIDER_MAX), model, nested_prov_len);
            if (v)
                return v;
        }
        const char *last_slash = find_last_char(model, model_len, '/');
        if (last_slash && last_slash + 1 < model + model_len) {
            v = lookup_model_candidates(last_slash + 1, (model + model_len) - (last_slash + 1));
            if (v)
                return v;
        }
        v = lookup_table(PROVIDER_MAX, ARRAY_LEN(PROVIDER_MAX), p, (size_t)(slash - p));
        if (v)
            return v;
    }
    return 0;
}

uint32_t hu_max_tokens_resolve(uint32_t override, const char *model_ref, size_t len) {
    if (override)
        return override;
    uint32_t v = hu_max_tokens_lookup(model_ref, len);
    return v ? v : HU_DEFAULT_MODEL_MAX_TOKENS;
}
