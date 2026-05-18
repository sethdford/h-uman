---
plan: docs/plans/2026-05-11-init-04-mlx-qwen3-provider.md
auditor: group-10-init-series
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: HIGH
---

## Plan Summary
Ship the first on-device frontier-class provider: `src/providers/mlx_qwen3.c`
implementing the full `hu_provider_t` vtable including `load_adapter` /
`unload_adapter` / `active_adapter`. Drives `mlx-lm` via a persistent
Python helper (`scripts/mlx_qwen3_serve.py`) over length-prefixed JSON,
default Qwen3-4B-Instruct AWQ 4-bit. Includes `human ml lora-convert
--to=mlx` to convert in-tree LoRA checkpoints to `adapters.safetensors`.

## Key Claims (from the plan)
- Provider source at `src/providers/mlx_qwen3.c`
- Python helper at `scripts/mlx_qwen3_serve.py`
- Subprocess lifecycle (spawn, health-probe, exponential-backoff resurrect, deinit-kill)
- `human ml lora-convert --to=mlx` CLI command
- Default Qwen3-4B-Instruct AWQ 4-bit quantization

## Evidence

### Implemented? (code exists)
- `src/providers/mlx.c` exists (not `mlx_qwen3.c`) — but it is a **deliberate stub**. File comment line 3: *"Today every chat path returns HU_ERR_NOT_SUPPORTED — the file exists so the M3 Bridge B adapter slot has a registered consumer symbol, not because chat works."*
- `include/human/providers/mlx.h` exists with `hu_mlx_config_t` (model_path, adapter_path, max_tokens) — but no Python-subprocess fields, no quantization knob.
- No `scripts/mlx_qwen3_serve.py` (and `scripts/mlx_qwen3*` returned no matches).
- No `human ml lora-convert --to=mlx` CLI surface in `src/ml/cli.c`.
- No subprocess spawn/health-probe/resurrect code.

### Proven? (tests exist)
- `tests/test_mlx_provider.c` exists (also for `test_mlx_local_voice.c`) — and explicitly pins the stub contract: *"Today every chat path returns HU_ERR_NOT_SUPPORTED — these tests pin that contract so a future helpers.c refactor or vtable change can't silently break the dispatcher's fallback assumption."*
- No tests cover Python subprocess lifecycle, AWQ quantization, or `load_adapter`.

### Wired? (called in runtime path / dispatch)
- Registered in `src/providers/factory.c` line 210 under name `"mlx"`, and presets `mlx_local` / `mlx-local` exist at lines 126–127.
- Wired only as a `HU_ERR_NOT_SUPPORTED` return path — runtime callers fall through to next provider.

## Gaps
- The entire Python helper + JSON pipe protocol is absent.
- No real `load_adapter` implementation; the M3 mission's "LoRA-on-frontier-model" gap remains open.
- No `lora-convert` CLI.

## Notes
This is an honest stub: the code, header, and tests explicitly document
that they exist as a placeholder for future work. The pre-existing M3
Bridge B narrative says this provider is the gating dependency for
Init 01, 02, 05, 06 — so the entire dependency chain is blocked here.
