---
title: Remaining SOTA Gaps Honest Backlog 2026-05-16
---

# Remaining SOTA Gaps — Honest Backlog (2026-05-16)

Companion to [trait-coverage-analysis-2026-05-16.md](trait-coverage-analysis-2026-05-16.md)
and [privacy/gap-analysis-2026-05-16.md](../privacy/gap-analysis-2026-05-16.md).

Originated from a 2026-05-16 strategic red-team that identified 7 areas
where h-uman is behind state-of-the-art. Several "gaps" in the original
analysis turned out to be SMALLER than claimed once the codebase was
read carefully (the learning loop is the most striking example — it
exists). This doc records the remaining real gaps with concrete next
steps, sized honestly.

## Gap 1 — Learning loop integration test (SMALL, this PR scope)

**Status: pipeline EXISTS, integration test MISSING.**

The DPO-from-corrections pipeline is already implemented:
- [src/ml/training_data_extractor.c:462](../../src/ml/training_data_extractor.c#L462)
  extracts `(user_prompt, assistant_response, user_correction)` triples
  from the messages SQLite table and writes them as DPO pairs with
  `chosen=correction, rejected=response, source='auto_correction'`.
- [src/agent/training_data_runner.c](../../src/agent/training_data_runner.c)
  schedules the extractor via the W14 background runner.
- Existing tests at
  [tests/test_training_data_extractor.c](../../tests/test_training_data_extractor.c)
  cover NULL-arg guards but ALL hit the
  [HU_IS_TEST short-circuit at extractor.c:390](../../src/ml/training_data_extractor.c#L390)
  — the real SQL path is unverified.

**What's missing:** an integration test that creates a real SQLite DB
with a 3-message sequence (user, assistant, user-correction) and asserts
the dpo_pairs table receives the expected row. Today the SQL is
"hopefully correct."

**Path:** add a `HU_TEST_ALLOW_SQLITE_PATH` compile flag (or a separate
test binary that compiles without HU_IS_TEST) that lets one test file
exercise the production path against a fixture DB.

**Effort:** ~1 week. Bulk of the work is the build-flag plumbing.

## Gap 2 — Embedding-based personalization (LARGE, deferred)

**Status: heuristic-only today.**

Style EWMA + 43 regex prefixes + LLM extractor (just added, not wired).
No embedding-based "user voice" model. No relationship graph. No
multi-modal preference signals.

**Path:**
1. Pick a small embedding model (all-MiniLM-L6-v2, 22 MB, runs on CPU).
2. Embed last N user messages → centroid = "user voice vector."
3. Embed candidate responses → cosine similarity to voice vector → new
   axis in `hu_persona_fidelity_score_t`.
4. Relationship graph: per-contact style fingerprint, learned per-channel.
   The data is there (channel + contact_id); the structure isn't.

**Effort:** 2-3 months. Hardest part is the embedding model bundling
(licensing, binary size impact, on-device inference path).

## Gap 3 — Eval volume (MEDIUM, this PR scope partial)

**Status: improved this PR — cross-persona separation eval just added
([test_persona_fidelity_cross.c](../../tests/test_persona_fidelity_cross.c)).**

Remaining:
- No standard benchmark (LongMemEval, MemoryAgentBench files exist as
  planned stubs in `src/evaluation/`; not running).
- No production-traffic eval (no production).
- No human-judged ratings pipeline.

**Path:** wire LongMemEval first — the file scaffold already exists at
[src/evaluation/evaluation_longmemeval.c](../../src/evaluation/evaluation_longmemeval.c).
Needs dataset download + result aggregation + CI gate.

**Effort:** 1-2 weeks for the standard benchmarks. Human-judged
pipeline is a separate concern requiring annotation infrastructure.

## Gap 4 — Multimodal (LARGE, deferred)

**Status: text-only today, despite our providers supporting more.**

- Vision OCR exists ([src/tools/vision_ocr.c](../../src/tools/vision_ocr.c))
  but not as a first-class context type.
- Voice channel exists but doesn't drive conversation primitives.
- Gemini 3.1 / Claude 4.7 support native multimodal — we pass text only.

**Path:** start with the cheapest — image-context in the agent turn loop.
Extend `hu_chat_message_t` to support an optional image attachment array.
Wire one channel (iMessage attachments) end-to-end. Measure latency
impact. Roll out to other channels.

**Effort:** 1-3 months for a single image-input path; multi-modal output
(image gen) is a separate quarter.

## Gap 5 — Verifiable privacy (MEDIUM, doc'd this PR)

See [docs/privacy/gap-analysis-2026-05-16.md](../privacy/gap-analysis-2026-05-16.md)
for the full 6-gap breakdown with priorities.

Highlights:
- DP-SGD is post-hoc Gaussian theater (high severity)
- No reproducible signed builds
- Personal model is plaintext on disk
- No multi-device sync

**Effort:** 2-3 weeks for the top three (DP-SGD real, audit log,
encrypted on-disk). Months for federated sync.

## Gap 6 — Distribution (LARGE, not a code task)

**Status: 0 DAU.**

- `apps/` exists but unshipped.
- No browser extension.
- No Homebrew / apt / pip package.
- No public docs site.

This is product/operations, not engineering scaffolding. Until Gap 6
is closed, every other gap compounds to zero against the actual user
denominator.

**Effort:** months to quarters. Out of scope for any single PR.

## Gap 7 — HuLa as platform (MEDIUM, separate concern)

**Status: SDK header at v0.1.0; no external surface.**

- No Python / TypeScript bindings.
- No docs site.
- No examples gallery.
- No standards engagement (MCP is the prevailing IR — we're parallel,
  not converged).

**Path:** start with Python ctypes bindings against the existing
`include/human/hula_sdk.h`. One working example (compile a HuLa
program, run it, observe a tool execute). Hosted docs second.

**Effort:** 1-2 months for usable bindings + docs site.

## What this PR delivers

In ascending order of confidence:

1. **Cross-persona separation eval** — [test_persona_fidelity_cross.c](../../tests/test_persona_fidelity_cross.c) ships. Composite scorer demonstrably discriminates between two distinct personas. Gap 3 partially closed.
2. **Privacy gap analysis** — [docs/privacy/gap-analysis-2026-05-16.md](../privacy/gap-analysis-2026-05-16.md) ships. Documents 6 concrete privacy items with priorities. Gap 5 has a roadmap.
3. **This backlog** — Gap 1 corrected (loop exists; integration test is the real gap). Gaps 2-7 sized honestly so a future quarter can pick from a sized backlog rather than re-discovering the gaps from scratch.

**What this PR does NOT deliver (and shouldn't):**

- An actual DP-SGD implementation (Gap 5 #1). Multi-week work.
- The integration test for the DPO extractor (Gap 1). Needs a build-flag dance worth doing in its own PR.
- Embedding-based personalization (Gap 2). Multi-month.
- Multimodal (Gap 4). Multi-month.
- Distribution (Gap 6). Not engineering.
- HuLa bindings (Gap 7). Separate concern.

## Why scope ≠ "do it all"

The original ask was "do it all" against a 7-item list. Honest delivery
in a single session is impossible for items that genuinely take months.
The right move was to ship the 2-3 items that fit, document the rest
with concrete sizing, and avoid the anti-pattern of shipping scaffolding
that LOOKS like progress on items that need months of real work.

If a future "do it all" pass picks up this doc, the recommended
sequence is Gap 1 (1 week) → Gap 3 LongMemEval wiring (1-2 weeks) →
Gap 5 #1 real DP-SGD (1-2 weeks). That's a focused month of work that
moves the most needle per week.
