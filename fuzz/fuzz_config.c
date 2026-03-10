/* libFuzzer harness for hu_config_parse_json. Must not crash on any input. */
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include <stddef.h>
#include <stdint.h>
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

    (void)hu_config_parse_json(&cfg, (const char *)data, size);
    hu_config_deinit(&cfg);
    return 0;
}
