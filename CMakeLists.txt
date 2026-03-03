cmake_minimum_required(VERSION 3.20)
project(seaclaw C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(SC_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(SC_ENABLE_SQLITE "Enable SQLite memory backend" ON)
option(SC_ENABLE_POSTGRES "Enable PostgreSQL memory backend" OFF)
option(SC_ENABLE_CURL "Enable libcurl for HTTP client" ON)
option(SC_ENABLE_TLS "Enable TLS via OpenSSL for WSS and HTTPS" ON)
option(SC_ENABLE_FIPS_CRYPTO "Use AES-256-GCM via OpenSSL for FIPS 140-2 compliance" OFF)
option(SC_BUILD_WASM "Build WASM/WASI target (seaclaw.wasm)" OFF)
option(SC_ENABLE_LTO "Enable Link-Time Optimization" OFF)
option(SC_ENABLE_READLINE "Enable readline/libedit for CLI line editing" OFF)
option(SC_ENABLE_LINENOISE "Enable linenoise for lightweight CLI line editing (vendor/linenoise)" OFF)
option(SC_ENABLE_TUI "Enable rich TUI mode with split-pane terminal UI (vendor/termbox2)" OFF)

# WASM build: cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/wasm32-wasi.cmake -DSC_BUILD_WASM=ON ..

# --- Per-channel build options (CLI ON by default, others OFF) ---
option(SC_ENABLE_CLI "Build CLI channel" ON)
option(SC_ENABLE_TELEGRAM "Build Telegram channel" OFF)
option(SC_ENABLE_DISCORD "Build Discord channel" OFF)
option(SC_ENABLE_SLACK "Build Slack channel" OFF)
option(SC_ENABLE_WHATSAPP "Build WhatsApp channel" OFF)
option(SC_ENABLE_MATRIX "Build Matrix channel" OFF)
option(SC_ENABLE_IRC "Build IRC channel" OFF)
option(SC_ENABLE_LINE "Build LINE channel" OFF)
option(SC_ENABLE_LARK "Build Lark channel" OFF)
option(SC_ENABLE_WEB "Build Web channel" OFF)
option(SC_ENABLE_EMAIL "Build Email channel" OFF)
option(SC_ENABLE_IMESSAGE "Build iMessage channel" OFF)
option(SC_ENABLE_MATTERMOST "Build Mattermost channel" OFF)
option(SC_ENABLE_ONEBOT "Build OneBot channel" OFF)
option(SC_ENABLE_DINGTALK "Build DingTalk channel" OFF)
option(SC_ENABLE_SIGNAL "Build Signal channel" OFF)
option(SC_ENABLE_NOSTR "Build Nostr channel" OFF)
option(SC_ENABLE_QQ "Build QQ channel" OFF)
option(SC_ENABLE_MAIXCAM "Build MaixCam channel" OFF)
option(SC_ENABLE_DISPATCH "Build Dispatch channel" OFF)
option(SC_ENABLE_ALL_CHANNELS "Build all channels" OFF)

# --- Per-engine build options ---
option(SC_ENABLE_NONE_ENGINE "Build no-op memory engine" ON)
option(SC_ENABLE_MARKDOWN_ENGINE "Build markdown memory engine" ON)
option(SC_ENABLE_MEMORY_LRU_ENGINE "Build in-memory LRU memory engine" ON)
option(SC_ENABLE_API_ENGINE "Build HTTP API memory engine" OFF)
option(SC_ENABLE_LUCID_ENGINE "Build Lucid memory engine" ON)
option(SC_ENABLE_LANCEDB_ENGINE "Build LanceDB memory engine" ON)
option(SC_ENABLE_REDIS_ENGINE "Build Redis memory engine" OFF)
# SC_ENABLE_SQLITE and SC_ENABLE_POSTGRES defined above

# Apply SC_ENABLE_ALL_CHANNELS: if ON, enable all channel options
if(SC_ENABLE_ALL_CHANNELS)
    set(SC_ENABLE_TELEGRAM ON)
    set(SC_ENABLE_DISCORD ON)
    set(SC_ENABLE_SLACK ON)
    set(SC_ENABLE_WHATSAPP ON)
    set(SC_ENABLE_MATRIX ON)
    set(SC_ENABLE_IRC ON)
    set(SC_ENABLE_LINE ON)
    set(SC_ENABLE_LARK ON)
    set(SC_ENABLE_WEB ON)
    set(SC_ENABLE_EMAIL ON)
    set(SC_ENABLE_IMESSAGE ON)
    set(SC_ENABLE_MATTERMOST ON)
    set(SC_ENABLE_ONEBOT ON)
    set(SC_ENABLE_DINGTALK ON)
    set(SC_ENABLE_SIGNAL ON)
    set(SC_ENABLE_NOSTR ON)
    set(SC_ENABLE_QQ ON)
    set(SC_ENABLE_MAIXCAM ON)
    set(SC_ENABLE_DISPATCH ON)
endif()

# --- Channel sources (conditionally added) ---
set(CHANNEL_SOURCES)
if(SC_ENABLE_CLI)
    list(APPEND CHANNEL_SOURCES src/channels/cli.c)
endif()
if(SC_ENABLE_TELEGRAM)
    list(APPEND CHANNEL_SOURCES src/channels/telegram.c)
endif()
if(SC_ENABLE_DISCORD)
    list(APPEND CHANNEL_SOURCES src/channels/discord.c)
endif()
if(SC_ENABLE_SLACK)
    list(APPEND CHANNEL_SOURCES src/channels/slack.c)
endif()
if(SC_ENABLE_WHATSAPP)
    list(APPEND CHANNEL_SOURCES src/channels/whatsapp.c)
endif()
if(SC_ENABLE_MATRIX)
    list(APPEND CHANNEL_SOURCES src/channels/matrix.c)
endif()
if(SC_ENABLE_IRC)
    list(APPEND CHANNEL_SOURCES src/channels/irc.c)
endif()
if(SC_ENABLE_LINE)
    list(APPEND CHANNEL_SOURCES src/channels/line.c)
endif()
if(SC_ENABLE_LARK)
    list(APPEND CHANNEL_SOURCES src/channels/lark.c)
endif()
if(SC_ENABLE_WEB)
    list(APPEND CHANNEL_SOURCES src/channels/web.c)
endif()
if(SC_ENABLE_EMAIL)
    list(APPEND CHANNEL_SOURCES src/channels/email.c)
endif()
if(SC_ENABLE_IMESSAGE)
    list(APPEND CHANNEL_SOURCES src/channels/imessage.c)
endif()
if(SC_ENABLE_MATTERMOST)
    list(APPEND CHANNEL_SOURCES src/channels/mattermost.c)
endif()
if(SC_ENABLE_ONEBOT)
    list(APPEND CHANNEL_SOURCES src/channels/onebot.c)
endif()
if(SC_ENABLE_DINGTALK)
    list(APPEND CHANNEL_SOURCES src/channels/dingtalk.c)
endif()
# signal, nostr, qq, maixcam, dispatch: only when all-channels build (saves ~60KB in default)
if(SC_ENABLE_ALL_CHANNELS)
    list(APPEND CHANNEL_SOURCES
        src/channels/signal.c
        src/channels/nostr.c
        src/channels/qq.c
        src/channels/maixcam.c
        src/channels/dispatch.c
        src/channels/thread_binding.c
    )
endif()

# --- Memory engine sources (conditionally added) ---
set(MEMORY_ENGINE_SOURCES)
if(SC_ENABLE_NONE_ENGINE)
    list(APPEND MEMORY_ENGINE_SOURCES src/memory/engines/none.c)
endif()
if(SC_ENABLE_MARKDOWN_ENGINE)
    list(APPEND MEMORY_ENGINE_SOURCES src/memory/engines/markdown.c)
endif()
if(SC_ENABLE_MEMORY_LRU_ENGINE)
    list(APPEND MEMORY_ENGINE_SOURCES src/memory/engines/memory_lru.c)
endif()
if(SC_ENABLE_SQLITE)
    list(APPEND MEMORY_ENGINE_SOURCES src/memory/engines/sqlite.c)
endif()
# postgres, redis, api: always built (mock in SC_IS_TEST; real when SC_ENABLE_*)
list(APPEND MEMORY_ENGINE_SOURCES
    src/memory/engines/postgres.c
    src/memory/engines/redis.c
    src/memory/engines/api.c
)
if(SC_ENABLE_LUCID_ENGINE)
    list(APPEND MEMORY_ENGINE_SOURCES src/memory/engines/lucid.c)
endif()
if(SC_ENABLE_LANCEDB_ENGINE)
    list(APPEND MEMORY_ENGINE_SOURCES src/memory/engines/lancedb.c)
endif()

# Threads for pthread (dispatcher parallel execution)
find_package(Threads QUIET)

# Core library sources (Phase 4-6, 11: agent, providers, channels, tools, memory, tunnel, gateway, runtime)
# Channels and memory engines are conditionally added above
set(SC_CORE_SOURCES
    src/platform.c
    src/util.c
    src/json_util.c
    src/bus.c
    src/session.c
    src/channel_loop.c
    src/channel_manager.c
    src/auth.c
    src/identity.c
    src/state.c
    src/heartbeat.c
    src/core/allocator.c
    src/core/error.c
    src/core/arena.c
    src/core/string.c
    src/core/json.c
    src/core/http.c
    src/core/process_util.c
    src/sse/sse_client.c
    src/websocket/websocket.c
    src/net_security.c
    src/config.c
    src/config_types.c
    src/config_mutator.c
    src/config_parse.c
    src/capabilities.c
    src/channel_catalog.c
    src/channel_adapters.c
    src/doctor.c
    src/status.c
    src/http_util.c
    src/mcp.c
    src/multimodal.c
    src/rag.c
    src/service.c
    src/skills.c
    src/skill_registry.c
    src/subagent.c
    src/update.c
    src/voice.c
    src/portable_atomic.c
    src/agent_routing.c
    src/health.c
    src/security/security.c
    src/security/policy.c
    src/security/pairing.c
    src/security/secrets.c
    src/security/audit.c
    src/security/noop_sandbox.c
    src/security/landlock.c
    src/security/firejail.c
    src/security/bubblewrap.c
    src/security/docker.c
    src/security/seatbelt.c
    src/security/seccomp.c
    src/security/landlock_seccomp.c
    src/security/appcontainer.c
    src/security/wasi_sandbox.c
    src/security/firecracker.c
    src/security/detect.c
    src/security/sandbox.c
    src/observability.c
    src/observability/log_observer.c
    src/observability/metrics_observer.c
    src/observability/multi_observer.c
    src/tunnel/none.c
    src/tunnel/cloudflare.c
    src/tunnel/ngrok.c
    src/tunnel/tailscale.c
    src/tunnel/custom.c
    src/tunnel/root.c
    src/gateway/gateway.c
    src/gateway/ws_server.c
    src/gateway/control_protocol.c
    src/gateway/event_bridge.c
    src/gateway/push.c
    src/runtime/native.c
    src/runtime/docker.c
    src/runtime/wasm_rt.c
    src/runtime/cloudflare.c
    src/runtime/factory.c
    src/agent/agent.c
    src/agent/context.c
    src/agent/prompt.c
    src/agent/memory_loader.c
    src/agent/dispatcher.c
    src/agent/compaction.c
    src/agent/spawn.c
    src/agent/mailbox.c
    src/agent/profile.c
    src/agent/planner.c
    src/agent/cli.c
    src/agent/tui.c
    src/agent/commands.c
    src/agent/context_tokens.c
    src/agent/max_tokens.c
    src/providers/error_classify.c
    src/providers/api_key.c
    src/providers/helpers.c
    src/providers/sse.c
    src/providers/scrub.c
    src/providers/reliable.c
    src/providers/router.c
    src/providers/codex_cli.c
    src/providers/openai_codex.c
    src/providers/runtime_bundle.c
    src/providers/openai.c
    src/providers/anthropic.c
    src/providers/gemini.c
    src/providers/ollama.c
    src/providers/openrouter.c
    src/providers/compatible.c
    src/providers/claude_cli.c
    src/providers/factory.c
    ${CHANNEL_SOURCES}
    ${MEMORY_ENGINE_SOURCES}
    src/memory/engines/registry.c
    src/memory/util.c
    src/memory/lifecycle/cache.c
    src/memory/lifecycle/hygiene.c
    src/memory/lifecycle/snapshot.c
    src/memory/lifecycle/summarizer.c
    src/memory/lifecycle/diagnostics.c
    src/memory/lifecycle/rollout.c
    src/memory/lifecycle/migrate.c
    src/memory/retrieval/keyword.c
    src/memory/retrieval/temporal.c
    src/memory/retrieval/reranker.c
    src/memory/retrieval/hybrid.c
    src/memory/retrieval/adaptive.c
    src/memory/retrieval/rrf.c
    src/memory/retrieval/qmd.c
    src/memory/retrieval/query_expansion.c
    src/memory/retrieval/llm_reranker.c
    src/memory/retrieval/engine.c
    src/memory/vector/circuit_breaker.c
    src/tools/validation.c
    src/tools/path_security.c
    src/tools/shell.c
    src/tools/file_read.c
    src/tools/file_write.c
    src/tools/file_edit.c
    src/tools/file_append.c
    src/tools/git.c
    src/tools/web_search.c
    src/tools/web_search_providers/common.c
    src/tools/web_search_providers/brave.c
    src/tools/web_search_providers/duckduckgo.c
    src/tools/web_search_providers/tavily.c
    src/tools/web_search_providers/exa.c
    src/tools/web_search_providers/firecrawl.c
    src/tools/web_search_providers/perplexity.c
    src/tools/web_search_providers/jina.c
    src/tools/web_search_providers/searxng.c
    src/tools/web_fetch.c
    src/tools/http_request.c
    src/tools/browser.c
    src/tools/memory_store.c
    src/tools/memory_recall.c
    src/tools/memory_list.c
    src/tools/memory_forget.c
    src/tools/message.c
    src/tools/delegate.c
    src/tools/database.c
    src/tools/spawn.c
    src/tools/cron_add.c
    src/tools/cron_list.c
    src/tools/cron_remove.c
    src/tools/cron_run.c
    src/tools/cron_runs.c
    src/tools/cron_update.c
    src/tools/browser_open.c
    src/tools/composio.c
    src/tools/hardware_memory.c
    src/tools/schedule.c
    src/tools/schema.c
    src/tools/schema_clean.c
    src/tools/pushover.c
    src/tools/hardware_info.c
    src/tools/i2c.c
    src/tools/spi.c
    src/tools/image.c
    src/tools/screenshot.c
    src/tools/claude_code.c
    src/tools/agent_spawn.c
    src/tools/agent_query.c
    src/tools/canvas.c
    src/tools/apply_patch.c
    src/tools/notebook.c
    src/tools/diff.c
    src/tools/factory.c
    src/mcp_server.c
    src/peripherals/arduino.c
    src/peripherals/stm32.c
    src/peripherals/rpi.c
    src/peripherals/factory.c
    src/hardware.c
    src/channels/thread_binding.c
    src/security/policy_engine.c
    src/security/replay.c
    src/observability/otel.c
    src/plugins/registry.c
    src/skillforge.c
    src/onboard.c
    src/interactions/choices.c
    src/daemon.c
    src/cron.c
    src/crontab.c
    src/migration.c
    src/version.c
    src/cli_commands.c
    src/cost.c
    src/memory/vector/math.c
    src/memory/vector/cosine.c
    src/memory/vector/chunker.c
    src/memory/vector/store_mem.c
    src/memory/vector/embedder_local.c
    src/memory/vector/embeddings.c
    src/memory/vector/embeddings_gemini.c
    src/memory/vector/embeddings_ollama.c
    src/memory/vector/embeddings_voyage.c
    src/memory/vector/provider_router.c
    src/memory/vector/outbox.c
    src/memory/vector/store.c
    src/memory/vector/store_pgvector.c
    src/memory/vector/store_qdrant.c
    src/memory/lifecycle/semantic_cache.c
)

# Crypto sources (generic C fallbacks + dispatch)
set(SC_CRYPTO_SOURCES
    asm/generic/chacha20.c
    asm/generic/sha256.c
    src/crypto/dispatch.c
)

# Arch-specific assembly (Phase 9)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set(SC_ASM_SOURCES
        asm/x86_64/chacha20.S
        asm/x86_64/sha256.S
    )
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
    set(SC_ASM_SOURCES
        asm/aarch64/chacha20.S
        asm/aarch64/sha256.S
    )
else()
    set(SC_ASM_SOURCES)
endif()

# Include directories (CMAKE_CURRENT_SOURCE_DIR when built as subdirectory)
set(SC_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${SC_ROOT}/include)
include_directories(${SC_ROOT}/asm/generic)

# --- Core static library ---
add_library(seaclaw_core STATIC ${SC_CORE_SOURCES} ${SC_CRYPTO_SOURCES} ${SC_ASM_SOURCES})
target_include_directories(seaclaw_core PUBLIC ${SC_ROOT}/include)
if(Threads_FOUND)
    target_link_libraries(seaclaw_core PUBLIC Threads::Threads)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(seaclaw_core PUBLIC m)
endif()
target_include_directories(seaclaw_core PRIVATE ${SC_ROOT}/asm/generic)
## SC_IS_TEST is intentionally NOT set here — seaclaw_core is the production library.
## Tests link against seaclaw_core_test (below) which defines SC_IS_TEST=1.

# --- Compile definitions for enabled channels (for conditional compilation in source) ---
if(SC_ENABLE_CLI)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_CLI=1)
endif()
if(SC_ENABLE_TELEGRAM)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_TELEGRAM=1)
endif()
if(SC_ENABLE_DISCORD)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_DISCORD=1)
endif()
if(SC_ENABLE_SLACK)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_SLACK=1)
endif()
if(SC_ENABLE_WHATSAPP)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_WHATSAPP=1)
endif()
if(SC_ENABLE_MATRIX)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_MATRIX=1)
endif()
if(SC_ENABLE_IRC)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_IRC=1)
endif()
if(SC_ENABLE_LINE)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_LINE=1)
endif()
if(SC_ENABLE_LARK)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_LARK=1)
endif()
if(SC_ENABLE_WEB)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_WEB=1)
endif()
if(SC_ENABLE_EMAIL)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_EMAIL=1)
endif()
if(SC_ENABLE_IMESSAGE)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_IMESSAGE=1)
endif()
if(SC_ENABLE_MATTERMOST)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_MATTERMOST=1)
endif()
if(SC_ENABLE_ONEBOT)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_ONEBOT=1)
endif()
if(SC_ENABLE_DINGTALK)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_DINGTALK=1)
endif()
if(SC_ENABLE_SIGNAL)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_SIGNAL=1)
endif()
if(SC_ENABLE_NOSTR)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_NOSTR=1)
endif()
if(SC_ENABLE_QQ)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_QQ=1)
endif()
if(SC_ENABLE_MAIXCAM)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_MAIXCAM=1)
endif()
if(SC_ENABLE_DISPATCH)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_DISPATCH=1)
endif()

