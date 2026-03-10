/* libFuzzer harness for hu_graph_upsert_entity and hu_graph_upsert_relation.
 * Opens in-memory graph, feeds fuzzed entity names/types/relations.
 * Goal: find crashes or memory corruption.
 * Requires HU_ENABLE_SQLITE. */
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BUF_MAX 512

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
#ifdef HU_ENABLE_SQLITE
    if (size < 2 || size > BUF_MAX)
        return 0;

    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    hu_error_t err = hu_graph_open(&alloc, "x", 1, &g);
    if (err != HU_OK || !g)
        return 0;

    char buf[BUF_MAX + 1];
    size_t copy_len = size < BUF_MAX ? size : BUF_MAX;
    memcpy(buf, data, copy_len);
    buf[copy_len] = '\0';

    /* Ensure we have at least one non-null byte for entity name */
    size_t name_len = 0;
    for (size_t i = 0; i < copy_len && buf[i] != '\0'; i++)
        name_len++;
    if (name_len == 0) {
        buf[0] = 'e';
        name_len = 1;
    }

    int64_t id1 = 0, id2 = 0;
    err = hu_graph_upsert_entity(g, buf, name_len, HU_ENTITY_PERSON, NULL, &id1);
    if (err != HU_OK) {
        hu_graph_close(g, &alloc);
        return 0;
    }

    /* Second entity from middle of buffer */
    size_t mid = copy_len / 2;
    if (mid > 0 && mid < copy_len) {
        size_t len2 = 0;
        for (size_t i = mid; i < copy_len && buf[i] != '\0' && len2 < 64; i++)
            len2++;
        if (len2 == 0) {
            buf[mid] = 'f';
            len2 = 1;
        }
        (void)hu_graph_upsert_entity(g, buf + mid, len2, HU_ENTITY_ORGANIZATION, NULL, &id2);
    } else {
        (void)hu_graph_upsert_entity(g, "b", 1, HU_ENTITY_PERSON, NULL, &id2);
    }

    if (id1 > 0 && id2 > 0)
        (void)hu_graph_upsert_relation(g, id1, id2, HU_REL_KNOWS, 0.8f, NULL, 0);

    hu_graph_close(g, &alloc);
#endif /* HU_ENABLE_SQLITE */
    return 0;
}
