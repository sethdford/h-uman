---
title: Standards Index
---

# Standards Index

Canonical source of truth for all project standards. AI agents and human contributors reference these docs -- they are never duplicated, only pointed to.

## How This Works

```
docs/standards/          <- Canonical source (write and review here)
    |
.cursor/rules/*.mdc      <- Cursor agent rules (reference, never duplicate)
    |
CLAUDE.md                <- Quick reference for all agents
    |
AGENTS.md                <- Full engineering protocol
```

**Rules:**

1. Every standard has ONE home in `docs/standards/`
2. AI config files (`.mdc`, `CLAUDE.md`, `AGENTS.md`) reference standards -- they don't duplicate them
3. Standards are concise and actionable -- no fluff, no essays
4. Every rule has a concrete example of right vs. wrong where applicable

---

## AI

| Doc                                                           | Covers                                                                                     | Used When                                  |
| ------------------------------------------------------------- | ------------------------------------------------------------------------------------------ | ------------------------------------------ |
| [agent-architecture.md](ai/agent-architecture.md)             | Pipeline design, vtable component responsibilities, orchestration patterns, retry/fallback | Modifying agent loop, adding components    |
| [skills-vs-agents.md](ai/skills-vs-agents.md)                 | Static SkillForge vs spawned agents; top_k catalog env; inheritance                         | Skills, `agent_spawn`, prompt sizing       |
| [fleet.md](ai/fleet.md)                                       | `hu_agent_pool_t` limits: depth, lifetime spawns, session budget; `/fleet`, spawn errors    | Sub-agents, `agent_spawn`, `/spawn`, cost caps |
| [conversation-design.md](ai/conversation-design.md)           | Response patterns, channel-aware behavior, intent routing, persona integration             | Building conversational features           |
| [hallucination-prevention.md](ai/hallucination-prevention.md) | Grounding hierarchy, tool output integrity, memory attribution, capability honesty         | Any AI response generation                 |
| [capability-claims.md](ai/capability-claims.md)               | Verifiable capability claims, evidence bars, avoiding overstated product language           | Marketing, docs, release notes             |
| [prompt-engineering.md](ai/prompt-engineering.md)             | System prompt structure, persona composition, RAG patterns, context window management      | Modifying prompts, adding providers        |
| [evaluation.md](ai/evaluation.md)                             | Quality dimensions, golden-set testing, scoring rubrics, per-provider metrics              | Prompt changes, model upgrades             |
| [citation-and-sourcing.md](ai/citation-and-sourcing.md)       | Source tiers, attribution patterns, freshness rules, confidence mapping                    | Agent responses that reference data        |
| [human-in-the-loop.md](ai/human-in-the-loop.md)               | Approval tiers, confirmation flow, security policy integration, escalation triggers        | Tool execution, security-sensitive actions |
| [disclosure.md](ai/disclosure.md)                             | AI identity disclosure, expertise boundaries, directional language, claims to avoid        | Agent responses, persona design            |
| [responsible-ai.md](ai/responsible-ai.md)                     | Model governance, selection criteria, bias detection, output monitoring, incident response | Model selection, provider evaluation       |
| [voice-providers.md](ai/voice-providers.md)                   | Voice provider vtable contract, duplex turn-taking, audio emotion, lifecycle              | Implementing or modifying voice providers  |

## Brand

| Doc                                    | Covers                                         | Used When                  |
| -------------------------------------- | ---------------------------------------------- | -------------------------- |
| [terminology.md](brand/terminology.md) | Canonical product, technical, and UI/CLI terms | Code, docs, UI, CLI naming |

## Design

| Doc                                                         | Covers                                                                 | Used When                         |
| ----------------------------------------------------------- | ---------------------------------------------------------------------- | --------------------------------- |
| [visual-standards.md](design/visual-standards.md)           | Visual hierarchy, composition, depth, spacing, quality checklist       | Any visual change                 |
| [motion-design.md](design/motion-design.md)                 | Animation principles, spring physics, timing, choreography             | Adding or modifying animation     |
| [ux-patterns.md](design/ux-patterns.md)                     | Layout archetypes, responsive behavior, interaction patterns           | Creating or restructuring views   |
| [design-strategy.md](design/design-strategy.md)             | Token philosophy, color hierarchy, theme support                       | Token values and design decisions |
| [design-system.md](design/design-system.md)                 | Design system overview, component rules                                | Building UI components            |
| [accessibility-testing.md](design/accessibility-testing.md) | WCAG compliance, Lighthouse, axe-core, screen reader, keyboard testing | Accessibility work, UI changes    |
| [visual-minimalism.md](design/visual-minimalism.md)         | Visual minimalism principles, glass/banner/scrollbar rules, squint test | Any visual change, empty states   |
| [spacing-discipline.md](design/spacing-discipline.md)       | Spacing scale semantics, content width tokens, component spacing contract | Spacing, layout, content width    |

## Engineering

