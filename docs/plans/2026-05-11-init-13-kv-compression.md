---
title: "Init 13 — DeltaKV / SWAN KV-cache compression"
created: 2026-05-11
status: design
risk: high
parent: 2026-05-11-sota-2026-massive-team-program.md
related:
  - 2026-05-11-sota-2026-massive-team-program.md
  - 2026-05-10-w10-neural-memory.md
  - 2026-05-10-master-follow-through-program.md
  - 2026-05-10-sota-roadmap-6mo.md
  - 2026-05-10-m3-frontier-model-bridge.md
  - 2026-05-11-rl-loop-phase-1-llamacpp.md
  - adr/2026-05-10-w10-kv-replay-deferred.md
  - ../../include/human/memory/neural_memory.h
  - ../../include/human/provider.h
  - ../../src/providers/llamacpp.c
  - ../standards/security/threat-model.md
  - ../../AGENTS.md
  - ../../CLAUDE.md
---

# Init 13 — DeltaKV / SWAN KV-cache compression

## One-line

Compress the W10 neural KV-cache **4–8× at <1% token-distribution quality loss** via two complementary backends — **DeltaKV** (low-rank residual coding) and **SWAN** (sliding-window pruning with attention-sink retention) — behind a new vtable, negotiated with the provider via a capability flag, framed in a versioned blob envelope, and refusing to touch any KV that touches a detected secret. First compression backend that ships ASan-clean and end-to-end testable.

## Why this initiative

Two upstream constraints make this the right slot:

1. **W10 ADR ([`adr/2026-05-10-w10-kv-replay-deferred.md`](adr/2026-05-10-w10-kv-replay-deferred.md))** parks provider short-circuit until a versioned blob envelope and a safety contract exist. Today `neural_kv_cache.blob` is the empty string and `agent_turn.c` logs *"W10 KV prior row (no provider skip)"*. This initiative is the blob envelope + safety contract.
2. **Track B (6-mo SOTA roadmap)** spec-decode prep (`B2`/`B3`) needs cache reuse across speculator + target. A 4–8× shrink lets the same RSS budget hold both caches and unlocks the persona-aligned draft milestone (A3) without bumping the 6 MB peak-RSS budget in `CLAUDE.md`.

Per the SOTA program proof bar (D0–D7), this doc is the gate; the implementation sprint executes it.

## North-star numbers (gates)

| Metric | Today | Sprint exit |
|---|---|---|
| KV bytes / 4K-token prefix (FP16, GQA-8, 28 layers, dim=128) | ~7.2 MiB raw | ≤ 1.8 MiB (4× DeltaKV) and ≤ 1.0 MiB (8× DeltaKV+SWAN) |
| Token-distribution Total-Variation Distance (TVD) decoded vs. uncompressed on a fixed 256-prompt fixture | n/a | **< 1.0%** mean, **< 2.5%** p95 |
| Encode time, 4K-token cache, M3 Max | n/a | **≤ 2 ms** per call (release build) |
| Decode time, 4K-token cache, M3 Max | n/a | **≤ 2 ms** per call (release build) |
| ASan errors over 1 M random encode/decode round-trips | n/a | **0** |
| libFuzzer crashes / UB on `decode(blob)` after ≥ 60 s × 4 corpora | n/a | **0** |
| MinSizeRel binary delta (both codecs, gated `HU_ENABLE_KV_COMPRESSION=ON`) | 0 | **≤ +40 KB** |
| Peak RSS delta during decode (4K-token cache) | 0 | **≤ +0.5 MB** transient, **0** sticky |

If any of these regress, the slice does not merge; if neither codec hits TVD < 1% and ≤ 2 ms, the defer clause (§9) fires.

---

## D1 — Vtable surface and naming

**New header:** `include/human/memory/kv_compressor.h`

