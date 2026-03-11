# human

C11 autonomous AI assistant runtime. ~1696 KB binary, <6 MB RAM, <30 ms startup.
Zero dependencies beyond libc (optional SQLite and libcurl).

Read `AGENTS.md` for the full engineering protocol. This file is the quick reference.

## Build & Test

```bash
# Dev build (ASan enabled, all channels)
cmake -B build -DHU_ENABLE_ALL_CHANNELS=ON -DHU_ENABLE_SQLITE=ON -DHU_ENABLE_PERSONA=ON -DHU_ENABLE_SKILLS=ON
cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)

# Run tests (4,638+ tests, must be 0 failures, 0 ASan errors)
./build/human_tests

# Release build
cmake -B build-release -DCMAKE_BUILD_TYPE=MinSizeRel -DHU_ENABLE_LTO=ON -DHU_ENABLE_ALL_CHANNELS=ON
cmake --build build-release -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

## Architecture

Vtable-driven and modular. Extend by implementing vtable structs + factory registration:

- `src/providers/` — `hu_provider_t` vtable (AI model providers)
- `src/channels/` — `hu_channel_t` vtable (messaging channels)
- `src/tools/` — `hu_tool_t` vtable (tool execution)
- `src/memory/` — `hu_memory_t` vtable (memory backends)
- `src/security/` — policy, pairing, secrets, sandboxing
- `src/runtime/` — `hu_runtime_t` vtable (native, docker, wasm)
- `src/peripherals/` — `hu_peripheral_t` vtable (Arduino, STM32, RPi)
- `src/persona/` — persona profiles, prompt builder, example banks

## Naming

- Functions, variables, fields, files: `snake_case`
- Types/structs: `hu_<name>_t` (e.g. `hu_provider_t`)
- Constants/macros: `HU_SCREAMING_SNAKE` (e.g. `HU_OK`, `HU_ERR_NOT_SUPPORTED`)
- Public functions: `hu_<module>_<action>` (e.g. `hu_provider_create`)
- Test functions: `subject_expected_behavior`

## Rules (mandatory)

- C11 standard. Compiles with `-Wall -Wextra -Wpedantic -Werror`.
- Free every allocation. ASan catches leaks. No exceptions.
- Never use `SQLITE_TRANSIENT` — use `SQLITE_STATIC` (null).
- Use `HU_IS_TEST` guards for side effects (network, spawning, hardware I/O).
- Tests: no real network, no browser, no process spawning, deterministic.
- Security: deny-by-default, HTTPS-only for outbound, never log secrets.
- KISS/YAGNI: no speculative abstractions or config flags without a caller.
- One concern per change. Don't mix feature + refactor + infra.
- Use `--hu-surface-container*` for branded tonal surfaces, `--hu-bg-surface` for neutral.
- Use tinted state overlays (`--hu-hover-overlay`, etc.) — they are primary-colored, not neutral.

## Commit Format

Conventional commits enforced by `.githooks/commit-msg`:

```
<type>[(<scope>)]: <description>
```

Types: `feat fix refactor test docs chore perf ci build style`

## CI Pipeline

| Workflow                    | What it checks                                                                                                                                  |
| --------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| `ci.yml`                    | C build + 3726 tests (Linux + macOS), UI tsc + vitest + build, website build, clang-tidy, E2E, visual regression, axe accessibility, Lighthouse |
| `benchmark.yml`             | Performance regression (binary size, startup time, RSS)                                                                                         |
| `codeql.yml`                | Static analysis security scanning                                                                                                               |
| `security.yml`              | Dependency audit, SBOM generation                                                                                                               |
| `release.yml`               | Build release artifacts, .deb packages, cross-ARM64                                                                                             |
| `competitive-benchmark.yml` | Weekly PageSpeed competitive analysis (15 brands, 7 scoring dimensions)                                                                         |

Rule: if CI will catch it, run the equivalent locally first.

## Persona System

Persona profiles live in `~/.human/personas/` (JSON). Key structs in `include/human/persona.h`:

- `hu_persona_t` — identity, traits, vocab, communication rules, values, decision style
- `hu_persona_overlay_t` — per-channel formality/length/emoji overrides
- `hu_persona_example_bank_t` — example conversations per channel

Extend via: `src/persona/` (persona.c, creator.c, analyzer.c, sampler.c, examples.c, feedback.c, cli.c).

## Key Paths

| Path                                  | What                                                        |
| ------------------------------------- | ----------------------------------------------------------- |
| `src/`                                | All C source (~715 files, ~139K lines)                      |
| `include/human/`                      | Public headers                                              |
| `docs/cross-platform-ci-readiness.md` | Platform support, build flags, known platform-specific code |
| `tests/`                              | 131 test files, 4,638+ tests                                |
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

- Typeface: **Avenir** (web: `var(--hu-font)`, never Google Fonts)
- Icons: **Phosphor Regular** (web: `ui/src/icons.ts`)
- Tokens: `--hu-*` CSS custom properties from `design-tokens/`
- Never use raw hex colors, pixel spacing, or pixel radii in any UI code.
