cmake_minimum_required(VERSION 3.20)
project(human C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- US-8.3: Reproducible Binary Builds -------------------------------------
#
# Goal: two independent builds from the same source tree, with the same
# toolchain and the same SOURCE_DATE_EPOCH, produce bytewise-identical
# `human` binaries (modulo ELF build-id and Mach-O LC_UUID, which carry
# host-derived randomness and are carved out by check-reproducible-build.sh).
#
# Design: sprints/sprint-8/designs/US-8.3.md
# Standard: docs/standards/engineering/reproducible-builds.md
#
# These flags are applied UNCONDITIONALLY (not gated on SOURCE_DATE_EPOCH)
# because a contract that is only checked sometimes is not a contract
# (rules/quality-gates.md: "no silent failures"). SOURCE_DATE_EPOCH, when
# set, gives the compiler a fixed value for any __DATE__/__TIME__ that
# slips through; -Werror=date-time additionally fails the build on any
# new use. Together they are belt-and-suspenders.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    # Strip absolute build-path prefixes from __FILE__, debug info, and
    # assertion strings. Maps source dir and build dir to canonical "." so
    # two builds from /tmp/build-a and /tmp/build-b yield the same bytes.
    add_compile_options(
        "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
        "-ffile-prefix-map=${CMAKE_BINARY_DIR}=.")
    # Promote any future __DATE__/__TIME__ macro usage from a warning to a
    # hard error. Design note AP-4: the story text said -Wno-date-time but
    # a suppressed warning is not a contract. AC-8.3.3 is still satisfied
    # because -Wdate-time IS emitted; we additionally fail the build.
    add_compile_options(-Werror=date-time)
endif()

# Deterministic archive creation: -D modifier zeroes mtime/uid/gid/mode in
# archive headers. Only GNU ar and llvm-ar accept it; Darwin's BSD ar does
# not. Darwin's libtool is invoked via CMAKE_C_CREATE_STATIC_LIBRARY by
# default and already produces deterministic archives in modern Xcode
# (since Xcode 10, archives use zerofill metadata). Probe -D once.
include(CheckSourceCompiles)
# Deterministic archives: rely on the platform default rather than a
# CMake-level ar flag override. Modern binutils ship with
# `--enable-deterministic-archives` at configure time (the upstream
# default since binutils 2.27; every major distro turns it on), and
# Darwin's libtool already zerofills archive metadata since Xcode 10.
#
# Earlier code here tried to wedge `-D` into ar invocations via
# CMAKE_C_ARCHIVE_{CREATE,APPEND,FINISH}. Both the probe AND the
# override turned out to be unreliable:
#   - The probe (`ar -D --version`) returned exit 0 on Ubuntu 24.04's
#     binutils-2.42 because `--version` short-circuits regardless of
#     preceding modifiers, falsely reporting that `qcD` is safe.
#   - The `CMAKE_C_ARCHIVE_FINISH "ar -D <TARGET>"` finalize step
#     fails with `-D: invalid option -- 'n'` at real link time on
#     several runner image versions (the 'n' is part of an emulation
#     suffix appended by gcc-ar/LTO-plugin wrapping that the override
#     doesn't account for).
# Dropping the override entirely sidesteps both pitfalls and loses
# nothing — the resulting archives are still deterministic, just via
# the platform default rather than an explicit per-call flag.
# PR #113 round 29.
message(STATUS "Reproducible builds: relying on platform-default deterministic archives (modern binutils + Darwin libtool)")

# Linker-level nondeterminism. ELF build-id and Mach-O LC_UUID embed
# host-derived randomness; suppress them so two builds match byte-for-byte.
# Linux: --build-id=none (binutils ld + lld). Darwin: -no_uuid (ld64).
#
# IMPORTANT: only apply to the shipped `human` binary, NOT to test
# binaries. On Darwin, dyld + ASan require LC_UUID for symbolication —
# `human_tests` will refuse to load without it. The reproducibility
# contract only covers the shipped binary, so the test binaries can
# keep their UUIDs. Applied later via target_link_options on `human`
# (search for "US-8.3 per-target" in this file).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # On Linux, the BuildId is purely metadata and removing it from
    # test binaries is harmless — leave globally applied.
    add_link_options("LINKER:--build-id=none")
endif()
# Darwin handling moved to the `human` target only (see below).
# --- end US-8.3 -------------------------------------------------------------

option(HU_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(HU_ENABLE_SQLITE "Enable SQLite memory backend" ON)
option(HU_ENABLE_POSTGRES "Enable PostgreSQL memory backend" OFF)
option(HU_ENABLE_PGVECTOR "Enable pgvector vector store (requires libpq)" OFF)
option(HU_ENABLE_CURL "Enable libcurl for HTTP client" ON)
option(HU_ENABLE_INTEGRATION_TESTS "Build human_integration_tests (real libcurl HTTP + SQLite file I/O; no HU_IS_TEST)" OFF)
option(HU_ENABLE_TLS "Enable TLS via OpenSSL for WSS and HTTPS" ON)
option(HU_ENABLE_FIPS_CRYPTO "Use AES-256-GCM via OpenSSL for FIPS 140-2 compliance" OFF)
option(HU_BUILD_WASM "Build WASM/WASI target (human.wasm)" OFF)
option(HU_ENABLE_LTO "Enable Link-Time Optimization" OFF)
option(HU_ENABLE_READLINE "Enable readline/libedit for CLI line editing" OFF)
option(HU_ENABLE_LINENOISE "Enable linenoise for lightweight CLI line editing (vendor/linenoise)" OFF)
option(HU_ENABLE_TUI "Enable rich TUI mode with split-pane terminal UI (vendor/termbox2)" OFF)

# --- Optional module groups (OFF by default to minimize binary size) ---
option(HU_ENABLE_PERIPHERALS "Build hardware peripheral support (Arduino, STM32, RPi, SPI, I2C)" OFF)
option(HU_ENABLE_TUNNELS "Build tunnel providers (ngrok, cloudflare, tailscale)" OFF)
option(HU_ENABLE_RUNTIME_EXOTIC "Build exotic runtime adapters (WASM, Cloudflare Workers)" OFF)
option(HU_ENABLE_TOOLS_BROWSER "Build browser automation tools (browser, screenshot, browser_open)" OFF)
option(HU_ENABLE_TOOLS_ADVANCED "Build advanced tools (composio, claude_code, canvas, notebook, database)" OFF)
option(HU_ENABLE_OTEL "Build OpenTelemetry observability exporter" OFF)
option(HU_ENABLE_CRON "Build cron scheduler support" OFF)
option(HU_ENABLE_PUSH "Build push notification support (FCM/APNS)" OFF)
option(HU_ENABLE_UPDATE "Build update check/apply support" OFF)
option(HU_ENABLE_SKILLS "Build skill acquisition and continuous learning" OFF)
option(HU_ENABLE_PERSONA "Enable persona system" ON)
option(HU_ENABLE_AUTHENTIC "Build authentic existence features" OFF)
option(HU_ENABLE_CARTESIA "Enable Cartesia TTS for voice messages" OFF)
option(HU_ENABLE_ML "Build ML training subsystem (BPE tokenizer, dataloader, experiment loop)" OFF)
option(HU_ENABLE_NEURAL_MEMORY "W10 neural memory placeholder (schema/ONNX path; default OFF)" OFF)
# Init 13 KV-cache compression (DeltaKV + SWAN) was ABANDONED on 2026-05-17:
# see docs/plans/2026-05-11-init-13-kv-compression.md (Status: ABANDONED).
# W13: learning loop (LoRA + DPO from W4/W5/W3 signals). Defaults follow ML.
if(HU_ENABLE_ML)
    option(HU_ENABLE_LEARNING "Build W13 learning loop (hu_learner_t + CPU/MLX/ggml backends)" ON)
else()
    option(HU_ENABLE_LEARNING "Build W13 learning loop (hu_learner_t + CPU/MLX/ggml backends)" OFF)
endif()
option(HU_ENABLE_FEEDS "Build feed ingestion and research agent" OFF)
option(HU_ENABLE_SOCIAL "Build social feed ingestion (Facebook, Instagram)" OFF)
option(HU_ENABLE_EMBEDDED_MODEL "Enable embedded model provider (llama.cpp/llama-cli)" OFF)
option(HU_ENABLE_LLAMACPP "Enable in-process llama.cpp provider with chat-time LoRA merging (W13 Bridge A)" OFF)
option(HU_ENABLE_MLX_PROVIDER "Enable MLX provider (M3 Bridge B) — stub when OFF, subprocess MLX runtime when ON" OFF)
# Phase 2 Task 6 (RL SOTA) — gate the mlx-lm-lora DPO subprocess
# integration test. OFF by default keeps CI hermetic; flipping ON
# locally activates the heavyweight Gemma download in
# tests/test_dpo_real_mlx.c (Task 7 territory). The actual
# target_compile_definitions wiring lives after both human_core and
# human_tests targets are declared (alongside HU_ENABLE_LLAMACPP's
# test wiring); CMake forbids defining symbols on a target before
# the target exists.
option(HU_HAVE_MLX_LM "Enable mlx-lm-lora DPO subprocess integration tests" OFF)
option(HU_HAVE_MLX_LM_KTO "Enable mlx-lm-lora KTO subprocess integration tests" OFF)
# Phase 4 Task 0 (RL SOTA) — mirrors HU_HAVE_MLX_LM_KTO. Default OFF; user
# opts in explicitly after verifying that `python3 -c "from
# mlx_lm_lora.trainer.grpo_trainer import train_grpo"` (or the fallback
# `python3 -m mlx_lm_lora.train --train-mode grpo`) succeeds on their box.
# No auto-detection in CMake — keeps configure output deterministic across
# Apple/Linux dev boxes that may or may not have the third-party package
# installed.
option(HU_HAVE_MLX_LM_GRPO "Enable mlx-lm-lora GRPO subprocess integration tests" OFF)
# Phase 5 Task 0 (RL SOTA) — master gate for the Phase 5 + Phase 6 RL
# evaluation infrastructure (competitive harness, gold-judge leaderboard,
# external-judge bridges, eval gate). OFF by default keeps the default
# release / dev build a zero-byte delta versus Phase 4 close-out; the
# rl_sota preset flips this ON so subsequent Phase 5 tasks can land code
# behind the flag without changing default builds. The actual
# target_compile_definitions wiring lives after the human / human_core /
# human_core_test / human_tests targets are declared (alongside
# HU_HAVE_MLX_LM_GRPO's wiring); CMake forbids defining symbols on a
# target before the target exists.
option(HU_ENABLE_RL_FULL "Enable RL evaluation infrastructure (Phase 5 + 6)" OFF)
# Phase 1 (RL SOTA) — Metal GPU offload for the vendored llama.cpp build.
# Defaults ON only on Apple platforms (the only OS llama.cpp ships Metal
# for). Linux and other non-Apple targets default OFF; setting it ON
# elsewhere is a no-op because upstream's Metal backend rejects
# non-Darwin builds.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    option(HU_LLAMACPP_METAL "Enable Metal GPU offload in vendored llama.cpp" ON)
else()
    option(HU_LLAMACPP_METAL "Enable Metal GPU offload in vendored llama.cpp" OFF)
endif()
option(HU_ENABLE_LIBSODIUM "Link libsodium for production-grade AEAD (XChaCha20-Poly1305) and KDF (Argon2id)" OFF)
option(HU_ENABLE_COREML "Enable CoreML/MLX provider (macOS only)" OFF)
if(APPLE)
    option(HU_ENABLE_APPLE_INTELLIGENCE "Enable Apple Intelligence provider (on-device)" ON)
else()
    option(HU_ENABLE_APPLE_INTELLIGENCE "Enable Apple Intelligence provider (on-device)" OFF)
endif()
option(HU_ENABLE_MCP "Enable MCP client (multi-server tool discovery)" ON)

# WASM build: cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/wasm32-wasi.cmake -DHU_BUILD_WASM=ON ..

# --- Per-channel build options (CLI ON by default, others OFF) ---
option(HU_ENABLE_CLI "Build CLI channel" ON)
option(HU_ENABLE_TELEGRAM "Build Telegram channel" OFF)
option(HU_ENABLE_DISCORD "Build Discord channel" OFF)
option(HU_ENABLE_SLACK "Build Slack channel" OFF)
option(HU_ENABLE_WHATSAPP "Build WhatsApp channel" OFF)
option(HU_ENABLE_MATRIX "Build Matrix channel" OFF)
option(HU_ENABLE_IRC "Build IRC channel" OFF)
option(HU_ENABLE_LINE "Build LINE channel" OFF)
option(HU_ENABLE_LARK "Build Lark channel" OFF)
option(HU_ENABLE_WEB "Build Web channel" OFF)
option(HU_ENABLE_EMAIL "Build Email channel" OFF)
option(HU_ENABLE_IMAP "Build IMAP channel" OFF)
if(APPLE)
    option(HU_ENABLE_IMESSAGE "Build iMessage channel" ON)
else()
    option(HU_ENABLE_IMESSAGE "Build iMessage channel" OFF)
endif()
option(HU_IMESSAGE_TAPBACK_ENABLED "Enable iMessage tapback reactions via JXA (requires accessibility)" OFF)
if(APPLE)
    option(HU_ENABLE_PWA "Build PWA channel (browser tab polling)" ON)
else()
    option(HU_ENABLE_PWA "Build PWA channel (browser tab polling)" OFF)
endif()
option(HU_ENABLE_MATTERMOST "Build Mattermost channel" OFF)
option(HU_ENABLE_ONEBOT "Build OneBot channel" OFF)
option(HU_ENABLE_DINGTALK "Build DingTalk channel" OFF)
option(HU_ENABLE_SIGNAL "Build Signal channel" OFF)
option(HU_ENABLE_NOSTR "Build Nostr channel" OFF)
option(HU_ENABLE_QQ "Build QQ channel" OFF)
option(HU_ENABLE_MAIXCAM "Build MaixCam channel" OFF)
option(HU_ENABLE_DISPATCH "Build Dispatch channel" OFF)
option(HU_ENABLE_GMAIL "Build Gmail channel (read-only OAuth2)" OFF)
option(HU_ENABLE_TEAMS "Build Microsoft Teams channel" OFF)
option(HU_ENABLE_TWILIO "Build Twilio SMS channel" OFF)
option(HU_ENABLE_GOOGLE_CHAT "Build Google Chat channel" OFF)
option(HU_ENABLE_GOOGLE_RCS "Build Google RCS channel" OFF)
option(HU_ENABLE_FACEBOOK "Build Facebook Messenger channel" OFF)
option(HU_ENABLE_INSTAGRAM "Build Instagram DMs channel" OFF)
option(HU_ENABLE_TWITTER "Build Twitter/X DMs channel" OFF)
option(HU_ENABLE_TIKTOK "Build TikTok channel" OFF)
option(HU_ENABLE_VOICE "Build Voice channel (Sonata)" OFF)
option(HU_ENABLE_MQTT "Build MQTT channel" OFF)
option(HU_ENABLE_ALL_CHANNELS "Build all channels" OFF)

# --- Per-engine build options ---
option(HU_ENABLE_NONE_ENGINE "Build no-op memory engine" ON)
option(HU_ENABLE_MARKDOWN_ENGINE "Build markdown memory engine" ON)
option(HU_ENABLE_MEMORY_LRU_ENGINE "Build in-memory LRU memory engine" ON)
option(HU_ENABLE_API_ENGINE "Build HTTP API memory engine" OFF)
option(HU_ENABLE_LUCID_ENGINE "Build Lucid memory engine" ON)
option(HU_ENABLE_LANCEDB_ENGINE "Build LanceDB memory engine" ON)
option(HU_ENABLE_REDIS_ENGINE "Build Redis memory engine" OFF)
# HU_ENABLE_SQLITE and HU_ENABLE_POSTGRES defined above

# --- Sonata Voice Pipeline (optional) ─────────────────────────────
option(HU_ENABLE_SONATA "Enable native Sonata voice pipeline (Rust)" OFF)

if(HU_ENABLE_SONATA)
    set(SONATA_DIR "${CMAKE_SOURCE_DIR}/../pocket-voice" CACHE PATH "Path to pocket-voice repo")

    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/libsonata_pipeline.a
        COMMAND cargo build --release --manifest-path ${SONATA_DIR}/Cargo.toml
                -p sonata-pipeline
        COMMENT "Building Sonata voice pipeline (Rust)"
        VERBATIM
    )
    add_custom_target(sonata_build DEPENDS ${CMAKE_BINARY_DIR}/libsonata_pipeline.a)

    add_compile_definitions(HU_HAS_SONATA=1)
    message(STATUS "Sonata voice pipeline: ENABLED (${SONATA_DIR})")
else()
    message(STATUS "Sonata voice pipeline: disabled")
endif()

# Apply HU_ENABLE_ALL_CHANNELS: if ON, enable all channel options
if(HU_ENABLE_ALL_CHANNELS)
    set(HU_ENABLE_TELEGRAM ON)
    set(HU_ENABLE_DISCORD ON)
    set(HU_ENABLE_SLACK ON)
    set(HU_ENABLE_WHATSAPP ON)
    set(HU_ENABLE_MATRIX ON)
    set(HU_ENABLE_IRC ON)
    set(HU_ENABLE_LINE ON)
    set(HU_ENABLE_LARK ON)
    set(HU_ENABLE_WEB ON)
    set(HU_ENABLE_EMAIL ON)
    set(HU_ENABLE_IMAP ON)
    set(HU_ENABLE_IMESSAGE ON)
    set(HU_ENABLE_MATTERMOST ON)
    set(HU_ENABLE_ONEBOT ON)
    set(HU_ENABLE_DINGTALK ON)
    set(HU_ENABLE_SIGNAL ON)
    set(HU_ENABLE_NOSTR ON)
    set(HU_ENABLE_QQ ON)
    set(HU_ENABLE_MAIXCAM ON)
    set(HU_ENABLE_DISPATCH ON)
    set(HU_ENABLE_GMAIL ON)
    set(HU_ENABLE_TEAMS ON)
    set(HU_ENABLE_TWILIO ON)
    set(HU_ENABLE_GOOGLE_CHAT ON)
    set(HU_ENABLE_GOOGLE_RCS ON)
    set(HU_ENABLE_FACEBOOK ON)
    set(HU_ENABLE_INSTAGRAM ON)
    set(HU_ENABLE_TWITTER ON)
    set(HU_ENABLE_TIKTOK ON)
    set(HU_ENABLE_VOICE ON)
    set(HU_ENABLE_MQTT ON)
    set(HU_ENABLE_FEEDS ON)
endif()

# --- Channel sources (conditionally added) ---
set(CHANNEL_SOURCES)
if(HU_ENABLE_CLI)
    list(APPEND CHANNEL_SOURCES src/channels/cli.c)
endif()
if(HU_ENABLE_TELEGRAM)
    list(APPEND CHANNEL_SOURCES src/channels/telegram.c)
    # Phase 2 of docs/plans/2026-05-18-imessage-sota.md: message_reaction
    # dispatch lives in its own TU to avoid the hu_reaction_type_t (channel.h)
    # ↔ hu_reaction_kind_t (reaction_event.h) enumerator collision — same
    # pattern as slack_reactions.c and imessage_reactions.c.
    list(APPEND CHANNEL_SOURCES src/channels/telegram_reactions.c)
endif()
if(HU_ENABLE_DISCORD)
    list(APPEND CHANNEL_SOURCES src/channels/discord.c)
    # Phase 2: MESSAGE_REACTION_ADD/REMOVE dispatch. Same enum-collision
    # constraint as telegram_reactions / slack_reactions / imessage_reactions.
    list(APPEND CHANNEL_SOURCES src/channels/discord_reactions.c)
endif()
if(HU_ENABLE_SLACK)
    list(APPEND CHANNEL_SOURCES src/channels/slack.c)
    # Phase 2 Task 12 (RL SOTA): reaction_added/removed webhook branch.
    # Lives in its own TU to avoid an enum-name collision between
    # human/channels/reaction_event.h (Task 10) and human/channel.h
    # (HU_REACTION_QUESTION / HU_REACTION_CUSTOM_EMOJI redefinition).
    # Same pattern as Task 11's imessage_reactions.c.
    list(APPEND CHANNEL_SOURCES src/channels/slack_reactions.c)
endif()
if(HU_ENABLE_WHATSAPP)
    list(APPEND CHANNEL_SOURCES src/channels/whatsapp.c)
endif()
if(HU_ENABLE_WHATSAPP OR HU_ENABLE_FACEBOOK OR HU_ENABLE_INSTAGRAM)
    list(APPEND CHANNEL_SOURCES src/channels/meta_common.c)
endif()
if(HU_ENABLE_FACEBOOK)
    list(APPEND CHANNEL_SOURCES src/channels/facebook.c)
endif()
if(HU_ENABLE_INSTAGRAM)
    list(APPEND CHANNEL_SOURCES src/channels/instagram.c)
endif()
if(HU_ENABLE_TWITTER)
    list(APPEND CHANNEL_SOURCES src/channels/twitter.c)
endif()
if(HU_ENABLE_TIKTOK)
    list(APPEND CHANNEL_SOURCES src/channels/tiktok.c)
endif()
if(HU_ENABLE_MATRIX)
    list(APPEND CHANNEL_SOURCES src/channels/matrix.c)
endif()
if(HU_ENABLE_IRC)
    list(APPEND CHANNEL_SOURCES src/channels/irc.c)
endif()
if(HU_ENABLE_LINE)
    list(APPEND CHANNEL_SOURCES src/channels/line.c)
endif()
if(HU_ENABLE_LARK)
    list(APPEND CHANNEL_SOURCES src/channels/lark.c)
endif()
if(HU_ENABLE_WEB)
    list(APPEND CHANNEL_SOURCES src/channels/web.c)
endif()
if(HU_ENABLE_EMAIL)
    list(APPEND CHANNEL_SOURCES src/channels/email.c)
endif()
if(HU_ENABLE_IMAP)
    list(APPEND CHANNEL_SOURCES src/channels/imap.c)
endif()
if(HU_ENABLE_IMESSAGE)
    list(APPEND CHANNEL_SOURCES src/channels/imessage.c)
    # Phase 2 Task 11 (RL SOTA): tapback inbound poll. Lives in its own TU
    # to avoid an enum-name collision between human/channels/reaction_event.h
    # (Task 10) and human/channel.h (HU_REACTION_QUESTION /
    # HU_REACTION_CUSTOM_EMOJI redefinition). See the navigation comment
    # in src/channels/imessage.c near hu_imessage_poll for the full story.
    list(APPEND CHANNEL_SOURCES src/channels/imessage_reactions.c)
    # Phase 1a of docs/plans/2026-05-18-imessage-sota.md: pure synthesis
    # primitives that render iMessage events into canonical English for
    # personal-model ingestion. No chat.db / Apple deps; ingest wiring
    # lands in Phase 1b.
    list(APPEND CHANNEL_SOURCES src/channels/imessage_ingest.c)
    # Phase 6 of docs/plans/2026-05-18-imessage-sota.md: schema-version
    # probe + drift canary. Caches the chat.db PRAGMA table_info result;
    # logs unknown columns at startup so operators see Apple-added fields
    # before features silently break.
    list(APPEND CHANNEL_SOURCES src/channels/imessage_schema.c)
    # Phase 5 of docs/plans/2026-05-18-imessage-sota.md: per-balloon
    # payload decoders (URL preview, Apple Pay, Placemark, Music, Poll).
    # Privacy contracts pinned in tests: never reads amount/currency or
    # latitude/longitude keys.
    list(APPEND CHANNEL_SOURCES src/channels/imessage_balloon_decode.c)
endif()
if(HU_ENABLE_PWA)
    list(APPEND CHANNEL_SOURCES src/channels/pwa.c)
    set(HU_HAS_PWA_FLAG TRUE)
endif()
if(HU_ENABLE_MATTERMOST)
    list(APPEND CHANNEL_SOURCES src/channels/mattermost.c)
endif()
if(HU_ENABLE_ONEBOT)
    list(APPEND CHANNEL_SOURCES src/channels/onebot.c)
endif()
if(HU_ENABLE_DINGTALK)
    list(APPEND CHANNEL_SOURCES src/channels/dingtalk.c)
endif()
if(HU_ENABLE_TEAMS)
    list(APPEND CHANNEL_SOURCES src/channels/teams.c)
endif()
if(HU_ENABLE_TWILIO)
    list(APPEND CHANNEL_SOURCES src/channels/twilio.c)
endif()
if(HU_ENABLE_GOOGLE_CHAT)
    list(APPEND CHANNEL_SOURCES src/channels/google_chat.c)
endif()
if(HU_ENABLE_GOOGLE_RCS)
    list(APPEND CHANNEL_SOURCES src/channels/google_rcs.c)
endif()
if(HU_ENABLE_GMAIL)
    list(APPEND CHANNEL_SOURCES src/channels/gmail_base64.c src/channels/gmail.c)
endif()
if(HU_ENABLE_VOICE)
    list(APPEND CHANNEL_SOURCES src/channels/voice_channel.c)
endif()
if(HU_ENABLE_MQTT)
    list(APPEND CHANNEL_SOURCES src/channels/mqtt.c)
endif()
# signal, nostr, qq, maixcam, dispatch: only when all-channels build (saves ~60KB in default)
if(HU_ENABLE_ALL_CHANNELS)
    list(APPEND CHANNEL_SOURCES
        src/channels/signal.c
        src/channels/nostr.c
        src/channels/qq.c
        src/channels/maixcam.c
        src/channels/dispatch.c
    )
    if(NOT HU_ENABLE_GMAIL)
        list(APPEND CHANNEL_SOURCES src/channels/gmail_base64.c src/channels/gmail.c)
    endif()
endif()

# --- Memory engine sources (conditionally added) ---
# none.c and markdown.c are always compiled — migration depends on them.
# The HU_HAS_*_ENGINE flags only control registry visibility.
set(MEMORY_ENGINE_SOURCES
    src/memory/engines/none.c
    src/memory/engines/markdown.c
)
if(HU_ENABLE_MEMORY_LRU_ENGINE)
    list(APPEND MEMORY_ENGINE_SOURCES src/memory/engines/memory_lru.c)
endif()
list(APPEND MEMORY_ENGINE_SOURCES src/memory/engines/sqlite.c)
# postgres, redis, api: always built (mock in HU_IS_TEST; real when HU_ENABLE_*)
list(APPEND MEMORY_ENGINE_SOURCES
    src/memory/engines/postgres.c
    src/memory/engines/redis.c
    src/memory/engines/api.c
)
if(HU_ENABLE_LUCID_ENGINE)
    list(APPEND MEMORY_ENGINE_SOURCES src/memory/engines/sqlite_lucid.c)
endif()
if(HU_ENABLE_LANCEDB_ENGINE)
    list(APPEND MEMORY_ENGINE_SOURCES src/memory/engines/sqlite_fts.c)
endif()

# Threads for pthread (dispatcher parallel execution)
find_package(Threads QUIET)

# Core library sources (Phase 4-6, 11: agent, providers, channels, tools, memory, tunnel, gateway, runtime)
# Channels and memory engines are conditionally added above
set(HU_CORE_SOURCES
    src/platform.c
    src/platform/calendar_macos.c
    src/terminal.c
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
    src/core/accel.c
    src/core/allocator.c
    src/core/error.c
    src/core/arena.c
    src/core/string.c
    src/core/io_secure.c
    src/core/json.c
    # Phase 3 of docs/plans/2026-05-18-imessage-sota.md: pure-C bplist00
    # parser. Used by imessage_ingest.c extractors for audio transcripts
    # and edit-chain history. No external deps.
    src/util/bplist.c
    # Phase 3 completion: daemon tick that polls chat.db for the
    # enriched-event columns (payload_data, message_summary_info,
    # group_action_type, balloon_bundle_id) and ingests into personal_model.
    # Test mode + non-Apple builds get a stub implementation.
    src/daemon_imessage_observer.c
    # Phase 4 of docs/plans/2026-05-18-imessage-sota.md: typedstream
    # attribute-run extractor — pulls mentions, OTP/2FA detection, link
    # spans, iOS 18 text effects, and formatting runs out of
    # attributedBody blobs. Clean-room implementation; no deps.
    src/util/typedstream.c
    src/core/http.c
    src/core/vertex_auth.c
    src/core/process_util.c
    src/sse/sse_client.c
    src/websocket/websocket.c
    src/net_security.c
    src/config.c
    src/config_schema.c
    src/config_merge.c
    src/config_migrate.c
    src/config_serialize.c
    src/config_getters.c
    src/config_validate.c
    src/config_types.c
    src/config_mutator.c
    src/config_parse.c
    src/config_parse_channels.c
    src/config_parse_providers.c
    src/config_parse_agent.c
    src/config_parse_behavior.c
    src/capabilities.c
    src/channel_catalog.c
    src/channel_adapters.c
    src/doctor.c
    src/doctor_fix.c
    src/humanness.c
    src/channels/channel_class.c
    src/channels/channel_monitor.c
    src/memory/evolved_opinions.c
    src/status.c
    src/http_util.c
    src/mcp.c
    src/mcp_jsonrpc.c
    src/mcp_transport_stdio.c
    src/mcp_transport_sse.c
    src/mcp_transport_http.c
    src/multimodal.c
    src/multimodal/calendar.c
    src/multimodal/document.c
    src/multimodal/image.c
    src/multimodal/audio.c
    src/multimodal/video.c
    src/multimodal/multimodal.c
    src/rag.c
    src/service.c
    src/skill_registry.c
    src/subagent.c
    src/voice.c
    src/voice_config.c
    src/voice/local_stt.c
    src/voice/local_tts.c
    src/portable_atomic.c
    src/agent_routing.c
    src/health.c
    src/security/security.c
    src/security/sycophancy_guard.c
    src/security/mcp_audit.c
    src/security/policy.c
    src/security/causal_armor.c
    src/security/history_scorer.c
    src/security/arg_inspector.c
    src/security/pairing.c
    src/security/secrets.c
    src/security/vault.c
    src/security/vault_aead.c
    src/security/audit.c
    src/security/delegation.c
    src/security/noop_sandbox.c
    src/security/docker.c
    src/security/detect.c
    src/security/exec_env.c
    src/security/adversarial.c
    src/security/sensitivity.c
    src/security/sandbox.c
    src/security/skill_trust.c
    src/security/keystore.c
    src/security/audit_log.c
    src/observability.c
    src/observability/log_observer.c
    src/observability/metrics_observer.c
    src/observability/multi_observer.c
    src/observability/bth_metrics.c
    src/observability/otlp.c
    src/observability/background_registry.c
    src/observability/validator_telemetry.c
    src/tunnel/none.c
    src/tunnel/root.c
    src/gateway/gateway.c
    src/gateway/thread_pool.c
    src/gateway/openai_compat.c
    src/gateway/oauth.c
    src/gateway/rate_limit.c
    src/gateway/ws_server.c
    src/gateway/control_protocol.c
    src/gateway/cp_chat.c
    src/gateway/cp_config.c
    src/gateway/cp_admin.c
    src/gateway/cp_memory.c
    src/gateway/cp_hula.c
    src/gateway/cp_voice.c
    src/gateway/cp_voice_clone.c
    src/gateway/cp_voice_stream.c
    src/gateway/cp_turing.c
    src/gateway/cp_mcp.c
    src/gateway/cp_security.c
    src/gateway/cp_canvas.c
    src/gateway/cp_tasks.c
    src/gateway/mcp_context.c
    src/agent/prompt_cache.c
    src/agent/agent_comm.c
    src/agent/acp_bridge.c
    src/agent/kv_cache.c
    src/tools/cache_ttl.c
    src/tools/vision_ocr.c
    src/voice/emotion_voice_map.c
    src/voice/audio_emotion.c
    src/persona/persona_fuse.c
    src/persona/humor.c
    src/persona/markdown_loader.c
    src/persona/somatic.c
    src/persona/narrative_self.c
    src/gateway/event_bridge.c
    src/gateway/tenant.c
    src/runtime/native.c
    src/runtime/docker.c
    src/runtime/gce.c
    src/runtime/factory.c
    src/hook.c
    src/hook_pipeline.c
    src/agent/action_preview.c
    src/agent/agent.c
    src/agent/agent_plan.c
    src/agent/agent_stream.c
    src/agent/agent_turn.c
    src/agent/best_of_n.c
    src/agent/humanness.c
    src/agent/context.c
    src/agent/prompt.c
    src/agent/memory_loader.c
    src/agent/channel_trust.c
    src/agent/agent_definition.c
    src/agent/agent_git.c
    src/agent/dispatcher.c
    src/agent/compaction.c
    src/agent/spawn.c
    src/agent/worktree.c
    src/agent/team.c
    src/agent/mailbox.c
    src/agent/task_list.c
    src/agent/task_store.c
    src/agent/profile.c
    src/agent/planner.c
    src/agent/plan_executor.c
    src/agent/mcts_planner.c
    src/agent/dag.c
    src/agent/dag_executor.c
    src/agent/hula.c
    src/agent/idempotency.c
    src/agent/hula_compiler.c
    src/agent/hula_emergence.c
    src/agent/hula_compiler_examples.c
    src/agent/hula_lite.c
    src/agent/hula_analytics.c
    src/agent/llm_compiler.c
    src/agent/orchestrator.c
    src/agent/orchestrator_llm.c
    src/agent/swarm.c
    src/agent/agent_profile.c
    src/agent/model_router.c
    src/agent/registry.c
    src/agent/undo.c
    src/agent/cli.c
    src/agent/tui.c
    src/agent/commands.c
    src/agent/context_tokens.c
    src/agent/choreography.c
    src/agent/max_tokens.c
    src/agent/preferences.c
    src/agent/awareness.c
    src/agent/outcomes.c
    src/agent/output_validator.c
    src/agent/output_validator_chain.c
    src/agent/stop_sequence_registry.c
    src/agent/validators/response_guard_validator.c
    src/agent/validators/channel_tags_validator.c
    src/agent/validators/ai_phrases_validator.c
    src/agent/validators/formal_structure_validator.c
    src/agent/validators/cot_audit_validator.c
    src/agent/validators/persona_narrator_validator.c
    src/agent/validators/assistant_closer_validator.c
    src/agent/validators/role_consistency_validator.c
    src/agent/validators/persona_fidelity_validator.c
    src/agent/validators/persona_voice_validator.c
    src/agent/validators/identity_short_circuit_validator.c
    src/agent/validators/default_chains.c
    src/agent/growth_narrative.c
    src/agent/frontier_persist.c
    src/context/conversation.c
    src/context/contact_send_recency.c
    src/context/filler_recency.c
    src/context/contact_send_recency.c
    src/context/contact_style_overlay.c
    src/context/style_tracker.c
    src/context/theory_of_mind.c
    src/context/mood.c
    src/context/emotional_state.c
    src/context/event_extract.c
    src/context/temporal_events.c
    src/context/vision.c
    src/context/intelligence.c
    src/intelligence/skill_system.c
    src/intelligence/trust.c
    src/feeds/processor.c
    src/feeds/awareness.c
    src/context/behavioral.c
    src/behavior/dialog_act.c
    src/behavior/affect.c
    src/behavior/change.c
    src/behavior/safety.c
    src/behavior/policy.c
    src/behavior/prompt.c
    src/behavior/support_strategy.c
    src/behavior/pressure.c
    src/behavior/behavior_trust.c
    src/behavior/trust_prompt.c
    src/behavior/user_sim.c
    src/behavior/pressure_history.c
    src/behavior/user_sim_scenario.c
    src/context/repair.c
    src/calibration/timing_analyzer.c
    src/calibration/style_analyzer.c
    src/calibration/calibrate.c
    src/calibration/clone.c
    src/calibration/ab_compare.c
    src/context/context_ext.c
    src/agent/episodic.c
    src/agent/reflection.c
    src/agent/gvr.c
    src/agent/degradation.c
    src/agent/token_budget.c
    src/agent/data_quality.c
    src/agent/mar.c
    src/agent/prompt_optimizer.c
    src/agent/checkpoint.c
    src/agent/approval_gate.c
    src/agent/workflow_event.c
    src/agent/chaos.c
    src/agent/scratchpad.c
    src/agent/tree_of_thought.c
    src/agent/constitutional.c
    src/agent/ab_response.c
    src/agent/input_guard.c
    src/agent/commitment.c
    src/agent/commitment_store.c
    src/agent/pattern_radar.c
    src/agent/proactive.c
    src/agent/proactive_ext.c
    src/agent/proactive_throttle.c
    src/agent/inner_thoughts.c
    src/agent/weather_awareness.c
    src/agent/weather_fetch.c
    src/agent/workspace_context.c
    src/agent/timing.c
    src/agent/governor.c
    src/agent/activation_steering.c
    src/agent/relationship_dynamics.c
    src/agent/arbitrator.c
    src/agent/planning.c
    src/agent/collab_planning.c
    src/agent/rel_dynamics.c
    src/context/rel_dynamics.c
    src/agent/conv_goals.c
    src/agent/superhuman.c
    src/agent/superhuman_commitment.c
    src/agent/superhuman_predictive.c
    src/agent/superhuman_emotional.c
    src/agent/superhuman_silence.c
    src/agent/anticipatory.c
    src/agent/conversation_plan.c
    src/agent/info_asymmetry.c
    src/agent/theory_of_mind.c
    src/agent/tool_call_parser.c
    src/agent/tool_router.c
    src/agent/speculative.c
    src/agent/uncertainty.c
    src/agent/autonomy.c
    src/permission.c
    src/mcp_manager.c
    src/mcp_tool_wrapper.c
    src/agent/compaction_structured.c
    src/agent/session_persist.c
    src/agent/instruction_discover.c
    src/agent/self_improve.c
    src/providers/error_classify.c
    src/providers/api_key.c
    src/providers/helpers.c
    src/providers/provider_http.c
    src/providers/sse.c
    src/providers/scrub.c
    src/providers/reliable.c
    src/providers/from_config.c
    src/providers/router.c
    src/providers/ensemble.c
    src/providers/codex_cli.c
    src/providers/openai_codex.c
    src/providers/runtime_bundle.c
    src/providers/openai.c
    src/providers/anthropic.c
    src/providers/gemini.c
    src/providers/structured_output.c
    src/providers/ollama.c
    src/providers/openrouter.c
    src/providers/compatible.c
    src/providers/claude_cli.c
    src/providers/factory.c
    ${CHANNEL_SOURCES}
    ${MEMORY_ENGINE_SOURCES}
    src/memory/engines/registry.c
    src/memory/factory.c
    src/memory/encrypted_store.c
    src/memory/util.c
    src/memory/contact_memory.c
    src/memory/stm.c
    src/memory/emotional_graph.c
    src/memory/comfort_patterns.c
    src/memory/emotional_moments.c
    src/memory/superhuman.c
    src/memory/contact_graph.c
    src/memory/fast_capture.c
    src/memory/promotion.c
    src/memory/consolidation.c
    src/memory/verify_claim.c
    src/memory/graph_index.c
    src/memory/policy.c
    src/memory/deep_extract.c
    src/memory/fact_extract.c
    src/memory/personal_model.c
    src/memory/personal_model_contradicts.c
    src/memory/minja_guard.c
    src/memory/hallucination_guard.c
    # W7: graph.h is the internal graph API. Direct includes from outside
    # src/memory/ and tests/ are deprecated — callers should migrate to
    # hu_memory_read / hu_memory_write via the facade (include/human/memory/memory.h).
    # Known remaining callers: src/agent/autodream.c (1 site),
    # src/daemon.c (9 sites). See docs/plans/2026-05-10-memory-v2-roadmap-overview.md.
    src/memory/graph.c
    src/memory/conflict_resolver.c
    src/memory/write_trust.c
    src/memory/cross_graph.c
    src/memory/erasure.c
    src/memory/neural_memory.c
    src/agent/self_rag.c
    src/agent/self_rag_atomic.c
    src/agent/self_rag_inline.c
    src/memory/memory.c
    src/memory/memory_v1_backend.c
    src/memory/encrypted_store.c
    src/memory/belief.c
    src/memory/hyperedge.c
    src/memory/pagerank.c
    src/agent/world_model.c
    src/agent/tom_scenario.c
    src/agent/retrieval_planner.c
    src/agent/retrieval_planner_llm.c
    src/agent/autodream.c
    src/agent/scheduler.c
    src/agent/scheduler_probes.c
    src/agent/scheduler_status_json.c
    src/agent/counterfactual.c
    src/agent/autodream_runner.c
    src/agent/case_based.c
    src/agent/response_verifier.c
    src/agent/response_guard.c
    src/agent/response_guard_retry.c
    src/agent/verifier_metrics.c
    src/agent/world_model_bridge.c
    # US-7.5: nightly LoRA retrain runner (MLX-Gemma subprocess path).
    # Always compiled (no learner dependency) — sibling to lora_training_runner.c.
    src/ml/lora_retrain_runner.c
    # PR #115 / integration-tests fix: US-11.8 helpers MUST link with
    # lora_retrain_runner.c regardless of HU_ENABLE_ML. The runner calls
    # `hu_lora_compute_kl_drift` and `hu_lora_ema_apply` from these
    # translation units; gating them behind HU_ENABLE_ML produced
    # undefined-symbol link errors in the HU_ENABLE_ML=OFF integration-
    # tests target (which still compiles lora_retrain_runner.c). Same
    # rationale as the runner: subprocess + file I/O, no learner dep.
    src/ml/lora_ema.c
    src/ml/cli_adapter_rollback.c
    # NOTE: src/agent/lora_training_runner.c and training_data_runner.c
    # are W13 learning-loop callers — they reference symbols defined only
    # when HU_ENABLE_LEARNING is on (hu_learner_pending_drain,
    # hu_learner_train, hu_training_data_extract*). Their sources are
    # appended below under the HU_ENABLE_LEARNING block. Their public
    # registration shims in world_model_bridge.c return
    # HU_ERR_NOT_SUPPORTED when learning is disabled, so the daemon's
    # wiring is a clean no-op rather than a link error.
    src/agent/kv_prewarm_runner.c
    src/agent/belief_reverify_runner.c
    src/persona/persona_deltas.c
    src/persona/delta_observer.c
    src/memory/multimodal_index.c
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
    src/memory/retrieval/entropy_gate.c
    src/memory/rerank.c
    src/memory/retrieval/qmd.c
    src/memory/retrieval/query_expansion.c
    src/memory/retrieval/llm_reranker.c
    src/memory/retrieval/strategy_learner.c
    src/memory/retrieval/engine.c
    src/memory/retrieval/multigraph.c
    src/memory/connections.c
    src/memory/knowledge.c
    src/memory/cognitive.c
    src/memory/deep_memory.c
    src/memory/rag_pipeline.c
    src/memory/compression.c
    src/visual/content.c
    src/memory/ingest.c
    src/memory/inbox.c
    src/memory/relational_episode.c
    src/memory/vector/circuit_breaker.c
    src/cognition/db.c
    src/cognition/cognition_trust.c
    src/cognition/emotional.c
    src/cognition/evolving.c
    src/cognition/metacognition.c
    src/cognition/skill_routing.c
    src/cognition/dual_process.c
    src/cognition/episodic.c
    src/cognition/attachment.c
    src/cognition/rupture_repair.c
    src/cognition/presence.c
    src/cognition/novelty.c
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
    src/tools/memory_store.c
    src/tools/save_for_later.c
    src/tools/memory_recall.c
    src/tools/memory_list.c
    src/tools/memory_forget.c
    src/tools/bff_memory.c
    src/tools/doc_ingest.c
    src/task_manager.c
    src/tools/ask_user.c
    src/tools/task_tools.c
    src/tools/webhook_tools.c
    src/tools/meeting_transcribe.c
    src/tools/message.c
    src/tools/delegate.c
    src/tools/spawn.c
    src/tools/schema.c
    src/tools/schema_clean.c
    src/tools/pushover.c
    src/tools/image.c
    src/tools/agent_spawn.c
    src/tools/agent_query.c
    src/tools/apply_patch.c
    src/tools/pdf.c
    src/tools/spreadsheet.c
    src/tools/report.c
    src/tools/broadcast.c
    src/tools/send_message.c
    src/tools/send_voice_message.c
    src/tools/calendar.c
    src/tools/skill_write.c
    src/tools/pwa.c
    src/pwa/bridge.c
    src/pwa/cdp.c
    src/pwa/context.c
    src/pwa/drivers.c
    src/pwa/learner.c
    src/tools/jira.c
    src/tools/social.c
    src/tools/facebook.c
    src/tools/instagram.c
    src/tools/twitter.c
    src/tools/cli_wrapper_common.c
    src/tools/gcloud.c
    src/tools/firebase.c
    src/tools/crm.c
    src/tools/analytics.c
    src/tools/invoice.c
    src/tools/workflow.c
    src/tools/homeassistant.c
    src/tools/diff.c
    src/tools/voice_clone.c
    src/tools/media_image.c
    src/tools/media_video.c
    src/tools/media_gif.c
    src/tools/factory.c
    src/tools/canvas_render.c
    src/tools/canvas_store.c
    src/tools/canvas_persist.c
    src/tools/declarative.c
    src/tools/mcp_resource_tools.c
    src/tools/cron_session_tools.c
    src/mcp_server.c
    src/channels/thread_binding.c
    src/channels/format.c
    src/channels/rate_limit.c
    src/channels/channel_embed.c
    src/channels/channel_http.c
    src/channels/webhook.c
    src/security/policy_engine.c
    src/security/replay.c
    src/plugins/registry.c
    src/plugin_loader.c
    src/plugin_discovery.c
    src/skills/skill_scaffold.c
    src/context/context_engine.c
    src/context/context_engine_rag.c
    src/mcp_registry.c
    src/onboard.c
    src/interactions/choices.c
    src/daemon.c
    src/daemon_lifecycle.c
    src/daemon_cron.c
    src/daemon_routing.c
    src/daemon_proactive.c
    src/cron.c
    src/crontab.c
    src/webhook.c
    src/migration.c
    src/version.c
    src/cli_commands.c
    src/cli_evaluation.c
    src/cost.c
    src/usage.c
    src/oauth.c
    src/memory/vector/math.c
    src/memory/vector/cosine.c
    src/memory/vector/chunker.c
    src/memory/vector/store_mem.c
    src/memory/vector/embedder_local.c
    src/memory/vector/embedder_gemini_adapter.c
    src/memory/vector/embeddings.c
    src/memory/vector/embeddings_gemini.c
    src/memory/vector/embeddings_ollama.c
    src/memory/vector/embeddings_voyage.c
    src/memory/vector/provider_router.c
    src/memory/vector/outbox.c
    src/memory/vector/store.c
    src/memory/vector/store_pgvector.c
    src/memory/vector/store_qdrant.c
    src/memory/vector/vector_retrieval_remote.c
    src/memory/lifecycle/semantic_cache.c
)

# --- Conditional module groups (feature-gated for binary size) ---

# Peripherals: Arduino, STM32, RPi + hardware tools
if(HU_ENABLE_PERIPHERALS)
    list(APPEND HU_CORE_SOURCES
        src/peripherals/arduino.c
        src/peripherals/stm32.c
        src/peripherals/rpi.c
        src/peripherals/factory.c
        src/hardware.c
        src/tools/hardware_info.c
        src/tools/hardware_memory.c
        src/tools/i2c.c
        src/tools/peripheral_ctrl.c
        src/tools/spi.c
    )
endif()

# Tunnels: ngrok, cloudflare, tailscale, custom
if(HU_ENABLE_TUNNELS)
    list(APPEND HU_CORE_SOURCES
        src/tunnel/cloudflare.c
        src/tunnel/ngrok.c
        src/tunnel/tailscale.c
        src/tunnel/custom.c
    )
endif()

# Exotic runtimes: WASM, Cloudflare Workers (stubs — only needed if configuring these runtimes)
if(HU_ENABLE_RUNTIME_EXOTIC)
    list(APPEND HU_CORE_SOURCES
        src/runtime/wasm_rt.c
        src/runtime/cloudflare.c
    )
endif()

# Platform-specific security sandboxes (auto-detected, no manual option needed)
if(APPLE)
    list(APPEND HU_CORE_SOURCES src/security/seatbelt.c)
endif()
if(UNIX AND NOT APPLE)
    list(APPEND HU_CORE_SOURCES
        src/security/landlock.c
        src/security/firejail.c
        src/security/bubblewrap.c
        src/security/seccomp.c
        src/security/landlock_seccomp.c
        src/security/firecracker.c
    )
endif()
if(WIN32)
    list(APPEND HU_CORE_SOURCES src/security/appcontainer.c)
endif()
# WASI sandbox is cross-platform
list(APPEND HU_CORE_SOURCES src/security/wasi_sandbox.c)

# Channel-agnostic reaction event normalizer (Phase 2 Task 10).
list(APPEND HU_CORE_SOURCES src/channels/reaction_event.c)

# Phase 2 Task 13 (RL SOTA): reaction event → dpo_pairs row handler.
# Writes into the daemon-owned hu_dpo_collector_t set via
# hu_reaction_handler_set_collector at startup. Same TU split rationale
# as Tasks 11/12: lives in src/agent/ rather than src/channels/ so it
# can include human/channels/reaction_event.h without colliding with
# human/channel.h's older hu_reaction_type_t enum.
list(APPEND HU_CORE_SOURCES src/agent/reaction_handler.c)
# CF-3: daemon reaction poll tick — always linked (daemon.c calls it
# unconditionally); must not sit behind HU_ENABLE_RL_FULL.
list(APPEND HU_CORE_SOURCES src/daemon_reaction_poll.c)

# SOTA capabilities
list(APPEND HU_CORE_SOURCES
    src/a2a.c
    src/eval.c
    src/eval/shape.c
    src/eval/consistency.c
    src/eval/persona_fidelity.c
    src/memory/fact_extract_llm.c
    src/evaluation/evaluation.c
    src/evaluation/evaluation_dataset_loader.c
    src/evaluation/evaluation_locomo.c
    src/evaluation/evaluation_longmemeval.c
    src/evaluation/evaluation_dmr.c
    src/evaluation/evaluation_minja.c
    src/evaluation/evaluation_memoryagentbench.c
    src/evaluation/evaluation_frontier_compare.c
    src/evaluation/evaluation_legacy_bridge.c
    src/evaluation/evaluation_facade_recall.c
    src/evaluation/evaluation_locomo_facade.c
    src/evaluation/evaluation_baseline.c
    src/evaluation/evaluation_regression.c
    src/eval/turing_score.c
    src/eval/turing_adversarial.c
    src/eval/longmemeval.c
    src/eval_judge.c
    src/eval_benchmarks.c
    src/mcp_resources.c
    src/eval_dashboard.c
    src/memory/corrective_rag.c
    src/memory/adaptive_rag.c
    src/memory/self_rag.c
    src/memory/tiers.c
    src/agent/process_reward.c
    src/ml/dpo.c
    # dpo_miner moved to if(HU_ENABLE_ML) block — references hu_pii_redact
    # (training_data_quality.c) which is itself ML-gated. Keeping it in
    # HU_CORE_SOURCES broke audit-integrity / Verify Audit Log Integrity /
    # static-analysis builds with an undefined-reference linker error.
    src/security/cot_audit.c
    src/security/escalate.c
    src/security/normalize.c
    src/security/moderation.c
    src/security/companion_safety.c
    src/tools/code_sandbox.c
    src/tools/computer_use.c
    src/tools/image_gen.c
    src/tools/visual_grounding.c
    src/tools/browser_use.c
    src/tools/gui_agent.c
    src/tools/lsp.c
    src/tools/tool_search.c
    src/voice/webrtc.c
    src/voice/webrtc_ice.c
    src/voice/webrtc_dtls.c
    src/voice/webrtc_srtp.c
    src/voice/realtime.c
    src/voice/gemini_live.c
    src/voice/provider_factory.c
    src/voice/mlx_local.c
    src/voice/duplex.c
    src/voice/session.c
    src/voice/opus.c
    src/voice/turn_signal.c
    src/voice/semantic_eot.c
    src/providers/embedded.c
    src/providers/llamacpp.c
    src/providers/mlx.c
    src/providers/llamacpp_sampling.c
    src/providers/llamacpp_kvcache.c
    src/providers/llamacpp_decode.c
)

if(HU_ENABLE_COREML)
    list(APPEND HU_CORE_SOURCES src/providers/coreml.c)
endif()

if(HU_ENABLE_APPLE_INTELLIGENCE)
    list(APPEND HU_CORE_SOURCES src/providers/apple.c)
endif()

# Persona system — always-on since v2026.4.0 (agent core references persona
# symbols unconditionally).  ~138 KB added to release binary.
list(APPEND HU_CORE_SOURCES
    src/persona/persona.c
    src/persona/render.c
    src/persona/examples.c
    src/persona/sampler.c
    src/persona/analyzer.c
    src/persona/creator.c
    src/persona/cli.c
    src/persona/feedback.c
    src/persona/eval.c
    src/persona/replay.c
    src/persona/circadian.c
    src/follow_up.c
    src/persona/temporal.c
    src/persona/relationship.c
    src/persona/auto_profile.c
    src/persona/auto_tune.c
    src/persona/voice_maturity.c
    src/persona/style_critique.c
    src/persona/style_mirror.c
    src/persona/style_clone.c
    src/persona/training.c
    src/persona/life_sim.c
    src/persona/mood.c
    src/persona/style_learner.c
    src/persona/micro_expression.c
    src/persona/creative_voice.c
    src/persona/genuine_boundaries.c
    src/tools/persona.c
    # Moment Context Decision Layer (Task 0.2) — unconditional per the
    # plan's spec; gate-symmetry rule requires source+test be registered
    # together with no HU_ENABLE_* wrap.
    src/persona/moment.c
    src/persona/moment_render.c
    # US-8.2 — Persona Encryption-at-Rest. Hard-requires libsodium via
    # the TU's `#error if !defined(HU_HAS_LIBSODIUM)`. The original
    # registration was unconditional with a "fail loudly" justification,
    # but in practice this broke every CI variant that didn't pass
    # `-DHU_ENABLE_LIBSODIUM=ON` (default OFF) — including all 12
    # inline-apt CI jobs and integration-tests / minimal-build /
    # cross-arm64 / feature-flags. Even installing libsodium-dev
    # apt-side isn't sufficient because find_package(sodium) only runs
    # when HU_ENABLE_LIBSODIUM=ON.
    #
    # The honest fix: only compile persona_crypt.c when libsodium is
    # actually present. Callers (src/persona/persona_storage.c) already
    # guard `hu_persona_crypt_*` calls with `#ifdef HU_HAS_LIBSODIUM`
    # so no API break. When libsodium IS enabled and found, the layer
    # is built; otherwise persona storage falls back to the PBKDF2 +
    # ChaCha20+HMAC path documented at CMakeLists.txt:2102.
    src/context/theory_of_mind.c
    src/context/self_awareness.c
    src/context/anticipatory.c
    src/context/protective.c
    src/context/humor.c
    src/context/social_graph.c
    src/memory/degradation.c
    src/memory/opinions.c
    src/memory/life_chapters.c
)

# Authentic existence (Phase 9: cognitive load, authentic)
if(HU_ENABLE_AUTHENTIC)
    list(APPEND HU_CORE_SOURCES
        src/context/cognitive_load.c
        src/context/authentic.c
    )
endif()
# authentic.c needed by persona (add if not already from AUTHENTIC)
if(NOT HU_ENABLE_AUTHENTIC)
    list(APPEND HU_CORE_SOURCES src/context/authentic.c)
endif()

# Browser automation tools
if(HU_ENABLE_TOOLS_BROWSER)
    list(APPEND HU_CORE_SOURCES
        src/tools/browser.c
        src/tools/browser_open.c
        src/tools/screenshot.c
    )
endif()

# Advanced tools: composio, claude_code, canvas, notebook, database
if(HU_ENABLE_TOOLS_ADVANCED)
    list(APPEND HU_CORE_SOURCES
        src/tools/composio.c
        src/tools/claude_code.c
        src/tools/canvas.c
        src/tools/notebook.c
        src/tools/database.c
    )
endif()

# Database introspection tool (SQLite-based, always available)
list(APPEND HU_CORE_SOURCES
    src/tools/db_introspect.c
)

# Music preview integration (iTunes Search API + 30s preview download)
list(APPEND HU_CORE_SOURCES src/music.c)

# Paperclip integration (requires libcurl for API calls)
if(HU_ENABLE_CURL)
    list(APPEND HU_CORE_SOURCES
        src/paperclip/client.c
        src/paperclip/heartbeat.c
        src/tools/paperclip.c
    )
endif()

# cartesia.c is always compiled (stubs return HU_ERR_NOT_SUPPORTED when disabled)
list(APPEND HU_CORE_SOURCES src/tts/cartesia.c)
list(APPEND HU_CORE_SOURCES src/tts/cartesia_stream.c)
list(APPEND HU_CORE_SOURCES src/tts/voice_clone.c)
list(APPEND HU_CORE_SOURCES src/tts/transcript_prep.c)

# Cartesia full pipeline (requires libcurl for API calls)
if(HU_ENABLE_CARTESIA)
    if(NOT HU_ENABLE_CURL)
        message(FATAL_ERROR "HU_ENABLE_CARTESIA requires HU_ENABLE_CURL=ON")
    endif()
    list(APPEND HU_CORE_SOURCES
        src/tts/audio_pipeline.c
        src/context/voice_decision.c
        src/tts/emotion_map.c
    )
    message(STATUS "Cartesia TTS: enabled")
else()
    message(STATUS "Cartesia TTS: disabled")
endif()

# OpenTelemetry observability exporter
if(HU_ENABLE_OTEL)
    list(APPEND HU_CORE_SOURCES
        src/observability/otel.c
    )
endif()

# Phase 7 core memory (episodic, consolidation engine, forgetting, prospective, emotional residue)
# Phase 7 feeds: OAuth token storage (oauth_tokens table)
if(HU_ENABLE_SQLITE)
    list(APPEND HU_CORE_SOURCES
        src/memory/sql_transaction.c
        src/memory/episodic.c
        src/memory/consolidation_engine.c
        src/memory/forgetting.c
        src/memory/forgetting_curve.c
        src/memory/prospective.c
        src/memory/emotional_residue.c
        src/memory/graph/memory_graph.c
        src/feeds/oauth.c
        src/intelligence/skills.c
        src/intelligence/self_improve.c
        src/intelligence/value_learning.c
        src/intelligence/online_learning.c
        src/agent/goals.c
        src/intelligence/world_model.c
        src/intelligence/experience.c
        src/intelligence/weakness.c
        src/intelligence/weakness_analyzer.c
        src/intelligence/distiller.c
    )
endif()

# Phase 7 feed ingestion (social F83/F84, Apple F85/F87/F88)
if(HU_ENABLE_SOCIAL)
    list(APPEND HU_CORE_SOURCES src/feeds/social.c)
endif()
if(HU_ENABLE_FEEDS)
    list(APPEND HU_CORE_SOURCES src/feeds/apple.c src/feeds/google.c src/feeds/music.c
        src/feeds/news.c src/feeds/email.c src/feeds/gmail.c src/feeds/imessage.c
        src/feeds/twitter.c src/feeds/file_ingest.c src/feeds/research.c src/feeds/findings.c
        src/feeds/research_executor.c src/feeds/trends.c)
endif()

# Skillforge + Phase 8 intelligence (skills, reflection, feedback, meta-learning, cycle)
if(HU_ENABLE_SKILLS)
    list(APPEND HU_CORE_SOURCES
        src/skillforge.c
        src/skills.c
        src/skills/skill_scaffold.c
        src/tools/skill_run.c
        src/intelligence/reflection.c
        src/intelligence/feedback.c
        src/intelligence/meta_learning.c
        src/intelligence/cycle.c
    )
    if(NOT HU_ENABLE_SQLITE)
        list(APPEND HU_CORE_SOURCES src/intelligence/skills.c)
    endif()
endif()

# Cron scheduler (cron.c and crontab.c are in HU_CORE_SOURCES unconditionally)
if(HU_ENABLE_CRON)
    list(APPEND HU_CORE_SOURCES
        src/tools/cron_add.c src/tools/cron_list.c src/tools/cron_remove.c
        src/tools/cron_run.c src/tools/cron_runs.c src/tools/cron_update.c
        src/tools/schedule.c)
endif()

# Push notifications
if(HU_ENABLE_PUSH)
    list(APPEND HU_CORE_SOURCES src/gateway/push.c)
endif()

# Update check/apply
if(HU_ENABLE_UPDATE)
    list(APPEND HU_CORE_SOURCES src/update.c)
endif()

# ML training subsystem (BPE tokenizer, dataloader, experiment loop, GPT model)
if(HU_ENABLE_ML)
    list(APPEND HU_CORE_SOURCES
        src/ml/tokenizer_bpe.c
        src/ml/dataloader.c
        src/ml/prepare.c
        src/ml/evaluator.c
        src/ml/experiment.c
        src/ml/experiment_store.c
        src/ml/gpt.c
        src/ml/muon_adamw.c
        src/ml/train.c
        src/ml/checkpoint.c
        src/ml/cli.c
        src/ml/lora.c
        # NOTE: src/ml/lora_ema.c and src/ml/cli_adapter_rollback.c moved
        # to the unconditional core sources block (alongside
        # lora_retrain_runner.c) per PR #115 / integration-tests fix.
        # They are subprocess + file I/O helpers with no learner dependency
        # and must link in HU_ENABLE_ML=OFF builds that still compile
        # lora_retrain_runner.c.
        src/ml/agent_trainer.c
        src/ml/m3_contact_routes.c
        src/ml/m3_frontier_adapter.c
        src/ml/m3_id_map.c
        src/ml/m3_rewrite_capture.c
        src/ml/mlx_admin.c
        # US-7.10 (Init #06 phase 1): RL trainer vtable + SimPO loss head.
        # NOTE: Sprint 11 US-11.5's rl_trainer_orpo.c is orphaned by main's
        # RL architecture rework — main moved to a shared-config-struct
        # dispatcher pattern (lambda_or/odds_clip fields on the unified
        # trainer struct) and dropped the per-trainer factory pattern.
        # The C-side ORPO production wiring is a Sprint 12 followup
        # (FU-11.5.a re-targeted at main's new framework).
        src/ml/rl_trainer.c
        # Sprint 8 US-8.1: DP-SGD with RDP accountant (verifiable privacy).
        # Pure-math privacy accounting + per-sample-clipped noisy SGD;
        # links inside HU_ENABLE_ML=ON alongside the other learners.
        src/ml/dp_sgd.c
        # Track D D2.2: persona-fidelity compute primitives shared by
        # `human ml fidelity-status` (CLI) and `metrics.fidelity`
        # (gateway) so both surfaces produce identical numbers.
        src/ml/fidelity.c
        # Phase A1.2 SOTA roadmap: PII redactor + quality filter for the
        # continuous-learning training-data pipeline. Pure C, no SQLite.
        src/ml/training_data_quality.c
        # dpo_miner consumes hu_pii_redact (above) + hu_dpo_collector
        # (ml/dpo.c is core, but the miner itself is ML-only — it's the
        # SQLite-backed "extract from corrections" pipeline).
        src/ml/dpo_miner.c
        src/providers/huml.c
        # Phase 2 Task 1 (RL SOTA): hu_rl_trainer_t vtable + factory.
        # Backends are stubbed at HU_ERR_NOT_SUPPORTED until Tasks 4
        # (HUML, in-process DPO + gradient check) and 6 (MLX subprocess
        # via the third-party mlx-lm-lora package).
        src/ml/rl_trainer.c
        src/ml/dpo_real_huml.c
        src/ml/dpo_real_mlx.c
        # Phase 3 Task 4 (RL SOTA): KTO (Kahneman-Tversky Optimization)
        # HUML backend + MLX stub. One-sided preference signals.
        src/ml/kto.c
        # Phase 3 Task 7 (RL SOTA): KTO MLX subprocess backend.
        src/ml/kto_mlx.c
        # Phase 3 Task 1 (RL SOTA): hu_value_head_t linear projection
        # (score = W . h + b) — trainable surface on top of frozen
        # backbones in the reward-model composition (Task 2) and
        # Bradley-Terry RM training loop (Task 3).
        src/ml/value_head.c
        # Phase 2 Task 2 (RL SOTA): teacher-forced log π(y|x) on hu_model_t.
        # Used by hu_dpo_real_huml in Task 4 and (paired with reference_model
        # from Task 3) the log π_ref counterparts that drive the DPO loss.
        src/ml/policy_logprobs.c
        # Phase 2 Task 3 (RL SOTA): frozen π_ref clone via deep-copy of params.
        # Used by hu_dpo_real_huml in Task 4 to anchor the policy-vs-reference
        # log-ratio in the DPO loss.
        src/ml/reference_model.c
        # Phase 4 Task 2 (RL SOTA): hu_rollout_t — sample N completions per
        # prompt from a hu_model_t. HUML backend uses xorshift64 PRNG seeded
        # per-rollout via splitmix64(seed XOR rollout_index) (R13: cross-
        # platform-deterministic; libc rand() / rand_r() differ across
        # glibc / Apple libc). MLX backend stubs at HU_ERR_NOT_SUPPORTED
        # until Task 8 wires scripts/grpo_mlx_train.py sampling mode.
        src/ml/rollout.c
        # Phase 2 Task 8 (RL SOTA): split `human ml dpo-train` into
        # hu_ml_cli_dpo_judge (legacy provider-scored path) +
        # hu_ml_cli_dpo_real (Phase 2 real-DPO via hu_rl_trainer_t).
        src/ml/cli_dpo.c
        # Phase 3 Task 2 (RL SOTA): hu_reward_model_t — vtable + HUML
        # factory (toy GPT backbone + Task 1 value head on last-position
        # logits) + M3 NaN contract for one-sided KTO pairs. MLX factory
        # stubs at HU_ERR_NOT_SUPPORTED until Task 8 wires
        # scripts/rm_mlx_train.py. The HUML ctx layout lives in
        # src/ml/reward_model_priv.h (plan H2 fix) so Task 3's
        # reward_model_train.c can access the value head for the SGD
        # step without a HU_IS_TEST accessor.
        src/ml/reward_model.c
        # Phase 3 Task 8 (RL SOTA): hu_reward_model_create_mlx — Apple-only
        # MLX subprocess backend. Delegates inference to
        # scripts/rm_mlx_train.py --infer via popen().
        src/ml/reward_model_mlx.c
        # Phase 3 Task 3 (RL SOTA): hu_reward_model_train — Bradley-Terry
        # mini-batch SGD on the value head (W, b); backbone is frozen
        # (Christiano 2017 §2.2). One-sided KTO pairs are skipped + counted
        # in metrics.skipped_count (KTO unpaired loss is Task 4). MLX path
        # returns HU_ERR_NOT_SUPPORTED — MLX-side RM training lives in
        # scripts/rm_mlx_train.py (Task 8). The HU_IS_TEST seams expose a
        # forward-only BT-loss helper + direct W[] pointer so AC-6's
        # finite-difference grad check can perturb the same storage the
        # SGD step writes (no test/prod gradient gap).
        src/ml/reward_model_train.c
        # Phase 3 Tasks 9-10: CLI handlers for kto-train and rm-train.
        src/ml/cli_kto.c
        src/ml/cli_rm.c
        # Phase 4 Task 9 (RL SOTA): `human ml grpo-train` CLI handler.
        # Mirrors the cli_kto.c / cli_dpo.c structural pattern: argv
        # parser (R12 strict integer parse for --rollouts), JSONL prompt
        # extraction (chosen/rejected ignored — GRPO samples rollouts
        # from the live policy), trainer creation via
        # hu_rl_trainer_create_grpo, step loop, final save_adapter.
        # Validation gates per umbrella §10: R9 reward-hacking (explicit
        # --reward-fn beats convenient default), R12 group-baseline
        # degeneracy (N<2 / N>1024), Phase 3 cli_rm precedent (--backend
        # mlx demands --backbone-path).
        src/ml/cli_grpo.c
        # Phase D Task D-1 (CF-7 RL SOTA): hu_ml_resolve_script_path —
        # resolves absolute path to a helper Python script under the
        # project's scripts/ directory, so MLX popen wrappers
        # (dpo_real_mlx, kto_mlx, grpo_mlx, reward_model_mlx) no longer
        # invoke `python3 scripts/<name>.py` with a CWD-relative path.
        # Closes CF-7 (popen relative-CWD shadow attack).
        src/ml/ml_scripts_dir.c
        # Phase 4 Task 1 (RL SOTA): KL divergence estimators (k1/k2/k3) +
        # k3 analytical backward grad. Pure C leaf math primitive — no
        # model coupling, no allocator. Used by the GRPO trainer
        # (Task 5) for the beta * KL penalty term in the loss.
        # Reference: Schulman 2020 "Approximating KL Divergence".
        src/ml/kl_divergence.c
        # Phase 4 Task 3 (RL SOTA): GRPO loss math — group-relative
        # advantage with std-floor (D7/R6), pessimistic PPO ratio clip
        # min (R8/F3), KL-aware loss (MED-1 kl_beta=0 disables; <0 →
        # default 0.04), log-ratio overflow clamp (D8), analytical
        # log-ratio gradient with finite-diff agreement to 5% (H4).
        # Loss-only — no model coupling.  Task 5 lands the trainer step
        # (hu_rl_trainer_t vtable wiring sampling + scoring + backward).
        # Defines HU_GRPO_HAVE_HUML_IMPL=1 so Task 0's #ifndef stub in
        # src/ml/rl_trainer.c for hu_grpo_huml_create falls out of the
        # link; the placeholder at the bottom of grpo.c returns
        # HU_ERR_NOT_SUPPORTED until Task 5 lands the real impl.
        src/ml/grpo.c
        # Phase 4 Task 8 (RL SOTA): GRPO MLX subprocess wrapper around
        # scripts/grpo_mlx_train.py.  Apple-only at runtime (the create
        # path returns HU_ERR_NOT_SUPPORTED on non-Apple); the M7 probe
        # short-circuit under HU_IS_TEST keeps the test suite hermetic.
        # Defines HU_GRPO_HAVE_MLX_IMPL=1 so Task 0's #ifndef stub in
        # src/ml/rl_trainer.c for hu_grpo_mlx_create falls out of the
        # link.  Real subprocess execution gated by HU_HAVE_MLX_LM_GRPO.
        src/ml/grpo_mlx.c
        # Phase 4 Task 4 (RL SOTA): hu_reward_source_t leaf vtable — three
        # backends (synthetic token-counting fn, Phase 3 hu_reward_model_t
        # composition, Phase 5 llm-judge stub). Used by the GRPO trainer
        # (Task 5) to attach a scalar reward to each rollout, and by the
        # CLI (Task 9) to dispatch on --reward-fn vs --reward-model
        # (R9 reward-hacking pin — no implicit default).
        src/ml/reward_source.c
    )
    if(HU_ENABLE_SQLITE)
        list(APPEND HU_CORE_SOURCES
            src/ml/training_data.c
            src/ml/training_data_extractor.c)
    endif()
endif()

# W13: learning loop sources (vtable dispatcher, CPU backend, MLX/ggml stubs).
# Gated independently from HU_ENABLE_ML so tests can opt-in even on minimal builds.
if(HU_ENABLE_LEARNING)
    list(APPEND HU_CORE_SOURCES
        src/ml/learner.c
        src/ml/learner_cpu.c
        src/ml/learner_mlx.c
        src/ml/learner_ggml.c
        src/ml/learner_bridge.c
        # W14 sleep-time runners that drain the W13 learner. These reference
        # hu_learner_* and hu_training_data_extract* symbols, so they must
        # be link-gated together with the learner backends.
        src/agent/lora_training_runner.c
        src/agent/training_data_runner.c
    )
endif()

# Data loader (always compiled)
list(APPEND HU_CORE_SOURCES
    src/data/loader.c
    src/data/embedded_registry.c
    src/data/data_prompts_group_chat_hint_txt.c
    src/data/data_prompts_safety_rules_txt.c
    src/data/data_prompts_default_identity_txt.c
    src/data/data_prompts_reasoning_instruction_txt.c
    src/data/data_prompts_autonomy_full_txt.c
    src/data/data_prompts_autonomy_readonly_txt.c
    src/data/data_prompts_autonomy_supervised_txt.c
    src/data/data_prompts_persona_reinforcement_txt.c
    src/data/data_conversation_ai_disclosure_patterns_json.c
    src/data/data_conversation_ai_phrases_json.c
    src/data/data_conversation_backchannel_phrases_json.c
    src/data/data_conversation_contractions_json.c
    src/data/data_conversation_conversation_intros_json.c
    src/data/data_conversation_crisis_keywords_json.c
    src/data/data_conversation_emotional_words_json.c
    src/data/data_conversation_engage_words_json.c
    src/data/data_conversation_filler_words_json.c
    src/data/data_conversation_personal_sharing_json.c
    src/data/data_conversation_starters_json.c
    src/data/data_conversation_time_gap_phrases_json.c
    src/data/data_conversation_vulnerability_keywords_json.c
    src/data/data_persona_circadian_phases_json.c
    src/data/data_persona_relationship_stages_json.c
    src/data/data_personas_default_json.c
    src/data/data_security_command_lists_json.c
    src/data/data_agent_commitment_patterns_json.c
    src/data/data_agent_conversation_plan_words_json.c
    src/data/data_agent_input_guard_patterns_json.c
    src/data/data_agent_multistep_indicators_json.c
    src/data/data_agent_sentiment_words_json.c
    src/data/data_cognition_dual_process_words_json.c
    src/data/data_memory_emotion_adjectives_json.c
    src/data/data_memory_relationship_words_json.c
    src/data/data_memory_topic_patterns_json.c
    src/data/data_memory_commitment_prefixes_json.c
    src/data/data_memory_emotion_prefixes_json.c
    src/data/data_prompts_tone_hints_json.c
    src/data/data_channels_telegram_commands_txt.c
    src/data/data_eval_golden_set_jsonl.c
)

# Crypto sources (generic C fallbacks + dispatch)
set(HU_CRYPTO_SOURCES
    asm/generic/chacha20.c
    asm/generic/sha256.c
    src/crypto/dispatch.c
)

# Arch-specific assembly (Phase 9)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set(HU_ASM_SOURCES
        asm/x86_64/chacha20.S
        asm/x86_64/sha256.S
    )
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
    set(HU_ASM_SOURCES
        asm/aarch64/chacha20.S
        asm/aarch64/sha256.S
    )