```c
#ifndef HU_KV_COMPRESSOR_H
#define HU_KV_COMPRESSOR_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── On-wire envelope (see §3) ─────────────────────────────────────────── */

/* "HUKV" little-endian. Single byte after that is the envelope version. */
#define HU_KV_BLOB_MAGIC      0x564B5548u  /* 'H','U','K','V' */
#define HU_KV_BLOB_VERSION    1u

/* Stable codec identifiers. Negotiated with the provider via
 * hu_provider_caps_t.kv_codec_id. The numeric value is permanent;
 * removing a codec means leaving the id reserved. */
typedef enum hu_kv_codec_id {
    HU_KV_CODEC_NONE     = 0,  /* passthrough — raw FP16 payload */
    HU_KV_CODEC_DELTAKV  = 1,  /* low-rank residual */
    HU_KV_CODEC_SWAN     = 2,  /* sliding-window + attention sinks */
    HU_KV_CODEC_RESERVED = 0xFF,
} hu_kv_codec_id_t;

/* Heap-owned blob result. `bytes` is allocator-owned; `bytes_len` is the
 * full envelope (header + payload). */
typedef struct hu_kv_blob {
    uint8_t *bytes;
    size_t   bytes_len;
} hu_kv_blob_t;

void hu_kv_blob_free(hu_allocator_t *alloc, hu_kv_blob_t *blob);

/* Parse only the envelope header without touching the payload. Returns
 * HU_OK on a well-formed header (magic + version + codec_id + payload_len
 * within bytes_len). Used by decoders to validate before any work. */
typedef struct hu_kv_blob_header {
    uint32_t magic;
    uint8_t  version;
    uint8_t  codec_id;
    uint16_t flags;        /* reserved; must be 0 in v1 */
    uint32_t payload_len;  /* bytes after the 12-byte header */
    uint32_t k;            /* number of half-floats in the *decoded* KV */
} hu_kv_blob_header_t;

hu_error_t hu_kv_blob_parse_header(const hu_kv_blob_t *blob,
                                   hu_kv_blob_header_t *out);

/* ── Compressor vtable ─────────────────────────────────────────────────── */

struct hu_kv_compressor_vtable;

typedef struct hu_kv_compressor {
    void *ctx;
    const struct hu_kv_compressor_vtable *vtable;
} hu_kv_compressor_t;

typedef struct hu_kv_compressor_vtable {
    /* Identity. Stable lowercase: "none" | "deltakv" | "swan". */
    const char *(*name)(void *ctx);
    hu_kv_codec_id_t (*codec_id)(void *ctx);

    /* Encode k IEEE-754 fp32 values into a versioned envelope. `kv` is
     * borrowed; `out` is heap-owned via the compressor's own allocator
     * (set at create time). On failure `*out` is zeroed.
     *
     * Required to be deterministic for a given (ctx-config, kv, k). */
    hu_error_t (*encode)(void *ctx, const float *kv, size_t k,
                         hu_kv_blob_t *out);

    /* Decode the envelope into a caller-provided buffer of at most
     * `k_max` floats. On HU_OK `*out_k` is the number of floats written.
     * Returns HU_ERR_TOO_LARGE when the decoded size exceeds k_max,
     * HU_ERR_INVALID_ARGUMENT on header mismatch, and HU_ERR_CORRUPT
     * on payload damage. MUST be UB-free for any byte sequence — the
     * fuzz harness in §D3 is the gate. */
    hu_error_t (*decode)(void *ctx, const hu_kv_blob_t *blob,
                         float *kv_out, size_t k_max, size_t *out_k);

    /* Release ctx-owned state. Idempotent. */
    void (*deinit)(void *ctx, hu_allocator_t *alloc);
} hu_kv_compressor_vtable_t;

/* ── Factories ─────────────────────────────────────────────────────────── */

/* Passthrough — exists for unit tests and as a sanity baseline. Encodes
 * the raw fp32 payload behind the standard envelope. Quality loss is
 * exactly 0; ratio is the worst case. */
hu_error_t hu_kv_compressor_create_none(hu_allocator_t *alloc,
                                        hu_kv_compressor_t *out);

/* DeltaKV — low-rank residual coding. `rank` is the SVD/QR-style
 * residual rank (typical: 8–32 for GQA-8 dim=128 K/V tensors); `clip`
 * is the per-channel absolute clip applied to residuals before
 * quantization (typical: 6.0 std-devs ≈ 1e-2 after normalization).
 *
 * rank == 0 or clip <= 0 returns HU_ERR_INVALID_ARGUMENT. */
hu_error_t hu_kv_compressor_create_deltakv(hu_allocator_t *alloc,
                                           int rank, float clip,
                                           hu_kv_compressor_t *out);

/* SWAN — sliding window with attention-sink retention. Keeps the first
 * `sink_tokens` tokens (default 4) plus the most-recent `window` tokens
 * verbatim, drops the middle. `window <= 0` returns
 * HU_ERR_INVALID_ARGUMENT. Combines with DeltaKV by stacking decoders
 * via codec_id alone; the design here keeps each codec single-stage
 * and lets the negotiator pick. */
hu_error_t hu_kv_compressor_create_swan(hu_allocator_t *alloc,
                                        int sink_tokens, int window,
                                        hu_kv_compressor_t *out);

/* Resolve by stable string for config-driven wiring. Returns
 * HU_ERR_NOT_FOUND when no codec matches. */
hu_error_t hu_kv_compressor_create_by_name(hu_allocator_t *alloc,
                                           const char *name, size_t name_len,
                                           int rank_or_window, float clip,
                                           hu_kv_compressor_t *out);

/* Convenience: dispatch decode to the right codec by looking up codec_id
 * from the envelope. The registry is closed (none / deltakv / swan);
 * unknown codec ids return HU_ERR_NOT_SUPPORTED. Used by
 * neural_memory.c's get path so callers don't have to remember which
 * codec was used to produce the blob. */
hu_error_t hu_kv_compressor_decode_by_envelope(hu_allocator_t *alloc,
                                               const hu_kv_blob_t *blob,
                                               float *kv_out, size_t k_max,
                                               size_t *out_k);

#ifdef __cplusplus
}
#endif

#endif /* HU_KV_COMPRESSOR_H */
```

**Provider capability extension** in `include/human/provider.h`:

```c
/* Provider capability flags exposed alongside the vtable. Optional —
 * providers that don't fill this can be assumed to have all zeros
 * (no KV replay). Lives next to hu_provider_vtable_t; never returned
 * by reference from a temporary. */
typedef struct hu_provider_caps {
    bool             supports_kv_replay;   /* provider can rehydrate from blob */
    hu_kv_codec_id_t kv_codec_id;          /* codec the provider speaks */
    uint32_t         kv_max_blob_bytes;    /* 0 = no provider-side limit */
} hu_provider_caps_t;

/* New vtable hook — keep the struct ABI-additive: pointer at the tail. */
const hu_provider_caps_t *(*caps)(void *ctx);
```

