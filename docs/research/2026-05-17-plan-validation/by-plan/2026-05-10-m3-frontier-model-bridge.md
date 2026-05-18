---
plan: docs/plans/2026-05-10-m3-frontier-model-bridge.md
auditor: group-8-behavior-m3-master-sota
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: HIGH
---

## Plan Summary
M3 is the multi-month plan to honestly bridge the "private learning" promise:
move from training a reference HUML GPT to fine-tuning the frontier chat
model the user actually talks to. It defines three bridges (A llama.cpp,
B MLX, C unified) and phases the work (0 honesty → 1 scaffold → 2 real LoRA →
3 Apple → 4 chat-time merge → 4.0 reference-GPT done, 4.1 config auto-load
done, 4.2 frontier merge pending vendor-in, 4.3 chat-path stub hooks).

## Key Claims (from the plan)
- Phase 0 (honesty): `--checkpoint` actually warm-starts reference GPT, caveat printed — DONE
- Bridge A.0 (Alpaca JSONL exporter) — DONE
- Bridge A.0.5 (banks-from-history) — DONE
- Bridge A scaffold (`src/providers/llamacpp.c` with NOT_SUPPORTED stubs unless `HU_ENABLE_LLAMACPP=ON`) — DONE
- Bridge A real impl behind `__has_include("llama.h")` — landed
- Bridge B (MLX provider) — scaffold only, NOT_SUPPORTED everywhere
- 4.0 huml chat-time LoRA merge via `hu_gpt_attach_lora` — DONE
- 4.1 config-driven adapter auto-load — DONE
- 4.3 chat-path stub hooks: `hu_agent_m3_on_provider_success` after every successful provider call
- AUDIT NOTE: `hu_m3_frontier_adapter_noop_infer` is literally `(void)adapter; return HU_OK`
- D1.3 rollback flag (`personalization.m3_adapter_disabled` + env override) — DONE
- D2.1 caveat snapshot tests — DONE
- D2.2 offline persona-fidelity scorer + lora-baseline + lora-ab + lora-runner + fidelity-status — DONE
- Provider dispatcher safety: cloud providers return NOT_SUPPORTED from `load_adapter` — DONE
- `test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` regression guard — DONE

## Evidence

### Implemented? (code exists)
- `src/ml/m3_frontier_adapter.c:89` — `hu_m3_frontier_adapter_noop_infer` is
  literally `(void)adapter; return HU_OK` (confirmed verbatim)
- `src/providers/llamacpp.c` (522+ LOC) with real implementation at line 185,
  gated on `HU_LLAMACPP_LINKED` (via `__has_include("llama.h")` at line 54).
  Adapter API: `llama_adapter_lora_init` + `llama_set_adapters_lora`.
- `src/providers/llamacpp_decode.c`, `_kvcache.c`, `_sampling.c` — supporting modules
- `src/providers/mlx.c` — all paths return `HU_ERR_NOT_SUPPORTED`; file is a
  registered-symbol stub for the future MLX subprocess/HTTP path
- `src/ml/lora.c` + `src/ml/checkpoint.c` + `src/ml/dpo.c` — reference-GPT training
- `src/ml/cli.c` carries `lora-persona`, `--export-jsonl`, `--from-history`,
  `lora-baseline`, `lora-ab`, `lora-runner`, `fidelity-status`
- `src/persona/examples.c:519` `hu_persona_banks_extract_from_history`
- `src/ml/fidelity.c` + `src/eval/persona_fidelity.c`
  (`hu_persona_fidelity_score_l1` + `hu_communication_style_fidelity_score`)
- `include/human/ml/m3_frontier_adapter.h` + helpers like
  `hu_ml_lora_persona_caveat_block`, `hu_m3_adapter_should_disable`

### Proven? (tests exist)
- `tests/test_personal_model_atomic_save.c` — pins fsync+rename atomicity
  (`test_personal_model_save_preserves_prior_state_when_tmp_blocked`)
- `tests/test_llamacpp_provider.c`, `_factory_config.c`, `_decode.c`,
  `_kvcache.c`, `_sampling.c`, `_lora_hotswap.c`, `_chat_metal.c`
- `tests/test_mlx_provider.c` — scaffold tests asserting NOT_SUPPORTED
- `tests/test_provider_all.c:3071` `test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat`
  + run-line `:3369`
- `tests/test_ml.c:4801+` covers `hu_m3_adapter_should_disable` precedence + caveat snapshots
- `tests/test_personal_model.c` + `_contradicts.c`

### Wired? (called in runtime path / dispatch)
- `hu_agent_m3_on_provider_success` invoked at 5 sites in
  `src/agent/agent_turn.c` (lines 4848, 4994, 5419, 5547, 5684) and 6 sites
  in `src/agent/agent_stream.c` (1397, 1469, 1837, 1906, 1931, 2273) —
  matches plan claim
- Call sites exist but every one of them ultimately invokes
  `hu_m3_frontier_adapter_noop_infer` which captures no signal. The plan
  explicitly admits this in the AUDIT NOTE; we confirm it verbatim.
- llama.cpp chat path is real when `HU_ENABLE_LLAMACPP=ON` AND a system
  `llama.h` is reachable; otherwise the provider degrades to NOT_SUPPORTED
  and the daemon-pattern test proves chat continues to work.

## Gaps
- **The actual M3 promise — "the model the user talks to learns" — is NOT shipped.**
  The chat path against cloud providers (Anthropic/OpenAI/Gemini) cannot consume
  any LoRA; `load_adapter` returns NOT_SUPPORTED for them by design.
- The personalization signal is plumbed but inert: no gradient accumulation,
  no signal capture, no state persistence in `noop_infer`.
- Bridge B (MLX) is a registered-symbol stub. Phase B.1/B.2/B.3 scaffolds
  (subprocess + HTTP front, `mlx_lm.lora` orchestration, draft-adapter for
  spec decode) have not landed.
- Persona-fidelity scoring (D2.2) is the only end-to-end evaluation path;
  the L1 (style) component is wired, but L2 (traits) and the composite
  scoring referenced in `persona_fidelity.c` need cross-validation against
  external eval data before being load-bearing.
- Bridge C (unified open-weight frontier provider with hot-swap per
  conversation) — explicitly punted to month 3+.

## Notes
- **This plan is the central honesty about what M3 has and has not shipped.**
  CLAUDE.md M3 status ("Hardest. Bridge A daemon-pattern proven.") is consistent
  with the audit. The plan does NOT overclaim — every "DONE" row I checked has
  matching code and tests; the "noop" admission is explicit and well-tested.
- The "Bridge A daemon-pattern proven" claim is corroborated by
  `tests/test_provider_all.c::test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat`
  (lines 3071, 3369). This is the *fallthrough safety* guarantee, not a
  personalization guarantee — distinction worth holding.
- Verdict is PARTIAL because the structural plumbing, dispatcher safety, and
  offline-evaluation framework are all real and proven, but the M3 product
  thesis ("LoRA that measurably changes inference behavior") is unmet
  (Bridge A real impl exists but only on opt-in builds; cloud providers
  cannot consume adapters at all).
