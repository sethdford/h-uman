/* Smoke test: exercise the production Vertex+ADC embedding path end-to-end.
 *
 * This is NOT a unit test — it links against libhuman_core.a (production
 * build, HU_IS_TEST not defined) and makes a real HTTPS request to
 * Vertex AI. Run from the project root after building build/human:
 *
 *   clang -I include tests/smoke_vertex_embed.c build/libhuman_core.a \
 *     $(pkg-config --libs libcurl) -framework Foundation -framework Security \
 *     -o /tmp/smoke_vertex && /tmp/smoke_vertex
 *
 * Exit 0 on success (and prints the embedding dims + first 3 values).
 * Exit non-zero on any failure with a diagnostic message. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/vector/embeddings.h"
#include "human/memory/vector/embeddings_gemini.h"
#include "human/vertex_adc.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Step 1: prove ADC token fetch works. */
    char *tok = NULL;
    size_t tlen = 0;
    hu_error_t err = hu_vertex_adc_token(&alloc, &tok, &tlen);
    if (err != HU_OK) {
        fprintf(stderr, "FAIL: hu_vertex_adc_token returned err=%d\n", (int)err);
        return 1;
    }
    if (!tok || tlen < 50) {
        fprintf(stderr, "FAIL: token looks bogus (len=%zu)\n", tlen);
        return 1;
    }
    printf("OK: token fetched (len=%zu prefix=%.10s...)\n", tlen, tok);
    alloc.free(alloc.ctx, tok, tlen + 1);

    /* Step 2: prove project resolution works. */
    const char *proj = hu_vertex_adc_default_project(&alloc);
    if (!proj || !proj[0]) {
        fprintf(stderr, "FAIL: no project resolved\n");
        return 1;
    }
    printf("OK: project=%s\n", proj);

    /* Step 3: prove constructor succeeds. */
    hu_embedding_provider_t ep = hu_embedding_gemini_create_vertex(&alloc, NULL, NULL, NULL, 0);
    if (!ep.ctx || !ep.vtable || !ep.vtable->embed) {
        fprintf(stderr, "FAIL: hu_embedding_gemini_create_vertex returned noop (ctx=%p)\n",
                (void *)ep.ctx);
        return 1;
    }
    printf("OK: provider created name=%s dims=%zu\n", ep.vtable->name(ep.ctx),
           ep.vtable->dimensions(ep.ctx));

    /* Step 4: prove a real embedding round-trip works. */
    const char *text = "the quick brown fox jumps over the lazy dog";
    hu_embedding_provider_result_t res = {0};
    err = ep.vtable->embed(ep.ctx, &alloc, text, strlen(text), &res);
    if (err != HU_OK) {
        fprintf(stderr, "FAIL: embed returned err=%d\n", (int)err);
        ep.vtable->deinit(ep.ctx, &alloc);
        return 1;
    }
    if (res.dimensions < 256 || !res.values) {
        fprintf(stderr, "FAIL: embedding too small (dims=%zu values=%p)\n", res.dimensions,
                (void *)res.values);
        if (res.values)
            alloc.free(alloc.ctx, res.values, res.dimensions * sizeof(float));
        ep.vtable->deinit(ep.ctx, &alloc);
        return 1;
    }
    printf("OK: embedding dims=%zu first3=[%f, %f, %f]\n", res.dimensions, res.values[0],
           res.values[1], res.values[2]);

    alloc.free(alloc.ctx, res.values, res.dimensions * sizeof(float));
    ep.vtable->deinit(ep.ctx, &alloc);
    printf("ALL OK\n");
    return 0;
}
