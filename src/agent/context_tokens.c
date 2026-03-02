#include "seaclaw/context_tokens.h"
#include <stdint.h>
#include "seaclaw/config_types.h"
#include "seaclaw/util.h"
#include <string.h>
#include <ctype.h>

typedef struct {
    const char *key;
    uint64_t tokens;
} ctx_entry_t;

static const ctx_entry_t MODEL_WINDOWS[] = {
    { "claude-opus-4-6", 200000 },
    { "claude-opus-4.6", 200000 },
    { "claude-sonnet-4-6", 200000 },
    { "claude-sonnet-4.6", 200000 },
    { "claude-haiku-4-5", 200000 },
    { "gpt-5.2", 128000 },
    { "gpt-5.2-codex", 128000 },
    { "gpt-4.5-preview", 128000 },
    { "gpt-4.1", 128000 },
    { "gpt-4.1-mini", 128000 },
    { "o3-mini", 128000 },
    { "gemini-2.5-pro", 200000 },
    { "gemini-2.5-flash", 200000 },
    { "gemini-2.0-flash", 200000 },
    { "deepseek-v3.2", 128000 },
    { "deepseek-chat", 128000 },
    { "deepseek-reasoner", 128000 },
    { "llama-4-70b-instruct", 128000 },
    { "llama-3.3-70b-versatile", 128000 },
    { "llama-3.1-8b-instant", 128000 },
    { "mixtral-8x7b-32768", 32768 },
};

static const ctx_entry_t PROVIDER_WINDOWS[] = {
    { "openrouter", 200000 },
    { "minimax", 200000 },
    { "openai-codex", 200000 },
    { "moonshot", 256000 },
    { "kimi", 262144 },
    { "kimi-coding", 262144 },
    { "xiaomi", 262144 },
    { "ollama", 128000 },
    { "qwen", 128000 },
    { "vllm", 128000 },
    { "github-copilot", 128000 },
    { "qianfan", 98304 },
    { "nvidia", 131072 },
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static int strncasecmpx(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = a[i] ? (int)tolower((unsigned char)a[i]) : 0;
        int cb = b[i] ? (int)tolower((unsigned char)b[i]) : 0;
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
    return 0;
}

static int str_eql(const char *a, size_t a_len, const char *b) {
    size_t bl = strlen(b);
    if (a_len != bl) return 0;
    return strncasecmpx(a, b, a_len) == 0;
}

static int starts_with_ic(const char *hay, size_t hay_len, const char *prefix) {
    size_t pl = strlen(prefix);
    if (hay_len < pl) return 0;
    return strncasecmpx(hay, prefix, pl) == 0;
}

static int ends_with_ic(const char *hay, size_t hay_len, const char *suffix) {
    size_t sl = strlen(suffix);
    if (hay_len < sl) return 0;
    return strncasecmpx(hay + hay_len - sl, suffix, sl) == 0;
}

static int is_all_digits(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return len > 0;
}

static void trim(const char **s, size_t *len) {
    while (*len > 0 && (*s)[0] <= ' ') { (*s)++; (*len)--; }
    while (*len > 0 && (*s)[*len - 1] <= ' ') (*len)--;
}

/* If model_id ends with -YYYYMMDD (8 digits), set *out_len to length before dash. */
static const char *find_last_char(const char *s, size_t len, char c) {
    for (size_t i = len; i > 0; i--) {
        if (s[i - 1] == c) return s + (i - 1);
    }
    return NULL;
}

static void strip_date_suffix(const char *model_id, size_t len, size_t *out_len) {
    *out_len = len;
    const char *last = find_last_char(model_id, len, '-');
    if (!last || last + 1 >= model_id + len) return;
    size_t suffix_len = (model_id + len) - (last + 1);
    if (suffix_len == 8 && is_all_digits(last + 1, suffix_len))
        *out_len = (size_t)(last - model_id);
}

static uint64_t lookup_table(const ctx_entry_t *tbl, size_t tbl_len,
                             const char *key, size_t key_len) {
    for (size_t i = 0; i < tbl_len; i++) {
        if (str_eql(key, key_len, tbl[i].key))
            return tbl[i].tokens;
    }
    return 0;
}

static int contains_str(const char *hay, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (hay_len < nlen) return 0;
    for (size_t i = 0; i <= hay_len - nlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) return 1;
    }
    return 0;
}

