---
title: Prunable Modules — Binary Size Reduction Candidates
---
# Prunable Modules — Binary Size Reduction Candidates

Modules that could be removed or further gated to reduce binary size.
Status key: **STUB** = returns HU_ERR_NOT_SUPPORTED for core operations,
**PARTIAL** = some functions work but core ones are stubbed on certain platforms.

## Stub Runtimes (always return HU_ERR_NOT_SUPPORTED)

| File                       | Object Size | Status | Notes                                                   |
| -------------------------- | ----------- | ------ | ------------------------------------------------------- |
| `src/runtime/wasm_rt.c`    | 5.5 KB      | STUB   | `wasm_wrap_command` always returns HU_ERR_NOT_SUPPORTED |
| `src/runtime/cloudflare.c` | 4.0 KB      | STUB   | `cf_wrap_command` always returns HU_ERR_NOT_SUPPORTED   |

**Now gated** behind `HU_ENABLE_RUNTIME_EXOTIC` (OFF by default).

## Peripherals + Hardware Tools

| File                          | Object Size | Status          | Notes                          |
| ----------------------------- | ----------- | --------------- | ------------------------------ |
| `src/peripherals/arduino.c`   | 14.6 KB     | REAL (serial)   | Requires Arduino board         |
| `src/peripherals/stm32.c`     | 12.6 KB     | REAL (probe-rs) | Linux-only outside tests       |
| `src/peripherals/rpi.c`       | 8.4 KB      | REAL (GPIO)     | Linux-only (`/sys/class/gpio`) |
| `src/peripherals/factory.c`   | 5.4 KB      | REAL            | Peripheral router              |
| `src/hardware.c`              | 5.5 KB      | REAL            | Hardware abstraction           |
| `src/tools/hardware_info.c`   | 17.2 KB     | REAL            | Hardware info tool             |
| `src/tools/hardware_memory.c` | 17.5 KB     | REAL            | Hardware memory tool           |
| `src/tools/i2c.c`             | 8.6 KB      | REAL            | I2C bus tool                   |
| `src/tools/spi.c`             | 8.6 KB      | REAL            | SPI bus tool                   |

**Now gated** behind `HU_ENABLE_PERIPHERALS` (OFF by default). ~98 KB object total.

## Tunnels

| File                      | Object Size | Status | Notes                    |
| ------------------------- | ----------- | ------ | ------------------------ |
| `src/tunnel/cloudflare.c` | 10.1 KB     | REAL   | cloudflared CLI wrapper  |
| `src/tunnel/ngrok.c`      | 10.4 KB     | REAL   | ngrok CLI wrapper        |
| `src/tunnel/tailscale.c`  | 10.3 KB     | REAL   | tailscale funnel wrapper |
| `src/tunnel/custom.c`     | 12.1 KB     | REAL   | Generic command wrapper  |

**Now gated** behind `HU_ENABLE_TUNNELS` (OFF by default). ~43 KB object total.

## Platform-Specific Security Sandboxes (Pruned via Auto-Detect)

On macOS, only seatbelt.c compiles. On Linux, only Linux-specific sandboxes compile.

| File                              | Object Size | Platform     | Notes                       |
| --------------------------------- | ----------- | ------------ | --------------------------- |
| `src/security/seatbelt.c`         | 9.1 KB      | macOS only   | SBPL sandbox profile        |
| `src/security/landlock.c`         | 6.5 KB      | Linux only   | Landlock LSM                |
| `src/security/seccomp.c`          | 6.4 KB      | Linux only   | seccomp-BPF filter          |
| `src/security/landlock_seccomp.c` | 6.4 KB      | Linux only   | Combined landlock + seccomp |
| `src/security/firejail.c`         | 6.7 KB      | Linux only   | Firejail command wrap       |
| `src/security/bubblewrap.c`       | 6.4 KB      | Linux only   | Bubblewrap namespace wrap   |
| `src/security/firecracker.c`      | 7.7 KB      | Linux only   | Firecracker MicroVM         |
| `src/security/appcontainer.c`     | 7.4 KB      | Windows only | AppContainer isolation      |

**Now auto-detected** by platform in CMakeLists.txt — no manual option needed.

## Candidates for Future Gating (Still Unconditionally Compiled)

These modules are real implementations but serve niche use cases. Gating them
behind options would reduce binary size for deployments that don't need them.

### Tools (~130 KB object total)

