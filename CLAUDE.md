# h-uman — not quite human.

C11 autonomous AI assistant runtime. ~2468 KB binary, <6 MB RAM, <30 ms startup.
Zero dependencies beyond libc (optional SQLite and libcurl).

Read `AGENTS.md` for the full engineering protocol. This file is the quick reference.

## Product Thesis (summary)

**The assistant that's actually yours** — a private, personal AI that runs on
your hardware, learns who you are locally, and never sends your identity to a
cloud. We don't compete on task execution, channel count, or benchmark scores
(table stakes). The honest moats are: **persona as compiled architecture** (41 C
modules, not markdown templates), **privacy by architecture** (local-first, not a
settings toggle), an **on-device personalization pipeline**, and **HuLa IR**
(typed, compiled tool orchestration).

Full thesis, the red-teamed reality check, the M1–M6 strategic missions, and the
competitive matrix live in **[`docs/PRODUCT.md`](docs/PRODUCT.md)** — kept out of
this always-loaded file so CLAUDE.md stays lean and the dated mission status
doesn't rot here.

## Build & Test

```bash
# Dev build — use CMake presets (recommended)
cmake --preset dev                 # ASan, all channels, SQLite, persona, skills, compile_commands.json
cmake --build --preset dev

# Other presets: test (no ASan), release (MinSizeRel+LTO), fuzz (Clang), minimal
cmake --list-presets               # show all available presets

# Run tests (13,735+ tests, must be 0 failures, 0 ASan errors)
./build/human_tests                          # full suite
./build/human_tests --suite=JSON             # run suites matching "JSON"
./build/human_tests --filter=config_parse    # run tests matching "config_parse"

# Agent workflow: targeted tests during iteration, full suite before commit
scripts/what-to-test.sh src/tools/shell.c    # find relevant suites for changed files
scripts/agent-preflight.sh                   # change-aware validation (auto-detects what changed)
```

## Architecture

See `ARCHITECTURE.md` for diagrams (system topology, request flow, module dependencies).

Vtable-driven and modular. Extend by implementing vtable structs + factory registration:

- `src/providers/` — `hu_provider_t` vtable (AI model providers)
- `src/channels/` — `hu_channel_t` vtable (messaging channels)
- `src/tools/` — `hu_tool_t` vtable (tool execution)
- `src/memory/` — `hu_memory_t` vtable (memory backends)
- `src/security/` — policy, pairing, secrets, sandboxing
- `src/runtime/` — `hu_runtime_t` vtable (native, docker are real; wasm/cloudflare/gce return `HU_ERR_NOT_SUPPORTED`)
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
- **AI Model Versions**: Never reference or use Gemini 2.0 or 2.5 models — they are deprecated. Always use Gemini 3.0+. Before writing any code that references a model version, do a web search AND probe the live Vertex AI endpoint (HTTP 200 from `:generateContent`) to verify availability. **Canonical lineup as of 2026-05-24 (empirically verified live on `johnb-2025/global`):**
  - **`gemini-3.5-flash`** — GA, launched 2026-05-19. **New default** for conversational/coding. Near-Pro quality at Flash speed/cost ($1.50/$9.00 per Mtok). Beats `gemini-3.1-pro-preview` on coding at ~25% lower cost.
  - **`gemini-3.1-pro-preview`** — Preview, launched 2026-02-19. Use for deep reasoning, analytical/deep tiers. $2/$12 per Mtok.
  - **`gemini-3.1-flash-lite-preview`** — Preview. Cheapest tier; use for high-volume classification, reflexive tier.
  - **`gemini-3.1-pro-preview-customtools`** — Pro variant optimized for custom-tool prioritization (view_file, search_code).
  - ❌ `gemini-3-pro-preview` — discontinued 2026-03-26, use `gemini-3.1-pro-preview`.
  - ❌ `gemini-3-flash-preview` — superseded by `gemini-3.5-flash`; still alive but not recommended for new code.
  - ❌ `gemini-3.1-flash-preview` (no such ID — only flash-lite for 3.1) and `gemini-3.5-pro` (3.5 is Flash-only).
  All Gemini access uses Vertex AI with Application Default Credentials (ADC), not API keys.
- **Gemini 3.x thinking-token budget gotcha** (discovered 2026-05-24, root cause of reactive-iMessage empty-response bug): Gemini 3.x models default to thinking-enabled with a large invisible thinking budget. Output tokens from `maxOutputTokens` are SHARED between thinking and the visible reply. A short max_tokens (e.g. 80) can leave 0 tokens for the actual response → empty content + `finishReason: MAX_TOKENS` + `thoughtsTokenCount: 72`. **Always pass `generationConfig.thinkingConfig.thinkingBudget` explicitly** in Vertex requests — `0` to disable for reflexive/short replies, a real number (e.g. 1024) for analytical tiers. The h-uman model_router carries a `thinking` field per tier; that value MUST flow to `thinkingConfig.thinkingBudget` in the request body. Verified live: with `thinkingBudget=0`, `gemini-3.5-flash` replies "Yeah, just chilling at home, what's up?" in 12 tokens; without it, same prompt returns empty after burning 72 thinking tokens.
- Use `--hu-surface-container*` for branded tonal surfaces, `--hu-bg-surface` for neutral.
- Use neutral state overlays (`--hu-hover-overlay`, etc.) — white/black veils on dark/light; brand shows in rings and primaries.

## Claude Code Features

Six features integrated from Claude Code architecture. See `docs/guides/claude-code-features.md` for full documentation:

1. **MCP Client** — Connect to external Model Context Protocol servers and discover tools
2. **Hook Pipeline** — Pre/post tool execution interception for security and auditing
3. **Permission Tiers** — Graduated access control (ReadOnly, WorkspaceWrite, DangerFullAccess)
4. **Structured Compaction** — XML-based context window compression with artifact pinning
5. **Session Persistence** — Auto-save and resume conversation history
6. **Instruction Discovery** — Merge .human.md/HUMAN.md/instructions.md from multiple levels

## Commit Format

Conventional commits enforced by `.githooks/commit-msg`:

```
<type>[(<scope>)]: <description>
```

Types: `feat fix refactor test docs chore perf ci build style`

## CI Pipeline

| Workflow                    | What it checks                                                                                                                                    |
| --------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ci.yml`                    | C build + 13,735+ tests (Linux + macOS), UI tsc + vitest + build, website build, clang-tidy, E2E, visual regression, axe accessibility, Lighthouse |
| `native-apps-fleet.yml`     | Multi-simulator iOS XCUITest + multi-API Android instrumented tests + SOTA gate (apps path / schedule / dispatch) |
| `.github/actions/ios-uitest` | Composite: XcodeGen + HumaniOS XCUITest (shared by `ci.yml` + fleet) |
| `benchmark.yml`             | Performance regression (binary size, startup time, RSS)                                                                                           |
| `codeql.yml`                | Static analysis security scanning                                                                                                                 |
| `security.yml`              | Dependency audit, SBOM generation                                                                                                                 |
| `release.yml`               | Build release artifacts (Linux x86_64 + macOS aarch64), Docker image, Trivy scan                                                                  |
| `competitive-benchmark.yml` | Weekly PageSpeed competitive analysis (15 brands, 7 scoring dimensions)                                                                           |

Rule: if CI will catch it, run the equivalent locally first.

## Persona System

Persona profiles live in `~/.human/personas/` (JSON). Key structs in `include/human/persona.h`:

- `hu_persona_t` — identity, traits, vocab, communication rules, values, decision style
- `hu_persona_overlay_t` — per-channel formality/length/emoji overrides
- `hu_persona_example_bank_t` — example conversations per channel

Extend via: `src/persona/` (persona.c, creator.c, analyzer.c, sampler.c, examples.c, feedback.c, cli.c).

## Key Paths

| Path                              | What                                                                  |
| --------------------------------- | --------------------------------------------------------------------- |
| `src/`                            | All C source (~1,050 `.c` files; ~484K lines of C with `include/` headers) |
| `include/human/`                  | Public headers                                                        |
| `tests/`                          | 760+ test files, 13,735+ tests                                       |
| `fuzz/`                           | 31 libFuzzer harnesses                                                |
| `ui/`                             | LitElement web dashboard                                              |
| `website/`                        | Astro marketing site                                                  |
| `apps/`                           | iOS, macOS, Android native apps + shared HumanKit                     |
| `design-tokens/`                  | W3C design tokens (source of truth for all UI)                        |
| `docs/`                           | Guides, plans, design docs                                            |
| `docs/standards/`                 | Canonical standards (AI, design, engineering, ops, quality, security) |
| `docs/CONCEPT_INDEX.md`           | Concept-to-file mapping (find the right file fast)                    |
| `docs/error-codes.md`             | All `HU_ERR_*` codes with usage guidelines                            |
| `scripts/`                        | Build, release, benchmark, check scripts                              |
| `scripts/agent-preflight.sh`      | Change-aware validation for agents                                    |
| `scripts/doc-fleet.sh`            | Docs gate: standards, terminology, frontmatter, repo-wide MD links |
| `scripts/what-to-test.sh`         | Maps changed files to relevant test suites                            |
| `scripts/gen-include-graph.sh`    | Module dependency graph (Mermaid or JSON)                             |
| `ARCHITECTURE.md`                 | System topology, request flow, module dependency diagrams             |
| `.claude/rules/`                  | Path-scoped rules for Claude Code agents                              |
| `.claude/skills/`                 | Executable playbooks (add-provider, add-channel, add-tool, preflight) |
| `.github/copilot-instructions.md` | GitHub Copilot agent context                                          |
| `CMakePresets.json`               | Named build presets (dev, test, release, fuzz, minimal)               |
| `.clang-tidy`                     | Static analysis config (matches CI)                                   |

## Standards

All project standards live in `docs/standards/`. This is the single source of truth -- read the applicable standard before writing code. Full index: `docs/standards/README.md`.

| Area        | Path                          | Covers                                                                                            |
| ----------- | ----------------------------- | ------------------------------------------------------------------------------------------------- |
| AI          | `docs/standards/ai/`          | Agent architecture, conversation, hallucination prevention, prompts, evaluation, disclosure       |
| Brand       | `docs/standards/brand/`       | Terminology governance                                                                            |
| Design      | `docs/standards/design/`      | Visual standards, motion, UX patterns, design strategy, design system                             |
| Engineering | `docs/standards/engineering/` | Principles, naming, testing, workflow, memory management, performance, API design, cross-platform |
| Operations  | `docs/standards/operations/`  | Incident response, monitoring                                                                     |
| Quality     | `docs/standards/quality/`     | Governance, ceremonies, code review, channel testing                                              |
| Security    | `docs/standards/security/`    | Threat model, sandbox, AI safety, data privacy                                                    |

## Risk Tiers

- **Low**: docs, comments, test additions, formatting
- **Medium**: most `src/` behavior changes
- **High**: `src/security/`, `src/gateway/gateway.c`, `src/tools/`, `src/runtime/`, config schema, vtable interfaces

## Design System (all platforms)

- Typeface: **Avenir** (web: `var(--hu-font)`, never Google Fonts)
- Icons: **Phosphor Regular** (web: `ui/src/icons.ts`)
- Tokens: `--hu-*` CSS custom properties from `design-tokens/`
- Never use raw hex colors, pixel spacing, or pixel radii in any UI code.