# --- Compile definitions for enabled memory engines ---
if(SC_ENABLE_NONE_ENGINE)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_NONE_ENGINE=1)
endif()
if(SC_ENABLE_MARKDOWN_ENGINE)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_MARKDOWN_ENGINE=1)
endif()
if(SC_ENABLE_MEMORY_LRU_ENGINE)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_MEMORY_LRU_ENGINE=1)
endif()
if(SC_ENABLE_API_ENGINE)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_API_ENGINE=1)
endif()
if(SC_ENABLE_LUCID_ENGINE)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_LUCID_ENGINE=1)
endif()
if(SC_ENABLE_REDIS_ENGINE)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_REDIS_ENGINE=1 SC_ENABLE_REDIS_ENGINE=1)
endif()
if(SC_ENABLE_LANCEDB_ENGINE)
    target_compile_definitions(seaclaw_core PRIVATE SC_HAS_LANCEDB_ENGINE=1)
endif()

# --- Size optimization: function/data sections for link-time dead code elimination ---
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(seaclaw_core PRIVATE -ffunction-sections -fdata-sections)
endif()

if(SC_ENABLE_SQLITE)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(SQLITE3 QUIET sqlite3)
    endif()
    if(SQLITE3_FOUND)
        target_include_directories(seaclaw_core PRIVATE ${SQLITE3_INCLUDE_DIRS})
        target_link_libraries(seaclaw_core PUBLIC ${SQLITE3_LIBRARIES})
    else()
        find_library(SQLITE3_LIBRARY sqlite3)
        if(SQLITE3_LIBRARY)
            target_link_libraries(seaclaw_core PUBLIC sqlite3)
        endif()
    endif()
    target_compile_definitions(seaclaw_core PRIVATE SC_ENABLE_SQLITE=1)
