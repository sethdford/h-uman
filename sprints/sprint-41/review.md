# Sprint 41 Review

## Shipped

| Item | Implementation |
|------|----------------|
| CF-4 finish | `lora_score_example_bank_before` + `lora_score_after_adapter_probes` (fidelity v2); baseline from bank mean |
| Gateway | `metrics.guard_rejects` RPC → `hu_guard_reject_stats_snapshot` JSON |
| EWMA G5 | `HU_GUARD_ASSISTANT_LEN_EWMA_ALPHA` 0.35 in `hu_agent_internal_recent_assistant_avg_len` |
| Daemon | `w14_eval_gate` + `gate_persona` / model on `w14_lora_ctx` when `HU_ENABLE_RL_FULL` |

## Validation

- Response guard + runner-eval-gate + gateway guard_rejects tests pass.
- `scripts/audit-imessage-leaks.sh --self-test` OK.
- Live audit: 6 historical hits in chat.db (pre-fix messages; expected).
