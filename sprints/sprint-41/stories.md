# Sprint 41 — CF-4 finish, gateway guard stats, EWMA G5, daemon eval gate

## Goals

1. **CF-4 finish** — Persona example-bank + post-adapter fidelity probes in `hu_lora_training_runner`.
2. **Gateway RPC** — `metrics.guard_rejects` for dashboard telemetry.
3. **EWMA G5** — Replace point average with α=0.35 EWMA in `recent_assistant_avg_len`.
4. **Daemon eval gate** — Wire `w14_lora_ctx.eval_gate` + `gate_persona` on learning builds.
