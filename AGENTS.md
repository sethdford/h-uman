# AGENTS.md — h-uman Agent Engineering Protocol

_not quite human._

This file defines the default working protocol for coding agents in this repository.
Scope: entire repository.

## 1) Project Snapshot (Read First)

human is a C11 autonomous AI assistant runtime optimized for:

- minimal binary size (~2468 KB release with LTO)
- minimal memory footprint (5–6 MB peak RSS measured)
- zero dependencies beyond libc, optional SQLite and libcurl
- Zig reference implementation archived in `archive/zig-reference/`

Core architecture is **vtable-driven** and modular. All extension work is done by implementing
vtable structs and registering them in factory functions.

Key extension points:

- `src/providers/` (`hu_provider_t`) — AI model providers
- `src/channels/` (`hu_channel_t`) — messaging channels
- `src/tools/` (`hu_tool_t`) — tool execution surface
- `src/memory/` (`hu_memory_t`) — memory backends
- `src/observability/` (`hu_observer_t`) — observability hooks
- `src/runtime/` (`hu_runtime_t`) — execution environments
- `src/peripherals/` (`hu_peripheral_t`) — hardware boards (Arduino, STM32, RPi)
- `src/persona/` — persona system (profile loading, prompt builder, example selection)
- `src/ml/` — on-device ML training (BPE, GPT, DPO, LoRA, feed predictor) — `HU_ENABLE_ML`

Current scale: **1995 source + header files, ~423K lines of C, ~280K lines of tests, 13,820 tests, 31 channels**.

Performance baseline (macOS aarch64, MinSizeRel+LTO):

| Metric                   | Measured       |
| ------------------------ | -------------- |
| Binary size              | ~2468 KB       |
| Text section             | 480 KB         |
| Cold-start (`--version`) | 4–27 ms avg    |
| Peak RSS (`--version`)   | ~5.7 MB        |
| Peak RSS (test suite)    | ~6.0 MB        |
| Test throughput          | 700+ tests/sec |

Build and test:

```bash
mkdir build && cd build
cmake .. -DHU_ENABLE_ALL_CHANNELS=ON -DHU_ENABLE_SKILLS=ON  # configure
cmake --build . -j$(nproc)              # dev build
./human_tests                        # run all tests
cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DHU_ENABLE_LTO=ON  # release build
```

## 2) Deep Architecture Observations (Why This Protocol Exists)

These codebase realities should drive every design decision:

1. **Vtable + factory architecture is the stability backbone**
   - Extension points are explicit and swappable via `void *ctx` + function pointer vtables.
   - Callers must OWN the implementing struct (local var or heap-alloc). Never return a vtable interface pointing to a temporary — the pointer will dangle.
   - Most features should be added via vtable implementation + factory registration, not cross-cutting rewrites.

2. **Binary size and memory are hard product constraints**
   - `cmake -DCMAKE_BUILD_TYPE=MinSizeRel -DHU_ENABLE_LTO=ON` is the release target. Every dependency and abstraction has a size cost.
   - Avoid adding unnecessary runtime allocations or large data tables without justification.
   - Current release binary: ~2468 KB (all features with LTO).

3. **Security-critical surfaces are first-class**
   - `src/gateway/gateway.c`, `src/security/`, `src/tools/`, `src/runtime/` carry high blast radius.
   - Defaults are secure-by-default (pairing, HTTPS-only, allowlists, AEAD encryption). Keep it that way.

4. **C11 is the baseline — strict standards compliance**
   - HTTP client: libcurl (`HU_ENABLE_CURL=ON`), conditionally compiled.
   - Child processes: `fork`/`exec` on POSIX, guarded by `HU_GATEWAY_POSIX`.
   - SQLite: linked via system or Homebrew paths.
   - All code compiles with `-Wall -Wextra -Wpedantic -Werror`.
   - Use `HU_IS_TEST` guards to bypass side effects (spawning, opening URLs, real hardware I/O).

5. **All 13,820+ tests must pass at zero ASan errors**
   - The test suite uses AddressSanitizer for leak and overflow detection.
   - Every allocation must be freed (`free()` or cleanup function).
   - Use `HU_IS_TEST` mock paths in tests — no network, no process spawning.

## 3) Engineering Principles (Normative)

These principles are mandatory. They are implementation constraints, not suggestions.

