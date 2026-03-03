---
title: seaclaw API Reference
description: Public API reference organized by module for the SeaClaw C runtime
updated: 2026-03-02
---

# seaclaw API Reference

seaclaw is a C11 autonomous AI assistant runtime with a vtable-driven architecture. This reference documents the public API organized by module.

## Quick Reference

| Module                    | Key Types                                                                                | Header                                                                                                            |
| ------------------------- | ---------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| [Core](core.md)           | `sc_allocator_t`, `sc_error_t`, `sc_json_value_t`, `sc_str_t`, `sc_arena_t`              | `core/allocator.h`, `core/error.h`, `core/json.h`, `core/slice.h`, `core/string.h`, `core/http.h`, `core/arena.h` |
| [Agent](agent.md)         | `sc_agent_t`, `sc_owned_message_t`                                                       | `agent.h`                                                                                                         |
| [Providers](providers.md) | `sc_provider_t`, `sc_chat_request_t`, `sc_chat_response_t`                               | `provider.h`, `providers/factory.h`                                                                               |
| [Channels](channels.md)   | `sc_channel_t`, `sc_channel_message_t`, `sc_channel_manager_t`                           | `channel.h`, `channel_manager.h`                                                                                  |
| [Tools](tools.md)         | `sc_tool_t`, `sc_tool_result_t`, `sc_tool_spec_t`                                        | `tool.h`, `tools/factory.h`                                                                                       |
| [Memory](memory.md)       | `sc_memory_t`, `sc_session_store_t`, `sc_memory_entry_t`, `sc_retrieval_engine_t`        | `memory.h`, `memory/retrieval.h`                                                                                  |
| [Gateway](gateway.md)     | `sc_gateway_config_t`, `sc_control_protocol_t`, `sc_push_manager_t`, `sc_event_bridge_t` | `gateway.h`, `gateway/control_protocol.h`, `gateway/push.h`, `gateway/event_bridge.h`                             |
| [Security](security.md)   | `sc_security_policy_t`, `sc_pairing_guard_t`, `sc_secret_store_t`, `sc_audit_logger_t`   | `security.h`, `security/audit.h`                                                                                  |
| [Config](config.md)       | `sc_config_t`, `sc_config_load`                                                          | `config.h`, `config_types.h`                                                                                      |

## Build and Include

```bash
# Build
mkdir build && cd build
cmake .. -DSC_ENABLE_ALL_CHANNELS=ON
cmake --build . -j$(nproc)
```

Include headers via `#include "seaclaw/<module>.h"` (e.g. `#include "seaclaw/agent.h"`).

## Error Handling

All API functions returning `sc_error_t` use:

- `SC_OK` (0) — success
- `SC_ERR_*` — domain-specific errors

Use `sc_error_string(sc_error_t err)` for a human-readable message.

## Allocation

- Core types use `sc_allocator_t`. Obtain the system allocator with `sc_system_allocator()`.
- Caller owns allocations from `alloc->alloc()`. Free with `alloc->free()`.
- Tool results may set `output_owned` / `error_msg_owned`; call `sc_tool_result_free()` to release.

## Thread Safety

- Most types are **not** thread-safe. Use external synchronization.
- Gateway, control protocol, and WebSocket handling run on a single thread (POSIX poll loop).
- Provider and channel vtables may be called from different threads depending on channel listener type.

---

See the linked module docs for detailed type definitions, function signatures, and usage examples.
