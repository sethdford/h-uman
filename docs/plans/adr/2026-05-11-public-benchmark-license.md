---
title: "ADR — Public benchmark license: CC-BY-4.0 for scenarios, CC-BY-SA-4.0 for methodology, Apache-2.0 for reference impl"
created: 2026-05-11
status: accepted
deciders: engineering, product
parent: ../2026-05-10-sota-roadmap-6mo.md
related:
  - ../2026-05-10-sota-roadmap-6mo.md
---

# ADR: Public benchmark license

## Context

The SOTA roadmap (Phase A6.3, B6.1, E5) publishes:

1. **Persona eval scenario suite** — synthetic conversational prompts and expected-trait rubrics.
2. **Performance bench rig + scenarios** — `scripts/bench-gemma-perf.py` plus the standardized prompts in `eval_suites/perf/`.
3. **Methodology paper** — explains the judge model, scoring rubric, statistical methods, inter-rater agreement targets.
4. **Reference implementation** — runner scripts, harness code, judge prompts.

Without explicit licensing, ambiguity arises about whether external parties can reproduce, fork, or extend the benchmark. Prior art:

| Benchmark | Scenarios license | Code license |
|---|---|---|
| MLPerf | Apache-2.0 | Apache-2.0 |
| HELM | Apache-2.0 | Apache-2.0 |
| MT-Bench | Apache-2.0 (questions); CC-BY-NC for some derivative datasets | Apache-2.0 |
| AlpacaEval | Apache-2.0 | Apache-2.0 |
| BIG-bench | Apache-2.0 | Apache-2.0 |

Most industry benchmarks use Apache-2.0 wholesale. The h-uman case is different on one axis: **the eval scenarios are tightly bound to a persona thesis** and we want derivative methodology to stay open (share-alike) so the persona-fidelity-eval field develops on shared rails rather than fragmenting into closed competitor variants.

## Decision

A three-license split:

| Artifact | License | Reasoning |
|---|---|---|
| **Eval scenarios** (`eval_suites/persona/scenarios/*.json`, `eval_suites/channel/<tier1>/*.json`) | **CC-BY-4.0** | Maximally permissive for adoption; attribution required so derivative works credit h-uman |
| **Methodology paper** (`docs/perf/competitive/methodology.md` and a possible arxiv preprint) | **CC-BY-SA-4.0** | Share-alike — derivatives of the methodology must also publish under share-alike. Keeps the eval-design field open. |
| **Reference implementation** (`scripts/bench-gemma-perf.py`, judge harness, runner code) | **Apache-2.0** | Matches the most common ML-tooling license; explicit patent grant; consistent with most of the repo. (Repo at large is currently unlicensed → add LICENSE at the root before publication.) |
| **Judge prompts** (`eval_suites/persona/judge-prompts/*.md`) | **CC-BY-4.0** | Treated as text content, not code |
| **Bench result artifacts** (nightly JSON, anchor baselines) | **CDLA-Permissive-2.0** | Numbers / data; permissive for redistribution |

Operational rules:

- **Privacy audit before any data publication.** No real user conversation history leaves the device. The persona eval scenarios are either:
  (a) **synthetic** (generated; not from real users), or
  (b) **explicitly user-consented and de-identified** (real-user-derived scenarios require written consent + a documented de-identification pass).
- **Legal review** required before Phase A6.3 / B6.1 / E5 publication. Specifically:
  - License compatibility audit (any third-party fixtures bundled in `eval_suites/` must be compatible with CC-BY-4.0).
  - Consent-form alignment (per the beta-cohort ADR).
  - Privacy claims in the methodology paper must match the threat model in `docs/standards/security/threat-model.md`.
- **License headers** in source files (`scripts/bench-gemma-perf.py` and any new harness code) before publication.
- **CITATION.cff** at repo root pointing reviewers/users at the canonical citation form.
- **Repo LICENSE file** lands before Phase A6.3 publication. ~~Use Apache-2.0 for code at the repo root unless a strong reason emerges to fork.~~ **Superseded by the 2026-05-17 addendum below**: the repo has been MIT-licensed since `a58a0ef3` (2026-02-16); MIT is retained at the root, and the benchmark reference implementation declares Apache-2.0 per-file via SPDX headers.

