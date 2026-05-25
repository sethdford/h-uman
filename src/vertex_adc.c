#include "human/vertex_adc.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HU_VERTEX_ADC_REFRESH_MARGIN_SECS 300
#define HU_VERTEX_ADC_TOKEN_URL           "https://oauth2.googleapis.com/token"
#define HU_VERTEX_ADC_MAX_FILE_BYTES      65536

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

#if !HU_IS_TEST
/* Cached credentials (loaded once from the ADC JSON file).
 * Only referenced from the production path — test stubs return a deterministic
 * fake token without ever loading ADC. */
static char *s_client_id = NULL;
static char *s_client_secret = NULL;
static char *s_refresh_token = NULL;
static char *s_quota_project = NULL;
static size_t s_client_id_len = 0;
static size_t s_client_secret_len = 0;
static size_t s_refresh_token_len = 0;
#endif /* !HU_IS_TEST */

/* Cached access token (refreshed on demand in prod, reset-cleared in test). */
static char *s_access_token = NULL;
static size_t s_access_token_len = 0;
static time_t s_expires_at = 0;

/* Allocator captured on first successful load so we can free correctly during
 * test-mode reset. */
static hu_allocator_t *s_cache_alloc = NULL;

#if !HU_IS_TEST

/* Read the ADC JSON file from $HOME/.config/gcloud/application_default_credentials.json.
 * Returns HU_OK and populates the static cred fields, or returns an error.
 * Must be called with s_mutex held. */
static hu_error_t load_adc_creds_locked(hu_allocator_t *alloc) {
    if (s_refresh_token)
        return HU_OK; /* already loaded */

    const char *home = getenv("HOME");
    if (!home || !home[0])
        return HU_ERR_PROVIDER_AUTH;

    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/.config/gcloud/application_default_credentials.json",
                     home);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "r");
    if (!f)
        return HU_ERR_PROVIDER_AUTH;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > HU_VERTEX_ADC_MAX_FILE_BYTES) {
        fclose(f);
        return HU_ERR_PROVIDER_AUTH;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }

    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        alloc->free(alloc->ctx, buf, (size_t)sz);
        return HU_ERR_IO;
    }

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, buf, (size_t)sz, &root);
    alloc->free(alloc->ctx, buf, (size_t)sz);
    if (err != HU_OK || !root)
        return HU_ERR_JSON_PARSE;

    const char *cid = hu_json_get_string(root, "client_id");
    const char *csec = hu_json_get_string(root, "client_secret");
    const char *rtok = hu_json_get_string(root, "refresh_token");
    const char *qproj = hu_json_get_string(root, "quota_project_id");

    if (!cid || !csec || !rtok) {
        hu_json_free(alloc, root);
        return HU_ERR_PROVIDER_AUTH;
    }

    s_client_id = hu_strdup(alloc, cid);
    s_client_secret = hu_strdup(alloc, csec);
    s_refresh_token = hu_strdup(alloc, rtok);
    if (qproj && qproj[0])
        s_quota_project = hu_strdup(alloc, qproj);
    s_client_id_len = s_client_id ? strlen(s_client_id) : 0;
    s_client_secret_len = s_client_secret ? strlen(s_client_secret) : 0;
    s_refresh_token_len = s_refresh_token ? strlen(s_refresh_token) : 0;

    hu_json_free(alloc, root);

    if (!s_client_id || !s_client_secret || !s_refresh_token)
        return HU_ERR_OUT_OF_MEMORY;

    s_cache_alloc = alloc;
    return HU_OK;
}

/* Exchange the refresh_token for a fresh access_token. Must be called with
 * s_mutex held. */
