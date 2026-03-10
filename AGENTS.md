# AGENTS.md — human Agent Engineering Protocol

This file defines the default working protocol for coding agents in this repository.
Scope: entire repository.

## 1) Project Snapshot (Read First)

human is a C11 autonomous AI assistant runtime optimized for:

- minimal binary size (~1679 KB release with LTO)
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

Current scale: **715 source + header files, ~138K lines of C, ~60K lines of tests, 3795 tests, 34 channels**.

Performance baseline (macOS aarch64, MinSizeRel+LTO):

| Metric                   | Measured       |
| ------------------------ | -------------- |
| Binary size              | ~1679 KB       |
| Text section             | 480 KB         |
| Cold-start (`--version`) | 4–27 ms avg    |
| Peak RSS (`--version`)   | ~5.7 MB        |
| Peak RSS (test suite)    | ~6.0 MB        |
| Test throughput          | 700+ tests/sec |

Build and test:

```bash
mkdir build && cd build
cmake .. -DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_SKILLS=ON  # configure
cmake --build . -j$(nproc)              # dev build
./human_tests                        # run all tests
cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON  # release build
```

## 2) Deep Architecture Observations (Why This Protocol Exists)

These codebase realities should drive every design decision:

1. **Vtable + factory architecture is the stability backbone**
   - Extension points are explicit and swappable via `void *ctx` + function pointer vtables.
   - Callers must OWN the implementing struct (local var or heap-alloc). Never return a vtable interface pointing to a temporary — the pointer will dangle.
   - Most features should be added via vtable implementation + factory registration, not cross-cutting rewrites.

2. **Binary size and memory are hard product constraints**
   - `cmake -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON` is the release target. Every dependency and abstraction has a size cost.
   - Avoid adding unnecessary runtime allocations or large data tables without justification.
   - Current release binary: ~1679 KB (all features with LTO).

3. **Security-critical surfaces are first-class**
   - `src/gateway/gateway.c`, `src/security/`, `src/tools/`, `src/runtime/` carry high blast radius.
   - Defaults are secure-by-default (pairing, HTTPS-only, allowlists, AEAD encryption). Keep it that way.

4. **C11 is the baseline — strict standards compliance**
   - HTTP client: libcurl (`HU_ENABLE_CURL=ON`), conditionally compiled.
   - Child processes: `fork`/`exec` on POSIX, guarded by `HU_GATEWAY_POSIX`.
   - SQLite: linked via system or Homebrew paths.
   - All code compiles with `-Wall -Wextra -Wpedantic -Werror`.
   - Use `HU_IS_TEST` guards to bypass side effects (spawning, opening URLs, real hardware I/O).

5. **All 3795+ tests must pass at zero ASan errors**
   - The test suite uses AddressSanitizer for leak and overflow detection.
   - Every allocation must be freed (`free()` or cleanup function).
   - Use `HU_IS_TEST` mock paths in tests — no network, no process spawning.

## 3) Engineering Principles (Normative)

These principles are mandatory. They are implementation constraints, not suggestions.

### 3.1 KISS

Required:

- Prefer straightforward control flow over meta-programming.
- Prefer explicit `#ifdef` branches and typed structs over hidden dynamic behavior.
- Keep error paths obvious and localized.

### 3.2 YAGNI

Required:

- Do not add config keys, vtable methods, or feature flags without a concrete caller.
- Do not introduce speculative abstractions.
- Keep unsupported paths explicit (`return HU_ERR_NOT_SUPPORTED`) rather than silent no-ops.

### 3.3 DRY + Rule of Three

Required:

- Duplicate small local logic when it preserves clarity.
- Extract shared helpers only after repeated, stable patterns (rule-of-three).
- When extracting, preserve module boundaries and avoid hidden coupling.

### 3.4 Fail Fast + Explicit Errors

Required:

- Prefer explicit errors for unsupported or unsafe states.
- Never silently broaden permissions or capabilities.
- In tests: `HU_IS_TEST` guards are acceptable to skip side effects (e.g., spawning browsers), but the guard must be explicit and documented.

### 3.5 Secure by Default + Least Privilege

Required:

- Deny-by-default for access and exposure boundaries.
- Never log secrets, raw tokens, or sensitive payloads.
- All outbound URLs must be HTTPS. HTTP is rejected at the tool layer.
- Keep network/filesystem/shell scope as narrow as possible.

### 3.6 Determinism + No Flaky Tests

Required:

- Tests must not spawn real network connections, open browsers, or depend on system state.
- Use `HU_IS_TEST` to bypass side effects (spawning, opening URLs, real hardware I/O).
- Tests must be reproducible across macOS and Linux.

## 4) Repository Map (High-Level)

```
src/
  main.c                CLI entrypoint and command routing
  agent/                agent loop, context, planner, compaction, dispatcher
  channels/             34 channel implementations (cli, telegram, discord, slack, ...)
  providers/            50+ AI provider implementations (9 core + 41 compatible services)
  tools/                67 tool implementations
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
  ...

include/human/       public C headers

tests/                 128 test files, 3795+ tests

asm/                   platform-specific assembly (aarch64, x86_64, generic C)

fuzz/                  libFuzzer harnesses (JSON, base64, URL encode, config)

archive/zig-reference/ archived Zig source (build.zig, src/)
```

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
5. **Document impact** — update comments/docs for behavior changes, risk, and side effects.

### 6.1 Code Naming Contract (Required)

Apply these naming rules consistently:

- All identifiers: `snake_case` for functions, variables, fields, modules, files.
- Types, structs, enums, unions: `hu_<name>_t` (e.g., `hu_provider_t`, `hu_channel_t`).
- Constants and macros: `HU_SCREAMING_SNAKE_CASE` (e.g., `HU_OK`, `HU_ERR_NOT_SUPPORTED`).
- Public functions: `hu_<module>_<action>` (e.g., `hu_provider_create`, `hu_channel_send`).
- Factory registration keys: stable, lowercase, user-facing (e.g., `"openai"`, `"telegram"`, `"shell"`).
- Tests: named by behavior (`subject_expected_behavior`), fixtures use neutral names.

### 6.2 Architecture Boundary Contract (Required)

- Extend capabilities by adding vtable implementations + factory wiring first.
- Keep dependency direction inward to contracts: concrete implementations depend on vtable/config/util, not on each other.
- Avoid cross-subsystem coupling (provider code importing channel internals, tool code mutating gateway policy).
- Keep module responsibilities single-purpose: orchestration in `agent/`, transport in `channels/`, model I/O in `providers/`, policy in `security/`, execution in `tools/`.

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

## 8) Validation Matrix

Required before any code commit:

```bash
cd build && cmake --build . -j$(nproc) && ./human_tests  # all tests must pass, 0 ASan errors
```

For release changes:

```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON && cmake --build . -j$(nproc)  # must compile clean
```

Additional expectations by change type:

- **Docs/comments only**: no build required, but verify no broken code references.
- **Security/runtime/gateway/tools**: include at least one boundary/failure-mode test.
- **Provider additions**: test vtable wiring + graceful failure without credentials.

If full validation is impractical, document what was run and what was skipped.

### 8.1 Git Hooks

The repository ships with pre-configured hooks in `.githooks/`. Activate once per clone:

```bash
git config core.hooksPath .githooks
```

Hooks:

| Hook         | What it does                                                                                    |
| ------------ | ----------------------------------------------------------------------------------------------- |
| `pre-commit` | Runs format checks — blocks commit if code is not formatted                                     |
| `commit-msg` | Enforces conventional commit format (`feat`, `fix`, `refactor`, `test`, `docs`, `chore`, etc.)  |
| `pre-push`   | Runs `cmake --build build-check && ./build-check/human_tests` — blocks push if any test fails |

To bypass a hook in an emergency: `git commit --no-verify` / `git push --no-verify`.

### 8.2 Branch Naming and Git Workflow

Required:

- Branch naming: `feat/<name>`, `fix/<name>`, `refactor/<name>`, `docs/<name>`, `test/<name>`, `chore/<name>`.
- Commit format: `<type>[(<scope>)]: <description>` (enforced by `.githooks/commit-msg`).
- Types: `feat`, `fix`, `refactor`, `test`, `docs`, `chore`, `perf`, `ci`, `build`, `style`.
- Pre-push hook runs the full test suite — use `git push --no-verify` only in emergencies.
- Pre-commit hook runs format check — use `git commit --no-verify` only in emergencies.
- Keep each commit to a single concern. Don't mix feature + refactor + infra in one commit.

## 9) Privacy and Sensitive Data (Required)

- Never commit real API keys, tokens, credentials, personal data, or private URLs.
- Use neutral placeholders in tests: `"test-key"`, `"example.com"`, `"user_a"`.
- Test fixtures must be impersonal and system-focused.
- Review `git diff --cached` before push for accidental sensitive strings.

## 10) Anti-Patterns (Do Not)