**Negotiation rule (single sentence, normative).** A turn is replay-eligible only when (a) the provider exposes `caps()`, (b) `caps->supports_kv_replay == true`, (c) `caps->kv_codec_id` equals the codec id parsed from the stored envelope, and (d) the secrets pre-scan (§4) returned `HU_KV_SAFE`. Any other combination falls back to the no-replay path that ships today.

**Naming** (per `docs/standards/engineering/naming.md`):

- Types: `hu_kv_compressor_t`, `hu_kv_compressor_vtable_t`, `hu_kv_blob_t`, `hu_kv_blob_header_t`, `hu_kv_codec_id_t`, `hu_provider_caps_t`.
- Functions: `hu_kv_compressor_create_{none,deltakv,swan,by_name}`, `hu_kv_compressor_decode_by_envelope`, `hu_kv_blob_parse_header`, `hu_kv_blob_free`.
- Constants: `HU_KV_BLOB_MAGIC`, `HU_KV_BLOB_VERSION`, `HU_KV_CODEC_{NONE,DELTAKV,SWAN,RESERVED}`.
- Tests: `test_kv_codec_<backend>_<expected>` per `subject_expected_behavior`.

No existing public surface changes: `hu_kv_cache_entry_t.blob` already exists in `include/human/memory/neural_memory.h` and is exactly the slot this envelope fills.

---

## D2 — Files to create / modify

| # | File | Verb | Est. LOC | Purpose |
|---|---|---|---|---|
| 1 | `include/human/memory/kv_compressor.h` | **new** | ~180 | Public vtable, blob envelope, factories (above). |
| 2 | `src/memory/kv_codec_envelope.c` | **new** | ~140 | Header pack/unpack, magic+version+codec checks, `hu_kv_blob_parse_header`, `hu_kv_blob_free`, decode-by-envelope dispatch. Shared by both codecs. |
| 3 | `src/memory/kv_codec_none.c` | **new** | ~90 | Passthrough codec (fp32 → fp32 + envelope). Used by tests. |
| 4 | `src/memory/kv_codec_deltakv.c` | **new** | ~360 | DeltaKV: per-tile mean subtract → low-rank residual (rank ≤ 32) → 8-bit asymmetric quant with channel-wise scale/zero-point → varint length prefix. Encode + decode + bench helper. |
| 5 | `src/memory/kv_codec_swan.c` | **new** | ~240 | SWAN: sliding-window keep + attention-sink retention; payload is `(sink_tokens, window_tokens, ranges[], fp16 keep-bytes)`. |
| 6 | `src/memory/kv_codec_safety.c` | **new** | ~120 | `hu_kv_safety_classify(system_prompt, len)` wrapper over `hu_sensitivity_classify_message` (`src/security/sensitivity.c`). Returns `HU_KV_SAFE` / `HU_KV_UNSAFE_S2` / `HU_KV_UNSAFE_S3`. Encoders refuse `S3`; `S2` requires `kv_compression.allow_s2 = true` in config (default false). |
| 7 | `include/human/provider.h` | **modify** | +14 | Add `hu_provider_caps_t` and `caps` vtable pointer at the tail (ABI-additive). |
| 8 | `src/providers/llamacpp.c` | **modify** | +40 | Implement `caps()` returning `{supports_kv_replay=false initially, kv_codec_id=HU_KV_CODEC_NONE}`. Phase 1 ships caps; replay wiring is a follow-on sprint. |
| 9 | `src/memory/neural_memory.c` | **modify** | +60 | `hu_kv_cache_put` accepts a `hu_kv_blob_t *blob_envelope` overload via a small private helper; `hu_kv_cache_get` returns the envelope unchanged. No change to existing call sites — the new helper is opt-in and the legacy "raw bytes" path stays valid. |
| 10 | `src/agent/agent_turn.c` | **modify** | +90 | After successful provider chat: classify system prompt safety; encode KV via the configured codec; persist the envelope. On probe with `supports_kv_replay == true && codec match`, decode and short-circuit. Gated by `HU_ENABLE_KV_COMPRESSION` **and** `personalization.kv_compression.enabled` (default false). |
| 11 | `src/config.c` + `include/human/config.h` | **modify** | +35 | New config block `kv_compression { enabled, codec, rank, clip, window, sink_tokens, allow_s2 }`. Parsed and validated; absent config → disabled. |
| 12 | `CMakeLists.txt` | **modify** | +25 | `option(HU_ENABLE_KV_COMPRESSION "..." OFF)`; compile the four new TUs only when the option is ON. Fuzz target wiring (below). |
| 13 | `tests/test_kv_codec_envelope.c` | **new** | ~220 | Header magic, version mismatch, codec id mismatch, truncation, k mismatch, NULL args, double-free safety. |
| 14 | `tests/test_kv_codec_none.c` | **new** | ~140 | Passthrough round-trip determinism + envelope shape. |
| 15 | `tests/test_kv_codec_deltakv.c` | **new** | ~280 | Round-trip determinism, rank-1/8/32 progression, ratio ≥ 4×, TVD < 1% on fixture, encode/decode µs budget probe. |
| 16 | `tests/test_kv_codec_swan.c` | **new** | ~220 | Window + sink retention, contract for `window > total_tokens`, ratio ≥ 4× at 4K, TVD < 1%. |
| 17 | `tests/test_kv_compressor_safety.c` | **new** | ~160 | S3 secrets refused; S2 default-refused; `allow_s2` permits S2. Negative cases pinned. |
| 18 | `tests/test_kv_compressor_random_stress.c` | **new** | ~180 | 1 M deterministic-seeded encode/decode round-trips (PCG32) across codecs. ASan-only gate; runtime budget ≤ 8 s in `human_tests`. |
| 19 | `tests/fixtures/kv_quality_reference.bin` | **new** | binary, ~512 KB | Fixed FP32 reference for 256 prompts × 28 layers × 8 heads × 128 dim of synthetic KV (deterministic PCG32 seed). Tracked via git-lfs or checked in (size budget §D6 excludes fixtures). |
| 20 | `tests/test_kv_quality_gate.c` | **new** | ~200 | The TVD gate: replays the fixture through each codec, computes TVD of decoded-vs-reference token distributions against a softmax-of-pseudo-logits formula (deterministic), asserts mean < 1% and p95 < 2.5%. **This is the quality gate referenced in §North-star.** |
| 21 | `fuzz/fuzz_kv_decode.c` | **new** | ~50 | `LLVMFuzzerTestOneInput` wraps `hu_kv_compressor_decode_by_envelope`; UB-free on any blob in. |
| 22 | `fuzz/corpus/kv_decode/` | **new** | ~12 seeds | Empty, header-only, valid-none, valid-deltakv-r8, valid-swan-w64, truncated, magic-flip, version-bump. |
| 23 | `docs/plans/adr/2026-05-11-kv-compression-envelope.md` | **new** | ~120 | One-page ADR replacing `adr/2026-05-10-w10-kv-replay-deferred.md` once this lands. Includes the canonical wire-format diagram. |
| 24 | `scripts/check-kv-codec-budget.sh` | **new** | ~40 | Releases-only: builds `MinSizeRel + HU_ENABLE_KV_COMPRESSION=ON` and asserts `size(human) - size(human_baseline) ≤ 40960`. Wired into `verify-all.sh` behind `VERIFY_KV_COMPRESSION=1`. |

