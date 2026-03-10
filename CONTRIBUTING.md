# Contributing to Human

Human is an autonomous AI assistant runtime written in C11. MIT License.

## Development Setup

**Dependencies:**

- CMake 3.20+
- C11 compiler (Clang or GCC)
- SQLite3 (system or Homebrew)
- libcurl (optional, `HU_ENABLE_CURL=ON`)

**macOS:** `brew install cmake sqlite` (libcurl typically present)

**Linux:** `apt install build-essential cmake libsqlite3-dev` (or equivalent)

## Build and Test

```bash
mkdir -p build && cd build
cmake .. -DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc)
./human_tests
```

All 3795+ tests must pass. AddressSanitizer must report zero errors — every allocation must be freed.

**Release build:**

```bash
cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON
cmake --build . -j$(nproc)
```

## Code Style

- Functions, variables, fields: `snake_case`
- Types, structs, enums: `hu_<name>_t` (e.g., `hu_provider_t`, `hu_channel_t`)
- Constants and macros: `HU_SCREAMING_SNAKE_CASE` (e.g., `HU_OK`, `HU_ERR_NOT_SUPPORTED`)
- Public functions: `hu_<module>_<action>` (e.g., `hu_provider_create`, `hu_channel_send`)
- Tests: named by behavior (`subject_expected_behavior`)

Code compiles with `-Wall -Wextra -Wpedantic -Werror`.

## Extending Human (Vtable Pattern)

All extension points use vtable structs + factory registration:

| Extension          | Vtable            | Location           | Register in               |
| ------------------ | ----------------- | ------------------ | ------------------------- |
| AI providers       | `hu_provider_t`   | `src/providers/`   | `src/providers/factory.c` |
| Messaging channels | `hu_channel_t`    | `src/channels/`    | channel factory           |
| Tools              | `hu_tool_t`       | `src/tools/`       | `src/tools/factory.c`     |
| Memory backends    | `hu_memory_t`     | `src/memory/`      | memory factory            |
| Runtimes           | `hu_runtime_t`    | `src/runtime/`     | runtime factory           |
| Peripherals        | `hu_peripheral_t` | `src/peripherals/` | peripheral factory        |

**Adding a provider:** Implement vtable (`chat`, `supports_native_tools`, `get_name`, `deinit`), register in factory, add tests for wiring and error paths.

**Adding a channel:** Implement `hu_channel_t` (`send`, `listen`, `name`, `is_configured`), keep semantics consistent with existing channels.

**Adding a tool:** Implement vtable (`execute`, `name`, `description`, `parameters_json`), validate/sanitize inputs, use `HU_IS_TEST` guard if spawning processes or opening network.

See `AGENTS.md` for detailed playbooks.

## Git Workflow

1. Fork the repo.
2. Create a branch from `main`.
3. Make changes. One concern per change.
4. Run `./build/human_tests` — must pass with zero ASan errors.
5. Push and open a PR.

## Good PR

- One concern per change
- Tests pass
- No ASan errors
- Code follows naming conventions
- No unrelated edits

## Questions

[GitHub Issues](https://github.com/sethdford/human/issues) — use for bugs, feature requests, or questions.