else()
    set(HU_ASM_SOURCES)
endif()

# Include directories (CMAKE_CURRENT_SOURCE_DIR when built as subdirectory)
set(HU_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${HU_ROOT}/include)
include_directories(${HU_ROOT}/asm/generic)

# Phase 5 Task 2 (RL SOTA) — bootstrap CI helper.  Gated behind
# HU_ENABLE_RL_FULL so default release/dev builds pay zero binary cost.
# Consumed by Task 5 hu_eval_gate_decide (LoRA promotion gate) and Task
# 9 competitive harness.  Subsequent Phase 5 tasks (judge external,
# leaderboard runners, eval gate, competitive harness) will append
# their TUs into this same gated block to keep the wiring uniform.
if(HU_ENABLE_RL_FULL)
    list(APPEND HU_CORE_SOURCES src/eval/bootstrap_ci.c)
    # Phase 5 Task 3 (RL SOTA) — hu_eval_judge_external_t vtable +
    # canned-response factory. Apple FM (Task 7) and Gemini Nano
    # (Task 8) factories live in dedicated TUs; eval_judge_external.c
    # keeps canned factory + #ifndef stubs when impl TUs are absent.
    list(APPEND HU_CORE_SOURCES src/eval/eval_judge_external.c)
    list(APPEND HU_CORE_SOURCES src/eval/leaderboard.c)
    list(APPEND HU_CORE_SOURCES src/eval/eval_gate.c)
    list(APPEND HU_CORE_SOURCES src/eval/stock_baseline.c)
    list(APPEND HU_CORE_SOURCES src/eval/external_judge_fixture.c)
    list(APPEND HU_CORE_SOURCES src/eval/apple_fm_client.c)
    list(APPEND HU_CORE_SOURCES src/eval/gemini_nano_client.c)
    list(APPEND HU_CORE_SOURCES src/eval/competitive_harness.c)
    list(APPEND HU_CORE_SOURCES src/eval/persona_rollout.c)
    list(APPEND HU_CORE_SOURCES src/eval/cli_eval.c)
    list(APPEND HU_CORE_SOURCES src/agent/adapter_id.c)
    list(APPEND HU_CORE_SOURCES src/ml/cli_demo.c)