endif()

if(SC_ENABLE_POSTGRES)
    find_package(PostgreSQL QUIET)
    if(PostgreSQL_FOUND)
        target_include_directories(seaclaw_core PRIVATE ${PostgreSQL_INCLUDE_DIRS})
        target_link_libraries(seaclaw_core PUBLIC PostgreSQL::libpq)
        target_compile_definitions(seaclaw_core PRIVATE SC_ENABLE_POSTGRES=1)
        message(STATUS "PostgreSQL memory backend: enabled")
    else()
        find_library(PQ_LIBRARY NAMES pq libpq)
        find_path(PQ_INCLUDE_DIR NAMES libpq-fe.h)
        if(PQ_LIBRARY AND PQ_INCLUDE_DIR)
            target_include_directories(seaclaw_core PRIVATE ${PQ_INCLUDE_DIR})
            target_link_libraries(seaclaw_core PUBLIC ${PQ_LIBRARY})
            target_compile_definitions(seaclaw_core PRIVATE SC_ENABLE_POSTGRES=1)
            message(STATUS "PostgreSQL memory backend: enabled")
        else()
            message(STATUS "PostgreSQL memory backend: disabled (libpq not found)")
        endif()
    endif()
endif()

if(UNIX AND NOT WIN32)
    target_compile_definitions(seaclaw_core PRIVATE SC_GATEWAY_POSIX=1)
