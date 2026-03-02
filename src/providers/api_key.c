#include "seaclaw/providers/api_key.h"
#include "seaclaw/core/string.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define getenv_safe(n) getenv(n)
#else
#include <stdlib.h>
#define getenv_safe(n) getenv(n)
#endif

static void trim(const char *s, size_t len, size_t *start_out, size_t *end_out) {
    size_t start = 0, end = len;
    while (start < len && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n'))
        start++;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n'))
        end--;
    *start_out = start;
    *end_out = end;
}

static const char *provider_env(const char *name, size_t name_len, char *buf, size_t buf_size) {
    static const struct { const char *key; size_t klen; const char *env; } map[] = {
        {"anthropic", 9, "ANTHROPIC_API_KEY"},
        {"openai", 6, "OPENAI_API_KEY"},
        {"openrouter", 10, "OPENROUTER_API_KEY"},
        {"gemini", 6, "GEMINI_API_KEY"},
        {"groq", 4, "GROQ_API_KEY"},
        {"ollama", 6, "API_KEY"},
        {"codex-cli", 9, "OPENAI_API_KEY"},
        {"claude_cli", 10, "ANTHROPIC_API_KEY"},
    };
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
        if (name_len == map[i].klen && memcmp(name, map[i].key, name_len) == 0) {
            const char *v = getenv_safe(map[i].env);
            if (v && strlen(v) > 0) return v;
        }
    }
    const char *v = getenv_safe("SEACLAW_API_KEY");
    if (v && strlen(v) > 0) return v;
    v = getenv_safe("API_KEY");
    if (v && strlen(v) > 0) return v;
    (void)buf;
    (void)buf_size;
    return NULL;
}

char *sc_api_key_resolve(sc_allocator_t *alloc,
    const char *provider_name, size_t provider_name_len,
    const char *api_key, size_t api_key_len)
{
    size_t start, end;
    if (api_key && api_key_len > 0) {
        trim(api_key, api_key_len, &start, &end);
        if (end > start) return sc_strndup(alloc, api_key + start, end - start);
    }
    char dummy[1];
    const char *v = provider_env(provider_name, provider_name_len, dummy, 0);
    if (v) {
        size_t n = strlen(v);
        trim(v, n, &start, &end);
        if (end > start) return sc_strndup(alloc, v + start, end - start);
    }
    return NULL;
}

bool sc_api_key_valid(const char *key, size_t key_len) {
    if (!key) return false;
    size_t start, end;
    trim(key, key_len, &start, &end);
    return end > start;
}

char *sc_api_key_mask(sc_allocator_t *alloc, const char *key, size_t key_len) {
    if (!key || key_len == 0) return sc_strndup(alloc, "[no key]", 8);
    if (key_len <= 4) return sc_strndup(alloc, "****", 4);
    size_t n = 4 + 3 + 1;
    char *buf = (char *)alloc->alloc(alloc->ctx, n);
    if (!buf) return NULL;
    memcpy(buf, key, 4);
    memcpy(buf + 4, "...", 3);
    buf[7] = '\0';
    return buf;
}
