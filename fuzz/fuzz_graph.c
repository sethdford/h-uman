/* libFuzzer harness for sc_graph_upsert_entity and sc_graph_upsert_relation.
 * Opens in-memory graph, feeds fuzzed entity names/types/relations.
 * Goal: find crashes or memory corruption.
 * Requires SC_ENABLE_SQLITE. */
#include "seaclaw/core/allocator.h"
#include "seaclaw/memory/graph.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BUF_MAX 512

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
#ifdef SC_ENABLE_SQLITE
    if (size < 2 || size > BUF_MAX)
        return 0;

    sc_allocator_t alloc = sc_system_allocator();
    sc_graph_t *g = NULL;
    sc_error_t err = sc_graph_open(&alloc, "x", 1, &g);
    if (err != SC_OK || !g)
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
    err = sc_graph_upsert_entity(g, buf, name_len, SC_ENTITY_PERSON, NULL, &id1);
    if (err != SC_OK) {
        sc_graph_close(g, &alloc);
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
        (void)sc_graph_upsert_entity(g, buf + mid, len2, SC_ENTITY_ORGANIZATION, NULL, &id2);
    } else {
        (void)sc_graph_upsert_entity(g, "b", 1, SC_ENTITY_PERSON, NULL, &id2);
    }

    if (id1 > 0 && id2 > 0)
        (void)sc_graph_upsert_relation(g, id1, id2, SC_REL_KNOWS, 0.8f, NULL, 0);

    sc_graph_close(g, &alloc);
#endif /* SC_ENABLE_SQLITE */
    return 0;
}
