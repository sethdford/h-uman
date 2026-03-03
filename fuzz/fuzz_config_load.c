/* Fuzz harness for sc_config_parse_json.
 * Takes arbitrary bytes as input, parses as config JSON.
 * Must handle malformed JSON gracefully without crashing. */
#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/arena.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536)
        return 0;

    sc_allocator_t backing = sc_system_allocator();
    sc_arena_t *arena = sc_arena_create(backing);
    if (!arena)
        return 0;

    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.arena = arena;
    cfg.allocator = sc_arena_allocator(arena);

    char *buf = (char *)cfg.allocator.alloc(cfg.allocator.ctx, size + 1);
    if (!buf) {
        sc_arena_destroy(arena);
        return 0;
    }
    memcpy(buf, data, size);
    buf[size] = '\0';

    sc_error_t err = sc_config_parse_json(&cfg, buf, size);
    cfg.allocator.free(cfg.allocator.ctx, buf, size + 1);

    (void)err;
    sc_config_deinit(&cfg);
    return 0;
}