| Doc                                                          | Covers                                                          | Used When                         |
| ------------------------------------------------------------ | --------------------------------------------------------------- | --------------------------------- |
| [principles.md](engineering/principles.md)                   | KISS, YAGNI, DRY, fail-fast, secure-by-default, determinism     | All code changes                  |
| [naming.md](engineering/naming.md)                           | Identifier conventions, type naming, constant naming            | All code changes                  |
| [anti-patterns.md](engineering/anti-patterns.md)             | Prohibited patterns with reasons and alternatives               | Code review, design decisions     |
| [testing.md](engineering/testing.md)                         | Test structure, naming, mocking, coverage, quality rules        | Writing or modifying tests        |
| [workflow.md](engineering/workflow.md)                       | Branching, commits, versioning, releases, hotfixes              | All development work              |
| [memory-management.md](engineering/memory-management.md)     | Allocator types, ownership rules, cleanup patterns, ASan        | Allocation and cleanup code       |
| [performance.md](engineering/performance.md)                 | Binary size, RSS, startup time, per-operation budgets           | Performance-sensitive changes     |
| [performance-testing.md](engineering/performance-testing.md) | Performance measurement, regression detection, profiling        | Performance testing, CI gates     |
| [api-design.md](engineering/api-design.md)                   | API surface, stability tiers, versioning, deprecation           | Public API changes                |
| [gateway-api.md](engineering/gateway-api.md)                 | HTTP/WebSocket gateway, REST conventions, security, RPC         | Gateway endpoints, webhooks       |
| [cross-platform.md](engineering/cross-platform.md)           | Platform detection, abstractions, guards, hardcoded paths       | Platform-specific code            |
| [dependencies.md](engineering/dependencies.md)               | Dependency tiers, version pinning, vulnerability scanning, SBOM | Adding dependencies, supply chain |
| [error-handling.md](engineering/error-handling.md)           | Error propagation, reporting, recovery patterns, anti-patterns  | Error handling, debugging         |
| [config-schema.md](engineering/config-schema.md)             | Config schema design, parsing, validation, upgrade paths        | Config changes, schema evolution  |
| [gateway-events.md](engineering/gateway-events.md)           | Gateway event naming, payload schemas, WebSocket/webhook design | Gateway events, control protocol  |
| [memory-lifecycle.md](engineering/memory-lifecycle.md)       | Memory tiers, consolidation, forgetting curve, RAG safety       | Memory system changes             |
| [reproducible-builds.md](engineering/reproducible-builds.md) | Bytewise-reproducible binary contract, carve-outs, CI gate      | CMake / build flag changes; release |

## Quality

| Doc                                              | Covers                                                                          | Used When                             |
| ------------------------------------------------ | ------------------------------------------------------------------------------- | ------------------------------------- |
| [governance.md](quality/governance.md)           | Five principles, gatekeeping mindset, drift prevention, compliance checklist    | Every change                          |
| [award-criteria.md](quality/award-criteria.md)   | Award-winning quality criteria, best-in-class benchmarks, submission checklists | UI quality targets, award submissions |
| [ceremonies.md](quality/ceremonies.md)           | Weekly drift audit, PR completion gate, release gate                            | Recurring quality checkpoints         |
| [code-review.md](quality/code-review.md)         | Review priorities, risk-adjusted depth, merge criteria                          | Before merging any change             |
| [channel-testing.md](quality/channel-testing.md) | Channel vtable tests, HU_IS_TEST mocking, memory testing, destroy convention    | Adding or modifying channels          |

## Operations

| Doc                                                     | Covers                                                              | Used When                    |
| ------------------------------------------------------- | ------------------------------------------------------------------- | ---------------------------- |
| [incident-response.md](operations/incident-response.md) | Severity levels, triage workflow, postmortem template, runbooks     | When things break            |
| [monitoring.md](operations/monitoring.md)               | Structured logging, health checks, alerting, AI-specific metrics    | All deployed services        |
| [observability.md](operations/observability.md)         | SLI/SLO definitions, structured logging, metrics, trace propagation | Observability, SLOs, metrics |
| [daemon.md](operations/daemon.md)                       | Daemon service loop, worker pooling, job scheduling, lifecycle      | Daemon operations, service loop  |

## Security

| Doc                                         | Covers                                                            | Used When                            |
| ------------------------------------------- | ----------------------------------------------------------------- | ------------------------------------ |
| [threat-model.md](security/threat-model.md) | STRIDE threat model, attack surface analysis                      | Security-sensitive changes           |
| [sandbox.md](security/sandbox.md)           | Sandbox isolation system, backend comparison                      | Runtime and tool execution changes   |
| [ai-safety.md](security/ai-safety.md)       | Prompt injection, output validation, safe tool execution          | AI agent behavior, tool security     |
| [data-privacy.md](security/data-privacy.md) | Data classification, lifecycle, retention, user rights, isolation | Data handling, memory, logging       |
| [compliance.md](security/compliance.md)     | GDPR, CCPA/CPRA compliance mapping, DPA requirements, audit trail | Data privacy compliance, deployments |
| [canvas-security.md](security/canvas-security.md) | Live Canvas sandbox model, iframe isolation, CSP, threat mitigations | Canvas rendering, security review    |
