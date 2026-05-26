# h-uman — not quite human.

C11 autonomous AI assistant runtime. ~1750 KB binary, <6 MB RAM, <30 ms startup.
Zero dependencies beyond libc (optional SQLite and libcurl).

Read `AGENTS.md` for the full engineering protocol. This file is the quick reference.

## Product Thesis

**The assistant that's actually yours.**

Every other AI assistant in 2026 is someone else's product renting you access. Gemini is Google's agent that happens to know your Gmail. Siri is Apple's voice layer that outsources its brain to Google. Claude Cowork is Anthropic's operator working in your folder. OpenClaw is a framework — powerful, but personality-free.

human is different: **a private, personal AI that runs on your hardware, learns who you are locally, and never sends your identity to a cloud.** The "every device" story is how we get there. The "actually yours" story is why someone chooses us.

### Red-Teamed Reality Check (April 2026)

This thesis was stress-tested. Here's what survived and what didn't.

**The privacy paradox is real but solvable.** 81% of consumers say they care about AI privacy; only 8-12% will configure privacy settings (Pew/Cisco/McKinsey 2025-2026). Stated willingness to pay a privacy premium: 10-30%. Actual behavior lags far behind. **Implication:** privacy-by-architecture (the default is private, no settings needed) beats privacy-as-feature (toggle in settings). Our thesis survives only if privacy is structural, not optional.

**AI app retention is brutal.** AI apps retain 21.1% of annual subscribers vs 30.7% for non-AI apps — 30% faster churn (RevenueCat 2026). Novelty exhaustion is the #1 killer. **But:** companion/personal AI shows 41% DAU/MAU vs 14% for utility AI. Personalization drives retention; task execution doesn't. This supports the persona thesis.

**Gemini already personalizes.** Google launched "Personal Intelligence" — connecting Gmail, Photos, Search, YouTube for personalized responses. They even have an "Import Memory" feature to poach users from other AIs. **Our "knows you" claim must be about WHERE data lives and WHO controls it, not WHETHER personalization exists.**

**OpenClaw already has persona plugins.** Multiple Personas (SOUL.md, PERSONALITY.md, MEMORY.md), personality-dynamics (mode switching, weekly auto-evolution), open-persona (meta-skill for persona packs). 6.2K stars. **Our persona depth is real (27 C modules vs markdown templates), but the moat is narrower than we claimed.**

### What We're Not Competing On (Table Stakes)

- **Task execution.** Commodity. Every framework does this.
- **Channel count.** 31 channels is breadth, not a moat. OpenClaw has ClawHub.
- **Chat interfaces.** Google has 2B+ devices. We can't out-distribute.
- **Benchmark scores.** We call the same frontier models. Can't beat them at their layer.
- **Binary size / startup time.** Developers appreciate it; users don't feel it.
- **Dashboard aesthetics.** Hygiene, not differentiation.
- **"We have persona."** OpenClaw has SOUL.md persona plugins. Existence of persona is no longer unique.

### What Actually Makes Us Better (Honest Moats)

1. **Persona as compiled architecture, not markdown templates.** 27 C modules with runtime integration (circadian timing, somatic markers, emotional cognition, humor bridging) vs OpenClaw's SOUL.md text files. The difference: our persona *changes how the agent behaves at the code level* — timing, tool selection, tone adaptation, proactive messaging. Theirs is a system prompt wrapper.
2. **Privacy by architecture, not by settings.** Data never leaves the device as a structural property. No "opt-in to privacy" toggle. Gemini's Personal Intelligence processes your data in Google's cloud (their privacy doc confirms this). We can't match their data breadth (Gmail/Photos/YouTube), but we own the trust story.
3. **On-device personalization pipeline (partial — see honest status below).** MLX LoRA fine-tuning on Apple Silicon is proven at 1B-7B models. Our ML subsystem has the training loop. **Gap:** our LoRA path currently trains a reference GPT, not the frontier model users chat with. Bridging this gap (via ggml/MLX integration) is the real technical challenge.
4. **HuLa IR.** Typed tool-orchestration with compiler and emergence. Genuinely novel. **Gap:** tightly coupled to internal agent; not yet a platform.
5. **Runs anywhere, owned by you.** Same binary from $5 board to data center. No subscription lock-in.