endif()

if(SC_ENABLE_CURL)
    find_package(CURL QUIET)
    if(TARGET CURL::libcurl)
        target_link_libraries(seaclaw_core PUBLIC CURL::libcurl)
        target_compile_definitions(seaclaw_core PRIVATE SC_HTTP_CURL=1)
        message(STATUS "HTTP client: libcurl enabled")
    elseif(CURL_FOUND)
        target_include_directories(seaclaw_core PRIVATE ${CURL_INCLUDE_DIRS})
        target_link_libraries(seaclaw_core PUBLIC ${CURL_LIBRARIES})
        target_compile_definitions(seaclaw_core PRIVATE SC_HTTP_CURL=1)
        message(STATUS "HTTP client: libcurl enabled")
    else()
        find_library(CURL_LIBRARY curl)
        find_path(CURL_INCLUDE_DIR NAMES curl/curl.h)
        if(CURL_LIBRARY AND CURL_INCLUDE_DIR)
            target_include_directories(seaclaw_core PRIVATE ${CURL_INCLUDE_DIR})
            target_link_libraries(seaclaw_core PUBLIC ${CURL_LIBRARY})
            target_compile_definitions(seaclaw_core PRIVATE SC_HTTP_CURL=1)
            message(STATUS "HTTP client: libcurl enabled")
        else()
            message(STATUS "HTTP client: disabled (libcurl not found)")
        endif()
    endif()