endif()

# Deduplicate source lists to prevent parallel build races
list(REMOVE_DUPLICATES HU_CORE_SOURCES)

# --- Core static library ---
add_library(human_core STATIC ${HU_CORE_SOURCES} ${HU_CRYPTO_SOURCES} ${HU_ASM_SOURCES})
target_include_directories(human_core PUBLIC ${HU_ROOT}/include)
if(Threads_FOUND)
    target_link_libraries(human_core PUBLIC Threads::Threads)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(human_core PUBLIC m dl)
endif()
if(APPLE)
    target_link_libraries(human_core PRIVATE "-framework ApplicationServices" "-framework ImageIO" "-framework Accelerate" "-framework IOKit" "-framework CoreFoundation")
    # New CBLAS headers; avoids -Wdeprecated-declarations on cblas_sgemm (macOS 13.3+).
    target_compile_definitions(human_core PRIVATE ACCELERATE_NEW_LAPACK=1)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(X11 QUIET)
    if(X11_FOUND AND TARGET X11::Xtst)
        target_link_libraries(human_core PRIVATE X11::X11 X11::Xtst)
        target_compile_definitions(human_core PRIVATE HU_HAS_X11=1)
    endif()
endif()
target_include_directories(human_core PRIVATE ${HU_ROOT}/asm/generic ${HU_ROOT}/src/agent ${HU_ROOT}/src/gateway)
## HU_IS_TEST is intentionally NOT set here — human_core is the production library.
## Tests link against human_core_test (below) which defines HU_IS_TEST=1.