- Do not add C dependencies or large Zig packages without strong justification (binary size impact).
- Do not return vtable interfaces pointing to temporaries — dangling pointer.
- Do not silently weaken security policy or access constraints.
- Do not add speculative config/feature flags "just in case".
- Do not skip `free()` — every allocation must be freed.
- Do not modify unrelated modules "while here".
- Do not include personal identity or sensitive information in tests, examples, docs, or commits.
- Do not use `SQLITE_TRANSIENT` — use `SQLITE_STATIC` (null) instead.
- Do not use `-Werror` exceptions — fix warnings at the source.

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

| Source                         | What We Take                                                           | Human Doc                      |
| ------------------------------ | ---------------------------------------------------------------------- | -------------------------------- |
| **Apple HIG**                  | Spring-first motion, clarity/deference/depth, spatial hierarchy        | `docs/motion-design.md`          |
| **Material Design 3**          | Canonical layouts, easing taxonomy, elevation, dynamic color           | `docs/ux-patterns.md`            |
| **Disney/Pixar 12 Principles** | Squash & stretch, anticipation, staging, timing, follow-through        | `docs/motion-design.md`          |
| **Edward Tufte**               | Data-ink ratio, chartjunk elimination, small multiples                 | `docs/visual-standards.md`       |
| **Dieter Rams**                | Less is more, honest design, long-lasting quality                      | `docs/visual-standards.md`       |
| **Gestalt Psychology**         | Proximity, similarity, continuity, closure, figure-ground              | `docs/ux-patterns.md`            |
| **Nielsen Norman Group**       | F-pattern scanning, progressive disclosure, recognition over recall    | `docs/ux-patterns.md`            |
| **Competitive Benchmarks**     | Named brand targets, quantified quality deltas, category-defining bars | `docs/competitive-benchmarks.md` |
| **Quality Scorecard**          | Per-surface scoring (7 dimensions), quarterly tracking                 | `docs/quality-scorecard.md`      |

**Mandatory document consultation before UI work:**

- **Creating or restructuring a view?** → Read `docs/ux-patterns.md` first. Every view must conform to an archetype.
- **Adding or modifying animation?** → Read `docs/motion-design.md` first. Every animation must follow Disney/Pixar + Apple + M3 principles.
- **Making any visual change?** → Read `docs/visual-standards.md` first. Verify hierarchy, spacing, color application, and quality checklist.
- **Token values and specifics?** → Read `docs/design-strategy.md` for the complete token reference.

### 12.1 Layout Archetypes (Required — see `docs/ux-patterns.md`)

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
- Maximum 3 type sizes visible in any single view section (see `docs/visual-standards.md` §7).

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
- **Centralized design strategy**: see `docs/design-strategy.md` for the full token reference.

Color accent hierarchy (60-30-10 rule — see `docs/visual-standards.md` §2.1):

- **Primary**: `--hu-accent` (Fidelity green) — brand identity, primary buttons, links, focus rings.
- **Secondary**: `--hu-accent-secondary` (amber) — warm highlights, featured content, CTAs needing contrast.
- **Tertiary**: `--hu-accent-tertiary` (indigo) — info states, data visualization, depth.
- **Error only**: coral — reserved exclusively for `--hu-error` / `--hu-error-dim`. Never use coral as a general accent.

Each accent provides `-hover`, `-subtle`, `-strong`, `-text`, and `on-accent-*` variants for both dark and light themes.

#### Tonal Surfaces (M3)

Surfaces are tinted with the primary accent for branded depth hierarchy:

- `--hu-surface-container` — default card/panel (4% fidelity green tint)
- `--hu-surface-container-high` — elevated interactive (6% tint)
- `--hu-surface-container-highest` — highest emphasis (8% tint)
- `--hu-surface-dim` / `--hu-surface-bright` — recessed / prominent extremes
- Use tonal surfaces instead of `--hu-bg-surface` when brand identity matters.
- Token source: `base.tokens.json` (color.tonal.\*) + `semantic.tokens.json`

#### Tinted State Layers (M3)

Interactive overlays are tinted with fidelity green, not neutral white/black:

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

### 12.5 Visual Hierarchy (Required — see `docs/visual-standards.md`)

Required:

- **Squint test**: primary action and content area must be identifiable at a glance.
- **Emphasis levels**: exactly ONE high-emphasis element per screen (M3 principle).
- **60-30-10 color ratio**: 60% background, 30% secondary, 10% accent.
- **Depth via elevation**: shadow intensity matches elevation level (Apple: Depth, M3: Elevation).
- **Spacing rhythm**: use token scale consistently. Never skip >2 steps in the spacing scale.
- **Whitespace**: empty space is a deliberate design choice, not a bug (Tufte: data-ink ratio).

