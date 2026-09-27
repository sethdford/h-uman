---
title: K3 prefill spike raw evidence (2026-09-26/27)
date: 2026-09-27
status: reference
---

# K3 prefill spike: raw evidence

Raw outputs behind §9 of `../../2026-09-26-k3-teacher-lane-design.md`.
Layers 0–9 of Kimi K3 on an M4 Max (internal SSD, `--cache-gb 8`), synthetic prompts only.

- `run_spike.sh`, `spike_summary.txt`: the runner and its combined output
- `results/r0_*`: 816-token prompt at prefill chunk 64 vs 4096 (logits compared bit-for-bit)
- `results/r1_*`, `r2_*`: full ~3.8k-token prompt, chunk 64 vs 4096
- `results/r3_*`, `r4_*`: prefix saved once, then suffix after `--load-state`
- `results/verify_new.*`: batched-projection build (`perf/batched-prefill` 69118b3) vs baseline
- `results/k3_sample.txt`: macOS `sample` profile, 30 s mid-prefill
- `results/logits.sha256`: checksums of the compared logits files (the binaries themselves were not kept)
- `engine-prefill-chunk-override.patch`: the throwaway `K3_PREFILL_CHUNK` env override used by the spike
- `prompts/`: the synthetic judge prompts (fictional persona, no real messages)

Not kept: 149 GB of re-downloadable shards (`moonshotai/Kimi-K3`, 13 of 96), the 654 MB saved prefix state, and the binaries.