# --- Compile definitions for module groups ---
if(HU_ENABLE_PERIPHERALS)
    target_compile_definitions(human_core PRIVATE HU_HAS_PERIPHERALS=1)
endif()
if(HU_ENABLE_TUNNELS)
    target_compile_definitions(human_core PRIVATE HU_HAS_TUNNELS=1)
endif()
if(HU_ENABLE_RUNTIME_EXOTIC)
    target_compile_definitions(human_core PRIVATE HU_HAS_RUNTIME_EXOTIC=1)
endif()
if(HU_ENABLE_TOOLS_BROWSER)
    target_compile_definitions(human_core PRIVATE HU_HAS_TOOLS_BROWSER=1)
endif()
if(HU_ENABLE_TOOLS_ADVANCED)
    target_compile_definitions(human_core PRIVATE HU_HAS_TOOLS_ADVANCED=1)
endif()
if(HU_ENABLE_OTEL)
    target_compile_definitions(human_core PRIVATE HU_HAS_OTEL=1)
endif()
if(HU_ENABLE_CRON)
    target_compile_definitions(human_core PRIVATE HU_HAS_CRON=1)
endif()
if(HU_ENABLE_PUSH)
    target_compile_definitions(human_core PRIVATE HU_HAS_PUSH=1)
endif()
if(HU_ENABLE_UPDATE)
    target_compile_definitions(human_core PRIVATE HU_HAS_UPDATE=1)
endif()
if(HU_ENABLE_SKILLS)
    target_compile_definitions(human_core PRIVATE HU_HAS_SKILLS=1)
endif()
if(HU_ENABLE_SOCIAL)
    target_compile_definitions(human_core PRIVATE HU_ENABLE_SOCIAL=1)
endif()
if(HU_ENABLE_FEEDS)
    target_compile_definitions(human_core PRIVATE HU_ENABLE_FEEDS=1)
endif()
target_compile_definitions(human_core PUBLIC HU_HAS_PERSONA=1)
if(HU_ENABLE_CARTESIA)
    target_compile_definitions(human_core PRIVATE HU_ENABLE_CARTESIA=1)
endif()
if(HU_ENABLE_ML)
    target_compile_definitions(human_core PRIVATE HU_ENABLE_ML=1)
endif()
if(HU_ENABLE_LEARNING)
    target_compile_definitions(human_core PRIVATE HU_ENABLE_LEARNING=1)
endif()
if(HU_ENABLE_NEURAL_MEMORY)
    target_compile_definitions(human_core PRIVATE HU_ENABLE_NEURAL_MEMORY=1)
endif()
if(HU_ENABLE_MCP)
    target_compile_definitions(human_core PRIVATE HU_ENABLE_MCP=1)
endif()

# --- Compile definitions for enabled channels (for conditional compilation in source) ---
if(HU_ENABLE_CLI)
    target_compile_definitions(human_core PRIVATE HU_HAS_CLI=1)
endif()
if(HU_ENABLE_TELEGRAM)
    target_compile_definitions(human_core PRIVATE HU_HAS_TELEGRAM=1)
endif()
if(HU_ENABLE_DISCORD)
    target_compile_definitions(human_core PRIVATE HU_HAS_DISCORD=1)
endif()
if(HU_ENABLE_SLACK)
    target_compile_definitions(human_core PRIVATE HU_HAS_SLACK=1)
endif()
if(HU_ENABLE_WHATSAPP)
    target_compile_definitions(human_core PRIVATE HU_HAS_WHATSAPP=1)
endif()
if(HU_ENABLE_FACEBOOK)
    target_compile_definitions(human_core PRIVATE HU_HAS_FACEBOOK=1)
endif()
if(HU_ENABLE_INSTAGRAM)
    target_compile_definitions(human_core PRIVATE HU_HAS_INSTAGRAM=1)
endif()
if(HU_ENABLE_TWITTER)
    target_compile_definitions(human_core PRIVATE HU_HAS_TWITTER=1)
endif()
if(HU_ENABLE_TIKTOK)
    target_compile_definitions(human_core PRIVATE HU_HAS_TIKTOK=1)
endif()
if(HU_ENABLE_MATRIX)
    target_compile_definitions(human_core PRIVATE HU_HAS_MATRIX=1)
endif()
if(HU_ENABLE_IRC)
    target_compile_definitions(human_core PRIVATE HU_HAS_IRC=1)
endif()
if(HU_ENABLE_LINE)
    target_compile_definitions(human_core PRIVATE HU_HAS_LINE=1)
endif()
if(HU_ENABLE_LARK)
    target_compile_definitions(human_core PRIVATE HU_HAS_LARK=1)
endif()
if(HU_ENABLE_WEB)
    target_compile_definitions(human_core PRIVATE HU_HAS_WEB=1)
endif()
if(HU_ENABLE_EMAIL)
    target_compile_definitions(human_core PRIVATE HU_HAS_EMAIL=1)
endif()
if(HU_ENABLE_IMAP)
    target_compile_definitions(human_core PRIVATE HU_HAS_IMAP=1)
endif()
if(HU_ENABLE_IMESSAGE)
    target_compile_definitions(human_core PRIVATE HU_HAS_IMESSAGE=1)
    if(HU_IMESSAGE_TAPBACK_ENABLED)
        target_compile_definitions(human_core PRIVATE HU_IMESSAGE_TAPBACK_ENABLED=1)
    endif()
endif()
if(HU_ENABLE_PWA)
    target_compile_definitions(human_core PRIVATE HU_HAS_PWA=1)
endif()
if(HU_ENABLE_MATTERMOST)
    target_compile_definitions(human_core PRIVATE HU_HAS_MATTERMOST=1)
endif()
if(HU_ENABLE_ONEBOT)
    target_compile_definitions(human_core PRIVATE HU_HAS_ONEBOT=1)
endif()
if(HU_ENABLE_DINGTALK)
    target_compile_definitions(human_core PRIVATE HU_HAS_DINGTALK=1)
endif()
if(HU_ENABLE_TEAMS)
    target_compile_definitions(human_core PRIVATE HU_HAS_TEAMS=1)
endif()
if(HU_ENABLE_TWILIO)
    target_compile_definitions(human_core PRIVATE HU_HAS_TWILIO=1)
endif()
if(HU_ENABLE_GOOGLE_CHAT)
    target_compile_definitions(human_core PRIVATE HU_HAS_GOOGLE_CHAT=1)
endif()
if(HU_ENABLE_GOOGLE_RCS)
    target_compile_definitions(human_core PRIVATE HU_HAS_GOOGLE_RCS=1)
endif()
if(HU_ENABLE_SIGNAL)
    target_compile_definitions(human_core PRIVATE HU_HAS_SIGNAL=1)
endif()
if(HU_ENABLE_NOSTR)
    target_compile_definitions(human_core PRIVATE HU_HAS_NOSTR=1)
endif()
if(HU_ENABLE_QQ)
    target_compile_definitions(human_core PRIVATE HU_HAS_QQ=1)
endif()
if(HU_ENABLE_MAIXCAM)
    target_compile_definitions(human_core PRIVATE HU_HAS_MAIXCAM=1)
endif()
if(HU_ENABLE_DISPATCH)
    target_compile_definitions(human_core PRIVATE HU_HAS_DISPATCH=1)
endif()
if(HU_ENABLE_GMAIL)
    target_compile_definitions(human_core PRIVATE HU_HAS_GMAIL=1)
endif()
if(HU_ENABLE_VOICE)
    # Voice channel (realtime/webrtc); Sonata Rust FFI only when HU_ENABLE_SONATA.
    target_compile_definitions(human_core PRIVATE HU_HAS_VOICE_CHANNEL=1 HU_ENABLE_VOICE=1)
endif()
if(HU_ENABLE_MQTT)
    target_compile_definitions(human_core PRIVATE HU_HAS_MQTT=1)
endif()

# --- Compile definitions for memory engines ---
# none + markdown are always compiled (migration depends on them), so always define.
target_compile_definitions(human_core PRIVATE HU_HAS_NONE_ENGINE=1 HU_HAS_MARKDOWN_ENGINE=1)
if(HU_ENABLE_MEMORY_LRU_ENGINE)
    target_compile_definitions(human_core PRIVATE HU_HAS_MEMORY_LRU_ENGINE=1)
endif()
if(HU_ENABLE_API_ENGINE)
    target_compile_definitions(human_core PRIVATE HU_HAS_API_ENGINE=1)
endif()
if(HU_ENABLE_LUCID_ENGINE)
    target_compile_definitions(human_core PRIVATE HU_HAS_LUCID_ENGINE=1)
endif()
if(HU_ENABLE_REDIS_ENGINE)
    target_compile_definitions(human_core PRIVATE HU_HAS_REDIS_ENGINE=1 HU_ENABLE_REDIS_ENGINE=1)
endif()
if(HU_ENABLE_LANCEDB_ENGINE)
    target_compile_definitions(human_core PRIVATE HU_HAS_LANCEDB_ENGINE=1)
endif()

# --- Size optimization: function/data sections for link-time dead code elimination ---
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(human_core PRIVATE -ffunction-sections -fdata-sections)
    # src/onboard.c embeds a starter-persona JSON literal that exceeds ISO C99's
    # required 4095-byte minimum string-literal length. The compiler we ship with
    # supports much longer literals; -Wno-overlength-strings just silences the
    # diagnostic so -Werror doesn't reject it. Scoped to this one file so the
    # global posture stays -Werror -Wall -Wextra -Wpedantic.
    set_source_files_properties(src/onboard.c PROPERTIES
        COMPILE_OPTIONS "-Wno-overlength-strings")
endif()

if(HU_ENABLE_SQLITE)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(SQLITE3 QUIET sqlite3)
    endif()
    if(SQLITE3_FOUND)
        target_include_directories(human_core PRIVATE ${SQLITE3_INCLUDE_DIRS})
        target_link_libraries(human_core PUBLIC ${SQLITE3_LIBRARIES})
    else()
        find_library(SQLITE3_LIBRARY sqlite3)
        if(SQLITE3_LIBRARY)
            target_link_libraries(human_core PUBLIC sqlite3)
        endif()
    endif()
    target_compile_definitions(human_core PRIVATE HU_ENABLE_SQLITE=1)
    target_compile_definitions(human_core PRIVATE "HU_EVAL_SUITES_DIR=\"${HU_ROOT}/eval_suites\"")
endif()

# Phase D Task D-1 (CF-7 RL SOTA): inject HU_ML_SCRIPTS_DIR compile-time
# default so hu_ml_resolve_script_path resolves to absolute paths without
# requiring HU_ML_SCRIPTS_DIR env in dev/test builds. Env override (set
# at runtime) still wins. Installed builds should override this at
# install time to point at <prefix>/share/human/scripts.
target_compile_definitions(human_core PRIVATE "HU_ML_SCRIPTS_DIR=\"${HU_ROOT}/scripts\"")

if(HU_ENABLE_POSTGRES)
    find_package(PostgreSQL QUIET)
    if(PostgreSQL_FOUND)
        target_include_directories(human_core PRIVATE ${PostgreSQL_INCLUDE_DIRS})
        target_link_libraries(human_core PUBLIC PostgreSQL::libpq)
        target_compile_definitions(human_core PRIVATE HU_ENABLE_POSTGRES=1 HU_ENABLE_PGVECTOR=1)
        message(STATUS "PostgreSQL memory backend: enabled")
    else()
        find_library(PQ_LIBRARY NAMES pq libpq)
        find_path(PQ_INCLUDE_DIR NAMES libpq-fe.h)
        if(PQ_LIBRARY AND PQ_INCLUDE_DIR)
            target_include_directories(human_core PRIVATE ${PQ_INCLUDE_DIR})
            target_link_libraries(human_core PUBLIC ${PQ_LIBRARY})
            target_compile_definitions(human_core PRIVATE HU_ENABLE_POSTGRES=1 HU_ENABLE_PGVECTOR=1)
            message(STATUS "PostgreSQL memory backend: enabled")
        else()
            message(STATUS "PostgreSQL memory backend: disabled (libpq not found)")
        endif()
    endif()
endif()

if(HU_ENABLE_PGVECTOR AND NOT HU_ENABLE_POSTGRES)
    find_package(PostgreSQL QUIET)
    if(PostgreSQL_FOUND)
        target_include_directories(human_core PRIVATE ${PostgreSQL_INCLUDE_DIRS})
        target_link_libraries(human_core PUBLIC PostgreSQL::libpq)
        target_compile_definitions(human_core PRIVATE HU_ENABLE_PGVECTOR=1)
        message(STATUS "pgvector vector store: enabled")
    else()
        find_library(PQ_LIBRARY NAMES pq libpq)
        find_path(PQ_INCLUDE_DIR NAMES libpq-fe.h)
        if(PQ_LIBRARY AND PQ_INCLUDE_DIR)
            target_include_directories(human_core PRIVATE ${PQ_INCLUDE_DIR})
            target_link_libraries(human_core PUBLIC ${PQ_LIBRARY})
            target_compile_definitions(human_core PRIVATE HU_ENABLE_PGVECTOR=1)
            message(STATUS "pgvector vector store: enabled")
        else()
            message(STATUS "pgvector vector store: disabled (libpq not found)")
        endif()
    endif()
endif()

if(UNIX AND NOT WIN32)
    target_compile_definitions(human_core PRIVATE HU_GATEWAY_POSIX=1)
endif()

if(HU_ENABLE_EMBEDDED_MODEL)
    target_compile_definitions(human_core PRIVATE HU_ENABLE_EMBEDDED_MODEL=1)
    message(STATUS "Embedded model provider: enabled (llama.cpp/llama-cli)")
else()
    message(STATUS "Embedded model provider: disabled")
endif()

# W13 Bridge A — in-process llama.cpp with chat-time LoRA merging.
# When ON, expects libllama headers and library to be reachable via
# either a vendored `third_party/llama.cpp/` build, find_package(Llama),
# or pkg-config. Today the source compiles unconditionally and the
# vtable returns HU_ERR_NOT_SUPPORTED until the upstream is linked.
#
# Mirroring the libsodium pattern below: resolve everything against the
# already-defined human_core target now and stash includes/defines into
# cache variables so the test target (added later in this file) can pick
# them up without re-running detection.
set(HU_LLAMACPP_FOUND OFF CACHE INTERNAL "Whether llama.cpp was located")
set(HU_LLAMACPP_INCLUDE_DIRS "" CACHE INTERNAL "")
set(HU_LLAMACPP_LIBRARIES "" CACHE INTERNAL "")
set(HU_LLAMACPP_LIBRARY_DIRS "" CACHE INTERNAL "")
if(HU_ENABLE_MLX_PROVIDER)
    target_compile_definitions(human_core PRIVATE HU_ENABLE_MLX_PROVIDER=1)