What this does NOT cover:

- The h-uman runtime binary itself — out of scope; licensed independently.
- The frontier model weights used as the judge — covered by their respective licenses (Google's Gemini terms, etc.); the benchmark publishes only judge **prompts** and **scores**, not weights.
- User-trained LoRA adapters — owned by the user (per the privacy-by-architecture thesis); the benchmark publishes neither training data nor adapter weights.

## Consequences

- **Positive:** the eval-design community can build on our methodology without fragmenting it (share-alike); adopters can use the scenarios with simple attribution; reference code carries the patent grant the ML-tooling ecosystem expects.
- **Negative:** three-license split adds bookkeeping. Mitigation: a single `LICENSES/` directory at the eval-suite root with explicit `LICENSE.md` files pointing at each artifact's license. The methodology paper carries its license in its frontmatter.
- **Legal review** is on the critical path for Phase A6.3 publication. Schedule with the beta-cohort legal review (per the cohort ADR) to consolidate review cycles.
- **Documented in:** `eval_suites/LICENSE.md` (TBD), `docs/perf/competitive/methodology.md` (TBD), repo-root `LICENSE` (TBD), `CITATION.cff` (TBD).

## Status

Accepted with addendum (2026-05-17). Bookkeeping work (LICENSE files, headers, CITATION.cff) lands in Phase E2 along with the suite directory structure. Legal review request goes out at month 5, paired with the beta-cohort review.

## Addendum (2026-05-17): Repo-root LICENSE clarification

This ADR's decision table (row "Reference implementation") includes the parenthetical "Repo at large is currently unlicensed → add LICENSE at the root before publication." **That parenthetical is factually incorrect.** The repo has carried an **MIT License** at `LICENSE` since the original nullclaw commit (`a58a0ef3`, 2026-02-16), three months before this ADR was written. The MIT license is also asserted in:

- `README.md` (license badge + "License: MIT — see [LICENSE](../../../LICENSE)")
- `CONTRIBUTING.md` ("Human is an autonomous AI assistant runtime written in C11. MIT License.")
- `npm/package.json` (`"license": "MIT"`)
- `npm/README.md` (License: MIT)

**Resolution:** the repo-root LICENSE remains MIT. Re-licensing the entire repo from MIT to Apache-2.0 would require contributor sign-off across every author since 2026-02-16, has no compelling benefit for the runtime code (MIT is permissive and well-understood), and would disrupt downstream consumers who have already vendored or depended on the MIT-licensed runtime.

**The ADR's per-artifact license assignments remain in force:**

- Eval scenarios → CC-BY-4.0 (in `eval_suites/` with explicit per-directory `LICENSE.md`)
- Methodology paper → CC-BY-SA-4.0 (license stated in its frontmatter)
- Reference benchmark implementation (`scripts/bench-gemma-perf.py`, judge harness, runner code) → **Apache-2.0** declared via SPDX header in each file (`// SPDX-License-Identifier: Apache-2.0`). This is permitted alongside an MIT repo root because the benchmark code is a self-contained artifact whose license is asserted at the file level; the repo root MIT covers everything not otherwise marked.
- Judge prompts → CC-BY-4.0 (per-file frontmatter)
- Bench result artifacts → CDLA-Permissive-2.0 (per-directory `LICENSE.md`)

**Rationale for the split-license approach:** projects like Linux (GPL kernel + permissive userspace headers), CPython (PSF + Apache for some modules), and Hugging Face Transformers (Apache repo with CC-BY model cards) all carry per-artifact licenses inside a single repo. The SPDX-Identifier convention makes this unambiguous to legal tooling.

**What this addendum closes:** the reconciliation work in task 10 of the 2026-05-17 plan-validation backlog. There is no longer a discrepancy: the repo root is MIT by deliberate retention, and the benchmark per-artifact licenses are declared via SPDX headers when those artifacts ship.
