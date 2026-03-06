/* libFuzzer harness for sc_config_parse_json. Must not crash on any input. */
#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/arena.h"
#include <stddef.h>
#include <stdint.h>
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

    (void)sc_config_parse_json(&cfg, (const char *)data, size);
    sc_config_deinit(&cfg);
    return 0;
}