endif()

if(SC_ENABLE_FIPS_CRYPTO)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(seaclaw_core PUBLIC OpenSSL::Crypto)
    target_compile_definitions(seaclaw_core PRIVATE SC_ENABLE_FIPS_CRYPTO=1)
    message(STATUS "FIPS crypto: AES-256-GCM enabled")
endif()

if(SC_ENABLE_TLS)
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_link_libraries(seaclaw_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(seaclaw_core PRIVATE SC_HAS_TLS=1)
        message(STATUS "TLS: OpenSSL enabled")
    else()
        find_library(OPENSSL_SSL_LIB ssl HINTS /opt/homebrew/opt/openssl@3/lib /opt/homebrew/opt/openssl/lib)
        find_library(OPENSSL_CRYPTO_LIB crypto HINTS /opt/homebrew/opt/openssl@3/lib /opt/homebrew/opt/openssl/lib)
        find_path(OPENSSL_INCLUDE openssl/ssl.h HINTS /opt/homebrew/opt/openssl@3/include /opt/homebrew/opt/openssl/include)
        if(OPENSSL_SSL_LIB AND OPENSSL_CRYPTO_LIB AND OPENSSL_INCLUDE)
            target_include_directories(seaclaw_core PRIVATE ${OPENSSL_INCLUDE})
            target_link_libraries(seaclaw_core PUBLIC ${OPENSSL_SSL_LIB} ${OPENSSL_CRYPTO_LIB})
            target_compile_definitions(seaclaw_core PRIVATE SC_HAS_TLS=1)
            message(STATUS "TLS: OpenSSL enabled (manual)")
        else()
            message(STATUS "TLS: disabled (OpenSSL not found)")
        endif()
    endif()
endif()

if(SC_ENABLE_LINENOISE)
    set(LINENOISE_DIR "${CMAKE_SOURCE_DIR}/vendor/linenoise")
    if(EXISTS "${LINENOISE_DIR}/linenoise.c")
        target_sources(seaclaw_core PRIVATE "${LINENOISE_DIR}/linenoise.c")
        target_include_directories(seaclaw_core PRIVATE "${LINENOISE_DIR}")
        target_compile_definitions(seaclaw_core PRIVATE SC_ENABLE_LINENOISE=1)
        message(STATUS "Linenoise: enabled (${LINENOISE_DIR})")
    else()
        message(WARNING "Linenoise: vendor/linenoise/linenoise.c not found. "
            "Download from https://github.com/antirez/linenoise")
    endif()
elseif(SC_ENABLE_READLINE)
    find_library(READLINE_LIBRARY NAMES readline edit)
    if(READLINE_LIBRARY)
        target_link_libraries(seaclaw_core PUBLIC ${READLINE_LIBRARY})
        target_compile_definitions(seaclaw_core PRIVATE SC_ENABLE_READLINE=1)
        message(STATUS "Readline: enabled (${READLINE_LIBRARY})")
    else()
        message(STATUS "Readline: disabled (library not found)")
    endif()
else()
    message(STATUS "Line editing: OFF (basic getchar fallback)")
endif()

if(SC_ENABLE_TUI)
    set(TERMBOX2_DIR "${CMAKE_SOURCE_DIR}/vendor/termbox2")
    if(EXISTS "${TERMBOX2_DIR}/termbox2.h")
        target_compile_definitions(seaclaw_core PRIVATE SC_ENABLE_TUI=1)
        target_include_directories(seaclaw_core PRIVATE "${TERMBOX2_DIR}")
        message(STATUS "TUI: enabled (termbox2 at ${TERMBOX2_DIR})")
    else()
        message(WARNING "TUI: vendor/termbox2/termbox2.h not found. "
            "Download from https://github.com/termbox/termbox2")
    endif()
else()
    message(STATUS "TUI: OFF (ANSI spinner mode)")
endif()

if(SC_ENABLE_ASAN)
    target_compile_options(seaclaw_core PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(seaclaw_core PRIVATE -fsanitize=address)
endif()

# --- Main executable ---
add_executable(seaclaw src/main.c)
target_link_libraries(seaclaw seaclaw_core)

if(SC_ENABLE_ASAN)
    target_compile_options(seaclaw PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(seaclaw PRIVATE -fsanitize=address)
endif()

# --- LTO (Link-Time Optimization) ---
if(SC_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_supported)
    if(lto_supported)
        set_property(TARGET seaclaw PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET seaclaw_core PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

# --- Release optimizations ---
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    if(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel" AND CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang")
        target_compile_options(seaclaw PRIVATE -Oz -fno-asynchronous-unwind-tables -fmerge-all-constants)
        target_compile_options(seaclaw_core PRIVATE -Oz -fno-asynchronous-unwind-tables -fmerge-all-constants)
    else()
        target_compile_options(seaclaw PRIVATE -Os)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(seaclaw PRIVATE -fvisibility=hidden)
        target_compile_options(seaclaw_core PRIVATE -fvisibility=hidden)
    endif()
    if(APPLE)
        set_target_properties(seaclaw PROPERTIES LINK_FLAGS "-Wl,-dead_strip")
    elseif(UNIX)
        set_target_properties(seaclaw PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    endif()
endif()

# --- Strip MinSizeRel binary (macOS: -x to preserve local syms, Linux: full strip) ---
if(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel" AND UNIX)
    if(APPLE)
        add_custom_command(TARGET seaclaw POST_BUILD COMMAND strip -x $<TARGET_FILE:seaclaw>)
    else()
        add_custom_command(TARGET seaclaw POST_BUILD COMMAND strip $<TARGET_FILE:seaclaw>)
    endif()
endif()

# --- Test-mode core library (same sources, compiled with SC_IS_TEST=1) ---
# When SC_ENABLE_ALL_CHANNELS=OFF, prod binary omits signal/nostr/qq/maixcam/dispatch,
# but tests need them — add to test lib only.
set(SC_TEST_EXTRA_CHANNELS)
if(NOT SC_ENABLE_ALL_CHANNELS)
    list(APPEND SC_TEST_EXTRA_CHANNELS
        src/channels/signal.c src/channels/nostr.c src/channels/qq.c
        src/channels/maixcam.c src/channels/dispatch.c)
endif()
add_library(seaclaw_core_test STATIC ${SC_CORE_SOURCES} ${SC_TEST_EXTRA_CHANNELS} ${SC_CRYPTO_SOURCES} ${SC_ASM_SOURCES})
target_include_directories(seaclaw_core_test PUBLIC ${SC_ROOT}/include)
target_include_directories(seaclaw_core_test PRIVATE ${SC_ROOT}/asm/generic)
target_compile_definitions(seaclaw_core_test PRIVATE SC_IS_TEST=1)
if(Threads_FOUND)
    target_link_libraries(seaclaw_core_test PUBLIC Threads::Threads)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(seaclaw_core_test PUBLIC m)
endif()
if(SC_ENABLE_CLI)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_CLI=1)
endif()
if(SC_ENABLE_TELEGRAM)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_TELEGRAM=1)
endif()
if(SC_ENABLE_DISCORD)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_DISCORD=1)
endif()
if(SC_ENABLE_SLACK)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_SLACK=1)
endif()
if(SC_ENABLE_WHATSAPP)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_WHATSAPP=1)
endif()
if(SC_ENABLE_MATRIX)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_MATRIX=1)
endif()
if(SC_ENABLE_IRC)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_IRC=1)
endif()
if(SC_ENABLE_LINE)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_LINE=1)
endif()
if(SC_ENABLE_LARK)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_LARK=1)
endif()
if(SC_ENABLE_WEB)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_WEB=1)
endif()
if(SC_ENABLE_EMAIL)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_EMAIL=1)
endif()
if(SC_ENABLE_IMESSAGE)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_IMESSAGE=1)
endif()
if(SC_ENABLE_MATTERMOST)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_MATTERMOST=1)
endif()
if(SC_ENABLE_ONEBOT)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_ONEBOT=1)
endif()
if(SC_ENABLE_DINGTALK)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_DINGTALK=1)
endif()
target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_SIGNAL=1 SC_HAS_NOSTR=1 SC_HAS_QQ=1 SC_HAS_MAIXCAM=1 SC_HAS_DISPATCH=1 SC_HAS_THREAD_BINDING=1)
if(SC_ENABLE_NONE_ENGINE)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_NONE_ENGINE=1)
endif()
if(SC_ENABLE_MARKDOWN_ENGINE)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_MARKDOWN_ENGINE=1)
endif()
if(SC_ENABLE_MEMORY_LRU_ENGINE)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_MEMORY_LRU_ENGINE=1)
endif()
if(SC_ENABLE_API_ENGINE)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_API_ENGINE=1)
endif()
if(SC_ENABLE_LUCID_ENGINE)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_LUCID_ENGINE=1)
endif()
if(SC_ENABLE_REDIS_ENGINE)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_REDIS_ENGINE=1)
endif()
if(SC_ENABLE_LANCEDB_ENGINE)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_LANCEDB_ENGINE=1)
endif()
if(SC_ENABLE_SQLITE)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_ENABLE_SQLITE=1)
    if(SQLITE3_FOUND)
        target_include_directories(seaclaw_core_test PRIVATE ${SQLITE3_INCLUDE_DIRS})
        target_link_libraries(seaclaw_core_test PUBLIC ${SQLITE3_LIBRARIES})
    elseif(SQLITE3_LIBRARY)
        target_link_libraries(seaclaw_core_test PUBLIC sqlite3)
    endif()
