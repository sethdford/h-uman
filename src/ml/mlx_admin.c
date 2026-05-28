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
#include "human/core/log.h"
#include "human/core/string.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ─────────────────────────────────────────────────────────────────────
 * Spec 1 Task 5 (AC-M3-3): swap-failure observability — counters,
 * classifier, labels, once-guard. Implemented even when curl is OFF so
 * that NOT_SUPPORTED still surfaces as a transport-style failure to any
 * caller scraping the counters.
 * ───────────────────────────────────────────────────────────────── */

/* One counter per reason. Indexed by `hu_mlx_swap_failure_reason_t`. */
static atomic_uint_least64_t g_swap_failure_counters[HU_MLX_SWAP_FAILURE__COUNT];

/* hu_log_info_once landmark — fires on first failure of any kind after
 * process start (or after reset_observability_for_test). */
static atomic_bool g_warned_swap_failure_first_seen = false;

const char *hu_mlx_admin_swap_failure_label(hu_mlx_swap_failure_reason_t reason) {
    switch (reason) {
    case HU_MLX_SWAP_FAILURE_NONE:
        return "none";
    case HU_MLX_SWAP_FAILURE_TRANSPORT:
        return "transport";
    case HU_MLX_SWAP_FAILURE_HTTP_4XX:
        return "http_4xx";
    case HU_MLX_SWAP_FAILURE_HTTP_5XX:
        return "http_5xx";
    case HU_MLX_SWAP_FAILURE_MISSING_ENDPOINT:
        return "missing_endpoint";
    case HU_MLX_SWAP_FAILURE_OTHER:
        return "other";
    default:
        return "unknown";
    }
}

hu_mlx_swap_failure_reason_t hu_mlx_admin_classify_swap_failure(hu_error_t err, long status_code) {
    if (err == HU_OK && status_code == 200)
        return HU_MLX_SWAP_FAILURE_NONE;
    if (err == HU_ERR_NOT_SUPPORTED || err == HU_ERR_IO)
        return HU_MLX_SWAP_FAILURE_TRANSPORT;
    /* err is HU_OK here — server replied. Classify by status code. */
    if (status_code == 404)
        return HU_MLX_SWAP_FAILURE_MISSING_ENDPOINT;
    if (status_code >= 400 && status_code < 500)
        return HU_MLX_SWAP_FAILURE_HTTP_4XX;
    if (status_code >= 500 && status_code < 600)
        return HU_MLX_SWAP_FAILURE_HTTP_5XX;
    return HU_MLX_SWAP_FAILURE_OTHER;
}

uint64_t hu_mlx_admin_swap_failure_counter(hu_mlx_swap_failure_reason_t reason) {
    if ((unsigned)reason >= (unsigned)HU_MLX_SWAP_FAILURE__COUNT)
        return 0;
    if (reason == HU_MLX_SWAP_FAILURE_NONE)
        return 0;
    return (uint64_t)atomic_load(&g_swap_failure_counters[(unsigned)reason]);
}

uint64_t hu_mlx_admin_swap_failure_total(void) {
    uint64_t sum = 0;
    for (unsigned i = (unsigned)HU_MLX_SWAP_FAILURE_TRANSPORT;
         i < (unsigned)HU_MLX_SWAP_FAILURE__COUNT; i++) {
        sum += (uint64_t)atomic_load(&g_swap_failure_counters[i]);
    }
    return sum;
}

void hu_mlx_admin_reset_observability_for_test(void) {
    for (unsigned i = 0; i < (unsigned)HU_MLX_SWAP_FAILURE__COUNT; i++)
        atomic_store(&g_swap_failure_counters[i], 0);
    atomic_store(&g_warned_swap_failure_first_seen, false);
}

/* ─────────────────────────────────────────────────────────────────────
 * Local-MLX health probe (Dermot humanness recovery C2). Lets the model
 * router gate Seth-voice mlx_local routing on the server actually being
 * reachable. The probe result is cached for 60s so the hot turn path
 * doesn't issue an HTTP round-trip every reply. The test override is
 * honored in BOTH curl and non-curl builds so router/daemon tests run
 * identically regardless of build flags. Cache state is only consulted
 * by the curl-backed probe below. */
static bool g_health_override_set = false;
static bool g_health_override_val = false;
static int64_t g_health_cache_ms = 0;
static bool g_health_cache_val = false;

void hu_mlx_admin_set_test_health(bool healthy) {
    g_health_override_set = true;
    g_health_override_val = healthy;
}

void hu_mlx_admin_clear_test_health(void) {
    g_health_override_set = false;
    g_health_override_val = false;
    g_health_cache_ms = 0;
    g_health_cache_val = false;
}

/* Record one swap-attempt outcome. Increments the per-reason counter
 * + emits the structured log line + fires the once-guard landmark.
 *
 * adapter_path is logged but no other request bytes — privacy contract
 * is "paths + hashes only, never prompt/response content". The contact
 * hash is passed through as a uint64 (computed by the caller from
 * memory_session_id via FNV-1a, matching the outcome ring conventions).
 *
 * Called from inside hu_mlx_admin_swap_adapter on every return path
 * other than INVALID_ARGUMENT. */
