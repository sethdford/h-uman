# Seth-Voice Default — Requirements

> Break the prompt-only personalization ceiling. Today the live chat path is
> cloud Gemini (structurally un-fine-tunable), the learned style reaches the
> model only as an uncoordinated side-block, the feedback loop is gated off by
> default, and "sounds like Seth" is measured offline against a model nobody
> talks to. Research (LaMP, arXiv 2507.04889, Anthropic PSM) pins prompt-only
> persona at 10–20% fidelity; SOTA is RAG + fine-tune + memory. This spec moves
> the live path onto the learnable local model and closes the loop.

## Background (verified against code, 2026-05-28)

- `hu_persona_select_examples` (src/persona/examples.c:228) scores few-shot examples **only** by topic keyword overlap — ignores the learned style fingerprint.
- `hu_personal_model_build_prompt` IS injected (src/agent/agent_turn.c:3577) as a separate ~8KB block — so learned style reaches the model, but uncoordinated with example selection.
- `mr_mlx_local_enabled` is "Opt-in; default false" (src/config_parse_agent.c:390); `mlx_local.streaming_enabled = false` (src/config_merge.c:477). **Default live path = cloud Gemini.**
- `hu_mlx_admin_swap_adapter` is curl-gated (src/ml/mlx_admin.c:194). `HU_ENABLE_CURL` is ON in the release preset, OFF in dev/test presets — so swap works in shipped binaries but not in dev builds.
- `reaction_collection.enabled` and `learning.nightly_lora_enabled` zero-init to false (not seeded in config_merge.c).
- The v4-repair LoRA adapter already proved **+27pp** persona fidelity offline (commit 9ab9b86e). The artifact works; it just isn't the default serving path.

## User stories

- As Seth, I want h-uman to talk to me through the **local Gemma+LoRA model that has learned my voice** by default (when available), so replies sound like me instead of a generic assistant — while transparently falling back to cloud when the local model is unavailable, so I'm never left without a reply.
- As Seth, I want my real reactions and edits (iMessage tapbacks, response corrections) to be **collected and turned into training pairs automatically**, so the model keeps getting more like me without me configuring anything.
- As Seth, I want the few-shot examples shown to the model to **match how I actually write** (lowercase, abbreviations, length), so the model isn't fed contradictory signals.
- As Seth, I want the output guards to respect **how I actually message each person** rather than a universal 250-char/“safe” register, so my real voice isn't flattened.
- As Seth, I want a way to **measure whether it actually sounds like me on the live path**, including blind judgment from people who know me, so progress is real and not a self-graded offline number.

## Acceptance criteria

### Phase 1 — Break the ceiling (default local path + close the loop)

- [ ] **AC-1**: When a usable Seth LoRA adapter exists AND the MLX server is reachable, the default model-routing decision selects the local Gemma+LoRA provider for conversational turns **without requiring `mr_mlx_local_enabled=true`** in config. A unit/integration test asserts the router returns the local provider under those preconditions and the cloud provider when either precondition is false.
- [ ] **AC-2**: When the local path is selected but fails (server unreachable, timeout, empty/degenerate output per existing response_guard), the turn **falls back to the cloud provider within one turn** and emits exactly one operator-visible log line naming the fallback reason. A test pins: local-fail → cloud reply returned, fallback logged once.
- [ ] **AC-3**: A `human doctor`-style readiness check reports the local-voice path status (adapter present? path/sha, MLX server reachable? curl-enabled build?) so the default is explainable. Test asserts each of the three sub-states renders distinctly.
- [ ] **AC-4**: `reaction_collection.enabled` defaults to **true** in a fresh config, and the spurious `unknown key: 'reaction_collection'` startup warning is eliminated (the key is recognized by the parser banner). Test: fresh config → enabled true; parsing a config containing the key produces no unknown-key warning.
- [ ] **AC-5**: Real user-feedback signals (iMessage tapback + response correction/edit) produce DPO pairs tagged with a non-synthetic `source` (e.g. `imessage_tapback`, `user_edit`) in `dpo_pairs`. Test exercises the collection path end-to-end with a fixture and asserts ≥1 real-sourced pair is written.
- [ ] **AC-6**: When `learning.nightly_lora_enabled=true` AND the real-pair count crosses the configured threshold, the nightly job trains an adapter and hot-swaps it via `hu_mlx_admin_swap_adapter`; the swap is a no-op-with-clear-log when curl is disabled (dev builds). Test pins both the curl-on swap-attempt path and the curl-off clear-log path.