endif()
if(SC_ENABLE_FIPS_CRYPTO)
    if(OpenSSL_FOUND)
        target_link_libraries(seaclaw_core_test PUBLIC OpenSSL::Crypto)
        target_compile_definitions(seaclaw_core_test PRIVATE SC_ENABLE_FIPS_CRYPTO=1)
    else()
        find_package(OpenSSL REQUIRED)
        target_link_libraries(seaclaw_core_test PUBLIC OpenSSL::Crypto)
        target_compile_definitions(seaclaw_core_test PRIVATE SC_ENABLE_FIPS_CRYPTO=1)
    endif()
endif()
if(SC_ENABLE_TLS)
    if(OpenSSL_FOUND)
        target_link_libraries(seaclaw_core_test PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_TLS=1)
    elseif(OPENSSL_SSL_LIB AND OPENSSL_CRYPTO_LIB AND OPENSSL_INCLUDE)
        target_include_directories(seaclaw_core_test PRIVATE ${OPENSSL_INCLUDE})
        target_link_libraries(seaclaw_core_test PUBLIC ${OPENSSL_SSL_LIB} ${OPENSSL_CRYPTO_LIB})
        target_compile_definitions(seaclaw_core_test PRIVATE SC_HAS_TLS=1)
    endif()
endif()
if(UNIX AND NOT WIN32)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_GATEWAY_POSIX=1)
endif()
if(SC_ENABLE_TUI)
    target_compile_definitions(seaclaw_core_test PRIVATE SC_ENABLE_TUI=1)
    set(TERMBOX2_DIR "${CMAKE_SOURCE_DIR}/vendor/termbox2")
    if(EXISTS "${TERMBOX2_DIR}/termbox2.h")
        target_include_directories(seaclaw_core_test PRIVATE "${TERMBOX2_DIR}")
    endif()
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(seaclaw_core_test PRIVATE -ffunction-sections -fdata-sections -Wall -Wextra -Wpedantic)
endif()
if(SC_ENABLE_ASAN)
    target_compile_options(seaclaw_core_test PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(seaclaw_core_test PRIVATE -fsanitize=address)
endif()

# --- Tests ---
enable_testing()

set(SC_TEST_SOURCES
    tests/test_main.c
    tests/test_agent_subsystems.c
    tests/test_crypto.c
    tests/test_wasm.c
    tests/test_allocator.c
    tests/test_json.c
    tests/test_string.c
    tests/test_slice.c
    tests/test_memory.c
    tests/test_tunnel.c
    tests/test_gateway.c
    tests/test_auth.c
    tests/test_security.c
    tests/test_provider.c
    tests/test_channel.c
    tests/test_tool.c
    tests/test_peripheral.c
    tests/test_vtables.c
    tests/test_e2e.c
    tests/test_subsystems.c
    tests/test_config_parse.c
    tests/test_adversarial.c
    tests/test_gateway_http.c
    tests/test_memory_full.c
    tests/test_tools_all.c
    tests/test_infrastructure.c
    tests/test_memory_subsystems.c
    tests/test_memory_new.c
    tests/test_http.c
    tests/test_sse.c
    tests/test_streaming.c
    tests/test_websocket.c
    tests/test_net_security.c
    tests/test_path_security.c
    tests/test_process_util.c
    tests/test_cron.c
    tests/test_channel_loop.c
    tests/test_rag.c
    tests/test_multimodal.c
    tests/test_retrieval.c
    tests/test_prompt.c
    tests/test_vector.c
    tests/test_vector_full.c
    tests/test_lifecycle.c
    tests/test_observer.c
    tests/test_session.c
    tests/test_bus.c
    tests/test_identity.c
    tests/test_channel_manager.c
    tests/test_new_modules.c
    tests/test_provider_all.c
    tests/test_channel_all.c
    tests/test_config_extended.c
    tests/test_json_extended.c
    tests/test_security_extended.c
    tests/test_core_extended.c
    tests/test_gateway_extended.c
    tests/test_agent_extended.c
    tests/test_skills.c
    tests/test_ported_modules.c
    tests/test_subagent.c
    tests/test_mcp.c
    tests/test_voice.c
    tests/test_vector_stores.c
    tests/test_cli.c
    tests/test_memory_engines_ext.c
    tests/test_runtime.c
    tests/test_util_modules.c
    tests/test_roadmap.c
)

add_executable(seaclaw_tests ${SC_TEST_SOURCES} wasm/wasm_alloc.c)
target_link_libraries(seaclaw_tests seaclaw_core_test)
target_include_directories(seaclaw_tests PRIVATE ${SC_ROOT}/tests)
target_compile_definitions(seaclaw_tests PRIVATE SC_IS_TEST=1 SC_GATEWAY_TEST_MODE=1 SC_HAS_NONE_ENGINE=1)
if(SC_ENABLE_TUI)
    target_compile_definitions(seaclaw_tests PRIVATE SC_ENABLE_TUI=1)
endif()
if(SC_ENABLE_SQLITE)
    target_compile_definitions(seaclaw_tests PRIVATE SC_ENABLE_SQLITE=1)
endif()
if(SC_ENABLE_TELEGRAM)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_TELEGRAM=1)
endif()
if(SC_ENABLE_DISCORD)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_DISCORD=1)
endif()
if(SC_ENABLE_SLACK)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_SLACK=1)
endif()
if(SC_ENABLE_WHATSAPP)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_WHATSAPP=1)
endif()
if(SC_ENABLE_MATRIX)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_MATRIX=1)
endif()
if(SC_ENABLE_IRC)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_IRC=1)
endif()
if(SC_ENABLE_LINE)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_LINE=1)
endif()
if(SC_ENABLE_LARK)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_LARK=1)
endif()
if(SC_ENABLE_WEB)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_WEB=1)
endif()
if(SC_ENABLE_EMAIL)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_EMAIL=1)
endif()
if(SC_ENABLE_IMESSAGE)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_IMESSAGE=1)
endif()
if(SC_ENABLE_MATTERMOST)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_MATTERMOST=1)
endif()
if(SC_ENABLE_ONEBOT)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_ONEBOT=1)
endif()
if(SC_ENABLE_DINGTALK)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_DINGTALK=1)
endif()
if(SC_ENABLE_LUCID_ENGINE)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_LUCID_ENGINE=1)
endif()
if(SC_ENABLE_LANCEDB_ENGINE)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_LANCEDB_ENGINE=1)
endif()
if(SC_ENABLE_POSTGRES)
    target_compile_definitions(seaclaw_tests PRIVATE SC_ENABLE_POSTGRES=1)
