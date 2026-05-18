/* MLX server admin client — see include/human/ml/mlx_admin.h.
 *
 * Phase B2 (2026-05-17 round 2) bridge: closes the WRITE side of the
 * M3 personalization loop by letting the daemon ask the MLX server to
 * hot-swap LoRA adapter weights at runtime. The server endpoint lives
 * in scripts/mlx-server.py; this file is the C side that calls it.
 *
 * Design choices:
 *   - Lives in src/ml/, NOT src/providers/. The provider vtable stays
 *     a NOT_SUPPORTED stub until we do direct libmlx integration —
 *     this admin path is HTTP-only and orthogonal to the chat vtable.
 *   - Takes the server base URL as a parameter rather than reading
 *     config inside. Lets tests inject a mock URL and lets the daemon
 *     supply whatever it's configured for.
 *   - HTTP failure modes are sharply distinguished from server-side
 *     rejection: HU_OK + status_code is "the server replied"; HU_ERR_IO
 *     is "we couldn't even reach it". The training-loop caller cares
 *     about the difference.
 */

#include "human/ml/mlx_admin.h"

#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HU_ENABLE_CURL
/* When curl is disabled (release minimal builds, fuzz-only configs)
 * the admin layer is a NOT_SUPPORTED stub — matching the M3 dispatcher
 * safety contract. The daemon falls through gracefully and the
 * personalization loop simply doesn't close until a curl-enabled
 * binary is in use. */
hu_error_t hu_mlx_admin_swap_adapter(hu_allocator_t *alloc, const char *base_url,
                                     size_t base_url_len, const char *adapter_path,
                                     size_t adapter_path_len, hu_mlx_admin_swap_result_t *result) {
    (void)alloc;
    (void)base_url;
    (void)base_url_len;
    (void)adapter_path;
    (void)adapter_path_len;
    if (result)
        memset(result, 0, sizeof(*result));
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_mlx_admin_current_adapter(hu_allocator_t *alloc, const char *base_url,
                                        size_t base_url_len, hu_mlx_admin_current_adapter_t *out) {
    (void)alloc;
    (void)base_url;
    (void)base_url_len;
    if (out)
        memset(out, 0, sizeof(*out));
    return HU_ERR_NOT_SUPPORTED;
}

void hu_mlx_admin_swap_result_free(hu_allocator_t *alloc, hu_mlx_admin_swap_result_t *result) {
    (void)alloc;
    (void)result;
}

void hu_mlx_admin_current_adapter_free(hu_allocator_t *alloc,
                                       hu_mlx_admin_current_adapter_t *current) {
    (void)alloc;
    (void)current;
}

#else /* HU_ENABLE_CURL */

/* Build a URL by appending `suffix` to `base_url`. Inserts a '/' if
 * base_url doesn't already end with one. Returned string is owned. */
static char *join_url(hu_allocator_t *alloc, const char *base_url, size_t base_url_len,
                      const char *suffix) {
    if (!base_url || base_url_len == 0 || !suffix)
        return NULL;
    size_t suffix_len = strlen(suffix);
    int needs_slash = base_url[base_url_len - 1] != '/' && suffix[0] != '/';
    size_t total = base_url_len + (needs_slash ? 1u : 0u) + suffix_len + 1u;
    char *url = (char *)alloc->alloc(alloc->ctx, total);
    if (!url)
        return NULL;
    memcpy(url, base_url, base_url_len);
    size_t off = base_url_len;
    if (needs_slash)
        url[off++] = '/';
    memcpy(url + off, suffix, suffix_len);
    url[off + suffix_len] = '\0';
    return url;
}

/* Parse `tensors_loaded` (number) and `adapter_path` (string) from a
 * JSON response body into the swap result. Tolerant: missing fields
 * leave the result at their zero/NULL defaults. */
static void parse_swap_response(hu_allocator_t *alloc, const char *body, size_t body_len,
                                hu_mlx_admin_swap_result_t *result) {
    if (!body || body_len == 0)
        return;
    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, body, body_len, &root) != HU_OK || !root)
        return;

    double tensors = hu_json_get_number(root, "tensors_loaded", 0.0);
    if (tensors > 0.0)
        result->tensors_loaded = (size_t)tensors;

    const char *path = hu_json_get_string(root, "adapter_path");
    if (path && path[0]) {
        size_t plen = strlen(path);
        char *copy = (char *)alloc->alloc(alloc->ctx, plen + 1);
        if (copy) {
            memcpy(copy, path, plen);
            copy[plen] = '\0';
            result->resolved_adapter_path = copy;
            result->resolved_adapter_path_len = plen;
        }
    }

    hu_json_free(alloc, root);
}