Full details: `docs/standards/engineering/principles.md`

Summary: **KISS** (straightforward control flow, explicit `#ifdef`), **YAGNI** (no speculative flags/methods), **DRY + Rule of Three** (extract after 3 repetitions), **Fail Fast** (explicit errors, `HU_IS_TEST` guards), **Secure by Default** (deny-by-default, HTTPS-only, never log secrets), **Determinism** (no real network/browser/hardware in tests, reproducible across macOS/Linux).

## 4) Repository Map (High-Level)

```
src/
  main.c                CLI entrypoint and command routing
  agent/                agent loop, context, planner, compaction, dispatcher
  channels/             64 channel implementations (cli, telegram, discord, slack, ...)
  providers/            50+ AI provider implementations (9 core + 41 compatible services)
  tools/                99 tool implementations
  memory/               SQLite + markdown + LRU + LanceDB + Lucid backends, embeddings, vector search, connections, consolidation, multimodal ingest
  security/             policy, pairing, secrets, sandbox backends (landlock, firejail, bwrap)
  runtime/              runtime adapters (native, docker, wasm, cloudflare)
  core/                 allocator, arena, error, json, http, string, slice
  observability/        log + metrics observers
  sse/                  SSE client
  websocket/            WebSocket client
  peripherals/          hardware peripherals (Arduino, STM32/Nucleo, RPi)
  persona/              persona profiles, prompt builder, example banks
  config.c              schema + config loading/merging (~/.human/config.json)
  gateway/gateway.c     webhook/HTTP gateway server
  ml/                   ML training (BPE, GPT, DPO, LoRA, feed predictor, experiment loop, CLI) — HU_ENABLE_ML
  feeds/                Feed processor, research agent, social feeds, file ingest
  pwa/                  Progressive web app bridge, context, entities, CDP
  voice/                Voice pipeline, realtime, WebRTC
  eval.c                Evaluation framework
  ...

include/human/       public C headers

tests/                 580+ test files, 9,800+ tests

apps/                  iOS, macOS, Android, shared (4 app directories)

asm/                   platform-specific assembly (aarch64, x86_64, generic C)

fuzz/                  31 libFuzzer harnesses

archive/zig-reference/ archived Zig source (build.zig, src/)
```

For a detailed concept-to-file mapping (which source files and test files correspond to each subsystem), see [`docs/CONCEPT_INDEX.md`](docs/CONCEPT_INDEX.md).

For system topology, request flow, and module dependency diagrams, see [`ARCHITECTURE.md`](ARCHITECTURE.md).

Each major `src/` subdirectory has its own `CLAUDE.md` with module-specific architecture, rules, and playbooks. Read the relevant module's `CLAUDE.md` before modifying it.

## 4.1) Standards Directory

All project standards live in `docs/standards/`. This is the single source of truth -- agent config files reference these, never duplicate them.

| Area        | Path                          | Covers                                                                                                                                              |
| ----------- | ----------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| AI          | `docs/standards/ai/`          | Agent architecture, conversation design, hallucination prevention, prompt engineering, evaluation, citation/sourcing, human-in-the-loop, disclosure |
| Brand       | `docs/standards/brand/`       | Terminology governance (canonical product, technical, and UI/CLI terms)                                                                             |
| Design      | `docs/standards/design/`      | Visual standards, motion design, UX patterns, design strategy, design system                                                                        |
| Engineering | `docs/standards/engineering/` | Principles, naming, anti-patterns, testing, workflow, memory management, performance budgets, C API design, gateway API, cross-platform             |
| Operations  | `docs/standards/operations/`  | Incident response, monitoring and observability                                                                                                     |
| Quality     | `docs/standards/quality/`     | Governance, ceremonies, code review, channel testing                                                                                                |
| Security    | `docs/standards/security/`    | Threat model, sandbox, AI safety, data privacy and lifecycle                                                                                        |

**Before writing code, read the applicable standard.** Full index: `docs/standards/README.md`.

## 5) Risk Tiers by Path (Review Depth Contract)

- **Low risk**: docs, comments, test additions, minor formatting
- **Medium risk**: most `src/**` behavior changes without boundary/security impact
- **High risk**: `src/security/**`, `src/gateway/gateway.c`, `src/tools/**`, `src/runtime/`, config schema, vtable interfaces

When uncertain, classify as higher risk.

