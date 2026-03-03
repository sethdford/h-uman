/* Memory backend benchmarks: lifecycle, store, retrieve.
 * Uses SC_IS_TEST build to avoid real I/O where engines support mocks.
 * none: no I/O. markdown: uses temp dir for file I/O.
 */
#include "bench.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#define MEMORY_ITERATIONS 1000

static const char *bench_key = "bench_key";
static const char *bench_content = "Benchmark message content for store/retrieve test.";
static const size_t bench_key_len = 8;
static const size_t bench_content_len = 52;

void run_bench_memory(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_category_t cat = {.tag = SC_MEMORY_CATEGORY_CORE};

    /* Create/destroy none backend */
    BENCH_RUN("memory_none_create_destroy", MEMORY_ITERATIONS, {
        sc_memory_t mem = sc_none_memory_create(&alloc);
        if (mem.vtable && mem.ctx)
            mem.vtable->deinit(mem.ctx);
    });

    /* Store and retrieve with none backend */
    BENCH_RUN("memory_none_store_get", MEMORY_ITERATIONS, {
        sc_memory_t mem = sc_none_memory_create(&alloc);
        if (mem.vtable && mem.ctx) {
            mem.vtable->store(mem.ctx, bench_key, bench_key_len, bench_content,
                              bench_content_len, &cat, NULL, 0);
            sc_memory_entry_t entry = {0};
            bool found = false;
            mem.vtable->get(mem.ctx, &alloc, bench_key, bench_key_len, &entry, &found);
            mem.vtable->deinit(mem.ctx);
        }
    });

    /* Recall (search) with none backend */
    BENCH_RUN("memory_none_recall", MEMORY_ITERATIONS, {
        sc_memory_t mem = sc_none_memory_create(&alloc);
        if (mem.vtable && mem.ctx) {
            sc_memory_entry_t *out = NULL;
            size_t count = 0;
            mem.vtable->recall(mem.ctx, &alloc, "query", 5, 10, NULL, 0, &out, &count);
            if (out)
                sc_memory_entry_free_fields(&alloc, out);
            alloc.free(alloc.ctx, out, 0);
            mem.vtable->deinit(mem.ctx);
        }
    });

#if !defined(_WIN32)
    /* markdown: create temp dir, then lifecycle */
    char tmpdir[] = "/tmp/seaclaw_bench_XXXXXX";
    if (mkdtemp(tmpdir) == NULL) {
        fprintf(stderr, "bench_memory: mkdtemp failed, skipping markdown benches\n");
        return;
    }

    BENCH_RUN("memory_markdown_create_destroy", MEMORY_ITERATIONS, {
        sc_memory_t mem = sc_markdown_memory_create(&alloc, tmpdir);
        if (mem.vtable && mem.ctx)
            mem.vtable->deinit(mem.ctx);
    });

    BENCH_RUN("memory_markdown_store_get", MEMORY_ITERATIONS, {
        sc_memory_t mem = sc_markdown_memory_create(&alloc, tmpdir);
        if (mem.vtable && mem.ctx) {
            mem.vtable->store(mem.ctx, bench_key, bench_key_len, bench_content,
                              bench_content_len, &cat, NULL, 0);
            sc_memory_entry_t entry = {0};
            bool found = false;
            mem.vtable->get(mem.ctx, &alloc, bench_key, bench_key_len, &entry, &found);
            if (entry.content)
                sc_memory_entry_free_fields(&alloc, &entry);
            mem.vtable->deinit(mem.ctx);
        }
    });

    (void)tmpdir; /* temp dir left for OS cleanup */
#endif
}