static hu_error_t refresh_token_locked(hu_allocator_t *alloc) {
    /* form body: client_id=...&client_secret=...&refresh_token=...&grant_type=refresh_token */
    size_t body_cap = s_client_id_len + s_client_secret_len + s_refresh_token_len + 128;
    char *body = (char *)alloc->alloc(alloc->ctx, body_cap);
    if (!body)
        return HU_ERR_OUT_OF_MEMORY;
    int n = snprintf(body, body_cap,
                     "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
                     s_client_id, s_client_secret, s_refresh_token);
    if (n <= 0 || (size_t)n >= body_cap) {
        alloc->free(alloc->ctx, body, body_cap);
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_http_response_t resp = {0};
    hu_error_t err =
        hu_http_request(alloc, HU_VERTEX_ADC_TOKEN_URL, "POST",
                        "Content-Type: application/x-www-form-urlencoded", body, (size_t)n, &resp);
    alloc->free(alloc->ctx, body, body_cap);
    if (err != HU_OK)
        return err;

    if (resp.status_code < 200 || resp.status_code >= 300) {
        hu_log_warn("vertex_adc", NULL, "OAuth refresh failed: HTTP %d (body_len=%zu)",
                    resp.status_code, resp.body_len);
        hu_http_response_free(alloc, &resp);
        return HU_ERR_PROVIDER_AUTH;
    }

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &root);
    hu_http_response_free(alloc, &resp);
    if (err != HU_OK || !root)
        return HU_ERR_JSON_PARSE;

    const char *tok = hu_json_get_string(root, "access_token");
    double exp_in = hu_json_get_number(root, "expires_in", 3600.0);
    if (!tok) {
        hu_json_free(alloc, root);
        return HU_ERR_PROVIDER_AUTH;
    }

    if (s_access_token) {
        alloc->free(alloc->ctx, s_access_token, s_access_token_len + 1);
        s_access_token = NULL;
        s_access_token_len = 0;
    }
    size_t tlen = strlen(tok);
    s_access_token = hu_strndup(alloc, tok, tlen);
    s_access_token_len = tlen;
    s_expires_at = time(NULL) + (time_t)exp_in;

    hu_json_free(alloc, root);
    return s_access_token ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

hu_error_t hu_vertex_adc_token(hu_allocator_t *alloc, char **out_token, size_t *out_len) {
    if (!alloc || !out_token || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_token = NULL;
    *out_len = 0;

    pthread_mutex_lock(&s_mutex);

    hu_error_t err = load_adc_creds_locked(alloc);
    if (err != HU_OK) {
        pthread_mutex_unlock(&s_mutex);
        return err;
    }

    time_t now = time(NULL);
    if (!s_access_token || now >= s_expires_at - HU_VERTEX_ADC_REFRESH_MARGIN_SECS) {
        err = refresh_token_locked(alloc);
        if (err != HU_OK) {
            pthread_mutex_unlock(&s_mutex);
            return err;
        }
    }

    *out_token = hu_strndup(alloc, s_access_token, s_access_token_len);
    *out_len = s_access_token_len;

    pthread_mutex_unlock(&s_mutex);
    return *out_token ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

const char *hu_vertex_adc_default_project(hu_allocator_t *alloc) {
    /* Env var takes precedence (matches src/tools/media_*.c convention). */
    const char *env = getenv("GOOGLE_CLOUD_PROJECT");
    if (env && env[0])
        return env;

    /* Otherwise return the cached quota_project_id from the ADC file.
     * Load creds lazily if not already loaded; ignore errors (return NULL). */
    pthread_mutex_lock(&s_mutex);
    if (!s_refresh_token && alloc) {
        (void)load_adc_creds_locked(alloc);
    }
    const char *p = s_quota_project;
    pthread_mutex_unlock(&s_mutex);
    return p;
}

void hu_vertex_adc_reset_cache_for_test(void) {
    /* No-op in production builds — the cache is intentionally process-static
     * so the daemon doesn't churn OAuth requests. Tests should use the
     * HU_IS_TEST build, which has its own deterministic stub. */
}

#else /* HU_IS_TEST */

/* Test stub: returns a deterministic fake token, no network, no file I/O. */
hu_error_t hu_vertex_adc_token(hu_allocator_t *alloc, char **out_token, size_t *out_len) {
    if (!alloc || !out_token || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    const char *fake = "test-vertex-adc-token";
    size_t flen = strlen(fake);
    char *dup = hu_strndup(alloc, fake, flen);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    *out_token = dup;
    *out_len = flen;
    return HU_OK;
}

const char *hu_vertex_adc_default_project(hu_allocator_t *alloc) {
    (void)alloc;
    const char *env = getenv("GOOGLE_CLOUD_PROJECT");
    if (env && env[0])
        return env;
    return "test-project";
}

void hu_vertex_adc_reset_cache_for_test(void) {
    pthread_mutex_lock(&s_mutex);
    if (s_access_token && s_cache_alloc) {
        s_cache_alloc->free(s_cache_alloc->ctx, s_access_token, s_access_token_len + 1);
    }
    s_access_token = NULL;
    s_access_token_len = 0;
    s_expires_at = 0;
    pthread_mutex_unlock(&s_mutex);
}

#endif /* HU_IS_TEST */