## 6) Agent Workflow (Required)

1. **Read before write** — inspect existing module, vtable wiring, and adjacent tests before editing.
2. **Define scope boundary** — one concern per change; avoid mixed feature+refactor+infra patches.
3. **Implement minimal patch** — apply KISS/YAGNI/DRY rule-of-three explicitly.
4. **Validate** — `cmake --build build && ./build/human_tests` must show 0 failures and 0 ASan errors.
   - During iteration, use `./build/human_tests --suite=<name>` to run targeted tests.
   - Use `scripts/what-to-test.sh <changed-files>` to find relevant suites.
   - Use `scripts/agent-preflight.sh` for change-aware validation before committing (runs `doc-fleet` when `docs/`, `human-skills/`, `skill-registry/`, or any `.md` changes).
   - Run full suite (`./build/human_tests` with no args) before final commit.
   - Optional Memory v2 layered smoke (G1.5 / execution plan): `bash scripts/memory-v2-local-proof.sh` (G2 header check + `--suite="v2 E2E"`; set `FULL_V2_SUITES=1` to fan out per W7–W16 suite substrings).
5. **Document impact** — update comments/docs for behavior changes, risk, and side effects.

### 6.1 Code Naming Contract (Required)

Full details: `docs/standards/engineering/naming.md`

Quick reference: `snake_case` for identifiers, `hu_<name>_t` for types, `HU_SCREAMING_SNAKE` for constants, `hu_<module>_<action>` for public functions, `subject_expected_behavior` for tests.

### 6.2 Architecture Boundary Contract (Required)

Full details: `docs/standards/engineering/principles.md` (Architecture Boundaries section)

Summary: vtable-first extension, inward dependency direction, no cross-subsystem coupling, single-purpose modules.

## 7) Change Playbooks

### 7.1 Adding a Provider

- Add `src/providers/<name>.c` implementing `hu_provider_t` vtable (`chat`, `supports_native_tools`, `get_name`, `deinit`).
- Register in `src/providers/factory.c`.
- Add tests for vtable wiring, error paths, and config parsing.

### 7.2 Adding a Channel

- Add `src/channels/<name>.c` implementing `hu_channel_t` vtable.
- Keep `send`, `listen`, `name`, `is_configured` semantics consistent with existing channels.
- Cover auth/config/health behavior with tests.

### 7.3 Adding a Tool

- Add `src/tools/<name>.c` implementing `hu_tool_t` vtable (`execute`, `name`, `description`, `parameters_json`).
- Validate and sanitize all inputs. Return `hu_tool_result_t`; never crash in the runtime path.
- Add `HU_IS_TEST` guard if the tool spawns processes or opens network connections.
- Register in `src/tools/factory.c`.

### 7.4 Adding a Peripheral

- Implement the `hu_peripheral_t` interface.
- Peripherals expose `read`/`write` methods that delegate to real hardware I/O.
- Use `probe-rs` CLI for STM32/Nucleo flash access; serial JSON protocol for Arduino.
- Non-Linux platforms must return `HU_ERR_NOT_SUPPORTED` (not silent 0).

### 7.5 Security / Runtime / Gateway Changes

- Include threat/risk notes in the commit or PR.
- Add/update tests for failure modes and boundaries.
- Keep observability useful but non-sensitive (no secrets in logs or errors).
- When adding a new gateway method in C, also add the mock response in `ui/src/demo-gateway.ts`.

### 7.6 Adding or Modifying a Persona

- Persona profiles are JSON files in `~/.human/personas/`.
- Core struct: `hu_persona_t` in `include/human/persona.h` — identity, traits, preferred/avoided vocab, communication rules, values, decision style.
- Per-channel overrides: `hu_persona_overlay_t` — formality, avg_length, emoji_usage, style_notes per channel.
- Example banks: `hu_persona_example_bank_t` — example conversations (context + incoming + response) grouped by channel.
- Implementation: `src/persona/persona.c` (loading/parsing), `creator.c` (generation), `analyzer.c` (analysis), `sampler.c` (sampling), `examples.c` (example selection), `feedback.c` (feedback loop), `cli.c` (CLI subcommands).
- Prompt builder composes system prompts from persona identity + traits + channel overlay + selected examples.
- Add tests for JSON parsing, overlay lookup, prompt composition, and edge cases (missing fields, empty arrays).