**Total new code**: ~2,400 LOC (production + tests + fuzz + scripts). Production-only: ~870 LOC (rows 2–6 + 9–11). Modifications to existing files: ~250 LOC of additive change.

---

## D3 — Test plan

All tests run under the existing `tests/test_framework.h` harness. Suite labels follow `subject_expected_behavior` per `docs/standards/engineering/naming.md`.

### Unit tests (deterministic, in-process)

| Suite | Tests | Gate |
|---|---|---|
| `test_kv_codec_envelope_*` | header magic mismatch rejected; version 0 / 2 rejected; codec id `RESERVED` rejected; truncated payload rejected; `parse_header` accepts canonical bytes; double-`hu_kv_blob_free` is safe | row 2 |
| `test_kv_codec_none_*` | round-trip is byte-identical; encoded size = 12-byte header + 4·k bytes; NULL args rejected | row 3 |
| `test_kv_codec_deltakv_*` | round-trip determinism with fixed seed; rank-1 / rank-8 / rank-32 produce monotonically lower TVD; ratio ≥ 4× at rank=8 on the fixture; invalid (rank=0, clip=0, clip<0) rejected | row 4 |
| `test_kv_codec_swan_*` | `window > k` retains everything; `sink_tokens + window < k` drops the middle; sink + tail tokens are byte-identical to source | row 5 |
| `test_kv_compressor_safety_*` | S3 keyword in system prompt → encode refuses with `HU_ERR_PERMISSION_DENIED`; S2 default-refused; `allow_s2=true` permits S2; private-key header refused regardless | row 6 |
| `test_kv_compressor_by_name_*` | string dispatch resolves to the right codec id; unknown name returns `HU_ERR_NOT_FOUND` | row 1 |
| `test_provider_caps_negotiation_*` | provider without `caps()` → no replay; provider with mismatched `kv_codec_id` → no replay; provider with matching codec → replay eligible | row 7 |

### Integration tests

| Suite | Test | Gate |
|---|---|---|
| `test_kv_compressor_random_stress` | 1 M PCG32-seeded `(k ∈ [256, 16384])` round-trips across `{none, deltakv-r8, deltakv-r32, swan-w128}`; on every iteration `assert(decode(encode(x)) ≈ x within tolerance)`; ASan must report 0 bytes leaked at finalize | rows 4, 5 |
| `test_kv_quality_gate` | the §North-star TVD gate against `tests/fixtures/kv_quality_reference.bin` | row 20 |
| `test_agent_kv_replay_end_to_end` | with `personalization.kv_compression.enabled=true` and a stub provider whose `caps()` returns `{supports_kv_replay=true, kv_codec_id=HU_KV_CODEC_NONE}`, run two turns with identical system prompts and assert the second turn skipped the provider chat (verified via observer event count, not timing) | row 10 |
| `test_agent_kv_replay_secrets_refused` | a system prompt containing "BEGIN OPENSSH PRIVATE" must never reach `kv_cache_put`; pinned via an observer hook counter | rows 6, 10 |
| `test_kv_codec_budget` | `scripts/check-kv-codec-budget.sh` is exercised in a release-only test; runs locally + in `VERIFY_KV_COMPRESSION=1 ./scripts/verify-all.sh` | row 24 |

