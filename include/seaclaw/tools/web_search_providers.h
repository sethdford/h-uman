#ifndef SC_WEB_SEARCH_PROVIDERS_H
#define SC_WEB_SEARCH_PROVIDERS_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"

/* URL-encode a string (for query params). Caller frees with alloc->free(ctx, *out, *out_len + 1). */
sc_error_t sc_web_search_url_encode(sc_allocator_t *alloc,
    const char *input, size_t input_len,
    char **out, size_t *out_len);

/* Brave: requires BRAVE_API_KEY. Returns owned output or sets out on failure. */
sc_error_t sc_web_search_brave(sc_allocator_t *alloc,
    const char *query, size_t query_len,
    int count, const char *api_key,
    sc_tool_result_t *out);

/* DuckDuckGo: no API key. */
sc_error_t sc_web_search_duckduckgo(sc_allocator_t *alloc,
    const char *query, size_t query_len,
    int count,
    sc_tool_result_t *out);

/* Tavily: requires TAVILY_API_KEY or WEB_SEARCH_API_KEY. */
sc_error_t sc_web_search_tavily(sc_allocator_t *alloc,
    const char *query, size_t query_len,
    int count, const char *api_key,
    sc_tool_result_t *out);

/* Exa: requires EXA_API_KEY or WEB_SEARCH_API_KEY. */
sc_error_t sc_web_search_exa(sc_allocator_t *alloc,
    const char *query, size_t query_len,
    int count, const char *api_key,
    sc_tool_result_t *out);

/* Firecrawl: requires FIRECRAWL_API_KEY or WEB_SEARCH_API_KEY. */
sc_error_t sc_web_search_firecrawl(sc_allocator_t *alloc,
    const char *query, size_t query_len,
    int count, const char *api_key,
    sc_tool_result_t *out);

/* Perplexity: requires PERPLEXITY_API_KEY or WEB_SEARCH_API_KEY. */
sc_error_t sc_web_search_perplexity(sc_allocator_t *alloc,
    const char *query, size_t query_len,
    int count, const char *api_key,
    sc_tool_result_t *out);

/* Jina: requires JINA_API_KEY or WEB_SEARCH_API_KEY. Plain text response. */
sc_error_t sc_web_search_jina(sc_allocator_t *alloc,
    const char *query, size_t query_len,
    int count, const char *api_key,
    sc_tool_result_t *out);

/* SearXNG: requires SEARXNG_BASE_URL (self-hosted, no API key). */
sc_error_t sc_web_search_searxng(sc_allocator_t *alloc,
    const char *query, size_t query_len,
    int count, const char *base_url,
    sc_tool_result_t *out);

#endif