### 7.7 ML Training / CLI

The `human ml` subcommand exposes on-device ML training. All gated behind `HU_ENABLE_ML`.

| Subcommand | What it does |
| --- | --- |
| `train` | Run time-budgeted training on tokenized data |
| `experiment` | Autonomous experiment loop (mutate config, train, eval, keep/discard) |
| `prepare` | Tokenize files into binary training data |
| `prepare-conversations` | Prepare conversation history as training data |
| `dpo-train` | Train with Direct Preference Optimization on collected pairs |
| `lora-persona` | Fine-tune LoRA adapter on persona example bank |
| `train-feed-predictor` | Train topic/trend predictor on feed data |
| `status` | Show ML subsystem status |

Key files: `src/ml/cli.c` (CLI handlers), `src/ml/dpo.c`, `src/ml/lora.c`, `src/ml/train.c`, `src/ml/experiment.c`.
Providers: `src/providers/huml.c` (HUML checkpoint inference), `src/providers/embedded.c` (llama-cli local inference).
Tests: `tests/test_ml.c`.

## 8) Validation Matrix

Required before any code commit:

```bash
cmake --build build -j$(nproc) && ./build/human_tests  # all tests must pass, 0 ASan errors
```

For faster iteration, use targeted tests:

```bash
./build/human_tests --suite=JSON           # run suites matching "JSON"
./build/human_tests --filter=config_parse  # run tests matching "config_parse"
scripts/agent-preflight.sh                 # change-aware validation (auto-detects what changed)
scripts/run-native-fleet-local.sh quick    # native apps: HumanKit + iOS/macOS SPM + Android unit (no Simulator)
python3 scripts/eval_synthetic_imessage_offline.py  # offline iMessage-shaped gates (seconds; mirrors CI)
```

CI runs **XCUITest** and **Android instrumented** in `.github/workflows/native-apps-fleet.yml` (matrix + SOTA gate) and a single iOS simulator in `ci.yml` via `.github/actions/ios-uitest`. Optional full local parity on macOS: `scripts/run-native-fleet-local.sh full` or `VERIFY_NATIVE=1 ./scripts/verify-all.sh`. Optional red-team / eval aggregation (offline): `VERIFY_REDTEAM=1 ./scripts/verify-all.sh` or `bash scripts/redteam-eval-fleet.sh`.

For release changes:

```bash
cmake --preset release && cmake --build --preset release  # must compile clean
```

CMake presets (`CMakePresets.json`) are available: `dev`, `test`, `release`, `fuzz`, `minimal`.

```bash
cmake --preset dev     # configure dev build (ASan, all features, compile_commands.json)
cmake --build --preset dev
```

Additional expectations by change type:

- **Docs/comments only**: no build required, but verify no broken code references.
- **Security/runtime/gateway/tools**: include at least one boundary/failure-mode test.
- **Provider additions**: test vtable wiring + graceful failure without credentials.
- **Native apps (`apps/`)**: `scripts/run-native-fleet-local.sh quick` (or rely on `scripts/agent-preflight.sh` when `apps/` changed).

If full validation is impractical, document what was run and what was skipped.

### 8.0 Unified Verification

Run the combined verification script to check everything at once:

```bash
./scripts/verify-all.sh
```

This runs: C build, C tests, UI check, doc index, standards drift, and token lint. See `docs/standards/quality/ceremonies.md` for the full ceremony schedule.

### 8.1 Git Hooks

The repository ships with pre-configured hooks in `.githooks/`. Activate once per clone:

```bash
git config core.hooksPath .githooks
```

Hooks:

| Hook         | What it does                                                                                   |
| ------------ | ---------------------------------------------------------------------------------------------- |
| `pre-commit` | Runs format checks — blocks commit if code is not formatted                                    |
| `commit-msg` | Enforces conventional commit format (`feat`, `fix`, `refactor`, `test`, `docs`, `chore`, etc.) |
| `pre-push`   | Runs `cmake --build build-check && ./build-check/human_tests` — blocks push if any test fails  |

To bypass a hook in an emergency: `git commit --no-verify` / `git push --no-verify`.

### 8.2 Branch Naming and Git Workflow

Full details: `docs/standards/engineering/workflow.md`

Quick reference: `<type>/<name>` branches, `<type>[(<scope>)]: <description>` commits (enforced by hooks), one concern per commit, max 3-day branch lifetime.

