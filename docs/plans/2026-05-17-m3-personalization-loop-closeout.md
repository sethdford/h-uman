---
title: M3 Personalization Loop — Phase B Closeout
status: phase-b-shipped, live-fire-proven (2026-05-17)
owner: ML subsystem
created: 2026-05-17
parent: docs/plans/2026-05-17-m3-mlx-bridge-execution-plan.md
related:
  - CLAUDE.md (M3 row in Strategic Missions)
  - docs/plans/2026-05-10-m3-frontier-model-bridge.md
  - docs/research/m3-live-fire-evidence.log
  - scripts/m3_outcome_driver.py
  - scripts/stub_mlx_server.py
  - scripts/live_fire_m3_loop.sh
  - .github/workflows/m3-loop-smoke.yml
---

# What this doc covers

The **personalization loop** axis — separate from the parent execution
plan's Phase B1-B5 "native MLX C linkage" axis. Both bridges share the
M3 codename, but they answer different questions:

| Axis | Question |
|---|---|
| Native MLX (parent plan B1-B5) | How does inference run in C against a real gemma-4-31b model? |
| **Personalization loop (this doc)** | **How do real conversations become training data and a hot-swapped adapter?** |

Phase B-pre (probe counter, observability seam, 2026-05-17) was the
shared precondition. After that, the parent plan tackles native MLX
runtime; this doc tackles the loop that USES whatever inference path
the daemon happens to be running.

# Why this is now closed

Every code boundary the loop crosses is **pinned by a test** and
**live-fire-proven on a release binary** against actual network I/O.

# What shipped

## Producer (C side — agent + ring buffer)

| Slice | Commit | Contract |
|---|---|---|
| Probe counter (B-pre) | first slice 2026-05-17 | `hu_m3_frontier_adapter_probe_count` observable from tests |
| Outcome capture in ring | round 2 2026-05-17 | `hu_m3_inference_outcome_t` (96 bytes, pinned by `_Static_assert`), 4096-record ring |
| 11+1 chat-path call sites wired | 2026-05-17 round 2 | `hu_agent_m3_record_chat_outcome` at every PASS branch in `agent_turn.c` + `agent_stream.c` |
| Token-count estimate (B3 v1 prerequisite) | `d4633bd9` | `prompt_tokens = prompt_len/4` so the driver's `pt>0 && ct>0` filter passes |
| Global adapter pointer | round 2 2026-05-17 | `hu_m3_outcomes_register_global_adapter` so the gateway endpoint reads the daemon's adapter |

## Endpoint (C side — gateway)

| Slice | Commit | Contract |
|---|---|---|
| `GET /v1/m3/outcomes` (B3 v0) | round 2 2026-05-17 | NDJSON, query params (`limit`, `turn_kind`, `since_ms`), localhost-only auth |
| Query-string path match | `234dde29` | `path_is()` doesn't terminate on `?` — handled explicitly for this endpoint |
| `HU_ENABLE_ML=OFF` guard | `91605d11` | Endpoint returns empty 200 NDJSON when ML disabled at build time |

## Driver (Python — closes the loop)

| Slice | Commit | Contract |
|---|---|---|
| Poll + watermark + dedup | `d4633bd9` | Atomic state save; `prompt_hash` dedup survives restarts |
| Selection policy | `d4633bd9` | PASS only, base adapter only, 50ms ≤ latency ≤ 30s, tokens > 0, per-contact cap 64 |
| Threshold trigger + simulate-train | `d4633bd9` | Default threshold 32 samples; `--simulate-train` produces placeholder adapter in seconds |
| Adapter swap call | `d4633bd9` | `POST /v1/adapters/swap` with absolute path; soft-fail when MLX unreachable |
| Default turn_kind = 0 (any) | `91605d11` | Stream + batch + proactive all eligible (was previously stream-only) |

## Test layer

| Test | What it pins |
|---|---|
| `test_m3_record_chat_outcome_populates_token_estimates` | Producer-side token estimate fires (catches "0 = unknown" regression) |
| `test_m3_agent_on_provider_success_advances_probe_count` | Producer-side hook reaches the adapter |
| All 67 `m3_*` tests (`./build/human_tests --filter=m3`) | Ring buffer wrap, snapshot ordering, struct size, FNV-1a determinism |
| `scripts/test_m3_outcome_driver.py` (20 assertions) | Driver chain: poll → select → dedup → train → swap, with fakes |
| `scripts/live_fire_m3_loop.sh` (10 numbered steps) | Real release binary, real daemon, stub MLX, real network I/O |

## CI

`.github/workflows/m3-loop-smoke.yml` triggers on every push that
touches the loop's surface:

- **driver-e2e job** — runs `scripts/test_m3_outcome_driver.py` against
  in-process fakes (Linux only, ~5s)
- **live-fire job** — builds release binary, runs the full
  orchestration on Linux and macOS (~3-5min each)

Failure uploads `/tmp/live-fire-run.log`, the gateway log, the stub-mlx
log, and the chat response bodies as artifacts (7-day retention).

## Operator entry point

```bash
make demo-loop          # cmake --preset release + build + live-fire script
```

idempotent — kills prior processes, wipes driver state, runs the full
loop, reports PASS/FAIL per step.

# Evidence

Verbatim live-fire output captured at `docs/research/m3-live-fire-evidence.log`.

End state from that run:

```
- 10 outcomes recorded in ring (guard=PASS, pt=17, ct=8, a=0,
  latency_ms ∈ {0, 151, 158, 782, 791})
- Driver fetched 10, selected 7, appended 3 (4 dedup_skipped)
- Adapter artifact written: m3-driver-{ts}.safetensors (82 bytes)
- Stub MLX active adapter flipped to the new path; tensors_loaded 42→43
```

# Cross-axis: what this does NOT touch

The native MLX C-linkage path (Phase B1-B5 in the parent plan) is
still in its original status — those phases are about *how inference
runs natively*, which is orthogonal to the loop that USES the inference.
A future commit could land real MLX C linkage AND the loop keeps
working without changes — that's the point of the seam.

Items deliberately left for Phase C (see
`docs/plans/2026-05-17-m3-phase-c-plan.md`):
- Real provider `usage` block → outcome.prompt_tokens/completion_tokens
  (currently a bytes/4 estimate)
- model_id / adapter_id mapping (currently both default to 0)
- training_loop.py adapter for `--source-jsonl` (currently the driver
  hands off via subprocess but `training_loop.py` doesn't accept this
  flag yet)
- ASan-caught stack-use-after-scope in chat path (separate slice; not
  on the loop's critical path)
- First REAL LoRA train against accumulated outcomes (currently
  simulate-train is the only proven path)
- A/B eval: outcome-trained adapter vs baseline

# Decision: when to advance status to "phase-c-shipped"

Phase C is complete when:
1. The same `make demo-loop` orchestration runs with `--simulate-train`
   REMOVED (real `training_loop.py` produces an MLX-compatible
   safetensors artifact in <60s for a 32-sample warmup)
2. The post-swap `tensors_loaded` count reflects a non-trivial number
   of tensors actually loaded (current stub-mlx just increments a
   counter)
3. A blind A/B eval shows the post-train adapter beats the pre-train
   baseline on at least one persona-fidelity metric by ≥1 stderr
4. The chat path holds clean under ASan stress (the dev preset's bug
   fix is in)