| File                       | Object Size | Use Case                           |
| -------------------------- | ----------- | ---------------------------------- |
| `src/tools/composio.c`     | 20.4 KB     | Composio integration (third-party) |
| `src/tools/claude_code.c`  | 18.3 KB     | Claude Code CLI delegation         |
| `src/tools/canvas.c`       | 20.8 KB     | Canvas CRUD operations             |
| `src/tools/notebook.c`     | 13.7 KB     | Notebook operations                |
| `src/tools/pushover.c`     | 16.1 KB     | Pushover notifications             |
| `src/tools/database.c`     | 12.7 KB     | Generic database queries           |
| `src/tools/apply_patch.c`  | 14.8 KB     | Unified diff patching              |
| `src/tools/diff.c`         | 12.0 KB     | File diffing/merging               |
| `src/tools/browser.c`      | 29.7 KB     | Full browser automation            |
| `src/tools/browser_open.c` | 15.9 KB     | Open URL in browser                |

### Memory Retrieval Pipeline (~100 KB object total)

| File                                     | Object Size | Notes                   |
| ---------------------------------------- | ----------- | ----------------------- |
| `src/memory/retrieval/keyword.c`         | 12.2 KB     | BM25 keyword search     |
| `src/memory/retrieval/reranker.c`        | 12.0 KB     | Cross-encoder reranking |
| `src/memory/retrieval/llm_reranker.c`    | 7.3 KB      | LLM-based reranking     |
| `src/memory/retrieval/query_expansion.c` | 12.4 KB     | Query expansion         |
| `src/memory/retrieval/rrf.c`             | 12.0 KB     | Reciprocal rank fusion  |
| `src/memory/retrieval/qmd.c`             | 10.0 KB     | Query-memory distance   |
| `src/memory/retrieval/temporal.c`        | 6.3 KB      | Temporal decay          |
| `src/memory/retrieval/adaptive.c`        | 6.0 KB      | Adaptive strategy       |
| `src/memory/retrieval/engine.c`          | 11.6 KB     | Retrieval orchestrator  |

### Memory Vector Stores (~85 KB object total)

| File                                    | Object Size | Notes                        |
| --------------------------------------- | ----------- | ---------------------------- |
| `src/memory/vector/store_qdrant.c`      | 13.9 KB     | Qdrant HTTP client           |
| `src/memory/vector/store_pgvector.c`    | 8.1 KB      | pgvector (requires Postgres) |
| `src/memory/vector/embeddings_gemini.c` | 12.6 KB     | Gemini embeddings API        |
| `src/memory/vector/embeddings_voyage.c` | 12.0 KB     | Voyage AI embeddings         |
| `src/memory/vector/embeddings_ollama.c` | 11.3 KB     | Ollama local embeddings      |
| `src/memory/vector/embedder_local.c`    | 9.8 KB      | Local vector embedder        |
| `src/memory/vector/outbox.c`            | 8.3 KB      | Embedding outbox queue       |

### Observability (~33 KB object total)

| File                                   | Object Size | Notes                  |
| -------------------------------------- | ----------- | ---------------------- |
| `src/observability/otel.c`             | 9.4 KB      | OpenTelemetry exporter |
| `src/observability/metrics_observer.c` | 8.4 KB      | Metrics collection     |
| `src/observability/log_observer.c`     | 14.6 KB     | Log observer           |

### Other Niche Modules

| File               | Object Size | Notes                    |
| ------------------ | ----------- | ------------------------ |
| `src/mcp_server.c` | 20.3 KB     | MCP server mode          |
| `src/skillforge.c` | 17.8 KB     | Skill forge / generation |
| `src/onboard.c`    | 13.9 KB     | First-run onboarding     |
| `src/daemon.c`     | 23.4 KB     | Daemon mode              |
| `src/cost.c`       | 20.1 KB     | Cost tracking            |
| `src/crontab.c`    | 15.4 KB     | Crontab management       |

## Summary

| Category                     | Object Size | Savings After LTO (est.) | Status                               |
| ---------------------------- | ----------- | ------------------------ | ------------------------------------ |
| Stub runtimes                | 9.5 KB      | ~2 KB                    | **Gated** (HU_ENABLE_RUNTIME_EXOTIC) |
| Peripherals + HW tools       | 98 KB       | ~15-20 KB                | **Gated** (HU_ENABLE_PERIPHERALS)    |
| Tunnels                      | 43 KB       | ~10-15 KB                | **Gated** (HU_ENABLE_TUNNELS)        |
| Platform sandbox auto-detect | 50 KB       | ~5-8 KB                  | **Auto-detected**                    |
| Future: niche tools          | 130 KB      | ~20-30 KB                | Candidate                            |
| Future: retrieval pipeline   | 100 KB      | ~15-25 KB                | Candidate                            |
| Future: vector stores        | 85 KB       | ~12-20 KB                | Candidate                            |
| Future: observability        | 33 KB       | ~5-8 KB                  | Candidate                            |
| Future: niche modules        | 110 KB      | ~15-25 KB                | Candidate                            |