## 8.3) Quality Ceremonies

Standards drift is prevented through recurring ceremonies. Full details in `docs/standards/quality/ceremonies.md`.

| Ceremony           | Cadence              | Purpose                                                                     |
| ------------------ | -------------------- | --------------------------------------------------------------------------- |
| Weekly Drift Audit | Every Monday, 15 min | Run `./scripts/verify-all.sh`, check plans, scan tokens, find orphaned docs |
| PR Completion Gate | Every PR             | Author verifies checklist before requesting review                          |
| Release Gate       | Every release tag    | Full drift audit + CHANGELOG + binary size check                            |

Drift detection scripts:

```bash
./scripts/verify-all.sh              # Combined verification (build + test + doc fleet + tokens)
./scripts/doc-fleet.sh               # Docs + human-skills frontmatter; repo-wide Markdown relative links
./scripts/redteam-eval-fleet.sh      # Eval + adversarial C suites, doctor, eval list, harness dry-run; set REDTEAM_FLEET_LIVE=1 for API
./scripts/check-doc-index.sh         # Orphaned standards only (also inside doc-fleet)
./scripts/check-standards-drift.sh   # Stale paths only (also inside doc-fleet)
```

## 9) Privacy and Sensitive Data (Required)

- Never commit real API keys, tokens, credentials, personal data, or private URLs.
- Use neutral placeholders in tests: `"test-key"`, `"example.com"`, `"user_a"`.
- Test fixtures must be impersonal and system-focused.
- Review `git diff --cached` before push for accidental sensitive strings.

## 10) Anti-Patterns (Do Not)

Full details: `docs/standards/engineering/anti-patterns.md`

Critical reminders: no vtable pointers to temporaries (dangling), no skipping `free()` (ASan catches), no `SQLITE_TRANSIENT` (use `SQLITE_STATIC`), no cross-subsystem coupling, no speculative flags, one concern per change.

## 10.1) AI Model Version Policy (Required)

- **Never reference Gemini 2.0 or 2.5 models** — they are deprecated/end-of-life. Always use Gemini 3.0+ (currently 3.1 series).
- **Always verify model IDs before writing code.** Do a web search for current Vertex AI model availability. Model names change frequently (previews rotate, versions get deprecated).
- **Current canonical models (as of March 2026):**
  - `gemini-3.1-pro-preview` — highest capability, reasoning, emotional nuance (global endpoint only)
  - `gemini-3.1-flash-lite-preview` — fastest, cheapest, high-volume tasks
  - `gemini-3-flash-preview` — balanced speed/quality
- **All Gemini access uses Vertex AI** with Application Default Credentials (ADC). Never use raw API keys for Gemini.
- **Python eval scripts**: use `google.cloud.aiplatform` or direct REST to `https://{region}-aiplatform.googleapis.com/v1/projects/{project}/locations/{location}/publishers/google/models/{model}:generateContent`. Authenticate with ADC OAuth2 tokens.
- **Model router**: `src/agent/model_router.c` selects tier based on message complexity. Emotional/vulnerable messages MUST route to a higher-tier model (pro or flash, never flash-lite alone).

## 11) Handoff Template (Agent → Agent / Maintainer)

When handing off work, include:

1. What changed
2. What did not change
3. Validation run and results (`./build/human_tests`)
4. Remaining risks / unknowns
5. Next recommended action

## 12) UI & Design System Contract

All UI surfaces (web dashboard, website, native apps, CLI/TUI) must follow these rules.
The design system is grounded in SOTA references from industry leaders.

### 12.0 SOTA Design References (Read Before Any UI Work)

The Human design system synthesizes principles from these authoritative sources.
Agents **must** consult the relevant reference docs before creating or modifying any UI:

| Source                         | What We Take                                                           | Human Doc                                   |
| ------------------------------ | ---------------------------------------------------------------------- | ------------------------------------------- |
| **Apple HIG**                  | Spring-first motion, clarity/deference/depth, spatial hierarchy        | `docs/standards/design/motion-design.md`    |
| **Material Design 3**          | Canonical layouts, easing taxonomy, elevation, dynamic color           | `docs/standards/design/ux-patterns.md`      |
| **Disney/Pixar 12 Principles** | Squash & stretch, anticipation, staging, timing, follow-through        | `docs/standards/design/motion-design.md`    |
| **Edward Tufte**               | Data-ink ratio, chartjunk elimination, small multiples                 | `docs/standards/design/visual-standards.md` |
| **Dieter Rams**                | Less is more, honest design, long-lasting quality                      | `docs/standards/design/visual-standards.md` |
| **Gestalt Psychology**         | Proximity, similarity, continuity, closure, figure-ground              | `docs/standards/design/ux-patterns.md`      |
| **Nielsen Norman Group**       | F-pattern scanning, progressive disclosure, recognition over recall    | `docs/standards/design/ux-patterns.md`      |
| **Competitive Benchmarks**     | Named brand targets, quantified quality deltas, category-defining bars | `docs/competitive-benchmarks.md`            |
| **Quality Scorecard**          | Per-surface scoring (7 dimensions), quarterly tracking                 | `docs/quality-scorecard.md`                 |

**Mandatory document consultation before UI work:**

- **Creating or restructuring a view?** → Read `docs/standards/design/ux-patterns.md` first. Every view must conform to an archetype.
- **Adding or modifying animation?** → Read `docs/standards/design/motion-design.md` first. Every animation must follow Disney/Pixar + Apple + M3 principles.
- **Making any visual change?** → Read `docs/standards/design/visual-standards.md` first. Verify hierarchy, spacing, color application, and quality checklist.
- **Token values and specifics?** → Read `docs/standards/design/design-strategy.md` for the complete token reference.

### 12.1 Layout Archetypes (Required — see `docs/standards/design/ux-patterns.md`)

Every view must conform to one of these canonical archetypes:

| Archetype            | Views                            | Key Rule                                                 |
| -------------------- | -------------------------------- | -------------------------------------------------------- |
| **Dashboard (Feed)** | Overview, Usage, Security        | Compact hero → scrollable card grid                      |
| **List-Detail**      | Sessions, Channels, Tools, Nodes | Dual-pane on expanded; single-pane navigation on compact |
| **Conversational**   | Chat, Voice                      | Content first (flex: 1), controls anchored at bottom     |
| **Settings**         | Config                           | Single-column, max-width 640px, section-grouped          |
| **Marketplace**      | Skills                           | Sticky search → responsive card grid                     |
| **Log/Terminal**     | Logs                             | Controls top → monospace output fills remaining space    |

Critical rules:

- **Conversation views**: conversation area MUST be primary (flex: 1), controls MUST anchor to bottom. Never place controls above conversation.
- **All views**: content occupies minimum 60% of viewport. Controls defer to content (Apple HIG: Deference).
- **Empty/loading/error states**: every view must handle all three. Never show a blank screen.

### 12.2 Typography

Required:

- **Avenir** is the canonical typeface across all platforms.
- Web: always use `var(--hu-font)` token. Never set `font-family` directly. Never import Google Fonts.
- Apple native: `Font.custom("Avenir-Book", size:)` / `"Avenir-Medium"` / `"Avenir-Heavy"` / `"Avenir-Black"`.
- Android: `AvenirFontFamily` from `Theme.kt`.
- CLI/TUI: terminal font (no control), but use token-derived ANSI colors from `design_tokens.h`.
- Maximum 3 type sizes visible in any single view section (see `docs/standards/design/visual-standards.md` §7).

### 12.3 Icons

Required:

- **Phosphor Regular** is the canonical icon library.
- Web dashboard: import from `ui/src/icons.ts`. Add new icons there using Phosphor Regular SVG paths.
- Website: inline Phosphor SVGs with `viewBox="0 0 256 256" fill="currentColor"`.
- Never use emoji characters as UI icons (no ⚠️, 💬, 🔧, ⚡, ⚙, etc.).
- Never create one-off SVGs when a Phosphor equivalent exists.
- Icons use `currentColor` — color is inherited from parent, never hardcoded.

### 12.4 Design Tokens

Required:

- Single source of truth: `design-tokens/` directory (W3C Design Tokens v2025.10 format).
- All platforms consume generated output, not hand-maintained values.
- CSS: `--hu-*` namespace. Never use raw hex colors, pixel spacing, or pixel radii.
- Token categories: color (base + semantic), spacing, radius, shadow, typography, motion, data-viz.
- Generated outputs: CSS custom properties, Kotlin constants, Swift constants, C `#define` macros.
- **Centralized design strategy**: see `docs/standards/design/design-strategy.md` for the full token reference.

