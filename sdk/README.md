# human Plugin SDK

This directory provides templates and examples for building third-party plugins that extend human: custom AI providers, messaging channels, and tools.

## Overview

human uses a **vtable-driven** architecture. Plugins implement struct vtables and register instances with the runtime. All extension points follow the same pattern: `(void *ctx, const struct hu_*_vtable *vtable)`.

## Prerequisites

- human headers in `include/human/`
- C11 compiler
- CMake 3.20+

## Quick Start

1. Copy a template from `sdk/templates/` (provider, channel, or tool).
2. Implement the vtable methods.
3. Build against human (link or compile into the main binary).
4. Register your plugin in the appropriate factory or manager.

## Templates

| Template | Path                      | Purpose                             |
| -------- | ------------------------- | ----------------------------------- |
| Provider | `sdk/templates/provider/` | Custom LLM backend                  |
| Channel  | `sdk/templates/channel/`  | Custom messaging channel            |
| Tool     | `sdk/templates/tool/`     | Custom tool (e.g. API call, script) |

## Examples

| Example      | Path                    | Description                            |
| ------------ | ----------------------- | -------------------------------------- |
| Weather Tool | `sdk/examples/weather/` | Complete working tool with JSON params |

## Registration

### Provider

Add your provider creation to `src/providers/factory.c` in `hu_provider_create()`:

```c
if (name_len == 10 && memcmp(name, "my_provider", 10) == 0) {
    return hu_my_provider_create(alloc, api_key, api_key_len,
        base_url, base_url_len, out);
}
```

Alternatively, build human with your provider as a linked object and extend the factory.

### Channel

Create your channel, then register with the channel manager:

```c
hu_channel_t my_ch;
hu_my_channel_create(&alloc, &my_ch);
hu_channel_manager_register(&mgr, "my_channel", "default", &my_ch,
    HU_CHANNEL_LISTENER_SEND_ONLY);
```

### Tool

Tools are created explicitly and passed to the agent. Either:

1. Add your tool to `hu_tools_create_default` in `src/tools/factory.c`, or
2. Build your own tool array and pass it to `hu_agent_from_config`:

```c
hu_tool_t tools[2];
hu_web_fetch_create(&alloc, 50000, &tools[0]);
hu_weather_create(&alloc, &tools[1]);
hu_agent_from_config(&agent, &alloc, provider, tools, 2, ...);
```

## HU_IS_TEST

Use `#if HU_IS_TEST` to bypass side effects in tests (network, process spawn, browser, etc.). Return stub data instead. This ensures tests are deterministic and do not require credentials or external services.

```c
#if HU_IS_TEST
    *out = hu_tool_result_ok("(stub)", 6);
    return HU_OK;
#else
    /* real implementation */
#endif
```

## Naming

- Types: `hu_<name>_t`
- Functions: `hu_<module>_<action>`
- Constants: `HU_SCREAMING_SNAKE_CASE`
- Factory registration keys: lowercase, user-facing (e.g. `"weather"`, `"my_channel"`)

## See Also

- [API Reference](../docs/api/README.md) for full type and function documentation
- [AGENTS.md](../AGENTS.md) for engineering protocol and change playbooks
