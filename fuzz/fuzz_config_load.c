/* Fuzz harness for hu_config_parse_json.
 * Takes arbitrary bytes as input, parses as config JSON.
 * Must handle malformed JSON gracefully without crashing. */
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536)
        return 0;

    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(backing);
    if (!arena)
        return 0;

    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);

    char *buf = (char *)cfg.allocator.alloc(cfg.allocator.ctx, size + 1);
    if (!buf) {
        hu_arena_destroy(arena);
        return 0;
    }
    memcpy(buf, data, size);
    buf[size] = '\0';

    hu_error_t err = hu_config_parse_json(&cfg, buf, size);
    cfg.allocator.free(cfg.allocator.ctx, buf, size + 1);

    (void)err;
    hu_config_deinit(&cfg);
    return 0;
}