endif()
if(SC_ENABLE_REDIS_ENGINE)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_REDIS_ENGINE=1 SC_ENABLE_REDIS_ENGINE=1)
endif()
if(SC_ENABLE_API_ENGINE)
    target_compile_definitions(seaclaw_tests PRIVATE SC_HAS_API_ENGINE=1)
endif()
if(SC_ENABLE_FIPS_CRYPTO)
    target_compile_definitions(seaclaw_tests PRIVATE SC_ENABLE_FIPS_CRYPTO=1)
endif()

if(SC_ENABLE_ASAN)
    target_compile_options(seaclaw_tests PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(seaclaw_tests PRIVATE -fsanitize=address)
endif()

add_test(NAME unit_tests COMMAND seaclaw_tests)

# --- WASM/WASI target (optional, use with wasm32-wasi toolchain) ---
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-wasi.cmake -DSC_BUILD_WASM=ON ..
if(SC_BUILD_WASM)
    include(cmake/wasm_sources.cmake)
    add_executable(seaclaw.wasm
        ${SC_WASM_MAIN}
        ${SC_WASM_SOURCES}
        ${SC_WASM_CORE_SOURCES}
        ${SC_WASM_CRYPTO_SOURCES}
    )
    target_include_directories(seaclaw.wasm PRIVATE ${SC_ROOT}/include ${SC_ROOT}/asm/generic)
    target_compile_definitions(seaclaw.wasm PRIVATE __wasi__=1 SC_IS_TEST=0)
    if(SC_ENABLE_SQLITE)
        target_compile_definitions(seaclaw.wasm PRIVATE SC_ENABLE_SQLITE=0)
    endif()
    set_target_properties(seaclaw.wasm PROPERTIES SUFFIX ".wasm" RUNTIME_OUTPUT_NAME "seaclaw")
    message(STATUS "WASM target: seaclaw.wasm (requires wasm32-wasi toolchain)")
endif()

# Compiler warnings (-Werror disabled to allow unused params in tool stubs)
target_compile_options(seaclaw_core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(seaclaw PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_options(seaclaw_tests PRIVATE -Wall -Wextra -Wpedantic)

# Install target
install(TARGETS seaclaw RUNTIME DESTINATION bin)

# SBOM Generation
add_custom_target(sbom
    COMMAND ${CMAKE_COMMAND} -E echo "Generating SBOM..."
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/generate-sbom.sh ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/sbom.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating CycloneDX SBOM"
)

# Fuzz testing (requires Clang with libFuzzer)
option(SC_ENABLE_FUZZING "Build fuzz testing harnesses" OFF)
if(SC_ENABLE_FUZZING)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "Fuzzing requires Clang compiler")
    endif()
    enable_language(CXX)
    set(FUZZ_SANITIZE "-fsanitize=fuzzer,address")
    set(FUZZ_FRAME "-fno-omit-frame-pointer")
    # macOS arm64 + LLVM 21: link Homebrew's libc++ to resolve __hash_memory (LLVM#77653)
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(FUZZ_EXTRA_LINK "-L/opt/homebrew/opt/llvm/lib/c++" "-L/opt/homebrew/opt/llvm/lib/unwind")
        set(FUZZ_EXTRA_LIBS "unwind" "c++")
    else()
        set(FUZZ_EXTRA_LINK "")
        set(FUZZ_EXTRA_LIBS "")
    endif()

    add_executable(fuzz_json_parser tests/fuzz/fuzz_json_parser.c)
    set_target_properties(fuzz_json_parser PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(fuzz_json_parser PRIVATE seaclaw_core ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_json_parser PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_json_parser PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_http_parse tests/fuzz/fuzz_http_parse.c)
    set_target_properties(fuzz_http_parse PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(fuzz_http_parse PRIVATE seaclaw_core ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_http_parse PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_http_parse PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_url_validation tests/fuzz/fuzz_url_validation.c)
    set_target_properties(fuzz_url_validation PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(fuzz_url_validation PRIVATE seaclaw_core ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_url_validation PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_url_validation PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})
endif()

# Benchmark target
add_custom_target(benchmark
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/benchmark.sh $<TARGET_FILE:seaclaw>
    DEPENDS seaclaw seaclaw_tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running benchmarks..."
)

# Print build info
message(STATUS "SeaClaw v0.1.0")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Channels: ${CHANNEL_SOURCES}")
message(STATUS "Engines: none=${SC_ENABLE_NONE_ENGINE} markdown=${SC_ENABLE_MARKDOWN_ENGINE} sqlite=${SC_ENABLE_SQLITE} postgres=${SC_ENABLE_POSTGRES}")
message(STATUS "ASan: ${SC_ENABLE_ASAN}")