static uint64_t infer_from_pattern(const char *model_id, size_t len) {
    if (len >= 5 && contains_str(model_id, len, "32768")) return 32768;
    if (starts_with_ic(model_id, len, "claude-")) return 200000;
    if (starts_with_ic(model_id, len, "gpt-") ||
        starts_with_ic(model_id, len, "o1") ||
        starts_with_ic(model_id, len, "o3")) return 128000;
    if (starts_with_ic(model_id, len, "gemini-")) return 200000;
    if (starts_with_ic(model_id, len, "deepseek-")) return 128000;
    if (starts_with_ic(model_id, len, "llama") ||
        starts_with_ic(model_id, len, "mixtral-")) return 128000;
    return 0;
}

static uint64_t lookup_model_candidates(const char *model_id_raw, size_t len) {
    const char *no_latest = model_id_raw;
    size_t no_latest_len = len;
    if (ends_with_ic(model_id_raw, len, "-latest"))
        no_latest_len = len - 7;

    size_t no_date_len = no_latest_len;
    strip_date_suffix(no_latest, no_latest_len, &no_date_len);

    uint64_t v = lookup_table(MODEL_WINDOWS, ARRAY_LEN(MODEL_WINDOWS),
                              model_id_raw, len);
    if (v) return v;
    if (no_latest_len != len) {
        v = lookup_table(MODEL_WINDOWS, ARRAY_LEN(MODEL_WINDOWS),
                         no_latest, no_latest_len);
        if (v) return v;
    }
    v = infer_from_pattern(no_latest, no_date_len);
    if (v) return v;
    v = infer_from_pattern(no_latest, no_latest_len);
    if (v) return v;
    return infer_from_pattern(model_id_raw, len);
}

uint64_t sc_context_tokens_default(void) {
    return SC_DEFAULT_AGENT_TOKEN_LIMIT;
}

uint64_t sc_context_tokens_lookup(const char *model_ref, size_t len) {
    if (!model_ref || len == 0) return 0;
    const char *p = model_ref;
    trim(&p, &len);
    if (len == 0) return 0;

    uint64_t v = lookup_model_candidates(p, len);
    if (v) return v;

    const char *slash = memchr(p, '/', len);
    if (slash && slash > p && slash + 1 < p + len) {
        const char *model = slash + 1;
        size_t model_len = (p + len) - model;
        v = lookup_model_candidates(model, model_len);
        if (v) return v;
        if (memchr(model, '/', model_len)) {
            const char *nested = memchr(model, '/', model_len);
            if (nested && nested + 1 < model + model_len) {
                size_t nested_prov_len = nested - model;
                size_t nested_mod_len = (model + model_len) - (nested + 1);
                v = lookup_model_candidates(nested + 1, nested_mod_len);
                if (v) return v;
                v = lookup_table(PROVIDER_WINDOWS, ARRAY_LEN(PROVIDER_WINDOWS),
                                 model, nested_prov_len);
                if (v) return v;
            }
        }
        const char *last_slash = find_last_char(model, model_len, '/');
        if (last_slash && last_slash + 1 < model + model_len) {
            v = lookup_model_candidates(last_slash + 1,
                                         (model + model_len) - (last_slash + 1));
            if (v) return v;
        }
        size_t prov_len = slash - p;
        v = lookup_table(PROVIDER_WINDOWS, ARRAY_LEN(PROVIDER_WINDOWS),
                         p, prov_len);
        if (v) return v;
    }
    return 0;
}

uint64_t sc_context_tokens_resolve(uint64_t override, const char *model_ref, size_t len) {
    if (override) return override;
    uint64_t v = sc_context_tokens_lookup(model_ref, len);
    return v ? v : SC_DEFAULT_AGENT_TOKEN_LIMIT;
}