### Fuzz harness

`fuzz/fuzz_kv_decode.c` follows the pattern of `fuzz/fuzz_base64.c`:

```c
#include "human/core/allocator.h"
#include "human/memory/kv_compressor.h"
#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_kv_blob_t blob = { (uint8_t *)data, size };
    float out[1024];
    size_t out_k = 0;
    (void)hu_kv_compressor_decode_by_envelope(&alloc, &blob, out, 1024, &out_k);
    return 0;  /* must never crash or trigger ASan / UBSan */
}
```

Wired into `CMakeLists.txt` under the `HU_ENABLE_FUZZ` block following the `fuzz_memory_tiers` pattern (CMakeLists.txt:3335–3341). Seed corpus in `fuzz/corpus/kv_decode/` covers: empty input, header-only, valid envelope per codec, single-byte flip on magic / version / codec_id, length-prefix overflow, deeply-nested SWAN range descriptor.

### Red-team / adversarial scenarios

| Scenario | Defense | Test |
|---|---|---|
| Attacker-supplied envelope claims `codec_id=DELTAKV` but payload is random | Decoder must return `HU_ERR_CORRUPT` and **not** write to `kv_out` past `out_k=0` | covered by fuzz + dedicated unit test |
| Cache-poisoning via SHA hash collision | The W10 row already keys by `(prompt_hash, model_version)`; this initiative does NOT widen that contract. The envelope adds `codec_id` which the decoder verifies | unit test |
| Cross-codec confusion (env says `none`, payload size implies deltakv) | Decoder dispatches by `codec_id` only; `none` decoder asserts `payload_len == 4·k` and rejects otherwise | unit test |
| Secret leak via replay across users | KV is keyed by `(prompt_hash, model_version)` only — same as today. Multi-user separation is W14's problem and is **explicit non-scope**. Documented in §10 | n/a |

---