Color accent hierarchy (60-30-10 rule — see `docs/standards/design/visual-standards.md` §2.1):

- **Primary**: `--hu-accent` (Human green) — brand identity, primary buttons, links, focus rings.
- **Secondary**: `--hu-accent-secondary` (amber) — warm highlights, featured content, CTAs needing contrast.
- **Tertiary**: `--hu-accent-tertiary` (indigo) — info states, data visualization, depth.
- **Error only**: coral — reserved exclusively for `--hu-error` / `--hu-error-dim`. Never use coral as a general accent.

Each accent provides `-hover`, `-subtle`, `-strong`, `-text`, and `on-accent-*` variants for both dark and light themes.

#### Tonal Surfaces (M3)

Surfaces are tinted with the primary accent for branded depth hierarchy:

- `--hu-surface-container` — default card/panel (4% Human green tint)
- `--hu-surface-container-high` — elevated interactive (6% tint)
- `--hu-surface-container-highest` — highest emphasis (8% tint)
- `--hu-surface-dim` / `--hu-surface-bright` — recessed / prominent extremes
- Use tonal surfaces instead of `--hu-bg-surface` when brand identity matters.
- Token source: `base.tokens.json` (color.tonal.\*) + `semantic.tokens.json`

#### Neutral State Layers (M3)

Interactive overlays are **neutral** (white veil on dark theme, black veil on light theme), not accent-tinted washes:

- `--hu-hover-overlay` / `--hu-pressed-overlay` / `--hu-focus-overlay` / `--hu-dragged-overlay`
- `--hu-disabled-overlay` remains neutral (no brand color on disabled states)

#### Dynamic Color Pipeline

