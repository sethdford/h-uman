---
title: Build Options
description: Every HU_ENABLE_* CMake option, its default, and which presets turn it on.
---

# Build Options

GENERATED FILE. Do not hand-edit — regenerate with:

```
bash scripts/dev/build-options-table.sh --write
```

Reflects `CMakeLists.txt` + `CMakePresets.json` as of commit `d3be0c4cb` (the most recent commit to touch either file) — keyed to a commit rather than wall-clock time so re-running this script with no changes to either file produces byte-identical output.

The "Presets ON" column lists every `configurePresets` entry (after resolving `inherits` chains) whose effective cache value for that option is `ON` — either because the preset (or a preset it inherits from) sets it explicitly, or because no preset in the chain overrides it and the option's own default (below) is `ON`.

**Not reflected here:** `HU_ENABLE_ALL_CHANNELS=ON` (set by the `test`, `release`, `fuzz` presets) also flips ~20 more `HU_ENABLE_<channel>` options ON at CMake configure time, via an `if(HU_ENABLE_ALL_CHANNELS) set(HU_ENABLE_TELEGRAM ON) ... endif()` block in `CMakeLists.txt` — that is control flow, not a declared default or a preset cache variable, so a preset that relies on it will show those channel options as off below even though they compile in. Only options declared with an unconditional top-level `option(HU_ENABLE_...)` (or `cmake_dependent_option`) appear in this table; options declared inside an `if()/else()` for a platform-dependent default (for example `HU_ENABLE_APPLE_INTELLIGENCE`, `HU_ENABLE_PWA`) are intentionally excluded — see the script's header comment.