### Strategic Missions (Red-Teamed)

Every mission below includes an honest difficulty assessment from code-level red teaming.

| # | Mission | Honest Difficulty | Success Metric |
|---|---------|------------------|----------------|
| **M1** | **Persona-First** — Make persona always-on | **Done (Phase 1).** 100+ `#ifdef` guards removed. Persona fields unconditional in `hu_agent_t`. `human init` creates starter persona with channel overlays AND Tier-1 example banks (telegram / discord / imessage / slack — 12 neutral examples shipped in `hu_starter_persona_json`, Sprint 2b Story A', commit 71de40e6, pinned by `persona_directive_starter_persona_ships_tier1_example_banks` + `persona_directive_tier1_overlay_bank_coherence`). `human onboard` exists (`src/onboard.c`) and is auto-suggested on first run when no config exists. 11,900+ tests passing. Remaining: A/B validation. | Persona context in every agent turn ✅; starter persona on first run ✅; onboarding wizard ✅; Tier-1 example banks ✅ |
| **M2** | **Personal Model** — Unified model-of-the-person from memory | **Hard.** Single artifact (`hu_personal_model_t`, `src/memory/personal_model.c`); facts/topics/goals/style are accumulated per turn, summarized via `hu_personal_model_build_prompt`, and injected into every system prompt (commit d1d9b0ee — `tests/test_personal_model.c::personal_model_reaches_system_prompt_via_config`). Per-turn save call site landed in commit 3ee98ef9 (`feat(agent,memory): per-turn personal-model save for crash safety`); the underlying `hu_personal_model_save` was made **actually atomic** in Phase 0 (May 2026) via `tmp + fwrite + fflush + fsync + rename`, pinned by `tests/test_personal_model_atomic_save.c::test_personal_model_save_preserves_prior_state_when_tmp_blocked` — a deterministic adversary test that pre-blocks the `<path>.tmp` slot with a directory and confirms the prior file's contents survive a failed save. Fact extraction has been upgraded to **typed propositional/prescriptive triples** via `hu_fact_extract` (`include/human/memory/fact_extract.h`): subject/predicate/object + confidence + per-fact provenance + trust tier + 90-day exponential half-life decay (`hu_heuristic_fact_effective_confidence`). Wired into `hu_personal_model_ingest` (`src/memory/personal_model.c:957`). Learned-style adaptation still lives only in the prompt summary, not in a model checkpoint — bridging that requires the M3 frontier-bridge to land. | Measurable adaptation in tone/timing after 50 conversations |
| **M3** | **Private Learning** — On-device ML personalization | **Bridge B production-streaming complete except SSE in `mlx_local`.** Dynamic learning loop CLOSED (2026-05-26): US-8 (commit 416e6c29) fires `training_loop.py` on DPO pair-count threshold → 2026-05-26 wired `hu_mlx_admin_swap_adapter` immediately after successful subprocess exit so the running `mlx_server.py` hot-loads the new adapter without daemon restart. Public counter `hu_training_runner_post_train_swap_attempts()` advances per dispatch reaching the swap step (under HU_IS_TEST the libcurl call is skipped, counter still bumps so tests pin the wire). All 6 original verifier-contract tests from the M3 bridge plan have shipped (full backlog resolved). Empirically validated: v4-repair adapter lifts persona fidelity 0.586 → 0.856 (+27pp, commit 9ab9b86e). The provider dispatcher safety contract is pinned: cloud providers return `HU_ERR_NOT_SUPPORTED` from `hu_provider_load_adapter` without crashing the daemon (regression guard `tests/test_provider_all.c::test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat`, commit 028f4544). **Remaining M3 work** (per refreshed `docs/plans/2026-05-17-m3-mlx-bridge-execution-plan.md`): (a) production daemon's `mlx_local` HTTP path consuming SSE chunks instead of buffered response (spec at `docs/plans/2026-05-26-m3-b4-mlx-local-sse/`, ~1 week implementation), (b) `HU_NIGHTLY_LORA_ENABLED` env → `cfg.learning.nightly_lora` config plumbing, (c) live Apple-Silicon re-validation of post-train swap with current adapter. The reference HUML GPT path (`lora-persona` CLI) remains for offline experimentation but is no longer the primary M3 inference target. | LoRA adapter that measurably improves persona fidelity on inference (✅ achieved 2026-05-25: +27pp) |
| **M4** | **Ship to Users** — 100 DAU | **Medium.** `human onboard` exists (interactive setup wizard). First-run code path checks for missing config and points the user at the wizard. Persona defaults still need to be richer per channel; config still assumes cloud provider credentials. | 100 DAU with 30% day-7 retention |
| **M5** | **HuLa as Platform** — Developer-facing SDK | **Hard.** Public SDK header lives at `include/human/hula_sdk.h` with semver macros (`HU_HULA_SDK_VERSION_STRING "0.1.0"`); JSON wire format is documented in the header. Still missing: language bindings (Python/Node), hosted docs, public examples gallery. | External devs write and run HuLa programs |
| **M6** | **Channel Focus** — Prioritize 4 Tier-1 (Telegram, Discord, iMessage, Slack) across 31 messaging channels | **Easy (strategy), Medium (execution).** 43 channel `.c` files. This is prioritization, not a code change. | Tier 1 score 8/10+ on naturalness eval |

### Competitive Position (April 2026 — Honest)

| Dimension | human | Gemini Agent | Claude Cowork | OpenClaw |
|-----------|-------|-------------|---------------|----------|
| Persona depth | **Deep** (27 compiled modules) | Basic (Personal Intelligence) | None | **Growing** (SOUL.md plugins, personality-dynamics) |
| Personalization | Memory stack with typed propositional/prescriptive fact extraction + half-life decay | **Google apps data** (Gmail, Photos, YouTube) | Chat memory | SOUL.md + MEMORY.md |
| On-device learning | Reference only (CPU, toy GPT) | No | No | No |
| Privacy architecture | **Structural** (local-first) | Cloud (Google infra) | Cloud (Anthropic) | Self-hosted (Node.js) |
| Tool orchestration | **HuLa IR** (compiled) | Prompt-chained | Prompt-chained | Prompt-chained |
| Distribution | **None** (0 users) | **2B+ devices** | **Desktop + API** | **100K+ GitHub stars** |
| Ecosystem | Small | **Google apps** | **Mac + tools** | **ClawHub** |
| Runtime footprint | **~1750 KB / 6 MB** | Cloud | Cloud | ~180 MB / 120 MB |

## Build & Test

```bash
# Dev build — use CMake presets (recommended)
cmake --preset dev                 # ASan, all channels, SQLite, persona, skills, compile_commands.json
cmake --build --preset dev

# Other presets: test (no ASan), release (MinSizeRel+LTO), fuzz (Clang), minimal
cmake --list-presets               # show all available presets

# Run tests (11,900+ tests, must be 0 failures, 0 ASan errors)
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
| `ci.yml`                    | C build + 10,600+ tests (Linux + macOS), UI tsc + vitest + build, website build, clang-tidy, E2E, visual regression, axe accessibility, Lighthouse |
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
| `src/`                            | All C source (~710 files, ~270K lines)                                |
| `include/human/`                  | Public headers                                                        |
| `tests/`                          | 603 test files, 10,900+ tests                                         |
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
