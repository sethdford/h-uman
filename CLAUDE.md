# seaclaw

C11 autonomous AI assistant runtime. ~1506 KB binary, <6 MB RAM, <30 ms startup.
Zero dependencies beyond libc (optional SQLite and libcurl).

Read `AGENTS.md` for the full engineering protocol. This file is the quick reference.

## Build & Test

```bash
# Dev build (ASan enabled, all channels)
cmake -B build -DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_SQLITE=ON -DSC_ENABLE_PERSONA=ON -DSC_ENABLE_SKILLS=ON
cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)

# Run tests (3,726+ tests, must be 0 failures, 0 ASan errors)
./build/seaclaw_tests

# Release build
cmake -B build-release -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON -DSC_ENABLE_ALL_CHANNELS=ON
cmake --build build-release -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

## Architecture

Vtable-driven and modular. Extend by implementing vtable structs + factory registration:

- `src/providers/` — `sc_provider_t` vtable (AI model providers)
- `src/channels/` — `sc_channel_t` vtable (messaging channels)
- `src/tools/` — `sc_tool_t` vtable (tool execution)
- `src/memory/` — `sc_memory_t` vtable (memory backends)
- `src/security/` — policy, pairing, secrets, sandboxing
- `src/runtime/` — `sc_runtime_t` vtable (native, docker, wasm)
- `src/peripherals/` — `sc_peripheral_t` vtable (Arduino, STM32, RPi)
- `src/persona/` — persona profiles, prompt builder, example banks

## Naming

- Functions, variables, fields, files: `snake_case`
- Types/structs: `sc_<name>_t` (e.g. `sc_provider_t`)
- Constants/macros: `SC_SCREAMING_SNAKE` (e.g. `SC_OK`, `SC_ERR_NOT_SUPPORTED`)
- Public functions: `sc_<module>_<action>` (e.g. `sc_provider_create`)
- Test functions: `subject_expected_behavior`

## Rules (mandatory)

- C11 standard. Compiles with `-Wall -Wextra -Wpedantic -Werror`.
- Free every allocation. ASan catches leaks. No exceptions.
- Never use `SQLITE_TRANSIENT` — use `SQLITE_STATIC` (null).
- Use `SC_IS_TEST` guards for side effects (network, spawning, hardware I/O).
- Tests: no real network, no browser, no process spawning, deterministic.
- Security: deny-by-default, HTTPS-only for outbound, never log secrets.
- KISS/YAGNI: no speculative abstractions or config flags without a caller.
- One concern per change. Don't mix feature + refactor + infra.
- Use `--sc-surface-container*` for branded tonal surfaces, `--sc-bg-surface` for neutral.
- Use tinted state overlays (`--sc-hover-overlay`, etc.) — they are primary-colored, not neutral.

## Commit Format

Conventional commits enforced by `.githooks/commit-msg`:

```
<type>[(<scope>)]: <description>
```

Types: `feat fix refactor test docs chore perf ci build style`

## CI Pipeline

| Workflow        | What it checks                                                                                                                                  |
| --------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| `ci.yml`        | C build + 3207 tests (Linux + macOS), UI tsc + vitest + build, website build, clang-tidy, E2E, visual regression, axe accessibility, Lighthouse |
| `benchmark.yml` | Performance regression (binary size, startup time, RSS)                                                                                         |
| `codeql.yml`    | Static analysis security scanning                                                                                                               |
| `security.yml`  | Dependency audit, SBOM generation                                                                                                               |
| `release.yml`   | Build release artifacts, .deb packages, cross-ARM64                                                                                             |

Rule: if CI will catch it, run the equivalent locally first.

## Persona System

Persona profiles live in `~/.seaclaw/personas/` (JSON). Key structs in `include/seaclaw/persona.h`:

- `sc_persona_t` — identity, traits, vocab, communication rules, values, decision style
- `sc_persona_overlay_t` — per-channel formality/length/emoji overrides
- `sc_persona_example_bank_t` — example conversations per channel

Extend via: `src/persona/` (persona.c, creator.c, analyzer.c, sampler.c, examples.c, feedback.c, cli.c).

## Key Paths

| Path                                  | What                                                        |
| ------------------------------------- | ----------------------------------------------------------- |
| `src/`                                | All C source (~715 files, ~136K lines)                      |
| `include/seaclaw/`                    | Public headers                                              |
| `docs/cross-platform-ci-readiness.md` | Platform support, build flags, known platform-specific code |
| `tests/`                              | 128 test files, 3,726+ tests                                 |
| `fuzz/`                               | libFuzzer harnesses                                         |
| `ui/`                                 | LitElement web dashboard                                    |
| `website/`                            | Astro marketing site                                        |
| `apps/`                               | iOS, macOS, Android, Flutter native apps                    |
| `design-tokens/`                      | W3C design tokens (source of truth for all UI)              |
| `docs/`                               | Guides, plans, design docs                                  |
| `docs/design-system-demo.html`        | Interactive design system demo                              |
| `scripts/`                            | Build, release, benchmark, check scripts                    |

## Risk Tiers

- **Low**: docs, comments, test additions, formatting
- **Medium**: most `src/` behavior changes
- **High**: `src/security/`, `src/gateway/gateway.c`, `src/tools/`, `src/runtime/`, config schema, vtable interfaces

## Design System (all platforms)

- Typeface: **Avenir** (web: `var(--sc-font)`, never Google Fonts)
- Icons: **Phosphor Regular** (web: `ui/src/icons.ts`)
- Tokens: `--sc-*` CSS custom properties from `design-tokens/`
- Never use raw hex colors, pixel spacing, or pixel radii in any UI code.