OKLCH palette auto-generated from brand hex (#7AB648):

- Tokens: `--hu-dynamic-{primary,secondary,tertiary,neutral,error}-{50..950}`
- P3 wide-gamut overrides included automatically
- Generated: `ui/src/styles/_dynamic-color.css` (built by `design-tokens/build.ts`)

#### Glass System

Three glass tiers + choreography + Apple visionOS material densities:

- CSS classes: `.hu-glass-subtle`, `.hu-glass-standard`, `.hu-glass-prominent`
- Choreography: `.hu-glass-enter` / `.hu-glass-exit` (blur reveals from 0)
- Materials: `--hu-glass-material-{ultra-thin,thin,regular,thick}-*`
- Token source: `design-tokens/glass.tokens.json`
- Visual reference: `docs/design-system-demo.html`

### 12.5 Visual Hierarchy (Required)

Full details: `docs/standards/design/visual-standards.md`

Key rules: squint test, ONE high-emphasis element per screen, 60-30-10 color ratio, spacing rhythm via token scale, whitespace is deliberate.

### 12.6 Motion & Animation (Required)

Full details: `docs/standards/design/motion-design.md`

Key rules: spring-first easings, `--hu-duration-*` / `--hu-ease-*` / `--hu-spring-*` tokens only, compositor properties only (transform, opacity, filter), `prefers-reduced-motion: reduce` respected, `hu-` prefix for keyframes, never exceed 700ms.

### 12.7 Data Visualization (Required)

Full details: `docs/standards/design/visual-standards.md` §9

Key rules: maximize data-ink ratio, use `--hu-chart-*` tokens (categorical, sequential, diverging, brand), direct labels over legends, small multiples over overlapping series. Tokens in `design-tokens/data-viz.tokens.json`.

### 12.8 Lint Enforcement

Required:

- Run `ui/scripts/lint-raw-values.sh` to detect design token drift.
- Flags raw hex/rgba, hardcoded durations, and raw breakpoints in `.ts` files.
- Wired into `npm run check` via `npm run lint:tokens`.

### 12.9 Accessibility (Required)

Full details: `docs/standards/design/ux-patterns.md` §5

Key rules: WCAG 2.1 AA (4.5:1 text, 3:1 UI), visible focus rings, keyboard operable, 44×44px touch targets, `prefers-color-scheme` + `prefers-reduced-motion` supported, no color-only information.

### 12.10 Change Playbook: Adding a UI Component

- Add `ui/src/components/hu-<name>.ts` as a LitElement web component.
- Use `--hu-*` tokens exclusively in `static styles`.
- Add test file for render, accessibility, and keyboard navigation.
- Register in component catalog (`ui/src/catalog/`).
- Update `ui/src/icons.ts` if the component needs a new icon.
- Run the **quality checklist** from `docs/standards/design/visual-standards.md` §10 before shipping.

### 12.11 Change Playbook: Adding or Modifying a View

- Identify which **layout archetype** (§12.1) the view conforms to.
- Follow the archetype's structural rules from `docs/standards/design/ux-patterns.md` §2.
- Implement empty state, loading skeleton, and error state (all three required).
- Verify responsive behavior at all four breakpoints (compact, medium, expanded, wide).
- Apply the **quality checklist** from `docs/standards/design/visual-standards.md` §10.

### 12.12 UI Performance Budgets (Required)

Required:

- Lighthouse targets: Performance ≥95, Accessibility ≥98, Best Practices ≥95, SEO ≥95.
- Animate only compositor properties (`transform`, `opacity`, `filter`). Never animate `width`, `height`, `top`, `left`, `margin`, `padding` — these cause layout thrashing.
- Lazy-load views not in the initial viewport.
- Breakpoint annotations: every `@media (max-width: Xpx)` must include `/* --hu-breakpoint-* */`.
- Alpha transparency: use `color-mix(in srgb, var(--hu-token) XX%, transparent)` — never `rgba()`.
- Run `npm run lint:tokens` and `bash scripts/lint-raw-colors.sh --all` before UI commits.

### 12.13 Competitive Benchmarking (Required)

Required:

- All UI work must be evaluated against the competitive benchmark registry (`docs/competitive-benchmarks.md`).
- Quality scorecard (`docs/quality-scorecard.md`) updated quarterly for all surfaces.
- Lighthouse CI thresholds: Performance ≥95, Accessibility ≥98, Best Practices ≥95, SEO ≥95.
- Core Web Vitals targets: LCP <1.5s (warn), CLS <0.05 (warn), TTI <2s (warn).
- Category-defining stretch targets: LCP <0.5s, CLS 0.00, INP <50ms (tracked, not yet enforced in CI).
- Run `scripts/benchmark-competitive.sh` before major website releases.
- New design patterns must reference which competitor inspired them and how Human exceeds them.
- Benchmark brands: Linear, Vercel, Raycast, Stripe, Figma, Superhuman, Apple, Spotify + Awwwards winners.
- Every quarter, evaluate and adopt one emerging web platform feature before competitors (see `docs/standards/design/design-strategy.md` §Design Innovation Pipeline).

### 12.14 Key Component APIs

- **`hu-card`**: `glass`, `solid`, `hoverable`, `clickable`, `tilt`, `mesh`, `chromatic`, `entrance` (IO-driven reveal), `surface` (`"default"` | `"high"` | `"highest"`)
- **`hu-input`**: `variant="tonal"` for branded surface background
- **`hu-badge`**: `info` variant uses `--hu-accent-tertiary` (steel blue)

### 12.15 CSS Utility Classes (theme.css)

| Class | Purpose |
|-------|---------|
| `.hu-interactive` | Layered hover/pressed with accent-tinted border |
| `.hu-interactive-subtle` | Lighter variant for dense UIs |
| `.hu-link-structural` | Steel blue link for informational content |
| `.hu-stagger` | Stagger children entrance (50ms steps, 300ms cap) |
| `.hu-cascade` | Cascade children entrance (30ms steps, 200ms cap) |
| `.hu-scroll-reveal` | Scroll-driven viewport entrance |
| `.hu-scroll-reveal-stagger` | Scroll-driven entrance for container children |

### 12.16 View Transitions

Two distinct transition types:

- **Route**: `main-content` view-transition with spring animation (`app.ts`)
- **Theme**: `root` crossfade via `document.startViewTransition()` on theme toggle

## 13) Vibe Coding Guardrails

When working in fast iterative mode:

- Keep each iteration reversible (small commits, clear rollback).
- Validate assumptions with code search before implementing.
- Prefer deterministic behavior over clever shortcuts.
- Do not "ship and hope" on security-sensitive paths.
- If uncertain about C API usage, check `src/` for existing patterns before guessing.
- If uncertain about architecture, read the vtable interface definition in `include/human/` before implementing.