### 12.6 Motion & Animation (Required — see `docs/motion-design.md`)

Required — grounded in Disney/Pixar 12 Principles + Apple HIG + Material 3:

- **Spring-first**: prefer spring easings for interactive elements (Apple HIG).
- **Squash & stretch**: buttons compress on press, rebound on release (Disney principle 1).
- **Anticipation**: hover states prepare users for action (Disney principle 2).
- **Staging**: stagger reveals, dim backgrounds for focus (Disney principle 3).
- **Timing**: use `--hu-duration-*` tokens. Small = fast, large = slow. Never exceed 700ms.
- **Follow-through**: child elements complete animation after parent (Disney principle 5).
- **Easing tokens**: `--hu-ease-out` (enter), `--hu-ease-in` (exit), `--hu-ease-spring` (interact). Never raw `cubic-bezier()`.
- **Spring tokens**: `--hu-spring-micro`, `--hu-spring-standard`, `--hu-spring-expressive`, `--hu-spring-dramatic`.
- **Choreography**: `--hu-stagger-delay` (50ms) between items, `--hu-stagger-max` (300ms) cap.
- **Performance**: animate only compositor properties (transform, opacity, filter). No layout thrashing.
- Every animation must respect `prefers-reduced-motion: reduce`.
- Keyframe names use `hu-` prefix.

### 12.7 Data Visualization (Required — see `docs/visual-standards.md` §9)

Required — grounded in Tufte's principles of analytical design:

- Maximize data-ink ratio. Remove grid lines, legends, and decoration that don't serve data.
- Use `--hu-chart-categorical-{1..8}` for multi-series charts (never ad-hoc colors).
- Use `--hu-chart-sequential-{100..800}` for ordered/heatmap data.
- Use `--hu-chart-diverging-{positive,neutral,negative}` for positive/negative indicators.
- Single-metric charts use `--hu-chart-brand`.
- Prefer direct labels over legends. Prefer small multiples over complex overlapping series.
- Token definitions live in `design-tokens/data-viz.tokens.json`.

### 12.8 Lint Enforcement

Required:

- Run `ui/scripts/lint-raw-values.sh` to detect design token drift.
- Flags raw hex/rgba, hardcoded durations, and raw breakpoints in `.ts` files.
- Wired into `npm run check` via `npm run lint:tokens`.

### 12.9 Accessibility (Required — see `docs/ux-patterns.md` §5)

Required:

- WCAG 2.1 AA minimum (4.5:1 text contrast, 3:1 UI contrast).
- All interactive elements: visible focus ring, keyboard operable.
- Touch targets: minimum 44×44px (Apple HIG).
- Modals: focus trap, Escape to close, `aria-modal`.
- `prefers-color-scheme` and `prefers-reduced-motion` both supported.
- No information conveyed by color alone — always pair with icon, label, or pattern.
- Semantic HTML: headings follow hierarchy, lists use proper elements, live regions for dynamic content.

### 12.10 Change Playbook: Adding a UI Component

- Add `ui/src/components/hu-<name>.ts` as a LitElement web component.
- Use `--hu-*` tokens exclusively in `static styles`.
- Add test file for render, accessibility, and keyboard navigation.
- Register in component catalog (`ui/src/catalog/`).
- Update `ui/src/icons.ts` if the component needs a new icon.
- Run the **quality checklist** from `docs/visual-standards.md` §10 before shipping.

### 12.11 Change Playbook: Adding or Modifying a View

- Identify which **layout archetype** (§12.1) the view conforms to.
- Follow the archetype's structural rules from `docs/ux-patterns.md` §2.
- Implement empty state, loading skeleton, and error state (all three required).
- Verify responsive behavior at all four breakpoints (compact, medium, expanded, wide).
- Apply the **quality checklist** from `docs/visual-standards.md` §10.

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
- Every quarter, evaluate and adopt one emerging web platform feature before competitors (see `docs/design-strategy.md` §Design Innovation Pipeline).

## 13) Vibe Coding Guardrails

When working in fast iterative mode:

- Keep each iteration reversible (small commits, clear rollback).
- Validate assumptions with code search before implementing.
- Prefer deterministic behavior over clever shortcuts.
- Do not "ship and hope" on security-sensitive paths.
- If uncertain about C API usage, check `src/` for existing patterns before guessing.
- If uncertain about architecture, read the vtable interface definition in `include/human/` before implementing.
