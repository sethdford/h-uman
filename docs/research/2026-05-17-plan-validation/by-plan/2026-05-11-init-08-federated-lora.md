---
plan: docs/plans/2026-05-11-init-08-federated-lora.md
auditor: group-10-init-series
audited_at: 2026-05-17
implemented: NONE
proven: NONE
wired: NONE
verdict: NOT_STARTED
confidence: HIGH
---

## Plan Summary
Phone, laptop, and desktop running `human` discover each other via mDNS,
authenticate via existing `pairing.c`, open a Noise-XX channel over TCP,
and run a single round of secure FedAvg over their per-device LoRA
gradients with optional DP-FedLoRA noise. Nothing leaves the user's
device fleet (no external aggregator).

## Key Claims (from the plan)
- mDNS device discovery wired to existing pairing
- Secure FedAvg implementation
- Optional DP-FedLoRA noise injection
- Reuses `pairing.c` (no new ceremony types)
- Noise-XX channel over TCP

## Evidence

### Implemented? (code exists)
- NONE FOUND. Grep for `fed_avg`, `fedlora`, `fed_lora`, `FedAvg`, `hu_federated`, `DP-FedLoRA` across `src/`, `include/`, `tests/` returned zero hits beyond the plan document.
- `find . -name "*fedlora*" -o -name "*federated*"` returns only the plan file.
- `src/security/pairing.c` exists (the plan's claimed reuse target) but contains no federated-LoRA additions.

### Proven? (tests exist)
- NONE FOUND.

### Wired? (called in runtime path / dispatch)
- N/A — no code to wire.

## Gaps
- mDNS device discovery, FedAvg, DP noise, and the cross-device transport all absent.

## Notes
Plan status header is "design (D0–D7 proof bar)". Among the most distant
from shipping in the Init series — depends on Init 04 having LoRAs to
average in the first place.
