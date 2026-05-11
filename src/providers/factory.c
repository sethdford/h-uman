#include "human/providers/factory.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/provider.h"
#include "human/providers/anthropic.h"
#include "human/providers/claude_cli.h"
#include "human/providers/codex_cli.h"
#include "human/providers/compatible.h"
#ifdef HU_ENABLE_APPLE_INTELLIGENCE
#include "human/providers/apple.h"
#endif
#ifdef HU_ENABLE_COREML
#include "human/providers/coreml.h"
#endif
#ifdef HU_ENABLE_EMBEDDED_MODEL
#include "human/providers/embedded.h"
#endif
/* W13 Bridge A — always include the header so the dispatcher can
 * compile-test the symbol; the implementation falls through to
 * HU_ERR_NOT_SUPPORTED when HU_ENABLE_LLAMACPP is undefined. */
#include "human/providers/llamacpp.h"
#ifdef HU_ENABLE_ML
#include "human/providers/huml.h"
#endif
#include "human/providers/gemini.h"
#include "human/providers/ollama.h"
#include "human/providers/openai.h"
#include "human/providers/openai_codex.h"
#include "human/providers/openrouter.h"
#include <string.h>

static const struct {
    const char *name;
    const char *url;
} hu_compat_providers[] = {
    {"groq", "https://api.groq.com/openai"},
    {"mistral", "https://api.mistral.ai/v1"},
    {"deepseek", "https://api.deepseek.com"},
    {"xai", "https://api.x.ai"},
    {"grok", "https://api.x.ai"},
    {"cerebras", "https://api.cerebras.ai/v1"},
    {"perplexity", "https://api.perplexity.ai"},
    {"cohere", "https://api.cohere.com/compatibility"},
    {"venice", "https://api.venice.ai"},
    {"vercel", "https://ai-gateway.vercel.sh/v1"},
    {"vercel-ai", "https://ai-gateway.vercel.sh/v1"},
    {"together", "https://api.together.xyz"},
    {"together-ai", "https://api.together.xyz"},
    {"fireworks", "https://api.fireworks.ai/inference/v1"},
    {"fireworks-ai", "https://api.fireworks.ai/inference/v1"},
    {"huggingface", "https://router.huggingface.co/v1"},
    {"aihubmix", "https://aihubmix.com/v1"},
    {"siliconflow", "https://api.siliconflow.cn/v1"},
    {"shengsuanyun", "https://router.shengsuanyun.com/api/v1"},
    {"chutes", "https://chutes.ai/api/v1"},
    {"synthetic", "https://api.synthetic.new/openai/v1"},
    {"opencode", "https://opencode.ai/zen/v1"},
    {"opencode-zen", "https://opencode.ai/zen/v1"},
    {"astrai", "https://as-trai.com/v1"},
    {"poe", "https://api.poe.com/v1"},
    {"moonshot", "https://api.moonshot.cn/v1"},
    {"kimi", "https://api.moonshot.cn/v1"},
    {"glm", "https://api.z.ai/api/paas/v4"},
    {"zhipu", "https://api.z.ai/api/paas/v4"},
    {"zai", "https://api.z.ai/api/coding/paas/v4"},
    {"z.ai", "https://api.z.ai/api/coding/paas/v4"},
    {"minimax", "https://api.minimax.io/v1"},
    {"qwen", "https://dashscope.aliyuncs.com/compatible-mode/v1"},
    {"dashscope", "https://dashscope.aliyuncs.com/compatible-mode/v1"},
    {"qianfan", "https://aip.baidubce.com"},
    {"baidu", "https://aip.baidubce.com"},
    {"doubao", "https://ark.cn-beijing.volces.com/api/v3"},
    {"volcengine", "https://ark.cn-beijing.volces.com/api/v3"},
    {"ark", "https://ark.cn-beijing.volces.com/api/v3"},
    {"moonshot-cn", "https://api.moonshot.cn/v1"},
    {"kimi-cn", "https://api.moonshot.cn/v1"},
    {"glm-cn", "https://open.bigmodel.cn/api/paas/v4"},
    {"zhipu-cn", "https://open.bigmodel.cn/api/paas/v4"},
    {"bigmodel", "https://open.bigmodel.cn/api/paas/v4"},
    {"zai-cn", "https://open.bigmodel.cn/api/coding/paas/v4"},
    {"z.ai-cn", "https://open.bigmodel.cn/api/coding/paas/v4"},
    {"minimax-cn", "https://api.minimaxi.com/v1"},
    {"minimaxi", "https://api.minimaxi.com/v1"},
    {"moonshot-intl", "https://api.moonshot.ai/v1"},
    {"moonshot-global", "https://api.moonshot.ai/v1"},
    {"kimi-intl", "https://api.moonshot.ai/v1"},
    {"kimi-global", "https://api.moonshot.ai/v1"},
    {"glm-global", "https://api.z.ai/api/paas/v4"},
    {"zhipu-global", "https://api.z.ai/api/paas/v4"},
    {"zai-global", "https://api.z.ai/api/coding/paas/v4"},
    {"z.ai-global", "https://api.z.ai/api/coding/paas/v4"},
    {"minimax-intl", "https://api.minimax.io/v1"},
    {"minimax-io", "https://api.minimax.io/v1"},
    {"minimax-global", "https://api.minimax.io/v1"},
    {"qwen-intl", "https://dashscope-intl.aliyuncs.com/compatible-mode/v1"},
    {"dashscope-intl", "https://dashscope-intl.aliyuncs.com/compatible-mode/v1"},
    {"qwen-us", "https://dashscope-us.aliyuncs.com/compatible-mode/v1"},
    {"dashscope-us", "https://dashscope-us.aliyuncs.com/compatible-mode/v1"},
    {"byteplus", "https://ark.ap-southeast.bytepluses.com/api/v3"},
    {"kimi-code", "https://api.kimi.com/coding/v1"},
    {"kimi_coding", "https://api.kimi.com/coding/v1"},
    {"volcengine-plan", "https://ark.cn-beijing.volces.com/api/coding/v3"},
    {"byteplus-plan", "https://ark.ap-southeast.bytepluses.com/api/coding/v3"},
    {"qwen-portal", "https://portal.qwen.ai/v1"},
    {"bedrock", "https://bedrock-runtime.us-east-1.amazonaws.com"},
    {"aws-bedrock", "https://bedrock-runtime.us-east-1.amazonaws.com"},
    {"cloudflare", "https://gateway.ai.cloudflare.com/v1"},
    {"cloudflare-ai", "https://gateway.ai.cloudflare.com/v1"},
    {"copilot", "https://api.githubcopilot.com"},
    {"github-copilot", "https://api.githubcopilot.com"},
    {"nvidia", "https://integrate.api.nvidia.com/v1"},
    {"nvidia-nim", "https://integrate.api.nvidia.com/v1"},
    {"build.nvidia.com", "https://integrate.api.nvidia.com/v1"},
    {"ovhcloud", "https://oai.endpoints.kepler.ai.cloud.ovh.net/v1"},
    {"ovh", "https://oai.endpoints.kepler.ai.cloud.ovh.net/v1"},
    {"lmstudio", "http://localhost:1234/v1"},
    {"lm-studio", "http://localhost:1234/v1"},
    {"vllm", "http://localhost:8000/v1"},
    {"llamacpp", "http://localhost:8080/v1"},
    {"llama.cpp", "http://localhost:8080/v1"},
    {"sglang", "http://localhost:30000/v1"},
    {"osaurus", "http://localhost:1337/v1"},
    {"litellm", "http://localhost:4000"},
    {"mlx_local", "http://127.0.0.1:8741/v1"},
    {"mlx-local", "http://127.0.0.1:8741/v1"},
};

