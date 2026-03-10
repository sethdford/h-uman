#ifndef HU_WEB_SEARCH_PROVIDERS_H
#define HU_WEB_SEARCH_PROVIDERS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/tool.h"

/* URL-encode a string (for query params). Caller frees with alloc->free(ctx, *out, *out_len + 1).
 */
hu_error_t hu_web_search_url_encode(hu_allocator_t *alloc, const char *input, size_t input_len,
                                    char **out, size_t *out_len);

/* Brave: requires BRAVE_API_KEY. Returns owned output or sets out on failure. */
hu_error_t hu_web_search_brave(hu_allocator_t *alloc, const char *query, size_t query_len,
                               int count, const char *api_key, hu_tool_result_t *out);

/* DuckDuckGo: no API key. */
hu_error_t hu_web_search_duckduckgo(hu_allocator_t *alloc, const char *query, size_t query_len,
                                    int count, hu_tool_result_t *out);

/* Tavily: requires TAVILY_API_KEY or WEB_SEARCH_API_KEY. */
hu_error_t hu_web_search_tavily(hu_allocator_t *alloc, const char *query, size_t query_len,
                                int count, const char *api_key, hu_tool_result_t *out);

/* Exa: requires EXA_API_KEY or WEB_SEARCH_API_KEY. */
hu_error_t hu_web_search_exa(hu_allocator_t *alloc, const char *query, size_t query_len, int count,
                             const char *api_key, hu_tool_result_t *out);

/* Firecrawl: requires FIRECRAWL_API_KEY or WEB_SEARCH_API_KEY. */
hu_error_t hu_web_search_firecrawl(hu_allocator_t *alloc, const char *query, size_t query_len,
                                   int count, const char *api_key, hu_tool_result_t *out);

/* Perplexity: requires PERPLEXITY_API_KEY or WEB_SEARCH_API_KEY. */
hu_error_t hu_web_search_perplexity(hu_allocator_t *alloc, const char *query, size_t query_len,
                                    int count, const char *api_key, hu_tool_result_t *out);

/* Jina: requires JINA_API_KEY or WEB_SEARCH_API_KEY. Plain text response. */
hu_error_t hu_web_search_jina(hu_allocator_t *alloc, const char *query, size_t query_len, int count,
                              const char *api_key, hu_tool_result_t *out);

/* SearXNG: requires SEARXNG_BASE_URL (self-hosted, no API key). */
hu_error_t hu_web_search_searxng(hu_allocator_t *alloc, const char *query, size_t query_len,
                                 int count, const char *base_url, hu_tool_result_t *out);

/* Shared result formatter for providers using standard title/url/snippet JSON. */
hu_error_t hu_web_search_format_results(hu_allocator_t *alloc, const char *query, size_t query_len,
                                        hu_json_value_t *results_array, int count,
                                        const char *title_key, const char *url_key,
                                        const char *snippet_key, hu_tool_result_t *out);

#endif