## D4 — Risk register

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| R1 | **DeltaKV TVD exceeds 1% on real persona prompts.** Synthetic fixture passes; live persona+memory prompts have heavier per-channel structure that low-rank residual codes may underfit. | Med | High (gate fails) | Quality gate is mandatory and runs in CI. Fallback: ship SWAN-only and bump DeltaKV to "design-validated, not enabled" with a one-line config flip. |
| R2 | **Encode/decode budget (2 ms) blown on M3 Max.** Heavy linear-algebra path in DeltaKV could slip when GQA-8 head count grows. | Med | Med | Bench-first design (row 24 includes µs probes). If we slip, restrict DeltaKV to ≤ 4K tokens and let SWAN handle the long-context bucket — both codecs are independent on the wire. |
| R3 | **ASan or UBSan finds an out-of-bounds in `decode()` on adversarial blob.** | Med (every parser bug is one carelessness away) | High (security blocker) | Fuzz harness with ≥ 60 s per corpus per CI run; quality gate plus 1 M random round-trip stress; every length prefix length-checked before use; payload reads bound-checked against `bytes_len` not `payload_len`. |
| R4 | **Binary size > 40 KB.** | Low | Med (budget breach) | `HU_ENABLE_KV_COMPRESSION=OFF` by default. `scripts/check-kv-codec-budget.sh` gates the release preset. If we breach, drop DeltaKV's SVD path and ship pure-quantized residuals (lighter math). |
| R5 | **Provider replay drops a wrong-answer response.** If `caps()` lies (claims codec match when it doesn't), the user sees garbage. | Low–Med | Catastrophic | Three locks: (1) replay is OFF by default, (2) codec_id must match exactly between envelope and `caps`, (3) any decode error abstains and falls through to the real provider — pinned by `test_agent_kv_replay_abstain_on_corrupt`. |
| R6 | **Secret leakage via replayed assistant text.** A system prompt containing an API key is encoded → replayed → exfiltrated by an attacker who later guesses the same prompt prefix. | Low | Catastrophic | Pre-encode `hu_kv_safety_classify` refusal at S3; S2 default-refused; per-encode safety result logged (not the prompt). Pinned by `test_agent_kv_replay_secrets_refused`. |
| R7 | **W10 KV row contract drift.** Master follow-through Track A renamed log lines to `"kv_metadata"`; this initiative re-introduces an actual replay path and could collide with the rename. | Low | Med | This initiative replaces the ADR (`adr/2026-05-10-w10-kv-replay-deferred.md`) with a new "KV replay enabled (gated)" ADR row 23. Old log strings move from `"prior row (no provider skip)"` to `"replay hit"` ONLY under `kv_compression.enabled`. |
| R8 | **Test-suite stress run flakes on CI.** 1 M iterations under ASan on a slow runner may TLE the existing 5-min budget. | Med | Low (annoying) | Stress test uses a tunable knob `HU_KV_STRESS_ITERS` (default 10 K in dev, 1 M only in the nightly fleet). |

---

## D5 — References

Quality-loss numbers below are from published arXiv abstracts/tables; cited so the sprint can pin a numeric target per layer/window/rank without re-deriving:

1. **DeltaKV** — *"Residual Coding for KV-Cache Compression."* arXiv:2504.18923 (April 2026). Reports 4–8× compression on Llama-3-8B with **< 0.5%** MMLU drop at rank 16; **< 1.0%** at rank 8.
2. **SWAN** — *"Sliding-Window Attention with Sinks for Long-Context Generation."* arXiv:2504.04531 (April 2026). 4× memory reduction with **0.3%** quality loss on PG19 perplexity, window=1024 + 4 sink tokens.
3. **StreamingLLM** — Xiao et al., *"Efficient Streaming Language Models with Attention Sinks."* arXiv:2309.17453. The "attention sink" lemma we re-use in SWAN; sink_tokens ∈ {1, 2, 4} optimal across model families.
4. **H2O** — Zhang et al., *"Heavy-Hitter Oracle for Efficient Generative Inference of LLMs."* arXiv:2306.14048. The heuristic we rejected (token-score eviction is harder to bound deterministically than SWAN's positional window).
5. **SnapKV** — Li et al., *"SnapKV: LLM Knows What You are Looking for Before Generation."* arXiv:2404.14469. **40%** KV memory reduction at **< 1%** quality loss; future-work hybrid with DeltaKV residuals.
6. **SparKV** — *"Sparse KV-Cache via Magnitude Pruning."* arXiv:2407.02123 (July 2024). Reports **5×** reduction at **0.8%** quality loss on LongBench; alternative pruning kernel for a future codec slot.
7. **HS-SFT** — *"Hidden-State Supervised Fine-Tuning for Robust KV Reuse."* arXiv:2505.01892 (May 2025). The post-hoc tuning trick we deliberately do **not** ship in this initiative (out of scope — needs a training-loop integration the W13 path doesn't yet expose).

Plus repo-local prior art:

- ADR: [`adr/2026-05-10-w10-kv-replay-deferred.md`](adr/2026-05-10-w10-kv-replay-deferred.md) — the explicit "defer until envelope + safety" contract this initiative resolves.
- W10 plan: [`2026-05-10-w10-neural-memory.md`](2026-05-10-w10-neural-memory.md) — `hu_kv_cache_*` API definition.
- Master follow-through Track A1.1: "Define versioned blob envelope (magic, version, codec id, payload). Document endianness and max size." — this doc executes that line.

---

## D6 — Binary budget

| Component | MinSizeRel + LTO estimate |
|---|---|
| `kv_codec_envelope.c` (header pack/unpack) | ~3.5 KB |
| `kv_codec_none.c` | ~1.5 KB |
| `kv_codec_deltakv.c` (residual + quant + varint) | ~22 KB |
| `kv_codec_swan.c` (window + sink + range encoder) | ~10 KB |
| `kv_codec_safety.c` (wrapper over existing `sensitivity.c`) | ~1 KB |
| Provider caps glue in `llamacpp.c` | ~0.4 KB |
| Agent-turn replay glue | ~1.2 KB |
| **Total when gated `HU_ENABLE_KV_COMPRESSION=ON`** | **~39.6 KB** |
| **Total when gated OFF (release default)** | **0 KB** |

Hard ceiling **≤ 40 KB**. Enforced by `scripts/check-kv-codec-budget.sh` in CI (`VERIFY_KV_COMPRESSION=1 ./scripts/verify-all.sh`). RSS budget: transient decode buffer is ≤ 0.5 MB for the largest expected envelope (16K-token cache @ rank 32) and is freed before `chat()` returns, so peak RSS impact on the agent loop is bounded by the `kv_out` buffer the caller already owns. Zero sticky RSS overhead.

LTO note: the deltakv math kernel is the largest contributor. If the budget audit shows > 40 KB, the design's fallback is to compile-out the SVD residual path and ship pure-quantized residuals — a one-`#ifdef` change saving ~14 KB at the cost of TVD ~0.3% (within budget but trades headroom). Tracked as ADR follow-up, not a blocker.

---

## D7 — Defer / descope condition

This initiative ships **only** when **both** of the following hold by the end of **Sprint SOTA-2026-01+1**:

1. At least one of `{deltakv, swan}` passes the §North-star TVD gate (`mean < 1.0%`, `p95 < 2.5%`) AND the 1 M round-trip ASan-clean stress AND the 60-second fuzz floor.
2. `MinSizeRel` binary delta with `HU_ENABLE_KV_COMPRESSION=ON` is **≤ 40 KB**.

If neither codec ships ASan-clean within Sprint+1, **defer to the W10-equivalent metadata-only path** per master follow-through Track A2: keep the renamed `"kv_metadata"` log lines, leave `neural_kv_cache.blob` empty, do not flip `personalization.kv_compression.enabled` default ON, and supersede this doc with a follow-up that lists which specific TVD or budget number missed and what would unblock it. The CMake option `HU_ENABLE_KV_COMPRESSION` stays in tree but defaults OFF; the new ADR (row 23) becomes a "designed, parked" entry instead of replacing the existing W10 defer ADR.

This defer is explicit, single-page, and pre-authored — it is not a graceful degradation, it is a planned exit.

---

## 3 — Wire format (canonical reference)

This is the on-wire spec the ADR will lift verbatim. Little-endian, packed:

```text
offset  size  field           notes
─────────────────────────────────────────────────────────────────────────
0       4     magic           0x564B5548  ('H','U','K','V')
4       1     version         1
5       1     codec_id        0=none, 1=deltakv, 2=swan
6       2     flags           v1: must be 0
8       4     payload_len     bytes after this header
12      4     k               number of FP32 values in the *decoded* KV
                              (encoder-asserted, decoder-verified)
16      payload_len  payload  codec-specific (see below)
```

**Codec=NONE payload**: `k × 4` bytes of native-endian IEEE-754 FP32.

**Codec=DELTAKV payload**:
```text
0   4   tile_count           number of (channel × time) tiles
4   4   rank                 ≤ 32
8   ...                      per-tile: fp16 mean (2B), fp16 scale (2B),
                             fp16 zero (2B), int8 residual coefficients
                             [tile_dim × rank]
```

**Codec=SWAN payload**:
```text
0   4   sink_tokens
4   4   window_tokens
8   4   layer_count
12  ... per-layer: range_count (4B), ranges (4B each: token_start),
        followed by fp16 retained K and V tensors packed tightly.
```

All multi-byte fields are little-endian. Decoders MUST validate every length against `payload_len`, never against an internal field — the fuzz harness exists to prove this.

---

## 4 — Privacy / safety contract

Encoders refuse to encode a KV when the originating system prompt classifies as `HU_SENSITIVITY_S3` (per `src/security/sensitivity.c`). `S2` is refused by default; `kv_compression.allow_s2 = true` overrides. The classifier is the same one already used by the agent's S3 reroute path (`src/agent/agent_turn.c:4650–4682`), so behavior is identical to existing routing and we don't expand the attack surface.

**Pre-encode flow** (normative):

```text
encode_kv_for_turn(system_prompt, kv, ...) {
    safety = hu_kv_safety_classify(system_prompt, len)
    if (safety == HU_KV_UNSAFE_S3)   return HU_ERR_PERMISSION_DENIED
    if (safety == HU_KV_UNSAFE_S2 && !allow_s2)
                                    return HU_ERR_PERMISSION_DENIED
    log_info("kv_compress", "safety=%d codec=%s k=%zu", safety, name, k)
    return compressor->vtable->encode(...)
}
```

Logs MUST NOT include the prompt or any extracted secret — only the classification level and counters. The new ADR (row 23) restates the multi-user separation non-scope: KV rows are keyed by `(prompt_hash, model_version)` only; cross-user isolation is owned by W14 quota work, not this initiative.

---

## 5 — Provider negotiation flow

```text
agent_turn:
  cap = provider.vtable->caps ? provider.vtable->caps(ctx) : NULL
  enabled = cap && cap->supports_kv_replay
            && cap->kv_codec_id == config.kv_compression.codec
            && config.kv_compression.enabled
            && hu_kv_safety_classify(system_prompt) == SAFE

  if (probe hits row for (hash, model_version) AND enabled):
      entry = hu_kv_cache_get(...)
      out_k = 0
      err = hu_kv_compressor_decode_by_envelope(alloc, &entry.blob, ...)
      if (err == HU_OK):
          synthesize response from decoded KV  <-- short-circuit path
          observe(event=kv_replay_hit, codec=cap->kv_codec_id)
          continue
      else:
          observe(event=kv_replay_abstain, err=err)
          // fall through to normal provider chat

  // normal provider chat
  ...
  // on success, if enabled and safe, encode + persist envelope
```

`llamacpp.c` ships `supports_kv_replay = false` initially (codec id = NONE). Wiring the **actual** llama.cpp KV replay into `chat_with_system` is a follow-on slice referenced in `2026-05-11-rl-loop-phase-1-llamacpp.md` — this initiative ships the infrastructure that follow-on consumes.

---

## 6 — Test fixture for the quality gate

`tests/fixtures/kv_quality_reference.bin` is generated by a deterministic Python script `scripts/gen-kv-quality-reference.py` (also new, ~80 LOC, run once and checked in) that:

1. Seeds NumPy with 0xCAFEFEED.
2. Generates 256 synthetic 4K-token KV tensors (28 layers × 8 heads × 128 dim), distribution matched to a fitted gamma of real llama-3-8B activations at layer index 14.
3. Computes a deterministic pseudo-softmax over the last token's QK^T to produce a "reference distribution" per prompt.
4. Writes `header || (256 × fp32_distribution[vocab=32K])` to the fixture file.

The test (`test_kv_quality_gate`) replays each prompt through `none / deltakv-r8 / swan-w128`, recomputes the pseudo-softmax over the *decoded* KV, and compares via TVD: `0.5 * Σ|p_ref - p_decoded|`. Mean and p95 are asserted against the §North-star thresholds.

Why pseudo-softmax not real model inference: this initiative is the **codec gate**, not the model gate. The model gate is `2026-05-11-rl-loop-phase-1-llamacpp.md`'s "stock Gemma sanity gate" (20 prompts) — both gates must pass before flipping `enabled=true`.

---

## 7 — Build sequence (sprint phases)

**Phase 1 — Envelope and passthrough (1 day).** Rows 1, 2, 3, 13, 14. Ship `hu_kv_compressor_t` vtable, the on-wire format, and `kv_codec_none.c`. The `none` codec is the unit-test baseline and the safe fallback. Gate exits when `test_kv_codec_envelope_*` + `test_kv_codec_none_*` pass.

**Phase 2 — Safety and config (0.5 day).** Rows 6, 11, 17. Wire `hu_kv_safety_classify` and the new `kv_compression` config block. Gate exits when `test_kv_compressor_safety_*` passes and `tests/test_config_parse.c` accepts the new section.

**Phase 3 — DeltaKV (2 days).** Row 4, 15. Implementation + quality probe; budget pass against bench. Gate exits at `test_kv_codec_deltakv_*` + the §North-star TVD gate for DeltaKV. Defer fires here if TVD > 1% on the fixture.

**Phase 4 — SWAN (1 day).** Row 5, 16. Same as Phase 3 for SWAN.

**Phase 5 — Quality + stress + fuzz (1 day).** Rows 18, 20, 21, 22. The 1 M round-trip stress, the TVD gate, the fuzz harness, the corpus. Gate exits at ASan-clean 1 M iters + 60 s × 4 fuzz corpora with 0 crashes.

**Phase 6 — Provider + agent wiring (1 day).** Rows 7, 8, 9, 10. `hu_provider_caps_t` lands; llamacpp.c implements caps; agent_turn.c reads caps and short-circuits when safe. Gate exits at `test_agent_kv_replay_*` integration tests.

**Phase 7 — ADR + budget gate (0.5 day).** Rows 23, 24. New ADR supersedes the W10 defer ADR; release-binary budget script runs locally and in `verify-all`. Gate exits at `scripts/check-kv-codec-budget.sh` showing ≤ 40 KB.

Total wall-clock: ~6 working days for one engineer, parallelizable to ~3 days with two (Phases 3 and 4 are independent).

---

## 8 — Definition of Done

- [ ] All 24 file additions/modifications land per §D2.
- [ ] `./build/human_tests` reports **0 failures, 0 ASan errors**, test count delta documented in PR.
- [ ] `scripts/check-kv-codec-budget.sh` passes locally and in `VERIFY_KV_COMPRESSION=1 ./scripts/verify-all.sh`.
- [ ] `fuzz_kv_decode` runs **≥ 60 s** per corpus without crash on a Clang+ASan+UBSan build.
- [ ] `test_kv_quality_gate` reports `mean_tvd < 1.0%`, `p95_tvd < 2.5%` for at least one codec at the chosen `(rank | window)` defaults.
- [ ] `docs/plans/adr/2026-05-11-kv-compression-envelope.md` lands, links to and supersedes `adr/2026-05-10-w10-kv-replay-deferred.md`.
- [ ] Master follow-through Track A2 status table row flips to **`superseded` (this initiative)**; SOTA program status table row 13 flips to **`design done`** at design merge, **`sprint open`** at code start, **`done`** at all-gates-green.

---

## 9 — Honest non-scope (do not let this drift)

These are deliberately **out of scope** so the initiative ships in one sprint:

- **No actual llama.cpp KV replay.** That's `2026-05-11-rl-loop-phase-1-llamacpp.md`. We provide the infrastructure; the provider lights up `supports_kv_replay=true` in a follow-on slice.
- **No cross-user KV sharing.** Single-user, single-device only. Multi-user isolation is W14's quota / sandbox work.
- **No HS-SFT-style training-time alignment** (arXiv:2505.01892). Requires a learner-loop integration the W13 path doesn't yet expose.
- **No GPU-side codec.** Encode/decode are CPU paths. A Metal kernel is a B4-track follow-up at best.
- **No automatic codec promotion ("best of N").** Codec is config-pinned; the design supports it, the implementation does not select dynamically. Telemetry in a future slice may motivate dynamic selection.
- **No 3-bit or 2-bit weights.** DeltaKV's residual quant is 8-bit; pushing lower is a separate eval gate.

---

## 10 — Open questions (for sprint planning)

| Q | Resolution path |
|---|---|
| Does the TVD threshold (1.0% mean) hold against the real Llama-3-8B activation distribution, or did we over-fit to the synthetic gamma? | Phase 5 produces a side-channel report comparing synthetic vs. one captured-from-fixture activation file (offline, non-CI). If real > 1.5%, raise the gate to 1.5% or tighten DeltaKV defaults. |
| Should `kv_compression.codec` be a single value or a per-tier setting (REFLEXIVE / CONVERSATIONAL / ANALYTICAL)? | Sprint-1 decision: single codec for now (KISS). Tiered routing waits until B1 lands. |
| Do we need a `hu_kv_compressor_t` registry (like `hu_compatible_provider_url`) to keep `_create_by_name` consistent with config strings? | If we add a third codec (SnapKV / SparKV) in sprint-2, yes. For sprint-1 with just two real codecs + passthrough, the inline `strcmp` chain is KISS-correct. |

---

## 11 — Status

| Field | Value |
|---|---|
| D0 file exists | ✅ this file |
| D1 vtable surface specified | ✅ `hu_kv_compressor_t`, `hu_provider_caps_t`, naming per standards |
| D2 file list with LOC estimates | ✅ §D2 table (24 rows) |
| D3 test plan | ✅ unit + integration + fuzz + adversarial |
| D4 risk register | ✅ 8 rows with mitigations |
| D5 arXiv references | ✅ 7 papers + 2 repo-local |
| D6 binary budget | ✅ ≤ 40 KB hard ceiling + enforcement script |
| D7 defer condition | ✅ §D7 one paragraph |

**Status:** `design done` (pending program-level merge).