static const size_t hu_compat_providers_count =
    sizeof(hu_compat_providers) / sizeof(hu_compat_providers[0]);

const char *hu_compatible_provider_url(const char *name) {
    if (!name)
        return NULL;
    for (size_t i = 0; i < hu_compat_providers_count; i++) {
        if (strcmp(hu_compat_providers[i].name, name) == 0)
            return hu_compat_providers[i].url;
    }
    return NULL;
}

hu_error_t hu_provider_create(hu_allocator_t *alloc, const char *name, size_t name_len,
                              const char *api_key, size_t api_key_len, const char *base_url,
                              size_t base_url_len, hu_provider_t *out) {
    if (!alloc || !name || name_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;

    if (name_len == 6 && memcmp(name, "openai", 6) == 0) {
        return hu_openai_create(alloc, api_key, api_key_len, base_url, base_url_len, out);
    }
    if (name_len == 9 && memcmp(name, "anthropic", 9) == 0) {
        return hu_anthropic_create(alloc, api_key, api_key_len, base_url, base_url_len, out);
    }
    if (name_len == 6 && memcmp(name, "gemini", 6) == 0) {
        return hu_gemini_create(alloc, api_key, api_key_len, base_url, base_url_len, out);
    }
    if (name_len == 6 && memcmp(name, "google", 6) == 0) {
        return hu_gemini_create(alloc, api_key, api_key_len, base_url, base_url_len, out);
    }
    if (name_len == 13 && memcmp(name, "google-gemini", 13) == 0) {
        return hu_gemini_create(alloc, api_key, api_key_len, base_url, base_url_len, out);
    }
    if (name_len == 6 && memcmp(name, "vertex", 6) == 0) {
        return hu_gemini_create(alloc, api_key, api_key_len, base_url, base_url_len, out);
    }
    if (name_len == 6 && memcmp(name, "ollama", 6) == 0) {
        return hu_ollama_create(alloc, api_key, api_key_len, base_url, base_url_len, out);
    }
    if (name_len == 10 && memcmp(name, "openrouter", 10) == 0) {
        return hu_openrouter_create(alloc, api_key, api_key_len, base_url, base_url_len, out);
    }
    if (name_len == 10 && memcmp(name, "compatible", 10) == 0) {
        return hu_compatible_create(alloc, api_key, api_key_len, base_url, base_url_len, out);
    }
    if (name_len == 10 && memcmp(name, "claude_cli", 10) == 0) {
        return hu_claude_cli_create(alloc, api_key, api_key_len, base_url, base_url_len, out);
    }
    if (name_len == 9 && memcmp(name, "codex_cli", 9) == 0) {
        return hu_codex_cli_create(alloc, api_key, api_key_len, base_url, base_url_len, out);
    }
    if (name_len == 12 && memcmp(name, "openai-codex", 12) == 0) {
        return hu_openai_codex_create(alloc, api_key, api_key_len, base_url, base_url_len, out);
    }

#ifdef HU_ENABLE_APPLE_INTELLIGENCE
    if (name_len == 5 && memcmp(name, "apple", 5) == 0) {
        hu_apple_config_t ac = {.base_url = base_url, .base_url_len = base_url_len};
        return hu_apple_provider_create(alloc, &ac, out);
    }
    if (name_len == 5 && memcmp(name, "apfel", 5) == 0) {
        hu_apple_config_t ac = {.base_url = base_url, .base_url_len = base_url_len};
        return hu_apple_provider_create(alloc, &ac, out);
    }
    if (name_len == 18 && memcmp(name, "apple-intelligence", 18) == 0) {
        hu_apple_config_t ac = {.base_url = base_url, .base_url_len = base_url_len};
        return hu_apple_provider_create(alloc, &ac, out);
    }
    if (name_len == 16 && memcmp(name, "foundationmodels", 16) == 0) {
        hu_apple_config_t ac = {.base_url = base_url, .base_url_len = base_url_len};
        return hu_apple_provider_create(alloc, &ac, out);
    }
#endif

#ifdef HU_ENABLE_COREML
    if (name_len == 7 && memcmp(name, "coreml", 7) == 0) {
        hu_coreml_config_t cc = {.model_path = base_url, .model_path_len = base_url_len};
        return hu_coreml_provider_create(alloc, &cc, out);
    }
    if (name_len == 3 && memcmp(name, "mlx", 3) == 0) {
        hu_coreml_config_t cc = {.model_path = base_url, .model_path_len = base_url_len};
        return hu_coreml_provider_create(alloc, &cc, out);
    }
#endif

#ifdef HU_ENABLE_EMBEDDED_MODEL
    if (name_len == 8 && memcmp(name, "embedded", 8) == 0) {
        hu_embedded_config_t ec = {0};
        if (base_url && base_url_len > 0) {
            ec.model_path = (char *)base_url;
        }
        return hu_embedded_provider_create(alloc, &ec, out);
    }
    if (name_len == 9 && memcmp(name, "llama-cli", 9) == 0) {
        hu_embedded_config_t ec = {0};
        if (base_url && base_url_len > 0) {
            ec.model_path = (char *)base_url;
        }
        return hu_embedded_provider_create(alloc, &ec, out);
    }
#endif

    /* W13 Bridge A — in-process llama.cpp. The factory always succeeds
     * when selected; the chat/load_adapter hooks return NOT_SUPPORTED
     * when libllama isn't linked. base_url, when present, is treated
     * as the GGUF model path. */
    if (name_len == 8 && memcmp(name, "llamacpp", 8) == 0) {
        hu_llamacpp_config_t lc = {0};
        if (base_url && base_url_len > 0) {
            char *path = hu_strndup(alloc, base_url, base_url_len);
            if (!path)
                return HU_ERR_OUT_OF_MEMORY;
            lc.model_path = path;
        }
        hu_error_t r = hu_llamacpp_provider_create(alloc, &lc, out);
        if (lc.model_path)
            alloc->free(alloc->ctx, lc.model_path, strlen(lc.model_path) + 1);
        return r;
    }

#ifdef HU_ENABLE_ML
    if (name_len == 4 && memcmp(name, "huml", 4) == 0) {
        hu_huml_config_t hc = {0};
        if (base_url && base_url_len > 0) {
            hc.checkpoint_path = base_url;
            hc.checkpoint_path_len = base_url_len;
        }
        return hu_huml_provider_create(alloc, &hc, out);
    }
#endif

    if (name_len > 7 && memcmp(name, "custom:", 7) == 0) {
        const char *url = name + 7;
        size_t url_len = name_len - 7;
        return hu_compatible_create(alloc, api_key, api_key_len, url, url_len, out);
    }
    if (name_len > 17 && memcmp(name, "anthropic-custom:", 17) == 0) {
        const char *url = name + 17;
        size_t url_len = name_len - 17;
        return hu_anthropic_create(alloc, api_key, api_key_len, url, url_len, out);
    }

    {
        char nbuf[128];
        if (name_len < sizeof(nbuf)) {
            memcpy(nbuf, name, name_len);
            nbuf[name_len] = '\0';
            const char *compat_url = hu_compatible_provider_url(nbuf);
            if (compat_url) {
                const char *url = base_url;
                size_t url_len = base_url_len;
                if (!url || url_len == 0) {
                    url = compat_url;
                    url_len = strlen(compat_url);
                }
                return hu_compatible_create(alloc, api_key, api_key_len, url, url_len, out);
            }
        }
    }

    return HU_ERR_NOT_SUPPORTED;
}

/* Phase 1 (RL SOTA) — entry-aware factory.
 *
 * The legacy hu_provider_create above only forwards (name, api_key,
 * base_url) — fine for cloud providers, but llamacpp also needs
 * context_size / threads / use_gpu / n_gpu_layers from the JSON config
 * to actually use Metal and tune the context window. This helper reads
 * those four fields off hu_provider_entry_t and forwards them.
 *
 * For non-llamacpp providers it falls through to the legacy path so
 * the existing call sites don't need to change.
 *
 * Test-only state: the most recent llamacpp config the factory built
 * is captured into s_last_llamacpp_* so test_llamacpp_factory_config.c
 * can verify the wiring without spinning up a real model. The
 * model_path is deep-copied because the factory frees the source after
 * hu_llamacpp_provider_create returns.
 */

#ifdef HU_IS_TEST
#include <stdlib.h>
static hu_llamacpp_config_t s_last_llamacpp_config;
static char *s_last_llamacpp_model_path_copy;
static bool s_last_llamacpp_config_set = false;

static void hu_llamacpp_factory_capture_for_test(const hu_llamacpp_config_t *cfg) {
    if (s_last_llamacpp_model_path_copy) {
        free(s_last_llamacpp_model_path_copy);
        s_last_llamacpp_model_path_copy = NULL;
    }
    s_last_llamacpp_config = *cfg;
    if (cfg->model_path) {
        size_t n = strlen(cfg->model_path);
        s_last_llamacpp_model_path_copy = (char *)malloc(n + 1);
        if (s_last_llamacpp_model_path_copy) {
            memcpy(s_last_llamacpp_model_path_copy, cfg->model_path, n + 1);
            s_last_llamacpp_config.model_path = s_last_llamacpp_model_path_copy;
        } else {
            s_last_llamacpp_config.model_path = NULL;
        }
    }
    s_last_llamacpp_config_set = true;
}

const hu_llamacpp_config_t *hu_llamacpp_factory_last_config(void) {
    return s_last_llamacpp_config_set ? &s_last_llamacpp_config : NULL;
}

void hu_llamacpp_factory_reset_for_test(void) {
    if (s_last_llamacpp_model_path_copy) {
        free(s_last_llamacpp_model_path_copy);
        s_last_llamacpp_model_path_copy = NULL;
    }
    s_last_llamacpp_config_set = false;
    memset(&s_last_llamacpp_config, 0, sizeof(s_last_llamacpp_config));
}
#endif

hu_error_t hu_provider_create_from_entry(hu_allocator_t *alloc,
                                         const hu_provider_entry_t *entry,
                                         hu_provider_t *out) {
    if (!alloc || !entry || !entry->name || !out)
        return HU_ERR_INVALID_ARGUMENT;
    size_t name_len = strlen(entry->name);
    bool is_llamacpp =
        (name_len == 8 && memcmp(entry->name, "llamacpp", 8) == 0) ||
        (name_len == 9 && memcmp(entry->name, "llama.cpp", 9) == 0);
    if (is_llamacpp) {
        hu_llamacpp_config_t lc = {0};
        if (entry->base_url && entry->base_url[0]) {
            char *path = hu_strdup(alloc, entry->base_url);
            if (!path) return HU_ERR_OUT_OF_MEMORY;
            lc.model_path = path;
        }
        lc.context_size = entry->context_size;
        lc.threads      = entry->threads;
        lc.use_gpu      = entry->use_gpu;
        lc.n_gpu_layers = entry->n_gpu_layers;
#ifdef HU_IS_TEST
        hu_llamacpp_factory_capture_for_test(&lc);
#endif
        hu_error_t r = hu_llamacpp_provider_create(alloc, &lc, out);
        if (lc.model_path)
            alloc->free(alloc->ctx, lc.model_path, strlen(lc.model_path) + 1);
        return r;
    }
    /* Non-llamacpp: defer to the legacy by-name path. */
    return hu_provider_create(alloc, entry->name, name_len,
                              entry->api_key,
                              entry->api_key ? strlen(entry->api_key) : 0,
                              entry->base_url,
                              entry->base_url ? strlen(entry->base_url) : 0,
                              out);
}