### Phase 2 — Fidelity quality (coordinate the signals)

- [ ] **AC-7**: `hu_persona_select_examples` (or a new style-aware selector wrapping it) scores examples by **style affinity to the learned personal-model fingerprint** (lowercase_ratio, abbreviation_ratio, avg length) in addition to topic overlap. Test: given two candidate examples equal on topic, the one closer to the fingerprint on ≥2 axes is selected.
- [ ] **AC-8**: The selector degrades gracefully — when no personal-model content exists, behavior is **identical to today's topic-only selection** (no regression). Test pins byte-identical selection output for the empty-model case.
- [ ] **AC-9**: `response_guard` G5 length-anomaly uses a **learned per-contact/per-channel baseline** (from personal_model + contact profile) instead of the universal relative-multiplier-with-floor. Test: a reply length that is normal for Seth-to-that-contact is NOT rejected, while a genuinely anomalous one still is.
- [ ] **AC-10**: The shape classifier's hard length caps become **per-contact-aware** (close contacts may exceed the universal cap up to a learned bound) while still blocking the structural fails (markdown, AI openers). Test: a 300-char iMessage to a `close` contact passes; the same with a bullet list still fails.

### Phase 3 — Measurement (prove it on the live path)

- [ ] **AC-11**: A live-path fidelity eval runs the **actual default serving path** (local-or-cloud as the router would choose) against held-out Seth-style fixtures and emits a fidelity score + the path used. Test: harness runs headless, produces a verdict JSON with `path_used` and a numeric score.
- [ ] **AC-12**: A blind human-eval harness exports anonymized response pairs (real Seth vs. model) to a rateable format and ingests 1–10 ratings on a fixed rubric (tone, vocabulary, humor, decision-style), producing an aggregate per-dimension score. Test: round-trips a fixture rating set into the aggregate.

## Non-goals

- **Not** training a new base model or changing the LoRA recipe (the v4-repair recipe + `lora-scale-default-or-die.md` rule stand).
- **Not** building a new fact-extraction or memory subsystem — those exist; this wires what's there.
- **Not** changing cloud provider selection logic for non-conversational tiers (analytical/tool-heavy turns may stay cloud).
- **Not** shipping streaming on the local path in this spec (`mlx_local.streaming_enabled` stays off; buffered is fine — streaming is a known-deferred, default-off path).
- **Not** auto-enabling `nightly_lora` by default (operator opt-in remains; only `reaction_collection` flips to default-on).
- **Not** re-mining/auto-rewriting the persona JSON file on a schedule (separate future work; this spec coordinates *selection*, not persona refresh).

## Constraints

- C11, `-Wall -Wextra -Wpedantic -Werror`, free every allocation (ASan clean). Full `human_tests` suite passes (0 failures, 0 ASan).
- No new external dependencies. Local path reuses the existing `hu_http_post_json` + MLX server contract (cross-language over HTTP, per `cross-language-via-http.md`).
- Default behavior change (AC-1, AC-4) must be **safe**: never leave a turn unanswered (AC-2 fallback is mandatory), never regress cloud-only users who have no adapter/server.
- Test discipline: no real network, deterministic, `HU_IS_TEST` guards on side effects. Test/source gate symmetry honored for any flag-gated source.
- Security: deny-by-default unchanged; never log secrets; local model output still passes the existing outbound guard chain.
- Per `tests-that-pin-bugs.md`: each guard/selector change ships a positive contract test, not a test that locks the old behavior.

## Proposed phasing & sequencing

| Phase | Workstreams | Why this order |
|---|---|---|
| **1** | AC-1…AC-6 | The ceiling-breaker. Until the live path is the learnable model AND real pairs flow, Phases 2–3 polish a model nobody learns from. Highest leverage. |
| **2** | AC-7…AC-10 | Quality once the right model is serving: stop feeding contradictory signals and stop flattening voice. |
| **3** | AC-11…AC-12 | Measurement closes the loop — without live + blind eval we can't tell if any of this worked. |

Recommendation: approve Phase 1 ACs as the first implementable slice; Phases 2–3 can be re-confirmed after Phase 1 lands, since Phase 1 may reshape what Phase 2 needs.
