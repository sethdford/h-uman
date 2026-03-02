# WASM-specific sources for SeaClaw.
# Include this when building with: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-wasi.cmake -DSC_BUILD_WASM=ON ..
#
# Minimal set: no shell, no sqlite, no process spawn, no providers/tools factories.
# main_wasi uses sc_wasm_provider_create and passes NULL tools.

set(SC_WASM_SOURCES
    wasm/wasi_bindings.c
    wasm/wasm_alloc.c
    wasm/wasm_provider.c
    wasm/wasm_channel.c
)

# Core sources compatible with WASM (no POSIX-only: config/health use fs; exclude for minimal)
set(SC_WASM_CORE_SOURCES
    src/core/allocator.c
    src/core/error.c
    src/core/arena.c
    src/core/string.c
    src/core/json.c
    src/security/security.c
    src/security/policy.c
    src/memory/engines/none.c
    src/tunnel/none.c
    src/runtime/wasm_rt.c
    src/agent/agent.c
    src/agent/context.c
    src/agent/dispatcher.c
    src/agent/compaction.c
    src/agent/prompt.c
    src/agent/memory_loader.c
)

# Crypto (generic C + dispatch)
set(SC_WASM_CRYPTO_SOURCES
    asm/generic/chacha20.c
    asm/generic/sha256.c
    src/crypto/dispatch.c
)

# Main entrypoint for WASM
set(SC_WASM_MAIN src/main_wasi.c)
