---
title: human API Reference
description: Public API reference organized by module for the Human C runtime
updated: 2026-03-02
---

# human API Reference

human is a C11 autonomous AI assistant runtime with a vtable-driven architecture. This reference documents the public API organized by module.

## Quick Reference

| Module                    | Key Types                                                                                | Header                                                                                                            |
| ------------------------- | ---------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| [Core](core.md)           | `hu_allocator_t`, `hu_error_t`, `hu_json_value_t`, `hu_str_t`, `hu_arena_t`              | `core/allocator.h`, `core/error.h`, `core/json.h`, `core/slice.h`, `core/string.h`, `core/http.h`, `core/arena.h` |
| [Agent](agent.md)         | `hu_agent_t`, `hu_owned_message_t`                                                       | `agent.h`                                                                                                         |
| [Providers](providers.md) | `hu_provider_t`, `hu_chat_request_t`, `hu_chat_response_t`                               | `provider.h`, `providers/factory.h`                                                                               |
| [Channels](channels.md)   | `hu_channel_t`, `hu_channel_message_t`, `hu_channel_manager_t`                           | `channel.h`, `channel_manager.h`                                                                                  |
| [Tools](tools.md)         | `hu_tool_t`, `hu_tool_result_t`, `hu_tool_spec_t`                                        | `tool.h`, `tools/factory.h`                                                                                       |
| [Memory](memory.md)       | `hu_memory_t`, `hu_session_store_t`, `hu_memory_entry_t`, `hu_retrieval_engine_t`        | `memory.h`, `memory/retrieval.h`                                                                                  |
| [Gateway](gateway.md)     | `hu_gateway_config_t`, `hu_control_protocol_t`, `hu_push_manager_t`, `hu_event_bridge_t` | `gateway.h`, `gateway/control_protocol.h`, `gateway/push.h`, `gateway/event_bridge.h`                             |
| [Security](security.md)   | `hu_security_policy_t`, `hu_pairing_guard_t`, `hu_secret_store_t`, `hu_audit_logger_t`   | `security.h`, `security/audit.h`                                                                                  |
| [Config](config.md)       | `hu_config_t`, `hu_config_load`                                                          | `config.h`, `config_types.h`                                                                                      |

## Build and Include

```bash
# Build
mkdir build && cd build
cmake .. -DHU_ENABLE_ALL_CHANNELS=ON
cmake --build . -j$(nproc)
```

Include headers via `#include "human/<module>.h"` (e.g. `#include "human/agent.h"`).

## Error Handling

All API functions returning `hu_error_t` use:

- `HU_OK` (0) — success
- `HU_ERR_*` — domain-specific errors

Use `hu_error_string(hu_error_t err)` for a human-readable message.

## Allocation

- Core types use `hu_allocator_t`. Obtain the system allocator with `hu_system_allocator()`.
- Caller owns allocations from `alloc->alloc()`. Free with `alloc->free()`.
- Tool results may set `output_owned` / `error_msg_owned`; call `hu_tool_result_free()` to release.

## Thread Safety

- Most types are **not** thread-safe. Use external synchronization.
- Gateway, control protocol, and WebSocket handling run on a single thread (POSIX poll loop).
- Provider and channel vtables may be called from different threads depending on channel listener type.

---

See the linked module docs for detailed type definitions, function signatures, and usage examples.