hu_error_t hu_mlx_admin_swap_adapter(hu_allocator_t *alloc, const char *base_url,
                                     size_t base_url_len, const char *adapter_path,
                                     size_t adapter_path_len, hu_mlx_admin_swap_result_t *result) {
    if (!alloc || !base_url || base_url_len == 0 || !adapter_path || adapter_path_len == 0 ||
        !result)
        return HU_ERR_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));

    char *url = join_url(alloc, base_url, base_url_len, "adapters/swap");
    if (!url)
        return HU_ERR_OUT_OF_MEMORY;

    /* Build the request body. Using the JSON builder keeps escaping
     * correct for paths containing spaces, quotes, or backslashes. */
    hu_json_buf_t buf;
    hu_error_t jerr = hu_json_buf_init(&buf, alloc);
    if (jerr != HU_OK) {
        alloc->free(alloc->ctx, url, strlen(url) + 1);
        return jerr;
    }
    jerr = hu_json_buf_append_raw(&buf, "{", 1);
    if (jerr == HU_OK)
        jerr = hu_json_append_key_value(&buf, "adapter_path", 12, adapter_path, adapter_path_len);
    if (jerr == HU_OK)
        jerr = hu_json_buf_append_raw(&buf, "}", 1);
    if (jerr != HU_OK) {
        alloc->free(alloc->ctx, url, strlen(url) + 1);
        return jerr;
    }

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_post_json(alloc, url, /*auth_header=*/NULL, buf.ptr, buf.len, &resp);
    alloc->free(alloc->ctx, url, strlen(url) + 1);
    hu_json_buf_free(&buf);

    if (err != HU_OK) {
        /* Transport-layer failure (DNS, connection refused, timeout).
         * status_code stays 0 to distinguish from a server-side reject. */
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }

    result->status_code = resp.status_code;
    if (resp.status_code == 200 && resp.body && resp.body_len > 0)
        parse_swap_response(alloc, resp.body, resp.body_len, result);
    hu_http_response_free(alloc, &resp);
    return HU_OK;
}

hu_error_t hu_mlx_admin_current_adapter(hu_allocator_t *alloc, const char *base_url,
                                        size_t base_url_len, hu_mlx_admin_current_adapter_t *out) {
    if (!alloc || !base_url || base_url_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    char *url = join_url(alloc, base_url, base_url_len, "adapters/current");
    if (!url)
        return HU_ERR_OUT_OF_MEMORY;

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url, /*auth_header=*/NULL, &resp);
    alloc->free(alloc->ctx, url, strlen(url) + 1);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }

    out->status_code = resp.status_code;
    if (resp.status_code == 200 && resp.body && resp.body_len > 0) {
        hu_json_value_t *root = NULL;
        if (hu_json_parse(alloc, resp.body, resp.body_len, &root) == HU_OK && root) {
            double tensors = hu_json_get_number(root, "tensors_loaded", 0.0);
            if (tensors > 0.0)
                out->tensors_loaded = (size_t)tensors;
            const char *path = hu_json_get_string(root, "adapter_path");
            if (path && path[0]) {
                size_t plen = strlen(path);
                char *copy = (char *)alloc->alloc(alloc->ctx, plen + 1);
                if (copy) {
                    memcpy(copy, path, plen);
                    copy[plen] = '\0';
                    out->adapter_path = copy;
                    out->adapter_path_len = plen;
                }
            }
            hu_json_free(alloc, root);
        }
    }
    hu_http_response_free(alloc, &resp);
    return HU_OK;
}

void hu_mlx_admin_swap_result_free(hu_allocator_t *alloc, hu_mlx_admin_swap_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->resolved_adapter_path) {
        alloc->free(alloc->ctx, result->resolved_adapter_path,
                    result->resolved_adapter_path_len + 1);
        result->resolved_adapter_path = NULL;
        result->resolved_adapter_path_len = 0;
    }
}

void hu_mlx_admin_current_adapter_free(hu_allocator_t *alloc,
                                       hu_mlx_admin_current_adapter_t *current) {
    if (!alloc || !current)
        return;
    if (current->adapter_path) {
        alloc->free(alloc->ctx, current->adapter_path, current->adapter_path_len + 1);
        current->adapter_path = NULL;
        current->adapter_path_len = 0;
    }
}

#endif /* HU_ENABLE_CURL */