endif()
if(HU_ENABLE_LLAMACPP)
    target_compile_definitions(human_core PRIVATE HU_ENABLE_LLAMACPP=1)
    set(HU_LLAMACPP_FOUND ON CACHE INTERNAL "")
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/llama.cpp/CMakeLists.txt")
        # Vendored llama.cpp. Build it as a static lib without examples
        # (those drag in unrelated deps and fail under -Werror).
        set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
        # Phase 1 (RL SOTA) — Metal GPU offload on Apple. Upstream
        # llama.cpp picks GGML_METAL up via its own option(); forcing
        # it here so the vendored sub-build matches our top-level
        # HU_LLAMACPP_METAL knob. EMBED_LIBRARY=ON ships the Metal
        # shaders inside the static lib so we don't need to copy
        # default.metallib alongside the binary.
        if(HU_LLAMACPP_METAL AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            set(GGML_METAL ON CACHE BOOL "" FORCE)
            set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
            message(STATUS "llama.cpp Metal: enabled")
        else()
            set(GGML_METAL OFF CACHE BOOL "" FORCE)
        endif()
        add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)
        target_include_directories(human_core PRIVATE third_party/llama.cpp/include)
        set(HU_LLAMACPP_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/third_party/llama.cpp/include" CACHE INTERNAL "")
        target_link_libraries(human_core PRIVATE llama)
        message(STATUS "llama.cpp provider: enabled (vendored)")
    else()
        # Resolve a pre-installed libllama. Try in order:
        #   1. find_package(Llama)  — upstream ships a CMake export ~b3000+
        #   2. pkg-config           — Linux distros ship a `llama.pc`
        #   3. Manual prefix probe  — Homebrew + /usr/local + /usr
        # On success we stash includes+libs into HU_LLAMACPP_* cache vars so
        # the test target wiring (later in this file) can mirror it without
        # re-running discovery.
        find_package(Llama QUIET)
        if(TARGET llama)
            target_link_libraries(human_core PUBLIC llama)
            message(STATUS "llama.cpp provider: enabled (find_package Llama)")
        else()
            find_package(PkgConfig QUIET)
            set(_hu_llama_pkgconfig_found OFF)
            if(PkgConfig_FOUND)
                pkg_check_modules(LLAMA QUIET llama)
                if(LLAMA_FOUND)
                    set(HU_LLAMACPP_INCLUDE_DIRS ${LLAMA_INCLUDE_DIRS} CACHE INTERNAL "")
                    set(HU_LLAMACPP_LIBRARIES ${LLAMA_LIBRARIES} CACHE INTERNAL "")
                    set(HU_LLAMACPP_LIBRARY_DIRS ${LLAMA_LIBRARY_DIRS} CACHE INTERNAL "")
                    set(_hu_llama_pkgconfig_found ON)
                endif()
            endif()
            if(NOT _hu_llama_pkgconfig_found)
                find_path(LLAMA_INCLUDE_DIR llama.h
                    PATHS /opt/homebrew/include /usr/local/include /usr/include)
                find_library(LLAMA_LIB llama
                    PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
                if(LLAMA_INCLUDE_DIR AND LLAMA_LIB)
                    set(HU_LLAMACPP_INCLUDE_DIRS ${LLAMA_INCLUDE_DIR} CACHE INTERNAL "")
                    set(HU_LLAMACPP_LIBRARIES ${LLAMA_LIB} CACHE INTERNAL "")
                    message(STATUS "llama.cpp provider: found manually at ${LLAMA_LIB}")
                endif()
            endif()
            if(HU_LLAMACPP_INCLUDE_DIRS OR HU_LLAMACPP_LIBRARIES)
                if(HU_LLAMACPP_INCLUDE_DIRS)
                    target_include_directories(human_core PRIVATE ${HU_LLAMACPP_INCLUDE_DIRS})
                endif()
                if(HU_LLAMACPP_LIBRARIES)
                    # PUBLIC for the same reason as libsodium below: the
                    # production `human` executable links human_core as a
                    # static lib and would otherwise drop the libllama
                    # search-path resolution.
                    target_link_libraries(human_core PUBLIC ${HU_LLAMACPP_LIBRARIES})
                endif()
                if(HU_LLAMACPP_LIBRARY_DIRS)
                    target_link_directories(human_core PUBLIC ${HU_LLAMACPP_LIBRARY_DIRS})
                endif()
                message(STATUS "llama.cpp provider: enabled (system libllama)")
            else()
                message(STATUS "llama.cpp provider: enabled (no vendored upstream and libllama not found; vtable will return NOT_SUPPORTED until libllama is linked)")
            endif()
        endif()
    endif()
else()
    message(STATUS "llama.cpp provider: disabled (compiles as NOT_SUPPORTED stub)")
endif()

# W15 — libsodium upgrade.
#
# Resolve the include + library locations now (human_core exists). The
# corresponding human_core_test wiring lives after that target is added,
# below. Defines HU_HAS_LIBSODIUM=1 on success so keystore.c routes
# random + KDF + AEAD through XChaCha20-Poly1305-IETF and Argon2id. The
# ciphertext envelope is versioned, so existing v0 (PBKDF2 +
# ChaCha20+HMAC) blobs remain decryptable.
set(HU_LIBSODIUM_FOUND OFF CACHE INTERNAL "Whether libsodium was located")
set(HU_LIBSODIUM_INCLUDE_DIRS "" CACHE INTERNAL "")
set(HU_LIBSODIUM_LIBRARIES "" CACHE INTERNAL "")
set(HU_LIBSODIUM_LIBRARY_DIRS "" CACHE INTERNAL "")
if(HU_ENABLE_LIBSODIUM)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSODIUM QUIET libsodium)
        if(LIBSODIUM_FOUND)
            set(HU_LIBSODIUM_INCLUDE_DIRS ${LIBSODIUM_INCLUDE_DIRS} CACHE INTERNAL "")
            set(HU_LIBSODIUM_LIBRARIES ${LIBSODIUM_LIBRARIES} CACHE INTERNAL "")
            set(HU_LIBSODIUM_LIBRARY_DIRS ${LIBSODIUM_LIBRARY_DIRS} CACHE INTERNAL "")
            set(HU_LIBSODIUM_FOUND ON CACHE INTERNAL "")
        endif()
    endif()
    if(NOT HU_LIBSODIUM_FOUND)
        # Manual fallback: probe common install prefixes (Homebrew Apple
        # Silicon, Homebrew Intel, /usr/local, /usr).
        find_path(SODIUM_INCLUDE_DIR sodium.h
            PATHS /opt/homebrew/include /usr/local/include /usr/include)
        find_library(SODIUM_LIB sodium
            PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
        if(SODIUM_INCLUDE_DIR AND SODIUM_LIB)
            set(HU_LIBSODIUM_INCLUDE_DIRS ${SODIUM_INCLUDE_DIR} CACHE INTERNAL "")
            set(HU_LIBSODIUM_LIBRARIES ${SODIUM_LIB} CACHE INTERNAL "")
            set(HU_LIBSODIUM_FOUND ON CACHE INTERNAL "")
            message(STATUS "libsodium: found manually at ${SODIUM_LIB}")
        endif()
    endif()
    if(HU_LIBSODIUM_FOUND)
        target_include_directories(human_core PRIVATE ${HU_LIBSODIUM_INCLUDE_DIRS})
        # PUBLIC link so the production `human` executable (which links
        # human_core as a static lib) inherits the libsodium symbol +
        # search-path resolution. Without this, ld emits
        # "library 'sodium' not found" at the human executable's link step
        # because PRIVATE link directories don't propagate from a static
        # library to its consumers.
        target_link_libraries(human_core PUBLIC ${HU_LIBSODIUM_LIBRARIES})
        if(HU_LIBSODIUM_LIBRARY_DIRS)
            target_link_directories(human_core PUBLIC ${HU_LIBSODIUM_LIBRARY_DIRS})
        endif()
        target_compile_definitions(human_core PRIVATE HU_HAS_LIBSODIUM=1)
        # Add persona_crypt.c here (rather than to HU_CORE_SOURCES) so
        # it only compiles when libsodium is actually available. The TU
        # has `#error if !defined(HU_HAS_LIBSODIUM)`, so including it
        # unconditionally breaks every CI variant where the option is
        # off (default OFF). The matching human_core_test wiring is in
        # a parallel block AFTER add_library(human_core_test ...) — see
        # search for "persona_crypt.c" in the test-library section.
        target_sources(human_core PRIVATE src/persona/persona_crypt.c)
        message(STATUS "libsodium: enabled (XChaCha20-Poly1305 + Argon2id)")
    else()
        message(WARNING "HU_ENABLE_LIBSODIUM=ON but libsodium not found; falling back to PBKDF2 + ChaCha20+HMAC")
    endif()
else()
    message(STATUS "libsodium: disabled (using PBKDF2 + ChaCha20+HMAC)")
endif()

if(HU_ENABLE_APPLE_INTELLIGENCE)
    target_compile_definitions(human_core PRIVATE HU_ENABLE_APPLE_INTELLIGENCE=1)
    message(STATUS "Apple Intelligence provider: enabled (on-device)")
else()
    message(STATUS "Apple Intelligence provider: disabled")
endif()

if(HU_ENABLE_COREML)
    target_compile_definitions(human_core PRIVATE HU_ENABLE_COREML=1)
    message(STATUS "CoreML/MLX provider: enabled")
else()
    message(STATUS "CoreML/MLX provider: disabled")
endif()

if(HU_ENABLE_CURL)
    find_package(CURL QUIET)
    target_compile_definitions(human_core PRIVATE HU_ENABLE_CURL=1)
    if(TARGET CURL::libcurl)
        target_link_libraries(human_core PUBLIC CURL::libcurl)
        target_compile_definitions(human_core PRIVATE HU_HTTP_CURL=1)
        message(STATUS "HTTP client: libcurl enabled")
    elseif(CURL_FOUND)
        target_include_directories(human_core PRIVATE ${CURL_INCLUDE_DIRS})
        target_link_libraries(human_core PUBLIC ${CURL_LIBRARIES})
        target_compile_definitions(human_core PRIVATE HU_HTTP_CURL=1)
        message(STATUS "HTTP client: libcurl enabled")
    else()
        find_library(CURL_LIBRARY curl)
        find_path(CURL_INCLUDE_DIR NAMES curl/curl.h)
        if(CURL_LIBRARY AND CURL_INCLUDE_DIR)
            target_include_directories(human_core PRIVATE ${CURL_INCLUDE_DIR})
            target_link_libraries(human_core PUBLIC ${CURL_LIBRARY})
            target_compile_definitions(human_core PRIVATE HU_HTTP_CURL=1)
            message(STATUS "HTTP client: libcurl enabled")
        else()
            message(STATUS "HTTP client: disabled (libcurl not found)")
        endif()
    endif()
endif()

if(HU_ENABLE_FIPS_CRYPTO)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(human_core PUBLIC OpenSSL::Crypto)
    target_compile_definitions(human_core PRIVATE HU_ENABLE_FIPS_CRYPTO=1)
    message(STATUS "FIPS crypto: AES-256-GCM enabled")
endif()

if(HU_ENABLE_TLS)
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_link_libraries(human_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(human_core PRIVATE HU_HAS_TLS=1)
        message(STATUS "TLS: OpenSSL enabled")
    else()
        find_library(OPENSSL_SSL_LIB ssl HINTS /opt/homebrew/opt/openssl@3/lib /opt/homebrew/opt/openssl/lib)
        find_library(OPENSSL_CRYPTO_LIB crypto HINTS /opt/homebrew/opt/openssl@3/lib /opt/homebrew/opt/openssl/lib)
        find_path(OPENSSL_INCLUDE openssl/ssl.h HINTS /opt/homebrew/opt/openssl@3/include /opt/homebrew/opt/openssl/include)
        if(OPENSSL_SSL_LIB AND OPENSSL_CRYPTO_LIB AND OPENSSL_INCLUDE)
            target_include_directories(human_core PRIVATE ${OPENSSL_INCLUDE})
            target_link_libraries(human_core PUBLIC ${OPENSSL_SSL_LIB} ${OPENSSL_CRYPTO_LIB})
            target_compile_definitions(human_core PRIVATE HU_HAS_TLS=1)
            message(STATUS "TLS: OpenSSL enabled (manual)")
        else()
            message(STATUS "TLS: disabled (OpenSSL not found)")
        endif()
    endif()
endif()

if(HU_ENABLE_LINENOISE)
    set(LINENOISE_DIR "${CMAKE_SOURCE_DIR}/vendor/linenoise")
    if(EXISTS "${LINENOISE_DIR}/linenoise.c")
        target_sources(human_core PRIVATE "${LINENOISE_DIR}/linenoise.c")
        target_include_directories(human_core PRIVATE "${LINENOISE_DIR}")
        target_compile_definitions(human_core PRIVATE HU_ENABLE_LINENOISE=1)
        message(STATUS "Linenoise: enabled (${LINENOISE_DIR})")
    else()
        message(WARNING "Linenoise: vendor/linenoise/linenoise.c not found. "
            "Download from https://github.com/antirez/linenoise")
    endif()
elseif(HU_ENABLE_READLINE)
    find_library(READLINE_LIBRARY NAMES readline edit)
    if(READLINE_LIBRARY)
        target_link_libraries(human_core PUBLIC ${READLINE_LIBRARY})
        target_compile_definitions(human_core PRIVATE HU_ENABLE_READLINE=1)
        message(STATUS "Readline: enabled (${READLINE_LIBRARY})")
    else()
        message(STATUS "Readline: disabled (library not found)")
    endif()
else()
    message(STATUS "Line editing: OFF (basic getchar fallback)")
endif()

if(HU_ENABLE_TUI)
    set(TERMBOX2_DIR "${CMAKE_SOURCE_DIR}/vendor/termbox2")
    if(EXISTS "${TERMBOX2_DIR}/termbox2.h")
        target_compile_definitions(human_core PRIVATE HU_ENABLE_TUI=1)
        target_include_directories(human_core PRIVATE "${TERMBOX2_DIR}")
        message(STATUS "TUI: enabled (termbox2 at ${TERMBOX2_DIR})")
    else()
        message(WARNING "TUI: vendor/termbox2/termbox2.h not found. "
            "Download from https://github.com/termbox/termbox2")
    endif()
else()
    message(STATUS "TUI: OFF (ANSI spinner mode)")
endif()

if(HU_ENABLE_ASAN)
    target_compile_options(human_core PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(human_core PRIVATE -fsanitize=address)
endif()

# src/onboard.c carries the starter persona JSON as a C string literal that
# exceeds the 4095-byte ISO C99 maximum. This is intentional (the literal is
# the single source of truth shared by `human init` and `human onboard`);
# disable the warning for that file only.
set_source_files_properties(src/onboard.c PROPERTIES COMPILE_FLAGS "-Wno-overlength-strings")

# --- Main executable ---
add_executable(human src/main.c src/bootstrap.c)
target_link_libraries(human human_core)

# US-8.3 per-target: Mach-O LC_UUID embeds host-derived randomness; zero
# it on the shipped binary only. Test binaries keep their UUID because
# dyld + ASan require it for symbolication. See
# docs/standards/engineering/reproducible-builds.md.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # target_link_options(human PRIVATE "LINKER:-no_uuid")  # Removed 2026-05-17: newer macOS arm64 dyld requires LC_UUID. Reproducibility now via strip-and-compare in check-reproducible-build.sh.
endif()

# ──────────────────────────────────────────────────────────────────────────
# P2E — Layer-direction topology check (memory v2 roadmap)
#
# Enforces the rule from `docs/plans/2026-05-10-memory-v2-roadmap-overview.md`
# §"Cross-cutting principles": "A workstream may never depend on a layer
# above its own. Violations are a build error (CMake topology check)."
#
# The check runs `scripts/check-layer-topology.sh`, which scans every .c
# file in the L0-L7 memory v2 stack and rejects any `#include` directive
# whose target header is owned by a higher layer. Bridge / wiring files
# are listed explicitly with rationale in the script.
#
# Skip on Windows (script is POSIX bash) and when explicitly disabled.
option(HU_ENABLE_TOPOLOGY_CHECK
       "Enforce 7-layer architectural dependency direction (memory v2 P2E)" ON)
# W7 Phase 1 — informational `hu_graph_*` substring inventory (agent/persona/feeds).
# Independent of HU_ENABLE_TOPOLOGY_CHECK; POSIX only (same bash contract as topology).
if(NOT WIN32)
    add_custom_target(human_w7_phase1_inventory
        COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/w7-phase1-graph-bypass-inventory.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "W7 Phase 1 graph bypass inventory (informational)"
        VERBATIM)
    # Memory v2 local proof bundle (G2 collision + v2 E2E suite); see
    # docs/plans/2026-05-10-memory-v2-execution-plan.md gate G1.5.
    # Only available when tests are built — generator expression below
    # otherwise fails configure when HU_BUILD_TESTS=OFF (release/docker).
    if(HU_BUILD_TESTS)
        add_custom_target(memory_v2_local_proof
            COMMAND ${CMAKE_COMMAND} -E env HUMAN_TESTS=$<TARGET_FILE:human_tests>
                bash ${CMAKE_SOURCE_DIR}/scripts/memory-v2-local-proof.sh
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            DEPENDS human_tests
            COMMENT "Memory v2 local proof (collision guard + v2 E2E suite)"
            VERBATIM)
    endif()
endif()
if(HU_ENABLE_TOPOLOGY_CHECK AND NOT WIN32)
    add_custom_target(human_topology_check
        COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/check-layer-topology.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking 7-layer architectural topology (P2E)"
        VERBATIM)
    add_dependencies(human human_topology_check)
endif()
# PCTT Task 9 — regression guard: prevent the three hardcoded DECISION-bucket
# fillers from being reintroduced under src/ after PCTT Task 4 deletes them.
# data/eval_*.json preserves the strings as fixtures and is excluded by the
# script. The guard is ALL (runs on every `cmake --build --preset dev`) but
# honours the HU_SKIP_FILLER_GUARD=1 env-var bypass for Wave 2 in-flight
# builds where Task 4 has not yet merged. Flip to remove the bypass once
# Task 4 lands on main.
# Skip on Windows (script is POSIX bash).
if(NOT WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/tools/check-no-hardcoded-fillers.sh")
    # Defense-in-depth: skip silently if tools/ wasn't copied into the build
    # context (the Dockerfile must COPY tools/ explicitly — see Dockerfile).
    # Local dev and CI runners always have tools/ so the guard runs there.
    add_custom_target(human_filler_guard
        ALL
        COMMAND sh ${CMAKE_SOURCE_DIR}/tools/check-no-hardcoded-fillers.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking no hardcoded fillers regressed (PCTT Task 9)"
        VERBATIM)
endif()
# Embed Info.plist for macOS TCC (accessibility, AppleEvents) identity
if(APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/resources/Info.plist")
    target_link_options(human PRIVATE
        "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_SOURCE_DIR}/resources/Info.plist")
endif()
if(HU_ENABLE_CURL)
    target_compile_definitions(human PRIVATE HU_ENABLE_CURL=1)
endif()
if(HU_ENABLE_SKILLS)
    target_compile_definitions(human PRIVATE HU_HAS_SKILLS=1)
endif()
if(HU_ENABLE_ML)
    target_compile_definitions(human PRIVATE HU_ENABLE_ML=1)
endif()
if(HU_ENABLE_CRON)
    target_compile_definitions(human PRIVATE HU_HAS_CRON=1)
endif()
if(HU_ENABLE_PUSH)
    target_compile_definitions(human PRIVATE HU_HAS_PUSH=1)
endif()
if(HU_ENABLE_UPDATE)
    target_compile_definitions(human PRIVATE HU_HAS_UPDATE=1)
endif()
if(HU_ENABLE_EMAIL)
    target_compile_definitions(human PRIVATE HU_HAS_EMAIL=1)
endif()
if(HU_ENABLE_IMESSAGE)
    target_compile_definitions(human PRIVATE HU_HAS_IMESSAGE=1)
    if(HU_IMESSAGE_TAPBACK_ENABLED)
        target_compile_definitions(human PRIVATE HU_IMESSAGE_TAPBACK_ENABLED=1)
    endif()
endif()
if(HU_ENABLE_PWA)
    target_compile_definitions(human PRIVATE HU_HAS_PWA=1)
endif()
if(HU_ENABLE_IMAP)
    target_compile_definitions(human PRIVATE HU_HAS_IMAP=1)
endif()
if(HU_ENABLE_TELEGRAM)
    target_compile_definitions(human PRIVATE HU_HAS_TELEGRAM=1)
endif()
if(HU_ENABLE_SLACK)
    target_compile_definitions(human PRIVATE HU_HAS_SLACK=1)
endif()
if(HU_ENABLE_WHATSAPP)
    target_compile_definitions(human PRIVATE HU_HAS_WHATSAPP=1)
endif()
if(HU_ENABLE_GMAIL)
    target_compile_definitions(human PRIVATE HU_HAS_GMAIL=1)
endif()
if(HU_ENABLE_FEEDS)
    target_compile_definitions(human PRIVATE HU_ENABLE_FEEDS=1)
endif()
if(HU_ENABLE_CARTESIA)
    target_compile_definitions(human PRIVATE HU_ENABLE_CARTESIA=1)
endif()
if(HU_ENABLE_SQLITE)
    target_compile_definitions(human PRIVATE HU_ENABLE_SQLITE=1)
    if(SQLITE3_FOUND)
        target_include_directories(human PRIVATE ${SQLITE3_INCLUDE_DIRS})
    endif()
endif()

if(HU_ENABLE_ASAN)
    target_compile_options(human PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(human PRIVATE -fsanitize=address)
endif()

# --- LTO (Link-Time Optimization) ---
if(HU_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_supported)
    if(lto_supported)
        set_property(TARGET human PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET human_core PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

# --- Release optimizations ---
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    if(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel" AND CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang")
        target_compile_options(human PRIVATE -Oz -fno-asynchronous-unwind-tables -fmerge-all-constants)
        target_compile_options(human_core PRIVATE -Oz -fno-asynchronous-unwind-tables -fmerge-all-constants)
    else()
        target_compile_options(human PRIVATE -Os)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(human PRIVATE -fvisibility=hidden)
        target_compile_options(human_core PRIVATE -fvisibility=hidden)
    endif()
    if(APPLE)
        set_target_properties(human PROPERTIES LINK_FLAGS "-Wl,-dead_strip")
    elseif(UNIX)
        set_target_properties(human PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
    endif()
endif()

# --- Strip MinSizeRel binary (macOS: -x to preserve local syms, Linux: full strip) ---
# Skip strip when cross-compiling (host strip can't parse target binary)
if(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel" AND UNIX AND NOT CMAKE_CROSSCOMPILING)
    if(APPLE)
        add_custom_command(TARGET human POST_BUILD COMMAND strip -x $<TARGET_FILE:human>)
    else()
        add_custom_command(TARGET human POST_BUILD COMMAND strip $<TARGET_FILE:human>)
    endif()
endif()

# --- macOS: codesign with stable "Human Local Dev" cert so TCC survives rebuilds ---
if(APPLE AND NOT CMAKE_CROSSCOMPILING)
    execute_process(
        COMMAND security find-identity -v -p codesigning
        OUTPUT_VARIABLE _signing_identities
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_signing_identities MATCHES "Human Local Dev")
        add_custom_command(TARGET human POST_BUILD
            COMMAND codesign --force --sign "Human Local Dev" $<TARGET_FILE:human>
            COMMENT "Signing human binary with Human Local Dev certificate")
    endif()
endif()

# --- Test-mode core library (same sources, compiled with HU_IS_TEST=1) ---
# When HU_ENABLE_ALL_CHANNELS=OFF, prod binary omits signal/nostr/qq/maixcam/dispatch,
# but tests need them — add to test lib only.
set(HU_TEST_EXTRA_CHANNELS)
if(NOT HU_ENABLE_ALL_CHANNELS)
    # Orphan channels referenced by tests/test_orphan_channel_audit.c.
    # mattermost + web were missing here — the benchmark/MinSizeRel build
    # turns HU_ENABLE_ALL_CHANNELS off and the test binary failed to link
    # with undefined references to hu_{mattermost,web}_{create,destroy}.
    list(APPEND HU_TEST_EXTRA_CHANNELS
        src/channels/signal.c src/channels/nostr.c src/channels/qq.c
        src/channels/maixcam.c src/channels/dispatch.c
        src/channels/gmail_base64.c src/channels/gmail.c
        src/channels/mattermost.c src/channels/web.c)
endif()
if(NOT HU_ENABLE_MQTT)
    list(APPEND HU_TEST_EXTRA_CHANNELS src/channels/mqtt.c)
endif()
if(NOT HU_ENABLE_FACEBOOK)
    list(APPEND HU_TEST_EXTRA_CHANNELS src/channels/facebook.c)
endif()
if(NOT HU_ENABLE_FACEBOOK AND NOT HU_ENABLE_INSTAGRAM AND NOT HU_ENABLE_WHATSAPP)
    list(APPEND HU_TEST_EXTRA_CHANNELS src/channels/meta_common.c)
endif()
if(NOT HU_ENABLE_INSTAGRAM)
    list(APPEND HU_TEST_EXTRA_CHANNELS src/channels/instagram.c)
endif()
if(NOT HU_ENABLE_TWITTER)
    list(APPEND HU_TEST_EXTRA_CHANNELS src/channels/twitter.c)
endif()
if(NOT HU_ENABLE_TIKTOK)
    list(APPEND HU_TEST_EXTRA_CHANNELS src/channels/tiktok.c)
endif()
if(NOT HU_ENABLE_GOOGLE_RCS)
    list(APPEND HU_TEST_EXTRA_CHANNELS src/channels/google_rcs.c)
endif()
if(NOT HU_ENABLE_SLACK)
    list(APPEND HU_TEST_EXTRA_CHANNELS src/channels/slack.c src/channels/slack_reactions.c)
endif()
# Tests always compile all optional modules so test suites don't break
set(HU_TEST_EXTRA_MODULES)
if(NOT HU_ENABLE_PERIPHERALS)
    list(APPEND HU_TEST_EXTRA_MODULES
        src/peripherals/arduino.c src/peripherals/stm32.c src/peripherals/rpi.c
        src/peripherals/factory.c src/hardware.c
        src/tools/hardware_info.c src/tools/hardware_memory.c
        src/tools/i2c.c src/tools/peripheral_ctrl.c src/tools/spi.c)
endif()
if(NOT HU_ENABLE_TUNNELS)
    list(APPEND HU_TEST_EXTRA_MODULES
        src/tunnel/cloudflare.c src/tunnel/ngrok.c
        src/tunnel/tailscale.c src/tunnel/custom.c)
endif()
if(NOT HU_ENABLE_RUNTIME_EXOTIC)
    list(APPEND HU_TEST_EXTRA_MODULES
        src/runtime/wasm_rt.c src/runtime/cloudflare.c)
endif()
# Tests need all sandbox backends (they compile cross-platform with guards)
if(NOT APPLE)
    list(APPEND HU_TEST_EXTRA_MODULES src/security/seatbelt.c)
endif()
if(NOT UNIX OR APPLE)
    list(APPEND HU_TEST_EXTRA_MODULES
        src/security/landlock.c src/security/firejail.c src/security/bubblewrap.c
        src/security/seccomp.c src/security/landlock_seccomp.c src/security/firecracker.c)
endif()
if(NOT WIN32)
    list(APPEND HU_TEST_EXTRA_MODULES src/security/appcontainer.c)
endif()
if(HU_ENABLE_RL_FULL)
    list(APPEND HU_TEST_EXTRA_MODULES src/providers/provider_test_seam.c)
endif()
if(NOT HU_ENABLE_TOOLS_BROWSER)
    list(APPEND HU_TEST_EXTRA_MODULES
        src/tools/browser.c src/tools/browser_open.c src/tools/screenshot.c)
endif()
if(NOT HU_ENABLE_TOOLS_ADVANCED)
    list(APPEND HU_TEST_EXTRA_MODULES
        src/tools/composio.c src/tools/claude_code.c src/tools/canvas.c
        src/tools/notebook.c src/tools/database.c)
endif()
if(NOT HU_ENABLE_OTEL)
    list(APPEND HU_TEST_EXTRA_MODULES src/observability/otel.c)
endif()
if(NOT HU_ENABLE_SKILLS)
    list(APPEND HU_TEST_EXTRA_MODULES
        src/skillforge.c
        src/skills.c
        src/tools/skill_run.c
        src/intelligence/skills.c
        src/intelligence/reflection.c
        src/intelligence/feedback.c
        src/intelligence/meta_learning.c
        src/intelligence/cycle.c
    )
endif()
if(NOT HU_ENABLE_SOCIAL)
    list(APPEND HU_TEST_EXTRA_MODULES src/feeds/social.c)
endif()
if(NOT HU_ENABLE_FEEDS)
    list(APPEND HU_TEST_EXTRA_MODULES src/feeds/apple.c src/feeds/google.c src/feeds/music.c
        src/feeds/news.c src/feeds/email.c src/feeds/gmail.c src/feeds/imessage.c
        src/feeds/twitter.c src/feeds/file_ingest.c src/feeds/research.c src/feeds/findings.c
        src/feeds/research_executor.c src/feeds/trends.c)
endif()
if(NOT HU_ENABLE_PUSH)
    list(APPEND HU_TEST_EXTRA_MODULES src/gateway/push.c)
endif()
if(NOT HU_ENABLE_UPDATE)
    list(APPEND HU_TEST_EXTRA_MODULES src/update.c)
endif()
if(NOT HU_ENABLE_CRON)
    list(APPEND HU_TEST_EXTRA_MODULES
        src/tools/cron_add.c src/tools/cron_list.c src/tools/cron_remove.c
        src/tools/cron_run.c src/tools/cron_runs.c src/tools/cron_update.c
        src/tools/schedule.c)
endif()
# Persona sources are always in HU_CORE_SOURCES since v2026.4.0
if(NOT HU_ENABLE_APPLE_INTELLIGENCE)
    list(APPEND HU_TEST_EXTRA_MODULES src/providers/apple.c)
endif()
if(NOT HU_ENABLE_CARTESIA)
    list(APPEND HU_TEST_EXTRA_MODULES
        src/tts/audio_pipeline.c
        src/context/voice_decision.c src/tts/emotion_map.c)
endif()
# Phase 7 modules (experience, forgetting, weakness) — tests need them when SQLite is OFF (minimal build)
if(NOT HU_ENABLE_SQLITE)
    list(APPEND HU_TEST_EXTRA_MODULES
        src/intelligence/experience.c
        src/intelligence/weakness.c
        src/intelligence/weakness_analyzer.c
        src/memory/forgetting.c
    )
endif()
add_library(human_core_test STATIC ${HU_CORE_SOURCES} ${HU_TEST_EXTRA_CHANNELS} ${HU_TEST_EXTRA_MODULES} ${HU_CRYPTO_SOURCES} ${HU_ASM_SOURCES})
target_include_directories(human_core_test PUBLIC ${HU_ROOT}/include)
target_include_directories(human_core_test PRIVATE ${HU_ROOT}/asm/generic)
target_compile_definitions(human_core_test PRIVATE HU_IS_TEST=1)
# Phase 6: lora_training_runner.c compiles hu_e2e_closed_loop_run under
# HU_IS_TEST even when HU_ENABLE_RL_FULL is OFF (dev preset).  tests/include
# must be visible to human_core_test, not only when RL_FULL is ON.
target_include_directories(human_core_test PRIVATE ${HU_ROOT}/tests/include)
target_compile_definitions(human_core_test PRIVATE HU_E2E_TMP_ROOT="${HU_ROOT}/tests/_tmp")
# W15 — wire libsodium into the test build to mirror production keystore.c.
# We use PUBLIC link directives so that human_tests (which links the static
# human_core_test library) inherits the libsodium link directory and library
# without needing to repeat the discovery dance.
#
# Also pulls in src/persona/persona_crypt.c — that source hard-#errors when
# HU_HAS_LIBSODIUM is undefined, so it MUST be gated here and cannot live in
# the unconditional HU_CORE_SOURCES list (see persona_crypt.c top comment +
# Sprint-7 merge-regression that re-broke this once before).
#
# NOTE: tests/test_persona_encryption.c is registered AFTER the
# `set(HU_TEST_SOURCES ...)` at line ~2834 (which would otherwise discard
# an append done here). See the matching `if(HU_LIBSODIUM_FOUND)` block in
# the gated test-sources section near line ~3440. Bugbot 8b687b9c caught
# this set-after-append silent loss.
if(HU_LIBSODIUM_FOUND)
    target_sources(human_core_test PRIVATE src/persona/persona_crypt.c)
    target_include_directories(human_core_test PRIVATE ${HU_LIBSODIUM_INCLUDE_DIRS})
    target_link_libraries(human_core_test PUBLIC ${HU_LIBSODIUM_LIBRARIES})
    if(HU_LIBSODIUM_LIBRARY_DIRS)
        target_link_directories(human_core_test PUBLIC ${HU_LIBSODIUM_LIBRARY_DIRS})
    endif()
    target_compile_definitions(human_core_test PRIVATE HU_HAS_LIBSODIUM=1)
endif()
# W13 Bridge A — mirror llama.cpp wiring into the test build.
# Phase 1 (RL SOTA) — also link `llama` so test files that include
# llama.h (notably tests/test_llamacpp_chat_metal.c) actually link.
# Without this mirror, tests that call any llama_* symbol fail at link
# with "undefined symbol" errors even though human_core links cleanly.
if(HU_LLAMACPP_FOUND)
    target_compile_definitions(human_core_test PRIVATE HU_ENABLE_LLAMACPP=1)
    if(HU_LLAMACPP_INCLUDE_DIRS)
        target_include_directories(human_core_test PRIVATE ${HU_LLAMACPP_INCLUDE_DIRS})
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/llama.cpp/CMakeLists.txt")
        # Vendored path — `llama` target exists in the build tree.
        target_link_libraries(human_core_test PRIVATE llama)
    elseif(HU_LLAMACPP_LIBRARIES)
        # System libllama path — link against the resolved libraries.
        target_link_libraries(human_core_test PUBLIC ${HU_LLAMACPP_LIBRARIES})
        if(HU_LLAMACPP_LIBRARY_DIRS)
            target_link_directories(human_core_test PUBLIC ${HU_LLAMACPP_LIBRARY_DIRS})
        endif()
    endif()
endif()
if(HU_ENABLE_APPLE_INTELLIGENCE)
    target_compile_definitions(human_core_test PRIVATE HU_ENABLE_APPLE_INTELLIGENCE=1)
endif()
if(HU_ENABLE_COREML)
    target_compile_definitions(human_core_test PRIVATE HU_ENABLE_COREML=1)
endif()
# (Note: libsodium wiring for human_core_test was previously duplicated here;
#  merged into the single block above per Bugbot 31c0a7dc.)
if(HU_ENABLE_FEEDS)
    target_compile_definitions(human_core_test PRIVATE HU_ENABLE_FEEDS=1)
endif()
if(HU_ENABLE_CARTESIA)
    target_compile_definitions(human_core_test PRIVATE HU_ENABLE_CARTESIA=1)
endif()
target_compile_definitions(human_core_test PRIVATE HU_ENABLE_PERSONA=1)
# Tests always have all module groups available
target_compile_definitions(human_core_test PRIVATE HU_HAS_PERIPHERALS=1 HU_HAS_TUNNELS=1 HU_HAS_RUNTIME_EXOTIC=1 HU_HAS_TOOLS_BROWSER=1 HU_HAS_TOOLS_ADVANCED=1 HU_HAS_OTEL=1 HU_HAS_CRON=1 HU_HAS_PUSH=1 HU_HAS_UPDATE=1 HU_HAS_SKILLS=1 HU_HAS_PERSONA=1)
if(HU_ENABLE_ML)
    target_compile_definitions(human_core_test PRIVATE HU_ENABLE_ML=1)
endif()
if(HU_ENABLE_LEARNING)
    target_compile_definitions(human_core_test PRIVATE HU_ENABLE_LEARNING=1)
endif()
if(HU_ENABLE_NEURAL_MEMORY)
    target_compile_definitions(human_core_test PRIVATE HU_ENABLE_NEURAL_MEMORY=1)
endif()
target_compile_options(human_core_test PRIVATE -UNDEBUG)
if(Threads_FOUND)
    target_link_libraries(human_core_test PUBLIC Threads::Threads)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(human_core_test PUBLIC m)
endif()
if(HU_ENABLE_CLI)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_CLI=1)
endif()
if(HU_ENABLE_TELEGRAM)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_TELEGRAM=1)
endif()
if(HU_ENABLE_DISCORD)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_DISCORD=1)
endif()
if(HU_ENABLE_SLACK)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_SLACK=1)
endif()
if(HU_ENABLE_WHATSAPP)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_WHATSAPP=1)
endif()
if(HU_ENABLE_MATRIX)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_MATRIX=1)
endif()
if(HU_ENABLE_IRC)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_IRC=1)
endif()
if(HU_ENABLE_LINE)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_LINE=1)
endif()
if(HU_ENABLE_LARK)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_LARK=1)
endif()
if(HU_ENABLE_WEB)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_WEB=1)
endif()
if(HU_ENABLE_EMAIL)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_EMAIL=1)
endif()
if(HU_ENABLE_IMAP)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_IMAP=1)
endif()
if(HU_ENABLE_IMESSAGE)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_IMESSAGE=1)
endif()
if(HU_ENABLE_PWA)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_PWA=1)
endif()
if(HU_ENABLE_MATTERMOST)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_MATTERMOST=1)
endif()
if(HU_ENABLE_ONEBOT)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_ONEBOT=1)
endif()
if(HU_ENABLE_DINGTALK)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_DINGTALK=1)
endif()
if(HU_ENABLE_TEAMS)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_TEAMS=1)
endif()
if(HU_ENABLE_TWILIO)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_TWILIO=1)
endif()
if(HU_ENABLE_GOOGLE_CHAT)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_GOOGLE_CHAT=1)
endif()
if(HU_ENABLE_GOOGLE_RCS)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_GOOGLE_RCS=1)
endif()
if(HU_ENABLE_INSTAGRAM)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_INSTAGRAM=1)
endif()
if(HU_ENABLE_TWITTER)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_TWITTER=1)
endif()
if(HU_ENABLE_TIKTOK)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_TIKTOK=1)
endif()
if(HU_ENABLE_SONATA)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_SONATA=1)
endif()
if(HU_ENABLE_VOICE)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_VOICE_CHANNEL=1 HU_ENABLE_VOICE=1)
endif()
target_compile_definitions(human_core_test PRIVATE HU_HAS_SIGNAL=1 HU_HAS_NOSTR=1 HU_HAS_QQ=1 HU_HAS_MAIXCAM=1 HU_HAS_DISPATCH=1 HU_HAS_GMAIL=1 HU_HAS_MQTT=1 HU_HAS_THREAD_BINDING=1 HU_HAS_FACEBOOK=1 HU_HAS_INSTAGRAM=1 HU_HAS_TWITTER=1 HU_HAS_TIKTOK=1 HU_HAS_GOOGLE_RCS=1)
target_compile_definitions(human_core_test PRIVATE HU_HAS_NONE_ENGINE=1 HU_HAS_MARKDOWN_ENGINE=1)
if(HU_ENABLE_MEMORY_LRU_ENGINE)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_MEMORY_LRU_ENGINE=1)
endif()
if(HU_ENABLE_API_ENGINE)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_API_ENGINE=1)
endif()
if(HU_ENABLE_LUCID_ENGINE)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_LUCID_ENGINE=1)
endif()
if(HU_ENABLE_REDIS_ENGINE)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_REDIS_ENGINE=1)
endif()
if(HU_ENABLE_LANCEDB_ENGINE)
    target_compile_definitions(human_core_test PRIVATE HU_HAS_LANCEDB_ENGINE=1)
