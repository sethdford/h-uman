---
title: Gemini Embedder Adapter Code Review
---
# Gemini Embedder Adapter Code Review

**Reviewed:** `embedder_gemini_adapter.h`, `embedder_gemini_adapter.c`, `bootstrap.c` (Gemini path)

**Scope:** Memory leaks, security, architecture, error handling, bootstrap fallback.

---

## Summary

The adapter correctly bridges `hu_embedding_provider_t` (values/dimensions) to `hu_embedder_t` (values/dim). Provider deinit is called exactly once in `deinit_impl`. Several issues were found: one critical memory leak, one edge-case leak, missing fallback on adapter failure, and defensive input validation gaps.

---

## Critical Issues

### 1. Memory leak when adapter allocation fails (Confidence: 95)

**File:** `src/bootstrap.c` (lines 521–527) + `src/memory/vector/embedder_gemini_adapter.c` (lines 74–77)

**Problem:** When `hu_embedder_gemini_adapter_create` fails to allocate its context, the Gemini provider created just before is never deinited. Ownership is effectively transferred to the adapter; if the adapter cannot be created, the provider leaks.

```c
// bootstrap.c
hu_embedding_provider_t gem_provider =
    hu_embedding_gemini_create(alloc, gemini_key, NULL, 0);
bi->embedder = hu_embedder_gemini_adapter_create(alloc, gem_provider);
// If adapter_create returns emb.ctx == NULL, gem_provider.ctx is never freed
```

**Fix:** In `hu_embedder_gemini_adapter_create`, when `alloc` for `ctx` fails, deinit the provider before returning:

```c
hu_embedder_t hu_embedder_gemini_adapter_create(hu_allocator_t *alloc,
                                                 hu_embedding_provider_t provider) {
    hu_embedder_t emb = {.ctx = NULL, .vtable = &gemini_adapter_vtable};
    if (!alloc || !provider.vtable)
        return emb;

    gemini_adapter_ctx_t *ctx =
        (gemini_adapter_ctx_t *)alloc->alloc(alloc->ctx, sizeof(gemini_adapter_ctx_t));
    if (!ctx) {
        if (provider.ctx && provider.vtable->deinit)
            provider.vtable->deinit(provider.ctx, alloc);
        return emb;
    }
    ctx->provider = provider;
    emb.ctx = ctx;
    return emb;
}
```

---

### 2. Empty embedding (dimensions == 0) can leak or misreport OOM (Confidence: 85)

**File:** `src/memory/vector/embedder_gemini_adapter.c` (lines 19–28)

**Problem:** When the provider returns `result.dimensions == 0` (e.g. Gemini for empty text), the adapter does:

```c
out->values = (float *)alloc->alloc(alloc->ctx, result.dimensions * sizeof(float));
if (!out->values) {
    hu_embedding_provider_free(alloc, &result);
    return HU_ERR_OUT_OF_MEMORY;
}
```

- `alloc(0)` is implementation-defined (may return NULL or a unique pointer).
- If NULL: we return `HU_ERR_OUT_OF_MEMORY` even though the provider returned `HU_OK`.
- If non-NULL: `hu_embedding_free` uses `e->dim > 0`, so it never frees `values` when `dim == 0` → leak.

**Fix:** Special-case `dimensions == 0`:

```c
if (result.dimensions == 0) {
    out->values = NULL;
    out->dim = 0;
    hu_embedding_provider_free(alloc, &result);
    return HU_OK;
}
```

---

## Important Issues

### 3. No fallback to local embedder when adapter creation fails (Confidence: 90)

**File:** `src/bootstrap.c` (lines 521–527)

**Problem:** If `hu_embedder_gemini_adapter_create` fails (e.g. OOM), `bi->embedder` is set to an embedder with `ctx == NULL`. The retrieval engine will then fail on `embed` with `HU_ERR_INVALID_ARGUMENT`. There is no fallback to the local embedder.

**Fix:** Check the result and fall back to local when the adapter is invalid:

```c
const char *gemini_key = getenv("GEMINI_API_KEY");
if (gemini_key && gemini_key[0]) {
    hu_embedding_provider_t gem_provider =
        hu_embedding_gemini_create(alloc, gemini_key, NULL, 0);
    bi->embedder = hu_embedder_gemini_adapter_create(alloc, gem_provider);
    if (!bi->embedder.ctx)
        bi->embedder = hu_embedder_local_create(alloc);
} else {
    bi->embedder = hu_embedder_local_create(alloc);
}
```

---

### 4. embed_batch: missing NULL checks for texts/text_lens (Confidence: 80)

**File:** `src/memory/vector/embedder_gemini_adapter.c` (lines 32–34)

**Problem:** With `count > 0`, `texts[i]` or `text_lens[i]` can be accessed when `texts` or `text_lens` is NULL, causing undefined behavior. The local embedder has the same pattern, but defensive checks are still desirable.

**Fix:** Add validation at the start:

```c
static hu_error_t embed_batch_impl(void *ctx, hu_allocator_t *alloc, const char **texts,
                                   const size_t *text_lens, size_t count, hu_embedding_t *out) {
    if (count > 0 && (!texts || !text_lens || !out))
        return HU_ERR_INVALID_ARGUMENT;
    // ...
}
```

---

### 5. embed_impl: text NULL not validated (Confidence: 75)

**File:** `src/memory/vector/embedder_gemini_adapter.c` (lines 11–12)

**Problem:** `embed_impl` checks `adapter`, `alloc`, `out` but not `text`. If `text == NULL` and `text_len > 0`, the provider may crash (e.g. `hu_json_append_string` in Gemini).

**Fix:** Add a check:

```c
if (!adapter || !adapter->provider.vtable || !alloc || !out || !text)
    return HU_ERR_INVALID_ARGUMENT;
```

Note: `text` can be NULL when `text_len == 0` (empty string). In that case, the provider should handle it. A stricter rule would be: require `text != NULL` when `text_len > 0`.

---

## Verified Correct

- **Provider deinit:** `deinit_impl` calls `provider.vtable->deinit(provider.ctx, alloc)` exactly once, then frees the adapter context. No double-free.
- **Bridge semantics:** `result.values`/`result.dimensions` are correctly mapped to `out->values`/`out->dim`.
- **Error propagation:** `embed_impl` and `embed_batch_impl` propagate provider errors and free partial results on failure.
- **embed_batch rollback:** On failure, previously filled `out[j]` entries are freed before returning.
- **Null ctx handling:** `deinit_impl` and `dimensions_impl` handle `ctx == NULL` safely.
- **Adapter creation validation:** `hu_embedder_gemini_adapter_create` validates `alloc` and `provider.vtable` before proceeding.

---

## Thread Safety

The adapter does not add locking. Thread safety depends on the underlying provider (e.g. Gemini HTTP client). Current callers (retrieval engine, bootstrap) appear single-threaded. No change recommended unless multi-threaded use is introduced.

---

## Recommendations

1. Apply the memory-leak fix in `hu_embedder_gemini_adapter_create` (issue 1).
2. Handle `dimensions == 0` explicitly in `embed_impl` (issue 2).
3. Add fallback to local embedder in `bootstrap.c` when adapter creation fails (issue 3).
4. Add defensive NULL checks in `embed_batch_impl` and `embed_impl` (issues 4 and 5).
5. Add tests for: adapter create failure, empty embedding, and `embed_batch` error rollback.