static void record_swap_outcome(const char *adapter_path, size_t adapter_path_len, hu_error_t err,
                                long status_code, uint64_t contact_hash) {
    hu_mlx_swap_failure_reason_t reason = hu_mlx_admin_classify_swap_failure(err, status_code);
    if (reason == HU_MLX_SWAP_FAILURE_NONE)
        return;

    atomic_fetch_add(&g_swap_failure_counters[(unsigned)reason], 1);

    /* Log unconditionally at error level so the failure is operator-
     * visible whether or not metrics are being scraped. Truncate the
     * adapter path to something reasonable for log lines. */
    const char *label = hu_mlx_admin_swap_failure_label(reason);
    int p_len = (int)adapter_path_len;
    if (p_len < 0)
        p_len = 0;
    if (p_len > 256)
        p_len = 256;
    hu_log_error("mlx_admin", NULL,
                 "m3 adapter swap failed: reason=%s status=%ld err=%d "
                 "contact_hash=%llu adapter_path=%.*s",
                 label, status_code, (int)err, (unsigned long long)contact_hash, p_len,
                 adapter_path ? adapter_path : "");

    /* Once-guard landmark — "the personalization loop is silently not
     * personalizing" was the 2026-05-18 incident shape. The first-
     * failure log is the operator's loudest signal that the swap path
     * is broken; subsequent failures still increment counters but don't
     * spam the log at info level. */
    hu_log_info_once(&g_warned_swap_failure_first_seen, "mlx_admin", NULL,
                     "m3 adapter swap path is failing — first failure reason=%s. Subsequent "
                     "failures will increment counters silently; inspect "
                     "hu_mlx_admin_swap_failure_total() to monitor.",
                     label);
}

#ifndef HU_ENABLE_CURL
/* When curl is disabled (release minimal builds, fuzz-only configs)
 * the admin layer is a NOT_SUPPORTED stub — matching the M3 dispatcher
 * safety contract. The daemon falls through gracefully and the
 * personalization loop simply doesn't close until a curl-enabled
 * binary is in use. Operator gets one clear log line on first swap
 * attempt explaining why the loop is inactive. */
static atomic_bool g_warned_curl_disabled = false;

hu_error_t hu_mlx_admin_swap_adapter(hu_allocator_t *alloc, const char *base_url,
                                     size_t base_url_len, const char *adapter_path,
                                     size_t adapter_path_len, hu_mlx_admin_swap_result_t *result) {
    (void)alloc;
    (void)base_url;
    (void)base_url_len;
    if (result)
        memset(result, 0, sizeof(*result));
    /* NOT_SUPPORTED counts as a transport-class failure — operator
     * still needs to know the personalization loop is silently
     * inactive. Per silent-config-gated-subsystems.md. */
    hu_log_info_once(&g_warned_curl_disabled, "mlx_admin", NULL,
                     "lora swap skipped: build lacks libcurl (HU_ENABLE_CURL=OFF) — "
                     "rebuild with cmake --preset release to enable on-device personalization");
    record_swap_outcome(adapter_path, adapter_path_len, HU_ERR_NOT_SUPPORTED, 0, 0);
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

bool hu_mlx_admin_probe_health(hu_allocator_t *alloc, const char *base_url, size_t base_url_len) {
    (void)alloc;
    (void)base_url;
    (void)base_url_len;
    /* Test override still applies (lets tests force healthy=true even in a
     * curl-less build); otherwise no transport → never healthy → cloud. */
    if (g_health_override_set)
        return g_health_override_val;
    return false;
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
        record_swap_outcome(adapter_path, adapter_path_len, HU_ERR_IO, 0, 0);
        return HU_ERR_IO;
    }

    result->status_code = resp.status_code;
    if (resp.status_code == 200 && resp.body && resp.body_len > 0)
        parse_swap_response(alloc, resp.body, resp.body_len, result);
    hu_http_response_free(alloc, &resp);
    /* Server replied — observability runs on every non-200 path. The
     * classifier distinguishes 4xx/5xx/missing-endpoint. */
    record_swap_outcome(adapter_path, adapter_path_len, HU_OK, result->status_code, 0);
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

bool hu_mlx_admin_probe_health(hu_allocator_t *alloc, const char *base_url, size_t base_url_len) {
    if (g_health_override_set)
        return g_health_override_val;
    if (!alloc || !base_url || base_url_len == 0)
        return false;

    int64_t now_ms = (int64_t)time(NULL) * 1000;
    if (g_health_cache_ms != 0 && now_ms - g_health_cache_ms < 60000)
        return g_health_cache_val;

    /* Reuse the known-good /adapters/current endpoint (the swap infra
     * targets the same v1 root) rather than assuming a /health route the
     * server may not expose. HTTP 200 == server up + serving. */
    char *url = join_url(alloc, base_url, base_url_len, "adapters/current");
    if (!url)
        return false; /* transient OOM — don't poison the cache */

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url, /*auth_header=*/NULL, &resp);
    alloc->free(alloc->ctx, url, strlen(url) + 1);
    bool healthy = (err == HU_OK && resp.status_code == 200);
    if (resp.owned && resp.body)
        hu_http_response_free(alloc, &resp);

    g_health_cache_ms = now_ms;
    g_health_cache_val = healthy;
    return healthy;
}

#endif /* HU_ENABLE_CURL */
