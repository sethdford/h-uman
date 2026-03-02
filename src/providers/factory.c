#include "seaclaw/providers/factory.h"
#include "seaclaw/providers/openai.h"
#include "seaclaw/providers/anthropic.h"
#include "seaclaw/providers/gemini.h"
#include "seaclaw/providers/ollama.h"
#include "seaclaw/providers/openrouter.h"
#include "seaclaw/providers/compatible.h"
#include "seaclaw/providers/claude_cli.h"
#include "seaclaw/providers/codex_cli.h"
#include "seaclaw/providers/openai_codex.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/provider.h"
#include <string.h>

static const struct { const char *name; const char *url; } sc_compat_providers[] = {
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
};

static const size_t sc_compat_providers_count =
    sizeof(sc_compat_providers) / sizeof(sc_compat_providers[0]);

const char *sc_compatible_provider_url(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sc_compat_providers_count; i++) {
        if (strcmp(sc_compat_providers[i].name, name) == 0)
            return sc_compat_providers[i].url;
    }
    return NULL;
}

sc_error_t sc_provider_create(sc_allocator_t *alloc,
    const char *name, size_t name_len,
    const char *api_key, size_t api_key_len,
    const char *base_url, size_t base_url_len,
    sc_provider_t *out)
{
    if (!alloc || !name || name_len == 0 || !out) return SC_ERR_INVALID_ARGUMENT;

    if (name_len == 6 && memcmp(name, "openai", 6) == 0) {
        return sc_openai_create(alloc, api_key, api_key_len,
            base_url, base_url_len, out);
    }
    if (name_len == 9 && memcmp(name, "anthropic", 9) == 0) {
        return sc_anthropic_create(alloc, api_key, api_key_len,
            base_url, base_url_len, out);
    }
    if (name_len == 6 && memcmp(name, "gemini", 6) == 0) {
        return sc_gemini_create(alloc, api_key, api_key_len,
            base_url, base_url_len, out);
    }
    if (name_len == 6 && memcmp(name, "google", 6) == 0) {
        return sc_gemini_create(alloc, api_key, api_key_len,
            base_url, base_url_len, out);
    }
    if (name_len == 13 && memcmp(name, "google-gemini", 13) == 0) {
        return sc_gemini_create(alloc, api_key, api_key_len,
            base_url, base_url_len, out);
    }
    if (name_len == 6 && memcmp(name, "ollama", 6) == 0) {
        return sc_ollama_create(alloc, api_key, api_key_len,
            base_url, base_url_len, out);
    }
    if (name_len == 10 && memcmp(name, "openrouter", 10) == 0) {
        return sc_openrouter_create(alloc, api_key, api_key_len,
            base_url, base_url_len, out);
    }
    if (name_len == 10 && memcmp(name, "compatible", 10) == 0) {
        return sc_compatible_create(alloc, api_key, api_key_len,
            base_url, base_url_len, out);
    }
    if (name_len == 10 && memcmp(name, "claude_cli", 10) == 0) {
        return sc_claude_cli_create(alloc, api_key, api_key_len,
            base_url, base_url_len, out);
    }
    if (name_len == 9 && memcmp(name, "codex_cli", 9) == 0) {
        return sc_codex_cli_create(alloc, api_key, api_key_len,
            base_url, base_url_len, out);
    }
    if (name_len == 12 && memcmp(name, "openai-codex", 12) == 0) {
        return sc_openai_codex_create(alloc, api_key, api_key_len,
            base_url, base_url_len, out);
    }

    if (name_len > 7 && memcmp(name, "custom:", 7) == 0) {
        const char *url = name + 7;
        size_t url_len = name_len - 7;
        return sc_compatible_create(alloc, api_key, api_key_len,
            url, url_len, out);
    }
    if (name_len > 17 && memcmp(name, "anthropic-custom:", 17) == 0) {
        const char *url = name + 17;
        size_t url_len = name_len - 17;
        return sc_anthropic_create(alloc, api_key, api_key_len,
            url, url_len, out);
    }

    {
        char nbuf[128];
        if (name_len < sizeof(nbuf)) {
            memcpy(nbuf, name, name_len);
            nbuf[name_len] = '\0';
            const char *compat_url = sc_compatible_provider_url(nbuf);
            if (compat_url) {
                const char *url = base_url;
                size_t url_len = base_url_len;
                if (!url || url_len == 0) {
                    url = compat_url;
                    url_len = strlen(compat_url);
                }
                return sc_compatible_create(alloc, api_key, api_key_len,
                    url, url_len, out);
            }
        }
    }

    return SC_ERR_NOT_SUPPORTED;
}