endif()
if(HU_ENABLE_SQLITE)
    target_compile_definitions(human_core_test PRIVATE HU_ENABLE_SQLITE=1)
    if(SQLITE3_FOUND)
        target_include_directories(human_core_test PRIVATE ${SQLITE3_INCLUDE_DIRS})
        target_link_libraries(human_core_test PUBLIC ${SQLITE3_LIBRARIES})
    elseif(SQLITE3_LIBRARY)
        target_link_libraries(human_core_test PUBLIC sqlite3)
    endif()
endif()

# Phase D Task D-1 (CF-7 RL SOTA): same HU_ML_SCRIPTS_DIR for the test
# library so hu_ml_resolve_script_path inside ml_scripts_dir.c falls back
# to the repo's scripts/ dir during test runs (the env override can still
# point tests at a temp dir for CWD-shadow regression tests).
target_compile_definitions(human_core_test PRIVATE "HU_ML_SCRIPTS_DIR=\"${HU_ROOT}/scripts\"")
if(HU_ENABLE_FIPS_CRYPTO)
    if(OpenSSL_FOUND)
        target_link_libraries(human_core_test PUBLIC OpenSSL::Crypto)
        target_compile_definitions(human_core_test PRIVATE HU_ENABLE_FIPS_CRYPTO=1)
    else()
        find_package(OpenSSL REQUIRED)
        target_link_libraries(human_core_test PUBLIC OpenSSL::Crypto)
        target_compile_definitions(human_core_test PRIVATE HU_ENABLE_FIPS_CRYPTO=1)
    endif()