| Option | Default | Presets ON | Gates |
|---|---|---|---|
| `HU_ENABLE_ASAN` | OFF | dev, dev-neural, rl_sota | Enable AddressSanitizer |
| `HU_ENABLE_SQLITE` | ON | dev, prod, test, release, fuzz, integration, dev-neural, rl_sota, release-reproducible | Enable SQLite memory backend |
| `HU_ENABLE_POSTGRES` | OFF | *(none)* | Enable PostgreSQL memory backend |
| `HU_ENABLE_PGVECTOR` | OFF | *(none)* | Enable pgvector vector store (requires libpq) |
| `HU_ENABLE_CURL` | ON | dev, prod, test, release, fuzz, integration, dev-neural, rl_sota, release-reproducible | Enable libcurl for HTTP client |
| `HU_ENABLE_INTEGRATION_TESTS` | OFF | integration | Build human_integration_tests (real libcurl HTTP + SQLite file I/O; no HU_IS_TEST) |
| `HU_ENABLE_TLS` | ON | dev, prod, test, release, fuzz, integration, dev-neural, rl_sota, release-reproducible | Enable TLS via OpenSSL for WSS and HTTPS |
| `HU_ENABLE_FIPS_CRYPTO` | OFF | *(none)* | Use AES-256-GCM via OpenSSL for FIPS 140-2 compliance |
| `HU_ENABLE_LTO` | OFF | release, minimal-release, release-reproducible | Enable Link-Time Optimization |
| `HU_ENABLE_READLINE` | OFF | *(none)* | Enable readline/libedit for CLI line editing |
| `HU_ENABLE_LINENOISE` | OFF | *(none)* | Enable linenoise for lightweight CLI line editing (vendor/linenoise) |
| `HU_ENABLE_TUI` | OFF | dev, prod, dev-neural, rl_sota | Enable rich TUI mode with split-pane terminal UI (vendor/termbox2) |
| `HU_ENABLE_PERIPHERALS` | OFF | *(none)* | Build hardware peripheral support (Arduino, STM32, RPi, SPI, I2C) |
| `HU_ENABLE_TUNNELS` | OFF | *(none)* | Build tunnel providers (ngrok, cloudflare, tailscale) |
| `HU_ENABLE_RUNTIME_EXOTIC` | OFF | *(none)* | Build exotic runtime adapters (WASM, Cloudflare Workers) |
| `HU_ENABLE_TOOLS_BROWSER` | OFF | *(none)* | Build browser automation tools (browser, screenshot, browser_open) |
| `HU_ENABLE_TOOLS_ADVANCED` | OFF | dev, prod, release, dev-neural, rl_sota, release-reproducible | Build advanced tools (composio, claude_code, canvas, notebook, database) |
| `HU_ENABLE_OTEL` | OFF | *(none)* | Build OpenTelemetry observability exporter |
| `HU_ENABLE_CRON` | OFF | dev, prod, release, dev-neural, rl_sota, release-reproducible | Build cron scheduler support |
| `HU_ENABLE_PUSH` | OFF | *(none)* | Build push notification support (FCM/APNS) |
| `HU_ENABLE_UPDATE` | OFF | dev, prod, test, release, dev-neural, rl_sota, release-reproducible | Build update check/apply support |
| `HU_ENABLE_SKILLS` | OFF | dev, prod, test, release, dev-neural, rl_sota, release-reproducible | Build skill acquisition and continuous learning |
| `HU_ENABLE_PERSONA` | ON | dev, prod, test, release, fuzz, minimal, minimal-release, integration, dev-neural, rl_sota, release-reproducible | Persona system (compiled unconditionally; OFF is rejected) |
| `HU_ENABLE_AUTHENTIC` | OFF | *(none)* | Build authentic existence features |
| `HU_ENABLE_CARTESIA` | OFF | dev, prod, release, dev-neural, rl_sota, release-reproducible | Enable Cartesia TTS for voice messages |
| `HU_ENABLE_ML` | OFF | dev, prod, release, dev-neural, rl_sota, release-reproducible | Build ML training subsystem (BPE tokenizer, dataloader, experiment loop) |
| `HU_ENABLE_NEURAL_MEMORY` | OFF | dev-neural | W10 neural memory placeholder (schema/ONNX path; default OFF) |
| `HU_ENABLE_SELF_MODEL` | OFF | *(none)* | Build agent behavioral self-model (per-turn observation ring buffer) |
| `HU_ENABLE_ACTION_LAYERS` | OFF | *(none)* | Inject drift + TOM-clarification directives into per-turn prompts (Spec 2026-05-24) |
| `HU_ENABLE_FEEDS` | OFF | dev, prod, release, dev-neural, rl_sota, release-reproducible | Build feed ingestion and research agent |
| `HU_ENABLE_SOCIAL` | OFF | *(none)* | Build social feed ingestion (Facebook, Instagram) |
| `HU_ENABLE_EMBEDDED_MODEL` | OFF | dev, prod, release, dev-neural, rl_sota, release-reproducible | Enable embedded model provider (llama.cpp/llama-cli) |
| `HU_ENABLE_LLAMACPP` | OFF | rl_sota | Enable in-process llama.cpp provider with chat-time LoRA merging (W13 Bridge A) |
| `HU_ENABLE_MLX_PROVIDER` | OFF | *(none)* | Enable MLX provider (M3 Bridge B) — stub when OFF, subprocess MLX runtime when ON |
| `HU_ENABLE_RL_FULL` | OFF | dev, prod, dev-neural, rl_sota | Enable RL evaluation infrastructure (Phase 5 + 6) |
| `HU_ENABLE_LIBSODIUM` | OFF | dev, prod, dev-neural, rl_sota | Link libsodium for production-grade AEAD (XChaCha20-Poly1305) and KDF (Argon2id) |
| `HU_ENABLE_COREML` | OFF | *(none)* | Enable CoreML/MLX provider (macOS only) |
| `HU_ENABLE_VISION_OCR` | OFF | *(none)* | Apple Vision OCR bridge for the vision_ocr tool (macOS only) |
| `HU_ENABLE_VOICE_VAD_TIMING` | OFF | *(none)* | Randomised 200-500 ms filler-to-reply delay on voice channels |
| `HU_ENABLE_MCP` | ON | dev, prod, test, release, fuzz, minimal, minimal-release, integration, dev-neural, rl_sota, release-reproducible | Enable MCP client (multi-server tool discovery) |
| `HU_ENABLE_CLI` | ON | dev, prod, test, release, fuzz, minimal, minimal-release, integration, dev-neural, rl_sota, release-reproducible | Build CLI channel |
| `HU_ENABLE_TELEGRAM` | OFF | dev, prod, dev-neural, rl_sota | Build Telegram channel |
| `HU_ENABLE_DISCORD` | OFF | *(none)* | Build Discord channel |
| `HU_ENABLE_SLACK` | OFF | dev, prod, dev-neural, rl_sota | Build Slack channel |
| `HU_ENABLE_WHATSAPP` | OFF | dev, prod, dev-neural, rl_sota | Build WhatsApp channel |
| `HU_ENABLE_MATRIX` | OFF | *(none)* | Build Matrix channel |
| `HU_ENABLE_IRC` | OFF | *(none)* | Build IRC channel |
| `HU_ENABLE_LINE` | OFF | *(none)* | Build LINE channel |
| `HU_ENABLE_LARK` | OFF | *(none)* | Build Lark channel |
| `HU_ENABLE_WEB` | OFF | *(none)* | Build Web channel |
| `HU_ENABLE_EMAIL` | OFF | dev, prod, dev-neural, rl_sota | Build Email channel |
| `HU_ENABLE_IMAP` | OFF | dev, prod, integration, dev-neural, rl_sota | Build IMAP channel |
| `HU_ENABLE_MATTERMOST` | OFF | *(none)* | Build Mattermost channel |
| `HU_ENABLE_ONEBOT` | OFF | *(none)* | Build OneBot channel |
| `HU_ENABLE_DINGTALK` | OFF | *(none)* | Build DingTalk channel |
| `HU_ENABLE_SIGNAL` | OFF | *(none)* | Build Signal channel |
| `HU_ENABLE_NOSTR` | OFF | *(none)* | Build Nostr channel |
| `HU_ENABLE_QQ` | OFF | *(none)* | Build QQ channel |
| `HU_ENABLE_DISPATCH` | OFF | *(none)* | Build Dispatch channel |
| `HU_ENABLE_GMAIL` | OFF | dev, prod, dev-neural, rl_sota | Build Gmail channel (read-only OAuth2) |
| `HU_ENABLE_TEAMS` | OFF | *(none)* | Build Microsoft Teams channel |
| `HU_ENABLE_TWILIO` | OFF | *(none)* | Build Twilio SMS channel |
| `HU_ENABLE_GOOGLE_CHAT` | OFF | *(none)* | Build Google Chat channel |
| `HU_ENABLE_GOOGLE_RCS` | OFF | *(none)* | Build Google RCS channel |
| `HU_ENABLE_FACEBOOK` | OFF | *(none)* | Build Facebook Messenger channel |
| `HU_ENABLE_INSTAGRAM` | OFF | *(none)* | Build Instagram DMs channel |
| `HU_ENABLE_TWITTER` | OFF | *(none)* | Build Twitter/X DMs channel |
| `HU_ENABLE_TIKTOK` | OFF | *(none)* | Build TikTok channel |
| `HU_ENABLE_VOICE` | OFF | *(none)* | Build Voice channel (Sonata) |
| `HU_ENABLE_MQTT` | OFF | *(none)* | Build MQTT channel |
| `HU_ENABLE_ALL_CHANNELS` | OFF | test, release, fuzz, release-reproducible | Build all channels |
| `HU_ENABLE_NONE_ENGINE` | ON | dev, prod, test, release, fuzz, minimal, minimal-release, integration, dev-neural, rl_sota, release-reproducible | Build no-op memory engine |
| `HU_ENABLE_MARKDOWN_ENGINE` | ON | dev, prod, test, release, fuzz, minimal, minimal-release, integration, dev-neural, rl_sota, release-reproducible | Build markdown memory engine |
| `HU_ENABLE_MEMORY_LRU_ENGINE` | ON | dev, prod, test, release, fuzz, minimal, minimal-release, integration, dev-neural, rl_sota, release-reproducible | Build in-memory LRU memory engine |
| `HU_ENABLE_API_ENGINE` | OFF | *(none)* | Build HTTP API memory engine |
| `HU_ENABLE_LUCID_ENGINE` | ON | dev, prod, test, release, fuzz, minimal, minimal-release, integration, dev-neural, rl_sota, release-reproducible | Build Lucid memory engine |
| `HU_ENABLE_LANCEDB_ENGINE` | ON | dev, prod, test, release, fuzz, minimal, minimal-release, integration, dev-neural, rl_sota, release-reproducible | Build LanceDB memory engine |
| `HU_ENABLE_REDIS_ENGINE` | OFF | *(none)* | Build Redis memory engine |
| `HU_ENABLE_SONATA` | OFF | *(none)* | Enable native Sonata voice pipeline (Rust) |
| `HU_ENABLE_SQLITE_VEC` | ON | dev, prod, test, release, fuzz, minimal, minimal-release, integration, dev-neural, rl_sota, release-reproducible | Vendored sqlite-vec store for semantic recall (adds ~300 KB; release-size builds turn it off) |
| `HU_ENABLE_TOPOLOGY_CHECK` | ON | dev, prod, test, release, fuzz, minimal, minimal-release, integration, dev-neural, rl_sota, release-reproducible | Enforce 7-layer architectural dependency direction (memory v2 P2E) |
| `HU_ENABLE_FUZZ` | OFF | fuzz | Build fuzz harnesses (JSON, config, tool params, URL, HTTP) |
| `HU_ENABLE_BENCH` | OFF | *(none)* | Build human_bench executable for core operation benchmarks |
