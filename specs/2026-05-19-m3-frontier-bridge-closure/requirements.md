# M3 Frontier-Bridge Full Closure — Requirements

## Goal

Close the M3 (Private Learning) loop so that a chat turn served on the user's hardware actually routes through a LoRA adapter trained on the user's own data, with empirical proof that the personalized turn differs from the base turn in a measurable, persona-fidelity-relevant way.

Today the loop is structurally wired (training entry point exists, admin endpoint exists, contact-routes table exists) but functionally broken in six verified places. This spec closes those six gaps and adds the empirical proof.

## User stories

- As a user of h-uman on Apple Silicon, I want chat turns to route through a LoRA adapter trained on my own messaging data, so that the responses I see are personalized at the model layer, not just by a JSON profile in the prompt.
- As an operator running h-uman locally, I want a single deterministic command that proves the M3 loop is healthy end-to-end (cold-start → DPO ingest → train → swap → served turns → A/B fidelity delta), so that I can verify "personalization is on" without manual orchestration.
- As a developer maintaining h-uman, I want the adapter-swap path to fail loudly with operator-visible logs when the MLX server endpoint is missing or returns an error, so that "personalization is on" is never silently a lie.
- As a data-quality reviewer, I want a fidelity-based A/B gate (not just safetensors structural metadata) so that "PASS" verdicts mean the adapter actually shifts outputs toward the user's style, not just that training produced a non-empty file.

## Acceptance criteria

- [ ] **AC-M3-1: Swap endpoint is real or fails loud.** `scripts/mlx-server.py` either (a) defines `POST /v1/adapters/swap` itself with documented request/response JSON shape, OR (b) detects at startup that the delegated upstream (gemma-realtime or other) does not expose the endpoint and exits non-zero with a clear error message naming the missing dependency. No silent fallthrough to `mlx_lm.server` when the swap endpoint is absent.
- [ ] **AC-M3-2: Streaming chat routes the adapter.** `hu_agent_m3_route_per_turn()` is invoked from BOTH the non-streaming chat path (`agent_turn.c`, today's only caller) and the streaming chat path (`agent_stream.c`). Pinned by a regression test that fails if either path omits the call. The test must exercise both real call sites, not stubs.
- [ ] **AC-M3-3: Adapter-swap failures are surfaced, not swallowed.** When `hu_mlx_admin_swap_adapter()` returns any error (transport, HTTP non-2xx, missing adapter file), the agent records:
  - a structured log line at `error` level with the requested adapter path, the contact hash, and the underlying error code,
  - a metric increment on a new counter `m3.adapter.swap_failure_total{reason=...}`,
  - a one-shot info log on the FIRST occurrence after process start (per `silent-config-gated-subsystems.md`).
  The turn continues on base chat (no fabricated success), but the failure is observable. Pinned by a test that injects HTTP 500.
- [ ] **AC-M3-4: Outcome ring buffer is populated from production paths.** `hu_m3_frontier_adapter_record_outcome()` is invoked from every `hu_agent_m3_on_provider_success()` call site (today: 11 sites) with `token_count`, `latency_ms`, `contact_hash`. Pinned by a test that runs a fixture turn and asserts the outcome ring depth advanced by exactly one.
- [ ] **AC-M3-5: A/B harness scores actual outputs, not safetensors structure.** The persona-fidelity A/B gate runs the candidate adapter against a held-out prompt set of ≥20 prompts, scores each pair using `communication_style_fidelity_score` (already present at `src/ml/fidelity.c`), and emits a PASS verdict only if `(candidate_fidelity - baseline_fidelity) ≥ threshold` with `threshold` configurable (default 0.05 on the existing 0..1 scale). The pre-existing metadata judge stays in place as a structural sanity check but no longer gates promotion alone.
- [ ] **AC-M3-6: Single-command live-fire E2E.** A new script `scripts/m3-live-fire.sh` (or named equivalent under `scripts/`) runs end-to-end from cold start: starts daemon → starts MLX server → ingests a fixture DPO pair set (≥50 pairs) → triggers a real MLX LoRA training run (rank-16, ≥50 iterations) → swaps the resulting adapter via admin endpoint → serves N≥10 turns through the swapped adapter → emits an A/B report and exits 0 iff fidelity-PASS per AC-M3-5. Exit non-zero with a named failure mode otherwise.
- [ ] **AC-M3-7: Daemon auto-invokes MLX training against frontier model.** When the dpo-pair threshold gate fires (see Spec 2 / reaction-loop pair-count trigger), the daemon enqueues a real MLX-bridged training run targeting the configured frontier model (default `mlx-community/gemma-4-26b-a4b-it-4bit`), NOT only the reference HUML GPT. On completion the post-training hook calls `hu_mlx_admin_swap_adapter()` automatically. Pinned by an E2E test that uses a fake `mlx_lm` subprocess shim (no real GPU work in tests, per `HU_IS_TEST`).

## Non-goals

- Multi-user adapter routing infrastructure beyond what `hu_m3_contact_routes_*` already provides.
- Cross-provider adapter support (Gemini Vertex, Anthropic API, OpenAI). This spec is local-MLX-only.
- Quantization-aware training, distillation, or model-architecture changes. Rank-16 LoRA stays.
- Web UI for monitoring training runs.
- Server-side identity / auth on the admin endpoint beyond what already exists.
- Adapter eviction, multi-adapter ensembling, or rolling-update semantics. One adapter per contact at a time.

## Constraints

- C11, `-Wall -Wextra -Wpedantic -Werror`. ASan-clean.
- Tests deterministic. No real network, no real subprocess training in unit tests; the live-fire script (AC-M3-6) is gated behind a separate runner, not part of `human_tests`.
- New code uses `HU_IS_TEST` guards for side effects (real HTTP, real subprocess) per project rule.
- No new dependencies beyond libcurl (already present) and the existing Python/MLX subprocess interface. No FFI into the Python interpreter (per `~/.claude/rules/cross-language-via-http.md`).
- HTTP boundary between C agent and MLX server stays explicit and versioned. The swap endpoint URL must be configurable, not hard-coded.
- Logging must NOT include adapter weights or user content. Adapter paths and contact hashes only.
- Backwards-compatible with the existing reference-HUML-GPT path; the toy GPT continues to work, just not as the only option.
- All seven ACs must be pinned by automated tests in `tests/`. AC-M3-6 is the only one that runs out-of-band (not in `human_tests`); the rest run in the standard suite.

## Glossary

- **Frontier model**: the real chat model the user converses with (e.g., Gemma 4-26B-it). Not the reference HUML GPT.
- **Adapter swap**: `POST /v1/adapters/swap` HTTP call from C agent to MLX server, transitioning the loaded LoRA from path A to path B.
- **Outcome ring**: `hu_m3_inference_outcome_t[]` 4096-slot ring at `src/ml/m3_frontier_adapter.c`, today allocated but unpopulated by production code.
- **Live-fire**: the single-command E2E script from AC-M3-6.
- **Fidelity-PASS**: A/B verdict from AC-M3-5: candidate's `communication_style_fidelity_score` exceeds baseline by the configured threshold.