endif()
if(HU_ENABLE_TLS)
    if(OpenSSL_FOUND)
        target_link_libraries(human_core_test PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(human_core_test PRIVATE HU_HAS_TLS=1)
    elseif(OPENSSL_SSL_LIB AND OPENSSL_CRYPTO_LIB AND OPENSSL_INCLUDE)
        target_include_directories(human_core_test PRIVATE ${OPENSSL_INCLUDE})
        target_link_libraries(human_core_test PUBLIC ${OPENSSL_SSL_LIB} ${OPENSSL_CRYPTO_LIB})
        target_compile_definitions(human_core_test PRIVATE HU_HAS_TLS=1)
    endif()
endif()
if(UNIX AND NOT WIN32)
    target_compile_definitions(human_core_test PRIVATE HU_GATEWAY_POSIX=1)
endif()
if(HU_ENABLE_TUI)
    target_compile_definitions(human_core_test PRIVATE HU_ENABLE_TUI=1)
    set(TERMBOX2_DIR "${CMAKE_SOURCE_DIR}/vendor/termbox2")
    if(EXISTS "${TERMBOX2_DIR}/termbox2.h")
        target_include_directories(human_core_test PRIVATE "${TERMBOX2_DIR}")
    endif()
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(human_core_test PRIVATE -ffunction-sections -fdata-sections -Wall -Wextra -Wpedantic -Wshadow -Wformat-security)
endif()
if(HU_ENABLE_ASAN)
    target_compile_options(human_core_test PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(human_core_test PRIVATE -fsanitize=address)
endif()

# --- Tests ---
option(HU_BUILD_TESTS "Build the test suite" ON)
if(NOT HU_BUILD_TESTS)
    return()
endif()
enable_testing()

set(HU_TEST_SOURCES
    tests/test_main.c
    tests/test_agent_definition.c
    tests/test_agent_git.c
    tests/test_bg_registry.c
    tests/test_canvas_render.c
    tests/test_canvas_e2e.c
    tests/test_canvas_persist.c
    tests/test_canvas_tool.c
    tests/test_consistency.c
    tests/test_mlx_provider.c
    tests/test_persona_fidelity.c
    tests/test_persona_fidelity_cross.c
    tests/test_persona_fidelity_judge.c
    tests/test_persona_fidelity_validator.c
    tests/test_persona_voice_validator.c
    tests/test_identity_short_circuit_validator.c
    tests/test_fact_extract_llm.c
    tests/test_fact_extract.c
    tests/test_personal_model.c
    tests/test_personal_model_atomic_save.c
    tests/test_persona_overlay_render.c
    # test_channel_overlay_apply.c — moved to gated block (needs both
    # HU_ENABLE_IMESSAGE + HU_ENABLE_TELEGRAM since it calls
    # hu_imessage_create AND hu_telegram_create). minimal-build doesn't
    # enable either, so unconditional registration breaks the link.
    tests/test_personal_model_simulation.c
    tests/test_personal_model_fidelity_v2.c
    # test_persona_encryption.c — moved to gated block below; references
    # hu_persona_crypt_* which only exists when HU_LIBSODIUM_FOUND.
    tests/test_persona_directive_channels.c
    tests/test_filler_recency.c
    tests/integration/test_sprint3_hybrid_recall.c
    tests/test_filler_pctt.c
    tests/test_hallucination_guard.c
    tests/test_humor_fw.c
    tests/test_markdown_loader.c
    tests/test_mcp_http_integration.c
    tests/test_self_improve.c
    tests/test_sycophancy_guard.c
    tests/test_task_store.c
    tests/test_trust_calibration.c
    tests/test_vision_ocr.c
    tests/test_channel_embeds.c
    tests/test_declarative_tools.c
    tests/test_skill_trust.c
    tests/test_agent_modules.c
    tests/test_tot_recursive.c
    tests/test_agent_subsystems.c
    tests/test_crypto.c
    tests/test_wasm.c
    tests/test_allocator.c
    tests/test_data_loader.c
    tests/test_idempotency.c
    tests/test_idempotency_hula_integration.c
    tests/test_json.c
    tests/test_string.c
    tests/test_io_secure.c
    tests/test_slice.c
    tests/test_tool_registry_honesty.c
    tests/test_memory.c
    tests/test_sql_transaction.c
    tests/test_normalize.c
    tests/test_memory_util.c
    tests/test_tunnel.c
    tests/test_gateway.c
    tests/test_auth.c
    tests/test_security.c
    tests/test_sensitivity.c
    tests/test_vault.c
    tests/test_vault_aead.c
    tests/test_provider.c
    tests/test_provider_http.c
    tests/test_ensemble.c
    tests/test_api_key.c
    tests/test_channel.c
    tests/test_channel_format.c
    tests/test_channel_rate_limit.c
    tests/test_channel_http.c
    tests/test_webhook_channel.c
    tests/test_cp_tasks.c
    tests/test_cp_canvas.c
    tests/test_vector_retrieval_remote.c
    tests/test_background_registry.c
    tests/test_anticipatory_state.c
    tests/test_tool.c
    tests/test_webhook.c
    tests/test_peripheral.c
    tests/test_peripheral_ctrl.c
    tests/test_value_learning.c
    tests/test_goal_engine.c
    tests/test_dispatch.c
    tests/test_policy_engine.c
    tests/test_vtables.c
    tests/test_e2e.c
    tests/test_e2e_conversation.c
    tests/test_e2e_agent_loop.c
    tests/test_subsystems.c
    tests/test_onboard_nextstep.c
    tests/test_daemon_cron.c
    tests/test_daemon_lifecycle.c
    tests/test_daemon_routing.c
    tests/test_daemon_proactive.c
    tests/test_daemon_trust.c
    tests/test_config_parse.c
    tests/test_config_migrate.c
    tests/test_adversarial.c
    tests/test_adversarial_detect.c
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
    tests/test_ws_integration.c
    tests/test_net_security.c
    tests/test_path_security.c
    tests/test_process_util.c
    tests/test_cron.c
    tests/test_cron_session_tools.c
    tests/test_channel_loop.c
    tests/test_rag.c
    tests/test_multimodal.c
    tests/test_multimodal_pipeline.c
    tests/test_multimodal_memory.c
    tests/test_multimodal_audio.c
    tests/test_multimodal_video.c
    tests/test_voice_duplex.c
    tests/test_turn_signal.c
    tests/test_sota_research.c
    tests/test_sota_wiring.c
    tests/test_sota_live_wiring.c
    tests/test_voice_rt_openai.c
    tests/test_voice_provider.c
    tests/test_voice_session.c
    tests/test_gemini_live.c
    tests/test_voice_factory_e2e.c
    tests/test_voice_streaming_e2e.c
    tests/test_mlx_local_voice.c
    tests/test_autonomy.c
    tests/test_retrieval.c
    tests/test_prompt.c
    tests/test_tool_search.c
    tests/test_vector.c
    tests/test_vector_full.c
    tests/test_lifecycle.c
    tests/test_observer.c
    tests/test_bth_metrics.c
    tests/test_session.c
    tests/test_bus.c
    tests/test_identity.c
    tests/test_channel_manager.c
    tests/test_new_modules.c
    tests/test_provider_all.c
    tests/test_channel_all.c
    tests/test_meta_common.c
    tests/test_channel_integration.c
    tests/test_gmail.c
    tests/test_imessage_extended.c
    tests/test_imessage_chatdb_fixture.c
    tests/test_imessage_adversarial.c
    tests/test_imessage_non_allowlisted.c
    tests/test_imessage_rich_link.c
    tests/test_imessage_react_contract.c
    tests/test_follow_up.c
    tests/test_config_extended.c
    tests/test_config_getters.c
    tests/test_config_validation.c
    tests/test_json_extended.c
    tests/test_security_extended.c
    tests/test_security_pipeline.c
    tests/test_core_extended.c
    tests/test_gateway_extended.c
    tests/test_gateway_voice.c
    tests/test_gateway_hula_traces.c
    tests/test_gateway_auth.c
    tests/test_pairing.c
    tests/test_agent_extended.c
    tests/test_agent_security.c
    tests/test_skills.c
    tests/test_ported_modules.c
    tests/test_doctor_imessage_diagnose.c
    tests/test_subagent.c
    tests/test_mcp.c
    tests/test_mcp_jsonrpc.c
    tests/test_voice.c
    tests/test_local_voice.c
    tests/test_vector_stores.c
    tests/test_cli.c
    tests/test_update.c
    tests/test_memory_engines_ext.c
    tests/test_memory_poisoning.c
    tests/test_runtime.c
    tests/test_runtime_bundle.c
    tests/test_util_modules.c
    tests/test_roadmap.c
    tests/test_new_features.c
    tests/test_ollama_integration.c
    tests/test_oauth.c
    tests/test_plugin.c
    tests/test_tenant.c
    tests/test_agent_teams.c
    tests/test_delegation.c
    tests/test_diagnostic_commands.c
    tests/test_untested_modules.c
    tests/test_modules_coverage.c
    tests/test_coverage_new.c
    tests/test_context.c
    tests/test_qmd.c
    tests/test_terminal.c
    tests/test_tavily.c
    tests/test_awareness.c
    tests/test_intelligence.c
    tests/test_intelligence_reflection.c
    tests/test_protective.c
    tests/test_humor.c
    tests/test_authentic.c
    tests/test_rag_pipeline.c
    tests/test_persona_training.c
    tests/test_behavioral.c
    tests/test_context_ext.c
    tests/test_entropy_gate.c
    tests/test_episodic.c
    tests/test_reflection.c
    tests/test_input_guard.c
    tests/test_externalization.c
    tests/test_conversation.c
    tests/test_vision.c
    tests/test_ab_response.c
    tests/test_event_extract.c
    tests/test_stm.c
    tests/test_emotional_graph.c
    tests/test_comfort_patterns.c
    tests/test_emotional_moments.c
    tests/test_emotional_state.c
    tests/test_contact_style_overlay.c
    tests/test_graph.c
    tests/test_w1_bitemporal.c
    tests/test_w2_autodream.c
    tests/test_w3_multigraph.c
    tests/test_w4_verifier.c
    tests/test_w5_persona_deltas.c
    tests/test_persona_delta_observer.c
    tests/test_world_model_bridge.c
    tests/test_signal_channel_wire.c
    tests/test_daemon_housekeeping.c
    tests/test_orphan_channel_audit.c
    tests/test_verifier_metrics.c
    tests/test_output_validator.c
    tests/test_chain_failure_paths.c
    tests/test_agent_fail_path_regressions.c
    tests/test_validators_builtin.c
    tests/test_pattern_c_paths.c
    tests/test_validators_persona_safety.c
    tests/test_validator_reject_discards.c
    tests/test_validator_telemetry.c
    tests/test_validator_chain_cache.c
    tests/test_daemon_e2e_validator.c
    tests/test_response_guard.c
    tests/test_response_guard_retry.c
    tests/test_w6_e2e_adversarial.c
    tests/test_w7_memory_facade.c
    tests/test_w8_belief_layer.c
    tests/test_w9_world_model.c
    tests/test_w10_neural_memory.c
    tests/test_w11_self_rag.c
    tests/test_w12_planner.c
    tests/test_w12_verifier_loop.c
    # W13/W14 learning-loop tests and W16 eval-CLI tests reference
    # hu_learner_* APIs that only exist when HU_ENABLE_LEARNING is on
    # (default OFF). Gated below in the if(HU_ENABLE_LEARNING) block.
    # Listing them here would break minimal-build / no-sqlite /
    # cross-arm64 with undefined-reference linker errors.
    tests/test_w15_backup_restore.c
    tests/test_w14_scheduler.c
    tests/test_w15_keystore.c
    tests/test_encrypted_store.c
    tests/test_b11_pressure_history_e2e.c
    tests/test_b9_user_sim_agent_turn_e2e.c
    tests/test_personal_model_contradicts.c
    tests/test_channel_trust.c
    tests/test_minja_guard.c
    tests/test_frontier_prompt.c
    tests/test_w11_abstain_calibration.c
    tests/test_fast_capture.c
    tests/test_promotion.c
    tests/test_consolidation.c
    tests/test_verify_claim.c
    tests/test_deep_extract.c
    tests/test_commitment.c
    tests/test_pattern_radar.c
    tests/test_proactive.c
    tests/test_proactive_throttle.c
    tests/test_inner_thoughts.c
    tests/test_weather_awareness.c
    tests/test_timing.c
    tests/test_calibration.c
    tests/test_behavioral_clone.c
    tests/test_governor.c
    tests/test_activation_steering.c
    tests/test_relationship_dynamics.c
    tests/test_arbitrator.c
    tests/test_planning.c
    tests/test_rel_dynamics.c
    tests/test_prospective.c
    tests/test_prospective_memory.c
    tests/test_emotional_residue.c
    tests/test_consolidation_engine.c
    tests/test_conv_goals.c
    tests/test_knowledge.c
    tests/test_usage.c
    tests/test_cognitive.c
    tests/test_deep_memory.c
    tests/test_compression.c
    tests/test_proactive_ext.c
    tests/test_degradation.c
    tests/test_memory_degradation.c
    tests/test_self_awareness.c
    tests/test_superhuman.c
    tests/test_contact_graph.c
    tests/test_tool_call_parser.c
    tests/test_tool_router.c
    tests/test_dag.c
    tests/test_hula.c
    tests/test_hula_golden.c
    tests/test_workflow_event.c
    tests/test_sota_features.c
    tests/test_mood.c
    tests/test_bth_e2e.c
    tests/test_style_tracker.c
    tests/test_theory_of_mind.c
    tests/test_anticipatory.c
    tests/test_visual_content.c
    tests/test_media_gen.c
    tests/test_opinions.c
    tests/test_life_chapters.c
    tests/test_social_graph.c
    tests/test_skill_system.c
    tests/test_feeds.c
    tests/test_apple_feeds.c
    tests/test_social_feeds.c
    tests/test_feed_processor.c
    tests/test_feed_awareness.c
    tests/test_forgetting_curve.c
    tests/test_weather_fetch.c
    tests/test_save_for_later.c
    tests/test_news_health_email.c
    tests/test_feedback.c
    tests/test_intelligence_skills.c
    tests/test_skill_unified.c
    tests/test_intelligence_cycle.c
    tests/test_reflection_advanced.c
    tests/test_privacy_audit.c
    tests/test_collab_planning.c
    # ML-coupled tests (test_ml.c, test_ml_cli_actually_trains.c,
    # test_ml_fidelity_judgment.c, test_dpo_judge_naming.c, test_dp_sgd.c,
    # test_lora.c, test_training_data*.c, test_agent_trainer.c) are
    # added below in the if(HU_ENABLE_ML) gate (CMakeLists.txt:3349+).
    # The sprint-7 main-merge re-introduced them here unconditionally,
    # regressing my round-10/12 fix. Re-applied: minimal-build /
    # no-sqlite / cross-arm64 variants reference hu_agent_t fields like
    # m3_adapter that only exist when HU_ENABLE_ML is on. The gate-
    # symmetry CI check (PR #113 round 14) would also catch this.
    tests/test_otel_traces.c
    tests/test_mcp_transport.c
    tests/test_mcp_transport_sse.c
    tests/test_mcp_audit.c
    tests/test_multigraph.c
    tests/test_memory_graph.c
    tests/test_experience.c
    tests/test_experience_engine.c
    tests/test_intelligence_wiring.c
    tests/test_prove_e2e.c
    tests/test_self_improve_loop.c
    tests/test_weakness.c
    tests/test_anti_sycophancy.c
    tests/test_mutual_tom.c
    tests/test_opinion_history.c
    tests/test_trust.c
    tests/test_sota_humanness.c
    tests/test_distiller.c
    tests/test_model_router.c
    tests/test_humanness_context.c
    tests/test_plan_executor.c
    tests/test_planner_mcts_wiring.c
    tests/test_cdp.c
    tests/test_emotional_cognition.c
    tests/test_emotional_contagion.c
    tests/test_evolving_cognition.c
    tests/test_metacognition.c
    tests/test_humanness_frontiers.c
    tests/test_skill_routing.c
    tests/test_dual_process.c
    tests/test_repair.c
)
if(HU_ENABLE_FEEDS)
    list(APPEND HU_TEST_SOURCES tests/test_google_feeds.c tests/test_music_feeds.c
        tests/test_research_feeds.c tests/test_research_executor.c)
endif()
list(APPEND HU_TEST_SOURCES tests/test_persona.c tests/test_circadian.c tests/test_relationship.c tests/test_replay.c tests/test_style_clone.c tests/test_life_sim.c tests/test_persona_mood.c tests/test_persona_feedback.c tests/test_persona_cli.c tests/test_voice_maturity.c tests/test_style_mirror.c tests/test_style_learner.c tests/test_temporal.c tests/test_inner_world.c tests/test_persona_eval.c tests/test_persona_filler_roundtrip.c tests/test_style_critique_patterns.c tests/test_style_self_critique.c)
# Moment Context Decision Layer (Task 0.2) — paired with src/persona/moment.c
# and src/persona/moment_render.c above; unconditional per gate-symmetry rule.
list(APPEND HU_TEST_SOURCES tests/test_moment_compose.c tests/test_moment_render.c)
list(APPEND HU_TEST_SOURCES tests/test_behavior_policy.c tests/test_behavior_dialog_act.c tests/test_behavior_affect.c tests/test_behavior_change.c tests/test_behavior_safety.c tests/test_behavior_prompt.c tests/test_behavior_support_strategy.c tests/test_behavior_trust.c tests/test_behavior_corpora.c tests/test_user_sim.c tests/test_tom_scenario_b8.c tests/test_chronotype.c tests/test_behavior_pressure.c tests/test_behavior_trust_prompt.c tests/test_longmemeval.c tests/test_sycophancy_pack.c tests/test_user_sim_scenario.c)
list(APPEND HU_TEST_SOURCES tests/test_cartesia.c)
list(APPEND HU_TEST_SOURCES tests/test_cartesia_stream.c)
list(APPEND HU_TEST_SOURCES tests/test_voice_clone.c)
list(APPEND HU_TEST_SOURCES tests/test_transcript_prep.c)
list(APPEND HU_TEST_SOURCES tests/test_send_voice_message.c)
list(APPEND HU_TEST_SOURCES tests/test_voice_message_integration.c)
if(HU_ENABLE_CARTESIA)
    list(APPEND HU_TEST_SOURCES tests/test_audio_pipeline.c tests/test_voice_decision.c tests/test_emotion_map.c)
endif()
if(HU_ENABLE_AUTHENTIC)
    list(APPEND HU_TEST_SOURCES tests/test_cognitive_load.c tests/test_phase9_integration.c)
endif()

# Music preview tests (pure computation — always compiled)
list(APPEND HU_TEST_SOURCES tests/test_music.c)

# Paperclip tests (requires libcurl)
if(HU_ENABLE_CURL)
    list(APPEND HU_TEST_SOURCES tests/test_paperclip.c)
endif()

list(APPEND HU_TEST_SOURCES tests/test_memory_features.c)
list(APPEND HU_TEST_SOURCES tests/test_agi_frontiers.c tests/test_world_model_graph.c tests/test_mcts_planner.c)
list(APPEND HU_TEST_SOURCES tests/test_orchestrator.c)
list(APPEND HU_TEST_SOURCES tests/test_swarm_execution.c tests/test_dynamic_decomposition.c)
list(APPEND HU_TEST_SOURCES tests/test_agent_matching.c tests/test_agent_communication.c)
list(APPEND HU_TEST_SOURCES tests/test_world_simulation.c tests/test_world_context.c)
list(APPEND HU_TEST_SOURCES tests/test_agent_registry.c)
list(APPEND HU_TEST_SOURCES tests/test_pwa.c)
list(APPEND HU_TEST_SOURCES tests/test_integration.c)
list(APPEND HU_TEST_SOURCES tests/test_hook_pipeline.c)
list(APPEND HU_TEST_SOURCES tests/test_agent_dispatch_hooks.c)
list(APPEND HU_TEST_SOURCES tests/test_config_reload.c)
list(APPEND HU_TEST_SOURCES tests/test_plugin_hooks.c)
# Phase 2 Task 10 (RL SOTA): hu_reaction_event_t + iMessage/Slack normalizers.
list(APPEND HU_TEST_SOURCES tests/test_reaction_event.c)
# Phase 2 Task 11 (RL SOTA): hu_imessage_poll_reactions tapback inbound poll.
# Default-skipped stub; opt in to the real chat.db path with -DHU_HAVE_CHATDB=1
# (compile-time define) + HU_CHATDB env var (runtime path to a real chat.db).
list(APPEND HU_TEST_SOURCES tests/test_imessage_reactions.c)
# Phase 1a of docs/plans/2026-05-18-imessage-sota.md: pure-synthesis tests
# for src/channels/imessage_ingest.c. No chat.db / Apple deps; tests run
# unconditionally and stay green on every build variant.
list(APPEND HU_TEST_SOURCES tests/test_imessage_ingest.c)
# Phase 1c end-to-end smoke: builds an in-memory chat.db, polls reactions,
# asserts personal_model state mutates. Internally guarded by
# HU_HAS_IMESSAGE + HU_ENABLE_SQLITE; stub runner on other build variants.
list(APPEND HU_TEST_SOURCES tests/test_imessage_personal_model_e2e.c)
# Phase 3: bplist00 parser tests. Pure-C, no gates needed.
list(APPEND HU_TEST_SOURCES tests/test_bplist.c)
# Phase 3 completion: daemon iMessage observer tick (audio + edits + group).
list(APPEND HU_TEST_SOURCES tests/test_imessage_observer.c)
# Phase 6: schema-version probe + drift canary tests.
list(APPEND HU_TEST_SOURCES tests/test_imessage_schema.c)
# Phase 4: typedstream attribute-run extractor tests. Pure-C, no gates.
list(APPEND HU_TEST_SOURCES tests/test_typedstream.c)
# Phase 5: per-balloon payload decoder tests. Pinned privacy contracts
# (no money / no coordinates).
list(APPEND HU_TEST_SOURCES tests/test_imessage_balloon_decode.c)
# Phase 2 of docs/plans/2026-05-18-imessage-sota.md: cross-channel reaction
# emit paths for Discord + Telegram. Internal #ifdef HU_HAS_* guards with
# stub runners per .claude/rules/test-source-gate-symmetry.md.
list(APPEND HU_TEST_SOURCES tests/test_discord_reactions.c)
list(APPEND HU_TEST_SOURCES tests/test_telegram_reactions.c)
# Phase 2 Task 12 (RL SOTA): Slack reaction_added/removed webhook branch
# (parse + filter + normalize via hu_slack_handle_reaction_event_for_test).
list(APPEND HU_TEST_SOURCES tests/test_slack_reactions.c)
# Phase 2 Task 13 (RL SOTA): hu_reaction_handler_handle_event — event ⇒
# dpo_pairs row + per-turn signal flag (consumed by Task 14's agent_turn
# substring-heuristic guard). Test calls sqlite3_open/_prepare_v2/_step
# directly to seed the dpo_pairs collector, so gate on HU_ENABLE_SQLITE
# to keep minimal-build / no-sqlite / cross-arm64 builds linkable.
if(HU_ENABLE_SQLITE)
    list(APPEND HU_TEST_SOURCES tests/test_reaction_handler_e2e.c)
endif()

# SOTA capability tests
list(APPEND HU_TEST_SOURCES
    tests/test_gvr.c
    tests/test_provider_degradation.c
    tests/test_apple_provider.c
    tests/test_escalate.c
    tests/test_a2a.c
    tests/test_eval.c
    tests/test_turing_score.c
    tests/test_turing_adversarial.c
    tests/test_eval_judge.c
    tests/test_eval_benchmarks.c
    tests/test_token_budget.c
    tests/test_data_quality.c
    tests/test_tool_validation.c
    tests/test_tool_ask_user.c
    tests/test_task_manager.c
    tests/test_task_tools.c
    tests/test_otlp.c
    tests/test_mar.c
    tests/test_mem_policy.c
    tests/test_prompt_optimizer.c
    tests/test_chaos.c
    tests/test_checkpoint.c
    tests/test_approval_gate.c
    tests/test_workflow_commands.c
    tests/test_scratchpad.c
    tests/test_mcp_resources.c
    tests/test_mcp_resource_tools.c
    tests/test_context_engine.c
    tests/test_exec_env.c
    tests/test_channel_monitor.c
    tests/test_channel_class.c
    tests/test_context_engine_rag.c
    tests/test_doctor_fix.c
    tests/test_doctor_personalization_warning.c
    tests/test_doctor_install.c
    tests/test_humanness.c
    tests/test_opinions_persistence.c
    tests/test_plugin_discovery.c
    tests/test_skill_scaffold.c
    tests/test_eval_runner.c
    tests/test_eval_history.c
    tests/test_eval_shape.c
    tests/test_corrective_rag.c
    tests/test_adaptive_rag.c
    tests/test_self_rag.c
    tests/test_memory_tiers.c
    tests/test_process_reward.c
    tests/test_dpo.c
    # test_dpo_miner.c is added in the if(HU_ENABLE_ML) block — its
    # impl (src/ml/dpo_miner.c) is ML-gated for hu_pii_redact (round
    # 11). Listing the test here would break audit-integrity /
    # static-analysis / no-ML builds.
    tests/test_molora_router.c
    tests/test_contact_send_recency.c
    # NOTE: 3 test files briefly registered during the PR #115 merge-
    # conflict resolution to satisfy linker refs from test_main.c are
    # de-registered here. All three were ghost-tests: present in
    # tests/ on the Sprint 11 HEAD side but never built (not in
    # main's CMakeLists, not called in main's test_main.c), exercising
    # features that aren't fully implemented on either branch:
    #   - tests/test_config_identity_links.c (identity_links JSON parser)
    #   - tests/test_memory_session_scoping.c (session-scoped writes)
    #   - tests/test_imessage_outbound_dedup.c (channel dedup behavior;
    #     test passes filtered but fails in full-suite due to state
    #     pollution from prior tests — the underlying dedup logic
    #     doesn't survive cross-test state leakage)
    # Building all three produced 11 failures in minimal-build. The
    # corresponding declarations and calls in test_main.c are also
    # removed in this commit. Re-register when the underlying features
    # are implemented and the tests reliably pass in full-suite order.
    tests/test_sota_e2e.c
    tests/test_sota_adversarial.c
    tests/test_otel.c
    tests/test_cot_audit.c
    tests/test_moderation.c
    tests/test_companion_safety.c
    tests/test_code_sandbox.c
    tests/test_compaction_hierarchical.c
    tests/test_computer_use.c
    tests/test_image_gen.c
    tests/test_visual_grounding.c
    tests/test_browser_use.c
    tests/test_gui_agent.c
    tests/test_lsp.c
    tests/test_webrtc.c
    tests/test_embedded_provider.c
    tests/test_llamacpp_provider.c
    tests/test_llamacpp_factory_config.c
    tests/test_llamacpp_sampling.c
    tests/test_llamacpp_kvcache.c
    tests/test_llamacpp_decode.c
    tests/test_llamacpp_lora_hotswap.c
    tests/test_llamacpp_chat_metal.c
    tests/test_llamacpp_best_of_n.c
    tests/test_doctor_best_of_n_warning.c
    tests/test_coreml_provider.c
    tests/test_forgetting.c
    tests/test_bootstrap.c
    tests/test_thread_pool.c
    tests/test_permission.c
    tests/test_shell_sandbox.c
    tests/test_mcp_manager.c
    tests/test_compaction_structured.c
    tests/test_session_persist.c
    tests/test_instruction_discover.c
    tests/test_adversarial_memory_safety.c
    tests/test_adversarial_injection.c
    tests/test_adversarial_dos_protocol.c
    tests/test_adversarial_concurrency.c
    tests/test_adversarial_integration.c
    tests/test_stop_sequences.c
    tests/test_structured_output.c
)

# ML + RL SOTA Phase 2–4 tests — only when HU_ENABLE_ML is ON.  CI
# build-and-test leaves ML OFF; human_tests must not force HU_ENABLE_ML=1
# nor link test TUs that reference hu_ml_* / hu_grpo_* / hu_kl_* symbols.
if(HU_ENABLE_ML)
    list(APPEND HU_TEST_SOURCES
        tests/test_ml.c
        tests/test_mlx_admin.c
        tests/test_ml_cli_actually_trains.c
        tests/test_ml_fidelity_judgment.c
        tests/test_dp_sgd.c
        tests/test_dpo_miner.c
        tests/test_dpo_judge_naming.c
        tests/test_rl_trainer.c
        tests/test_policy_logprobs.c
        tests/test_reference_model.c
        tests/test_dpo_real_loss.c
        tests/test_dpo_real_e2e.c
        tests/test_dpo_real_mlx.c
        tests/test_value_head.c
        tests/test_cli_dpo.c
        tests/test_reward_model_train.c
        tests/test_reward_model_inference.c
        tests/test_kto_loss.c
        tests/test_ml_scripts_dir.c
        tests/test_kl_divergence.c
        tests/test_grpo_loss.c
        tests/test_grpo_mlx.c
        tests/test_grpo_huml.c
        tests/test_grpo_e2e.c
        tests/test_rollout.c
        tests/test_reward_source.c
        tests/test_cli_grpo.c
        tests/test_lora.c
        tests/test_training_data.c
        tests/test_training_data_extractor.c
        tests/test_training_data_quality.c
        tests/test_dpo_extractor_integration.c
        tests/test_agent_trainer.c)
endif()

# test_channel_overlay_apply.c needs both iMessage AND Telegram symbols.
if(HU_ENABLE_IMESSAGE AND HU_ENABLE_TELEGRAM)
    list(APPEND HU_TEST_SOURCES tests/test_channel_overlay_apply.c)
endif()

# Persona encryption-at-rest tests — only when libsodium was located.
# Calls hu_persona_crypt_* which is only compiled inside the matching
# if(HU_LIBSODIUM_FOUND) block near line ~2605 (production) + ~2644
# (test library). Registered HERE because `set(HU_TEST_SOURCES ...)`
# at ~2834 happens AFTER the libsodium target_sources block — appends
# before the set() get silently discarded (Bugbot 8b687b9c).
if(HU_LIBSODIUM_FOUND)
    list(APPEND HU_TEST_SOURCES tests/test_persona_encryption.c)
endif()

# W13 learning loop — only when HU_ENABLE_LEARNING is ON (requires ML).
if(HU_ENABLE_LEARNING)
    list(APPEND HU_TEST_SOURCES
        tests/test_w13_learner.c
        tests/test_w14_runners.c
        tests/test_w14_lora_retrain.c
        tests/test_w14_dual_lora.c
        tests/test_w16_evaluation.c
        tests/test_w16_eval_cli.c
        tests/test_learner_bridge.c
        tests/test_v2_e2e_adversarial.c
        tests/test_v2_wiring_e2e.c)
endif()

# Phase 5 Task 2 (RL SOTA) — bootstrap_ci tests live in the same gated
# bucket as the impl above.  Mirrored guard so the test TU is only
# compiled when HU_ENABLE_RL_FULL is ON (default release/dev keep the
# pre-Phase-5 test surface untouched).
if(HU_ENABLE_RL_FULL)
    list(APPEND HU_TEST_SOURCES tests/test_bootstrap_ci.c)
    # Phase 5 Task 3 (RL SOTA) — hu_eval_judge_external_t tests
    # (canned cycle, verdict round-trip, zero-verdicts rejection,
    # Apple FM / Gemini Nano stubs return HU_ERR_NOT_SUPPORTED, ASan
    # leak-free deinit). Same gating as Task 2.
    list(APPEND HU_TEST_SOURCES tests/test_eval_judge_external.c)
    list(APPEND HU_TEST_SOURCES tests/test_leaderboard.c)
    list(APPEND HU_TEST_SOURCES tests/test_eval_gate.c)
    list(APPEND HU_TEST_SOURCES tests/test_stock_baseline.c)
    list(APPEND HU_TEST_SOURCES tests/test_apple_fm_client.c)
    list(APPEND HU_TEST_SOURCES tests/test_gemini_nano_client.c)
    list(APPEND HU_TEST_SOURCES tests/test_competitive_harness.c)
    list(APPEND HU_TEST_SOURCES tests/test_persona_rollout.c)
    list(APPEND HU_TEST_SOURCES tests/test_cli_eval_phase5.c)
    list(APPEND HU_TEST_SOURCES tests/test_cli_demo_evidence.c)
    list(APPEND HU_TEST_SOURCES tests/test_lora_ab_require_positive_forwards_to_gate.c)
    list(APPEND HU_TEST_SOURCES tests/test_lora_training_runner_eval_gate.c)
    list(APPEND HU_TEST_SOURCES tests/test_daemon_reaction_poll_wiring.c)
    list(APPEND HU_TEST_SOURCES tests/test_daemon_reaction_poll_production.c)
    list(APPEND HU_TEST_SOURCES tests/test_lora_training_runner_proof_directory.c)
    list(APPEND HU_TEST_SOURCES tests/test_e2e_rl_loop.c)
endif()

add_executable(human_tests ${HU_TEST_SOURCES} wasm/wasm_alloc.c src/bootstrap.c)
target_link_libraries(human_tests human_core_test)
target_include_directories(human_tests PRIVATE ${HU_ROOT}/tests ${HU_ROOT}/tests/include
    ${HU_ROOT}/src/gateway
    ${HU_ROOT}/src/channels
    # Phase 4 Task 3 (RL SOTA): tests/test_grpo_loss.c needs
    # src/ml/grpo_loss_priv.h (private header — NOT exported via
    # include/ to keep the public surface lean).  Same pattern used by
    # src/ml/reward_model.c + src/ml/reward_model_train.c for the
    # Phase 3 reward_model_priv.h (H2 fix).
    ${HU_ROOT}/src/ml)
target_compile_definitions(human_tests PRIVATE "HU_EVAL_SUITES_DIR=\"${HU_ROOT}/eval_suites\"")
# Phase D Task D-1 (CF-7 RL SOTA): same HU_ML_SCRIPTS_DIR for tests.
target_compile_definitions(human_tests PRIVATE "HU_ML_SCRIPTS_DIR=\"${HU_ROOT}/scripts\"")
target_compile_definitions(human_tests PRIVATE "HU_TEST_DATA_DIR=\"${HU_ROOT}/data\"")
# HU_HAS_PERSONA always set for tests: test lib has persona (main or extra), struct layout must match
target_compile_definitions(human_tests PRIVATE HU_IS_TEST=1 HU_GATEWAY_TEST_MODE=1 HU_HAS_NONE_ENGINE=1 HU_HAS_CRON=1 HU_HAS_PUSH=1 HU_HAS_UPDATE=1 HU_HAS_SKILLS=1 HU_HAS_PERSONA=1 HU_HAS_RUNTIME_EXOTIC=1)
# Mirror HU_HAS_LIBSODIUM onto human_tests so tests/test_main.c's
# `#ifdef HU_HAS_LIBSODIUM` call-site guard for run_persona_encryption_tests()
# actually activates. Without this, the runner is registered (because
# test_persona_encryption.c is in HU_TEST_SOURCES under HU_LIBSODIUM_FOUND)
# but the call site stays `#endif`-disabled and the tests silently skip.
# Bugbot 8b687b9c caught the half-wired guard.
if(HU_LIBSODIUM_FOUND)
    target_compile_definitions(human_tests PRIVATE HU_HAS_LIBSODIUM=1)
endif()
if(HU_ENABLE_APPLE_INTELLIGENCE)
    target_compile_definitions(human_tests PRIVATE HU_ENABLE_APPLE_INTELLIGENCE=1)
endif()
if(HU_ENABLE_COREML)
    target_compile_definitions(human_tests PRIVATE HU_ENABLE_COREML=1)
endif()
target_compile_definitions(human_tests PRIVATE HU_ENABLE_PERSONA=1)
if(HU_ENABLE_CARTESIA)
    target_compile_definitions(human_tests PRIVATE HU_ENABLE_CARTESIA=1)
endif()
if(HU_ENABLE_FEEDS)
    target_compile_definitions(human_tests PRIVATE HU_ENABLE_FEEDS=1)
endif()
if(HU_ENABLE_AUTHENTIC)
    target_compile_definitions(human_tests PRIVATE HU_ENABLE_AUTHENTIC=1)
endif()
if(UNIX AND NOT WIN32)
    target_compile_definitions(human_tests PRIVATE HU_GATEWAY_POSIX=1)
endif()
if(HU_ENABLE_TUI)
    target_compile_definitions(human_tests PRIVATE HU_ENABLE_TUI=1)
endif()
if(HU_ENABLE_CURL)
    target_compile_definitions(human_tests PRIVATE HU_ENABLE_CURL=1)
endif()
if(HU_ENABLE_SQLITE)
    target_compile_definitions(human_tests PRIVATE HU_ENABLE_SQLITE=1)
    if(SQLITE3_FOUND)
        target_include_directories(human_tests PRIVATE ${SQLITE3_INCLUDE_DIRS})
    endif()
endif()
if(HU_ENABLE_TELEGRAM)
    target_compile_definitions(human_tests PRIVATE HU_HAS_TELEGRAM=1)
endif()
if(HU_ENABLE_DISCORD)
    target_compile_definitions(human_tests PRIVATE HU_HAS_DISCORD=1)
endif()
if(HU_ENABLE_SLACK)
    target_compile_definitions(human_tests PRIVATE HU_HAS_SLACK=1)
endif()
if(HU_ENABLE_WHATSAPP)
    target_compile_definitions(human_tests PRIVATE HU_HAS_WHATSAPP=1)
endif()
if(HU_ENABLE_FACEBOOK)
    target_compile_definitions(human_tests PRIVATE HU_HAS_FACEBOOK=1)
endif()
if(HU_ENABLE_MATRIX)
    target_compile_definitions(human_tests PRIVATE HU_HAS_MATRIX=1)
endif()
if(HU_ENABLE_IRC)
    target_compile_definitions(human_tests PRIVATE HU_HAS_IRC=1)
endif()
if(HU_ENABLE_LINE)
    target_compile_definitions(human_tests PRIVATE HU_HAS_LINE=1)
endif()
if(HU_ENABLE_LARK)
    target_compile_definitions(human_tests PRIVATE HU_HAS_LARK=1)
endif()
if(HU_ENABLE_WEB)
    target_compile_definitions(human_tests PRIVATE HU_HAS_WEB=1)
endif()
if(HU_ENABLE_EMAIL)
    target_compile_definitions(human_tests PRIVATE HU_HAS_EMAIL=1)
endif()
if(HU_ENABLE_IMAP)
    target_compile_definitions(human_tests PRIVATE HU_HAS_IMAP=1)
endif()
if(HU_ENABLE_NOSTR)
    target_compile_definitions(human_tests PRIVATE HU_HAS_NOSTR=1)
endif()
if(HU_ENABLE_MQTT)
    target_compile_definitions(human_tests PRIVATE HU_HAS_MQTT=1)
endif()
if(HU_ENABLE_IMESSAGE)
    target_compile_definitions(human_tests PRIVATE HU_HAS_IMESSAGE=1)
    if(HU_IMESSAGE_TAPBACK_ENABLED)
        target_compile_definitions(human_tests PRIVATE HU_IMESSAGE_TAPBACK_ENABLED=1)
    endif()
endif()
if(HU_ENABLE_PWA)
    target_compile_definitions(human_tests PRIVATE HU_HAS_PWA=1)
endif()
if(HU_ENABLE_MATTERMOST)
    target_compile_definitions(human_tests PRIVATE HU_HAS_MATTERMOST=1)
endif()
if(HU_ENABLE_ONEBOT)
    target_compile_definitions(human_tests PRIVATE HU_HAS_ONEBOT=1)
endif()
if(HU_ENABLE_DINGTALK)
    target_compile_definitions(human_tests PRIVATE HU_HAS_DINGTALK=1)
endif()
if(HU_ENABLE_TEAMS)
    target_compile_definitions(human_tests PRIVATE HU_HAS_TEAMS=1)
endif()
if(HU_ENABLE_TWILIO)
    target_compile_definitions(human_tests PRIVATE HU_HAS_TWILIO=1)
endif()
if(HU_ENABLE_GOOGLE_CHAT)
    target_compile_definitions(human_tests PRIVATE HU_HAS_GOOGLE_CHAT=1)
endif()
if(HU_ENABLE_GOOGLE_RCS)
    target_compile_definitions(human_tests PRIVATE HU_HAS_GOOGLE_RCS=1)
endif()
if(HU_ENABLE_INSTAGRAM)
    target_compile_definitions(human_tests PRIVATE HU_HAS_INSTAGRAM=1)
endif()
if(HU_ENABLE_TWITTER)
    target_compile_definitions(human_tests PRIVATE HU_HAS_TWITTER=1)
endif()
if(HU_ENABLE_TIKTOK)
    target_compile_definitions(human_tests PRIVATE HU_HAS_TIKTOK=1)
endif()
if(HU_ENABLE_GMAIL)
    target_compile_definitions(human_tests PRIVATE HU_HAS_GMAIL=1)
endif()
if(HU_ENABLE_DISPATCH)
    target_compile_definitions(human_tests PRIVATE HU_HAS_DISPATCH=1)
endif()
if(HU_ENABLE_VOICE)
    target_compile_definitions(human_tests PRIVATE HU_ENABLE_VOICE=1 HU_HAS_VOICE_CHANNEL=1)
endif()
if(HU_ENABLE_LUCID_ENGINE)
    target_compile_definitions(human_tests PRIVATE HU_HAS_LUCID_ENGINE=1)
endif()
if(HU_ENABLE_LANCEDB_ENGINE)
    target_compile_definitions(human_tests PRIVATE HU_HAS_LANCEDB_ENGINE=1)
endif()
if(HU_ENABLE_POSTGRES)
    target_compile_definitions(human_tests PRIVATE HU_ENABLE_POSTGRES=1)
endif()
if(HU_ENABLE_PGVECTOR)
    target_compile_definitions(human_tests PRIVATE HU_ENABLE_PGVECTOR=1)
endif()
if(HU_ENABLE_REDIS_ENGINE)
    target_compile_definitions(human_tests PRIVATE HU_HAS_REDIS_ENGINE=1 HU_ENABLE_REDIS_ENGINE=1)
endif()
if(HU_ENABLE_API_ENGINE)
    target_compile_definitions(human_tests PRIVATE HU_HAS_API_ENGINE=1)
endif()
if(HU_ENABLE_FIPS_CRYPTO)
    target_compile_definitions(human_tests PRIVATE HU_ENABLE_FIPS_CRYPTO=1)
endif()
if(HU_ENABLE_ML)
    target_compile_definitions(human_tests PRIVATE HU_ENABLE_ML=1)
endif()
if(HU_ENABLE_LEARNING)
    target_compile_definitions(human_tests PRIVATE HU_ENABLE_LEARNING=1)
endif()
# Phase 1 (RL SOTA) — propagate HU_ENABLE_LLAMACPP into the test build so
# .c sources can gate stub-only assertions with #if !defined(HU_ENABLE_LLAMACPP).
# The human_core target already has this define under HU_LLAMACPP_FOUND, but
# its PRIVATE scope doesn't reach test object files compiled directly into
# human_tests.
if(HU_ENABLE_LLAMACPP)
    target_compile_definitions(human_tests PRIVATE HU_ENABLE_LLAMACPP=1)
    if(HU_LLAMACPP_INCLUDE_DIRS)
        target_include_directories(human_tests PRIVATE ${HU_LLAMACPP_INCLUDE_DIRS})
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/llama.cpp/CMakeLists.txt")
        # Match line 2855's plain signature (target_link_libraries(target lib)
        # without PRIVATE/PUBLIC keyword) — CMake forbids mixing.
        target_link_libraries(human_tests llama)
    elseif(HU_LLAMACPP_LIBRARIES)
        target_link_libraries(human_tests ${HU_LLAMACPP_LIBRARIES})
    endif()
endif()

# Phase 2 Task 6 (RL SOTA) — propagate HU_HAVE_MLX_LM into human_core and
# human_tests. Plan body (lines 1838–1844) shows a single block "near
# HU_ENABLE_LLAMACPP option"; structurally that block has to live AFTER
# both targets are declared (mirroring HU_ENABLE_LLAMACPP's own split:
# option at line 46, target_compile_definitions on human_tests right
# above this comment). The flag is OFF by default; flipping it ON
# bypasses the dummy-adapter shortcut in src/ml/dpo_real_mlx.c and
# activates the real Gemma DPO test path (Task 7).
if(HU_HAVE_MLX_LM)
    target_compile_definitions(human_core PRIVATE HU_HAVE_MLX_LM=1)
    target_compile_definitions(human_core_test PRIVATE HU_HAVE_MLX_LM=1)
    target_compile_definitions(human_tests PRIVATE HU_HAVE_MLX_LM=1)
endif()

# Phase 3 Task 7 (RL SOTA) — propagate HU_HAVE_MLX_LM_KTO into targets.
# OFF by default; flipping ON bypasses the dummy-adapter shortcut in
# src/ml/kto_mlx.c and activates the real MLX KTO subprocess path.
if(HU_HAVE_MLX_LM_KTO)
    target_compile_definitions(human_core PRIVATE HU_HAVE_MLX_LM_KTO=1)
    target_compile_definitions(human_core_test PRIVATE HU_HAVE_MLX_LM_KTO=1)
    target_compile_definitions(human_tests PRIVATE HU_HAVE_MLX_LM_KTO=1)
endif()

# Phase 4 Task 8 (RL SOTA) — propagate HU_GRPO_HAVE_HUML_IMPL and
# HU_GRPO_HAVE_MLX_IMPL globally so Task 0's #ifndef-guarded stubs in
# src/ml/rl_trainer.c fall out of EVERY TU, not just the TUs that
# define the macro source-locally (grpo.c and grpo_mlx.c).  Without
# this, the macros are TU-local: rl_trainer.c still compiles both
# stubs and the linker hits duplicate-symbol against the strong impls
# in grpo.c (Task 5) and grpo_mlx.c (Task 8).  This is the canonical
# Phase 4 fix for round-3 critic L3 (strict C11, no __attribute__((weak)));
# the source-level #defines remain as documentation but are now
# redundant with these target_compile_definitions.
if(HU_ENABLE_ML)
    target_compile_definitions(human_core PRIVATE HU_GRPO_HAVE_HUML_IMPL=1 HU_GRPO_HAVE_MLX_IMPL=1)
    target_compile_definitions(human_core_test PRIVATE HU_GRPO_HAVE_HUML_IMPL=1 HU_GRPO_HAVE_MLX_IMPL=1)
    target_compile_definitions(human_tests PRIVATE HU_GRPO_HAVE_HUML_IMPL=1 HU_GRPO_HAVE_MLX_IMPL=1)
endif()

# Phase 4 Task 0 (RL SOTA) — propagate HU_HAVE_MLX_LM_GRPO into targets.
# OFF by default; flipping ON activates the real MLX GRPO subprocess path
# (scripts/grpo_mlx_train.py — landed by Task 8). When unset, the GRPO
# MLX backend ships with HU_ERR_NOT_SUPPORTED and gated tests skip cleanly.
if(HU_HAVE_MLX_LM_GRPO)
    target_compile_definitions(human_core PRIVATE HU_HAVE_MLX_LM_GRPO=1)
    target_compile_definitions(human_core_test PRIVATE HU_HAVE_MLX_LM_GRPO=1)
    target_compile_definitions(human_tests PRIVATE HU_HAVE_MLX_LM_GRPO=1)
endif()

# Phase 5 Task 0 (RL SOTA) — propagate HU_ENABLE_RL_FULL into targets.
# OFF by default keeps default release / dev builds a zero-byte delta vs
# Phase 4 close-out; the rl_sota preset flips this ON so Tasks 1-13 can
# land code (and #ifdef-gated cmd_eval subcommands per Task 10) behind
# the flag without changing the default build surface. The macro is
# defined on both the library (human_core / human_core_test) and the
# executables (human / human_tests) so that source files in either
# stage of the build graph can use it.
if(HU_ENABLE_RL_FULL)
    target_compile_definitions(human PRIVATE HU_ENABLE_RL_FULL=1)
    target_compile_definitions(human_core PRIVATE HU_ENABLE_RL_FULL=1)
    target_compile_definitions(human_core_test PRIVATE HU_ENABLE_RL_FULL=1)
    target_compile_definitions(human_tests PRIVATE HU_ENABLE_RL_FULL=1)
    target_compile_definitions(human_tests PRIVATE HU_E2E_TMP_ROOT="${HU_ROOT}/tests/_tmp")
    target_include_directories(human_core_test PRIVATE ${HU_ROOT}/tests/include)
    target_compile_definitions(human_core PRIVATE HU_EVAL_JUDGE_HAVE_APPLE_FM_IMPL=1)
    target_compile_definitions(human_core PRIVATE HU_EVAL_JUDGE_HAVE_GEMINI_NANO_IMPL=1)
    target_compile_definitions(human_core_test PRIVATE HU_EVAL_JUDGE_HAVE_APPLE_FM_IMPL=1)
    target_compile_definitions(human_core_test PRIVATE HU_EVAL_JUDGE_HAVE_GEMINI_NANO_IMPL=1)
    target_compile_definitions(human_tests PRIVATE HU_EVAL_JUDGE_HAVE_APPLE_FM_IMPL=1)
    target_compile_definitions(human_tests PRIVATE HU_EVAL_JUDGE_HAVE_GEMINI_NANO_IMPL=1)
endif()

# Keep assert() active in tests even for Release/MinSizeRel builds (which define NDEBUG).
# Five test files use libc assert() for guards; without this, those asserts become no-ops
# and subsequent dereferences segfault.
target_compile_options(human_tests PRIVATE -UNDEBUG)

if(HU_ENABLE_ASAN)
    target_compile_options(human_tests PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(human_tests PRIVATE -fsanitize=address)
endif()

add_test(NAME unit_tests COMMAND human_tests)

if(HU_ENABLE_INTEGRATION_TESTS)
    if(NOT HU_ENABLE_CURL)
        message(FATAL_ERROR "HU_ENABLE_INTEGRATION_TESTS requires HU_ENABLE_CURL=ON")
    endif()
    if(NOT HU_ENABLE_SQLITE)
        message(FATAL_ERROR "HU_ENABLE_INTEGRATION_TESTS requires HU_ENABLE_SQLITE=ON")
    endif()
    add_executable(human_integration_tests
        tests/integration/test_integration_main.c
        tests/integration/test_integ_http.c
        tests/integration/test_integ_sqlite.c
        tests/integration/test_integ_imap.c
        tests/integration/test_integ_imessage.c
    )
    target_link_libraries(human_integration_tests PRIVATE human_core)
    target_include_directories(human_integration_tests PRIVATE ${HU_ROOT}/tests ${HU_ROOT}/include)
    target_compile_definitions(human_integration_tests PRIVATE HU_ENABLE_CURL=1 HU_HTTP_CURL=1
        HU_ENABLE_SQLITE=1)
    if(HU_ENABLE_IMESSAGE)
        target_compile_definitions(human_integration_tests PRIVATE HU_HAS_IMESSAGE=1)
        if(HU_IMESSAGE_TAPBACK_ENABLED)
            target_compile_definitions(human_integration_tests PRIVATE HU_IMESSAGE_TAPBACK_ENABLED=1)
        endif()
    endif()
    if(HU_ENABLE_IMAP)
        target_compile_definitions(human_integration_tests PRIVATE HU_HAS_IMAP=1)
    endif()
    if(UNIX AND NOT WIN32)
        target_compile_definitions(human_integration_tests PRIVATE HU_GATEWAY_POSIX=1)
    endif()
    target_compile_options(human_integration_tests PRIVATE -Wall -Wextra -Wpedantic -Wshadow
        -Wformat-security -Werror)
    if(HU_ENABLE_ASAN)
        target_compile_options(human_integration_tests PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(human_integration_tests PRIVATE -fsanitize=address)
    endif()
    add_test(NAME integration_tests COMMAND human_integration_tests)
endif()

# --- WASM/WASI target (optional, use with wasm32-wasi toolchain) ---
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-wasi.cmake -DHU_BUILD_WASM=ON ..
if(HU_BUILD_WASM)
    include(cmake/wasm_sources.cmake)
    add_executable(human.wasm
        ${HU_WASM_MAIN}
        ${HU_WASM_SOURCES}
        ${HU_WASM_CORE_SOURCES}
        ${HU_WASM_CRYPTO_SOURCES}
    )
    target_include_directories(human.wasm PRIVATE ${HU_ROOT}/include ${HU_ROOT}/asm/generic ${HU_ROOT}/src/agent)
    target_compile_definitions(human.wasm PRIVATE __wasi__=1 HU_IS_TEST=0)
    if(HU_ENABLE_SQLITE)
        target_compile_definitions(human.wasm PRIVATE HU_ENABLE_SQLITE=0)
    endif()
    set_target_properties(human.wasm PROPERTIES SUFFIX ".wasm" RUNTIME_OUTPUT_NAME "human")
    message(STATUS "WASM target: human.wasm (requires wasm32-wasi toolchain)")
endif()

# Compiler warnings
target_compile_options(human_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wformat-security -Werror)
target_compile_options(human PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wformat-security -Werror)
target_compile_options(human_tests PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wformat-security)

# Per-file warning exceptions.
#
# src/onboard.c contains hu_starter_persona_json — an inline 4.3 KB JSON
# literal that exceeds the 4095-byte ISO C99 minimum for string literals
# (and so trips -Wpedantic -> -Werror=overlength-strings on Linux GCC). The
# literal is legitimately large configuration data, not a bug; Apple Clang
# on macOS accepts it, but GCC enforces the standard's minimum. Splitting
# the literal would require changing hu_starter_persona_json's public type
# (array -> pointer) and touching 7+ callers across src/ and tests/, which
# is out of scope for a content-only doctrine change. The proper long-term
# fix is to migrate the JSON into the data/ + embed-data.sh pipeline (same
# pattern used for data/prompts/safety_rules.txt). Until then, silence the
# warning for this single file. Tracked: TODO move hu_starter_persona_json
# into data/personas/starter.json.
set_source_files_properties(src/onboard.c PROPERTIES
    COMPILE_FLAGS "-Wno-overlength-strings")

# Synthetic pressure tests (requires libcurl for real HTTP)
# Default ON when libcurl is available so the harness builds in CI by default.
# Per Task #5 (2026-05-17): we want the binary built by default; runtime --smoke
# mode in tests/synthetic/synthetic_main.c lets CI exercise the binary without
# GEMINI_API_KEY. Full runs still require credentials and network.
if(HU_ENABLE_CURL)
    option(HU_ENABLE_SYNTHETIC "Build Gemini-driven synthetic pressure test harness" ON)
else()
    option(HU_ENABLE_SYNTHETIC "Build Gemini-driven synthetic pressure test harness" OFF)
endif()
if(HU_ENABLE_SYNTHETIC AND NOT HU_ENABLE_CURL)
    message(WARNING "HU_ENABLE_SYNTHETIC requires HU_ENABLE_CURL; disabling synthetic harness")
    set(HU_ENABLE_SYNTHETIC OFF CACHE BOOL "Build Gemini-driven synthetic pressure test harness" FORCE)
endif()
if(HU_ENABLE_SYNTHETIC)
    file(GLOB HU_SYNTH_SOURCES tests/synthetic/synthetic_*.c)
    add_executable(human_synthetic ${HU_SYNTH_SOURCES})
    target_link_libraries(human_synthetic human_core)
    target_include_directories(human_synthetic PRIVATE ${HU_ROOT}/include ${HU_ROOT}/src ${HU_ROOT}/tests/synthetic)
    target_compile_definitions(human_synthetic PRIVATE HU_ENABLE_CURL=1)
    target_compile_options(human_synthetic PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wformat-security -Wno-gnu-zero-variadic-macro-arguments)
    if(HU_ENABLE_ASAN)
        # human_synthetic links libhuman_core.a which was built with ASan;
        # the executable itself must also link with -fsanitize=address so the
        # ASan runtime symbols (__asan_*) resolve.
        target_compile_options(human_synthetic PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(human_synthetic PRIVATE -fsanitize=address)
    endif()
    if(UNIX AND NOT WIN32)
        target_compile_definitions(human_synthetic PRIVATE HU_GATEWAY_POSIX=1)
    endif()
    if(HU_ENABLE_SQLITE)
        target_compile_definitions(human_synthetic PRIVATE HU_ENABLE_SQLITE=1)
        if(SQLITE3_FOUND)
            target_include_directories(human_synthetic PRIVATE ${SQLITE3_INCLUDE_DIRS})
        endif()
    endif()
endif()

# Channel conversation & chaos tests
# Default ON when libcurl is available so the harness builds in CI by default.
# Per Task #5 (2026-05-17): M6 Tier-1 channel ambition (Telegram/Discord/iMessage/
# Slack at 8/10+ naturalness) needs this harness running. Runtime --smoke mode in
# tests/synthetic/channel_main.c lets CI exercise the binary without
# GEMINI_API_KEY. Full runs still require credentials and network.
if(HU_ENABLE_CURL)
    option(HU_ENABLE_CHANNEL_TESTS "Build channel conversation+chaos test harness" ON)
else()
    option(HU_ENABLE_CHANNEL_TESTS "Build channel conversation+chaos test harness" OFF)
endif()
if(HU_ENABLE_CHANNEL_TESTS AND NOT HU_ENABLE_CURL)
    message(WARNING "HU_ENABLE_CHANNEL_TESTS requires HU_ENABLE_CURL; disabling channel harness")
    set(HU_ENABLE_CHANNEL_TESTS OFF CACHE BOOL "Build channel conversation+chaos test harness" FORCE)
endif()
if(HU_ENABLE_CHANNEL_TESTS)
    file(GLOB HU_CHANNEL_TEST_SOURCES tests/synthetic/channel_*.c)
    list(APPEND HU_CHANNEL_TEST_SOURCES
        tests/synthetic/synthetic_gemini.c
        tests/synthetic/synthetic_regression.c
        tests/synthetic/synthetic_utils.c)
    add_executable(human_channel_tests ${HU_CHANNEL_TEST_SOURCES})
    target_link_libraries(human_channel_tests human_core_test)
    target_include_directories(human_channel_tests PRIVATE
        ${HU_ROOT}/include ${HU_ROOT}/src ${HU_ROOT}/tests/synthetic)
    target_compile_definitions(human_channel_tests PRIVATE
        HU_ENABLE_CURL=1 HU_IS_TEST=1)
    target_compile_options(human_channel_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wformat-security
        -Wno-gnu-zero-variadic-macro-arguments)
    if(HU_ENABLE_ASAN)
        # human_channel_tests links libhuman_core_test.a which was built with
        # ASan; the executable itself must also link with -fsanitize=address so
        # the ASan runtime symbols (__asan_*) resolve.
        target_compile_options(human_channel_tests PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(human_channel_tests PRIVATE -fsanitize=address)
    endif()
    if(UNIX AND NOT WIN32)
        target_compile_definitions(human_channel_tests PRIVATE HU_GATEWAY_POSIX=1)
    endif()
    if(HU_ENABLE_SQLITE)
        target_compile_definitions(human_channel_tests PRIVATE HU_ENABLE_SQLITE=1)
        if(SQLITE3_FOUND)
            target_include_directories(human_channel_tests PRIVATE ${SQLITE3_INCLUDE_DIRS})
        endif()
    endif()
    # Channel HU_HAS_* defines — enable all channels for the channel test harness
    if(HU_ENABLE_ALL_CHANNELS)
        target_compile_definitions(human_channel_tests PRIVATE
            HU_HAS_TELEGRAM=1 HU_HAS_DISCORD=1 HU_HAS_SLACK=1 HU_HAS_WHATSAPP=1
            HU_HAS_FACEBOOK=1 HU_HAS_INSTAGRAM=1 HU_HAS_TWITTER=1 HU_HAS_MATRIX=1
            HU_HAS_IRC=1 HU_HAS_LINE=1 HU_HAS_LARK=1 HU_HAS_WEB=1 HU_HAS_EMAIL=1
            HU_HAS_IMAP=1 HU_HAS_IMESSAGE=1 HU_HAS_MATTERMOST=1 HU_HAS_ONEBOT=1
            HU_HAS_DINGTALK=1 HU_HAS_TEAMS=1 HU_HAS_TWILIO=1 HU_HAS_GOOGLE_CHAT=1
            HU_HAS_GOOGLE_RCS=1 HU_HAS_SIGNAL=1 HU_HAS_NOSTR=1 HU_HAS_QQ=1
            HU_HAS_MAIXCAM=1 HU_HAS_MQTT=1 HU_HAS_GMAIL=1 HU_HAS_CLI=1
            HU_HAS_DISPATCH=1 HU_HAS_PERSONA=1 HU_HAS_TIKTOK=1)
    else()
        if(HU_ENABLE_TELEGRAM)
            target_compile_definitions(human_channel_tests PRIVATE HU_HAS_TELEGRAM=1)
        endif()
        if(HU_ENABLE_DISCORD)
            target_compile_definitions(human_channel_tests PRIVATE HU_HAS_DISCORD=1)
        endif()
        if(HU_ENABLE_SLACK)
            target_compile_definitions(human_channel_tests PRIVATE HU_HAS_SLACK=1)
        endif()
        if(HU_ENABLE_TIKTOK)
            target_compile_definitions(human_channel_tests PRIVATE HU_HAS_TIKTOK=1)
        endif()
        if(HU_ENABLE_IMESSAGE)
            target_compile_definitions(human_channel_tests PRIVATE HU_HAS_IMESSAGE=1)
        endif()
        if(HU_ENABLE_SIGNAL)
            target_compile_definitions(human_channel_tests PRIVATE HU_HAS_SIGNAL=1)
        endif()
        target_compile_definitions(human_channel_tests PRIVATE HU_HAS_DISPATCH=1 HU_HAS_PERSONA=1)
    endif()
endif()

# Install targets
install(TARGETS human RUNTIME DESTINATION bin)
install(FILES docs/man/human.1 DESTINATION share/man/man1 OPTIONAL)
install(FILES docs/man/human-gateway.1 DESTINATION share/man/man1 OPTIONAL)
install(FILES completions/human.bash DESTINATION share/bash-completion/completions RENAME human OPTIONAL)
install(FILES completions/_human DESTINATION share/zsh/site-functions RENAME _human OPTIONAL)

# SBOM Generation
add_custom_target(sbom
    COMMAND ${CMAKE_COMMAND} -E echo "Generating SBOM..."
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/generate-sbom.sh ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/sbom.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating CycloneDX SBOM"
)

# Fuzz testing (requires Clang with libFuzzer)
option(HU_ENABLE_FUZZ "Build fuzz harnesses (JSON, config, tool params, URL, HTTP)" OFF)
if(HU_ENABLE_FUZZ)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "HU_ENABLE_FUZZ requires Clang compiler")
    endif()
    enable_language(CXX)
    set(FUZZ_SANITIZE "-fsanitize=fuzzer,address")
    set(FUZZ_FRAME "-fno-omit-frame-pointer")
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(FUZZ_EXTRA_LINK "-L/opt/homebrew/opt/llvm/lib/c++" "-L/opt/homebrew/opt/llvm/lib/unwind")
        set(FUZZ_EXTRA_LIBS "unwind" "c++")
    else()
        set(FUZZ_EXTRA_LINK "")
        set(FUZZ_EXTRA_LIBS "")
    endif()

    add_executable(fuzz_json_parse fuzz/fuzz_json_parse.c)
    set_target_properties(fuzz_json_parse PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_json_parse PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_json_parse PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_json_parse PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_json_parse PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_json_parse PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_config_load fuzz/fuzz_config_load.c)
    set_target_properties(fuzz_config_load PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_config_load PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_config_load PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_config_load PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_config_load PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_config_load PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_tool_params fuzz/fuzz_tool_params.c)
    set_target_properties(fuzz_tool_params PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_tool_params PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_tool_params PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_tool_params PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_tool_params PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_tool_params PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_base64 fuzz/fuzz_base64.c)
    set_target_properties(fuzz_base64 PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_base64 PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_base64 PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_base64 PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_base64 PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_base64 PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_json fuzz/fuzz_json.c)
    set_target_properties(fuzz_json PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_json PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_json PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_json PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_json PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_json PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_config fuzz/fuzz_config.c)
    set_target_properties(fuzz_config PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_config PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_config PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_config PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_config PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_config PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_url_encode fuzz/fuzz_url_encode.c)
    set_target_properties(fuzz_url_encode PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_url_encode PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_url_encode PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_url_encode PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_url_encode PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_url_encode PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_persona_json fuzz/fuzz_persona_json.c)
    set_target_properties(fuzz_persona_json PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_persona_json PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_persona_json PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_persona_json PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_persona_json PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_persona_json PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_sse fuzz/fuzz_sse.c)
    set_target_properties(fuzz_sse PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_sse PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_sse PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_sse PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_sse PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_sse PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_http_parse fuzz/fuzz_http_parse.c)
    set_target_properties(fuzz_http_parse PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_http_parse PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_http_parse PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_http_parse PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_http_parse PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_http_parse PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    if(HU_ENABLE_IMESSAGE)
        add_executable(fuzz_imessage_attributed_body fuzz/fuzz_imessage_attributed_body.c)
        set_target_properties(fuzz_imessage_attributed_body PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_definitions(fuzz_imessage_attributed_body PRIVATE HU_IS_TEST=1)
        target_compile_options(fuzz_imessage_attributed_body PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(fuzz_imessage_attributed_body PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
        target_compile_options(fuzz_imessage_attributed_body PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
        target_link_options(fuzz_imessage_attributed_body PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})
    endif()

    add_executable(fuzz_persona_parse fuzz/fuzz_persona_parse.c)
    set_target_properties(fuzz_persona_parse PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_persona_parse PRIVATE HU_IS_TEST=1 HU_HAS_PERSONA=1)
    target_compile_options(fuzz_persona_parse PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_persona_parse PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_persona_parse PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_persona_parse PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_graph fuzz/fuzz_graph.c)
    set_target_properties(fuzz_graph PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_graph PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_graph PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_graph PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_graph PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_graph PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_fast_capture fuzz/fuzz_fast_capture.c)
    set_target_properties(fuzz_fast_capture PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_fast_capture PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_fast_capture PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_fast_capture PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_fast_capture PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_fast_capture PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_control_protocol fuzz/fuzz_control_protocol.c)
    set_target_properties(fuzz_control_protocol PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_control_protocol PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_control_protocol PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_control_protocol PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_control_protocol PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_control_protocol PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_openai_compat fuzz/fuzz_openai_compat.c)
    set_target_properties(fuzz_openai_compat PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_openai_compat PRIVATE HU_IS_TEST=1 HU_GATEWAY_POSIX=1)
    target_compile_options(fuzz_openai_compat PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_openai_compat PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_openai_compat PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_openai_compat PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_prompt_injection fuzz/fuzz_prompt_injection.c)
    set_target_properties(fuzz_prompt_injection PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_options(fuzz_prompt_injection PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_prompt_injection PRIVATE ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_prompt_injection PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_prompt_injection PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_query_param fuzz/fuzz_query_param.c)
    set_target_properties(fuzz_query_param PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_options(fuzz_query_param PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_query_param PRIVATE ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_query_param PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_query_param PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_json_parser tests/fuzz/fuzz_json_parser.c)
    set_target_properties(fuzz_json_parser PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(fuzz_json_parser PRIVATE human_core ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_json_parser PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_json_parser PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_url_validation tests/fuzz/fuzz_url_validation.c)
    set_target_properties(fuzz_url_validation PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(fuzz_url_validation PRIVATE human_core ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_url_validation PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_url_validation PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_prm fuzz/fuzz_prm.c)
    set_target_properties(fuzz_prm PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_prm PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_prm PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_prm PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_prm PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_prm PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_adaptive_rag fuzz/fuzz_adaptive_rag.c)
    set_target_properties(fuzz_adaptive_rag PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_adaptive_rag PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_adaptive_rag PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_adaptive_rag PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_adaptive_rag PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_adaptive_rag PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_self_rag fuzz/fuzz_self_rag.c)
    set_target_properties(fuzz_self_rag PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_self_rag PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_self_rag PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_self_rag PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_self_rag PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_self_rag PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_memory_tiers fuzz/fuzz_memory_tiers.c)
    set_target_properties(fuzz_memory_tiers PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_memory_tiers PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_memory_tiers PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_memory_tiers PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_memory_tiers PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_memory_tiers PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_visual_grounding fuzz/fuzz_visual_grounding.c)
    set_target_properties(fuzz_visual_grounding PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_visual_grounding PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_visual_grounding PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_visual_grounding PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_visual_grounding PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_visual_grounding PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_ensemble_rerank fuzz/fuzz_ensemble_rerank.c)
    set_target_properties(fuzz_ensemble_rerank PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_ensemble_rerank PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_ensemble_rerank PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_ensemble_rerank PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_ensemble_rerank PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_ensemble_rerank PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_contact_phone fuzz/fuzz_contact_phone.c)
    set_target_properties(fuzz_contact_phone PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_contact_phone PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_contact_phone PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_contact_phone PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_contact_phone PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_contact_phone PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    if(HU_ENABLE_TWILIO)
        add_executable(fuzz_twilio_webhook fuzz/fuzz_twilio_webhook.c)
        set_target_properties(fuzz_twilio_webhook PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_definitions(fuzz_twilio_webhook PRIVATE HU_IS_TEST=1)
        target_compile_options(fuzz_twilio_webhook PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(fuzz_twilio_webhook PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
        target_compile_options(fuzz_twilio_webhook PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
        target_link_options(fuzz_twilio_webhook PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})
    endif()

    add_executable(fuzz_compaction_overflow fuzz/fuzz_compaction_overflow.c)
    set_target_properties(fuzz_compaction_overflow PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_compaction_overflow PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_compaction_overflow PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_compaction_overflow PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_compaction_overflow PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_compaction_overflow PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_instruction_discovery fuzz/fuzz_instruction_discovery.c)
    set_target_properties(fuzz_instruction_discovery PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_instruction_discovery PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_instruction_discovery PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_instruction_discovery PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_instruction_discovery PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_instruction_discovery PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_mcp_json_rpc fuzz/fuzz_mcp_json_rpc.c)
    set_target_properties(fuzz_mcp_json_rpc PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_mcp_json_rpc PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_mcp_json_rpc PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_mcp_json_rpc PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_mcp_json_rpc PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_mcp_json_rpc PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_normalize fuzz/fuzz_normalize.c)
    set_target_properties(fuzz_normalize PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_normalize PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_normalize PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_normalize PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_normalize PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_normalize PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_hook_injection fuzz/fuzz_hook_injection.c)
    set_target_properties(fuzz_hook_injection PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_hook_injection PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_hook_injection PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_hook_injection PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_hook_injection PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_hook_injection PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_session_deserialization fuzz/fuzz_session_deserialization.c)
    set_target_properties(fuzz_session_deserialization PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_session_deserialization PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_session_deserialization PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_session_deserialization PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_session_deserialization PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_session_deserialization PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})

    add_executable(fuzz_output_validator_chain fuzz/fuzz_output_validator_chain.c)
    set_target_properties(fuzz_output_validator_chain PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(fuzz_output_validator_chain PRIVATE HU_IS_TEST=1)
    target_compile_options(fuzz_output_validator_chain PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(fuzz_output_validator_chain PRIVATE human_core_test ${FUZZ_EXTRA_LIBS})
    target_compile_options(fuzz_output_validator_chain PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME})
    target_link_options(fuzz_output_validator_chain PRIVATE ${FUZZ_SANITIZE} ${FUZZ_FRAME} ${FUZZ_EXTRA_LINK})
endif()

# Benchmark suite (micro-benchmarks of core operations)
option(HU_ENABLE_BENCH "Build human_bench executable for core operation benchmarks" OFF)
if(HU_ENABLE_BENCH)
    set(BENCH_SOURCES
        bench/bench_main.c
        bench/bench_json.c
        bench/bench_memory.c
        bench/bench_config.c
        bench/bench_runner.c
    )
    add_executable(human_bench ${BENCH_SOURCES})
    target_link_libraries(human_bench human_core)
    target_include_directories(human_bench PRIVATE ${HU_ROOT} ${HU_ROOT}/bench)
    target_compile_definitions(human_bench PRIVATE HU_IS_TEST=1)
    target_compile_options(human_bench PRIVATE -Wall -Wextra -Wpedantic -O2)
    if(HU_ENABLE_ASAN)
        message(STATUS "Bench: human_bench built without ASan for performance measurement")
    endif()
    message(STATUS "Bench: human_bench enabled")
endif()

# Benchmark target
add_custom_target(benchmark
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/benchmark.sh $<TARGET_FILE:human>
    DEPENDS human human_tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running benchmarks..."
)

# Print build info
message(STATUS "Human v0.5.0")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Channels: ${CHANNEL_SOURCES}")
message(STATUS "Engines: none=${HU_ENABLE_NONE_ENGINE} markdown=${HU_ENABLE_MARKDOWN_ENGINE} sqlite=${HU_ENABLE_SQLITE} postgres=${HU_ENABLE_POSTGRES}")
message(STATUS "ASan: ${HU_ENABLE_ASAN}")

message(STATUS "ASan: ${HU_ENABLE_ASAN}")

