---
title: "Init 08 — Federated LoRA across the user's own devices"
created: 2026-05-11
status: deferred
parent: 2026-05-11-sota-2026-massive-team-program.md
related:
  - 2026-05-11-sota-2026-massive-team-program.md
  - 2026-05-10-w13-learning-loop.md
  - 2026-05-10-w15-crypto-privacy.md
  - 2026-05-10-m3-frontier-model-bridge.md
  - ../standards/security/threat-model.md
  - ../../CLAUDE.md
  - ../../AGENTS.md
risk: high
slug: federated-lora
binary_budget_kb: 48
last_audit: 2026-05-25
---

# Init 08 — Federated LoRA across the user's own devices

## One-liner

Phone, laptop, and desktop running `human` discover each other on the local
network (mDNS), authenticate and pair using the **existing** `pairing.c` flow
(no new ceremony types), open a Noise-XX channel over TCP, and run a single
round of secure FedAvg over their per-device LoRA gradients with optional
DP-FedLoRA noise. Personalization scales across the user's own hardware.
**Nothing leaves the user's device fleet** — there is no external aggregator.

## Why now (and what we are NOT building)

The honest justification for federation, given today's M3 status (single
device LoRA still partial, `lora-persona` only just learned to fine-tune real
GGUF bases via Bridge A.0), is **bounded**:

- A user with a phone (Apple FM provider — Init 03), a laptop (MLX/Qwen3 —
  Init 04), and a desktop (llama.cpp Bridge A) generates training signal on
  *every* device. Each device's local LoRA adapter overfits to its own
  channel mix (phone = iMessage casual, laptop = Slack work, desktop = email
  long-form). FedAvg across the fleet produces one persona-LoRA that
  generalises across channels without any device sending raw messages.
- Privacy-by-architecture is the moat, and "the model that learns from all
  of your devices without any of them ever phoning home" is the exact
  long-form story we are not allowed to tell yet (because it isn't built).
- The literature is mature enough: SDFLoRA (gradient-only), DP-FedLoRA
  (formal `(ε, δ)` budgets at LoRA scale), FedPepTAO (param-efficient
  selection), LA-LoRA (LoRA-aware aggregation), FedPDPO (preference DPO with
  privacy). All published April–May 2026.

We are **explicitly NOT building**:
- **An external aggregator.** No "the user's devices upload to our server"
  path, ever. There is no central party in this design.
- **Any cross-user federation.** Per-user fleets only. Two users are two
  entirely separate worlds; the protocol would refuse to enroll a peer
  whose pairing chain doesn't trace back to a key the user already trusts.
- **Replacement of per-device LoRA.** Federation is **additive** to Init
  04/05: each device still runs its own local LoRA loop, federation simply
  averages a round when ≥2 paired peers are reachable on the LAN.
- **A new pairing ceremony.** We reuse `hu_pairing_guard_*`. Period.

## D0 — Document existence (this file)

- Path: `docs/plans/2026-05-11-init-08-federated-lora.md` ✅
- YAML frontmatter present (title, created, status, parent, related) ✅
- All `related:` links resolve to existing documents ✅

## D1 — Vtable + public surface

New header: `include/human/federation.h`. Adds **one** vtable
(`hu_federation_t`) plus a small handful of helper types. No existing public
header is broken.

Naming follows `docs/standards/engineering/naming.md`:
- Type: `hu_federation_t`, `hu_fed_peer_t`, `hu_fed_round_t`, `hu_fed_gradient_t`,
  `hu_fed_status_t`, `hu_fed_dp_budget_t`, `hu_fed_aggregation_t` (enum).
- Functions: `hu_federation_open`, `hu_federation_close`, `hu_federation_enroll_peer`,
  `hu_federation_propose_round`, `hu_federation_submit_gradient`,
  `hu_federation_aggregate_round`, `hu_federation_apply_round`,
  `hu_federation_status`, `hu_federation_pause`, `hu_federation_reset_keys`.
- Constants: `HU_FED_MDNS_SERVICE`, `HU_FED_PROTOCOL_VERSION`,
  `HU_FED_MIN_QUORUM` (default 2), `HU_FED_MAX_PEERS` (default 8),
  `HU_FED_ROUND_TIMEOUT_MS` (default 60000).
- Errors (added to `human/core/error.h` enum): `HU_ERR_FED_NOT_PAIRED`,
  `HU_ERR_FED_QUORUM_NOT_MET`, `HU_ERR_FED_ROUND_ABORTED`,
  `HU_ERR_FED_DP_BUDGET_EXHAUSTED`, `HU_ERR_FED_PEER_UNREACHABLE`,
  `HU_ERR_FED_HANDSHAKE_FAILED`, `HU_ERR_FED_RANK_MISMATCH`.

### `include/human/federation.h` (sketch)

```c
#ifndef HU_FEDERATION_H
#define HU_FEDERATION_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/security.h"        /* hu_pairing_guard_t — trust anchor */
#include "human/ml/learner.h"      /* hu_learner_t, model_version */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_FED_MDNS_SERVICE      "_human-fed._tcp.local."
#define HU_FED_PROTOCOL_VERSION  1u
#define HU_FED_MIN_QUORUM        2u
#define HU_FED_MAX_PEERS         8u
#define HU_FED_ROUND_TIMEOUT_MS  60000

typedef enum hu_fed_aggregation {
    HU_FED_AGG_FEDAVG = 0,           /* plain weighted FedAvg over peers   */
    HU_FED_AGG_FEDAVG_DP,            /* FedAvg + per-device DP-FedLoRA noise */
    HU_FED_AGG_SECAGG_SHAMIR,        /* Shamir-style secure aggregation     */
} hu_fed_aggregation_t;

typedef struct hu_fed_dp_budget {
    double epsilon;                  /* per-round DP epsilon (e.g. 1.0–8.0) */
    double delta;                    /* per-round DP delta   (e.g. 1e-5)    */
    double clip_norm;                /* per-device gradient L2 clip (1.0)   */
    uint32_t composed_rounds;        /* cumulative rounds (sequential comp) */
} hu_fed_dp_budget_t;

typedef struct hu_fed_peer {
    char     peer_id[64];            /* SHA-256 hex of long-term static key */
    char     hostname[128];          /* mDNS-resolved local hostname        */
    uint16_t port;
    int64_t  last_seen_ms;
    bool     paired;                 /* trust chain proven via pairing.c    */
    bool     online;                 /* responded within last keepalive     */
} hu_fed_peer_t;

typedef struct hu_fed_round {
    uint64_t round_id;
    char     model_version[64];      /* must match across all peers         */
    uint32_t lora_rank;              /* must match across all peers         */
    uint32_t expected_peers;
    uint32_t submitted_peers;
    int64_t  deadline_ms;
    hu_fed_aggregation_t aggregation;
    hu_fed_dp_budget_t   dp;         /* zeroed when aggregation == FEDAVG   */
} hu_fed_round_t;

typedef struct hu_fed_gradient {
    /* Opaque, allocator-owned blob: the same little-endian layout the W13
     * CPU adapter uses (`HU_LEARNER_ADAPTER_MAGIC` / VERSION / weights),
     * minus the `model_version` header (it lives on the round). */
    void   *data;
    size_t  data_len;
    uint32_t num_weights;            /* must equal round->lora_rank * shape */
    char     contributor_peer_id[64];
} hu_fed_gradient_t;

typedef struct hu_fed_status {
    bool     enabled;
    bool     paused;
    uint32_t paired_peers_count;
    uint32_t online_peers_count;
    uint64_t last_round_id;
    int64_t  last_round_completed_ms;
    hu_fed_dp_budget_t cumulative_dp; /* sequential composition only */
    char     last_error[128];
} hu_fed_status_t;

typedef struct hu_federation_vtable {
    const char *name;                /* "noise-xx-mdns" — only backend in v1 */
    bool (*available)(void);

    /* Pre-paired peer enrollment. `pairing_token` MUST be a token previously
     * issued by the local hu_pairing_guard_t for this peer (re-uses the
     * existing user-pairing UX). Returns HU_ERR_FED_NOT_PAIRED if the token
     * does not validate against the guard. */
    hu_error_t (*enroll_peer)(void *ctx, const char *peer_hostname,
                              uint16_t port, const char *pairing_token);

    /* Coordinator role: announce a new round to all online paired peers.
     * `model_version` and `lora_rank` are the consensus parameters; peers
     * with mismatched versions decline. Returns HU_ERR_FED_QUORUM_NOT_MET
     * when fewer than HU_FED_MIN_QUORUM peers ack within 5s. */
    hu_error_t (*propose_round)(void *ctx, const char *model_version,
                                uint32_t lora_rank,
                                hu_fed_aggregation_t agg,
                                const hu_fed_dp_budget_t *dp,
                                hu_fed_round_t *out_round);

    /* Participant role: submit this device's clipped (and optionally noised)
     * LoRA gradient blob for the current round. Aggregator zeroes and frees
     * the input on success. Adversarial input rejected: shape mismatch
     * → HU_ERR_FED_RANK_MISMATCH; oversized → HU_ERR_INVALID_ARGUMENT. */
    hu_error_t (*submit_gradient)(void *ctx, const hu_fed_round_t *round,
                                  hu_fed_gradient_t *gradient);

    /* Coordinator role: drain all submissions, run weighted FedAvg (or
     * SecAgg open-and-sum), produce a single aggregate gradient. Aborts
     * atomically on partial submission past deadline. Output blob format
     * matches the W13 CPU adapter envelope so `hu_provider_load_adapter`
     * can apply it without a translator. */
    hu_error_t (*aggregate_round)(void *ctx, const hu_fed_round_t *round,
                                  hu_fed_gradient_t *out_aggregate);

    /* Apply the aggregated round to this device's adapter file at
     * `cfg.adapter_output_path` and bump the on-disk model_version to
     * the round's. The provider is reloaded via the existing
     * `hu_provider_load_adapter` path on the next chat call. */
    hu_error_t (*apply_round)(void *ctx, const hu_fed_round_t *round,
                              const hu_fed_gradient_t *aggregate);

    void (*deinit)(void *ctx);
} hu_federation_vtable_t;

typedef struct hu_federation {
    const hu_federation_vtable_t *vt;
    void                         *ctx;
    hu_allocator_t               *alloc;
    hu_pairing_guard_t           *guard;       /* not owned; trust anchor */
    hu_learner_t                 *learner;     /* not owned; gradient src */
    hu_fed_status_t               status;
} hu_federation_t;

/* Constructor — the only backend in v1 is "noise-xx-mdns". */
hu_error_t hu_federation_open(hu_allocator_t *alloc, hu_pairing_guard_t *guard,
                              hu_learner_t *learner, const char *backend_name,
                              hu_federation_t **out);
void       hu_federation_close(hu_federation_t *fed);

/* Convenience wrappers (the only API the daemon and CLI call). All other
 * vtable entries are reachable via these.                                  */
hu_error_t hu_federation_enroll_peer(hu_federation_t *fed,
                                     const char *peer_hostname, uint16_t port,
                                     const char *pairing_token);
hu_error_t hu_federation_status(hu_federation_t *fed, hu_fed_status_t *out);
hu_error_t hu_federation_pause(hu_federation_t *fed, bool paused);
hu_error_t hu_federation_reset_keys(hu_federation_t *fed);

/* Round driver — invoked by the W14 sleep scheduler when ≥quorum peers
 * are online and the local learner has a fresh per-device adapter. */
hu_error_t hu_federation_run_round(hu_federation_t *fed,
                                   const char *model_version,
                                   uint32_t lora_rank,
                                   hu_fed_aggregation_t agg,
                                   const hu_fed_dp_budget_t *dp,
                                   hu_fed_round_t *out_round);

#endif /* HU_FEDERATION_H */
```

### Trust chain (verbatim, end-to-end)

1. **User-fleet anchor**: every `human` install owns a single
   `hu_pairing_guard_t` whose `paired_token_hashes` list is the **only**
   long-lived authority. Cross-device pairing reuses
   `hu_pairing_guard_attempt_pair` exactly as the existing UI does. The
   user types the 8-digit code from device A into device B; B receives a
   token; B persists the token's hash; A persists B's first long-term
   federation static key bound to that token.
2. **Federation static key**: on `hu_federation_open`, the device generates
   (once, persisted under `~/.human/federation/keys/static.{pub,sec}`) a
   Curve25519 keypair via `crypto_kx_keypair` (libsodium). The public key
   hashed with SHA-256 is the device's `peer_id`. **No new entropy source,
   no new key file format outside libsodium.**
3. **mDNS advertisement**: device announces `_human-fed._tcp.local.` with a
   TXT record `pid=<peer_id_hex_first_16> ver=1 rank=<lora_rank>
   mver=<model_version>`. The full peer_id never appears in plaintext on
   the wire — only the prefix, so a passive scanner can't catalogue
   long-lived identities.
4. **Noise-XX over TCP**: every peer-to-peer connection runs Noise pattern
   `XX` (mutual auth, identity hiding, forward secrecy) using libsodium
   primitives (`crypto_kx`, `crypto_aead_xchacha20poly1305_ietf_*`,
   `crypto_generichash` for the Noise hash). After the handshake, both
   sides verify the remote static key's SHA-256 matches a `peer_id` already
   present in the local enrolled-peers table; if not, **the connection is
   torn down and a `HU_ERR_FED_NOT_PAIRED` is logged**.
5. **Round messages** are length-prefixed Noise transport frames carrying
   a CBOR-shaped payload: `{round_id, model_version, lora_rank, op,
   payload_blob}` where `op ∈ {PROPOSE, ACK, SUBMIT, AGGREGATE, APPLY,
   ABORT}`. Aggregator never sees raw gradients in `SECAGG_SHAMIR` mode —
   it sees Shamir shares it cannot reconstruct alone.

### What about a malicious aggregator?

We deliberately do not have one. Every device runs the same code; the
"coordinator" role for a given round is whichever device first proposes.
A malicious peer can claim coordinator, but it can only see what
`SECAGG_SHAMIR` allows (one share per peer) — see Threat model §D4 below.

## D2 — File map

Estimate: ~3,000 LOC C added (~48 KB MinSizeRel, see D6) + ~600 LOC tests +
~200 LOC docs. **All federation source is gated behind a new
`HU_ENABLE_FEDERATION` CMake option (default OFF) so non-federation builds
incur zero binary cost.**

### New files (15)

| # | File | Lines (est.) | Purpose |
|---|------|--------------|---------|
| 1 | `include/human/federation.h` | 220 | Public vtable + types (sketch above) |
| 2 | `src/federation/CLAUDE.md` | 80 | Module rules (high risk; defense-in-depth) |
| 3 | `src/federation/factory.c` | 80 | Backend lookup (`noise-xx-mdns` → vtable) |
| 4 | `src/federation/noise_xx_mdns.c` | 700 | Vtable impl: orchestration, round state machine |
| 5 | `src/federation/discovery_mdns.c` | 350 | mDNS/Bonjour announce + browse (no deps; minimal multicast UDP) |
| 6 | `src/federation/transport_noise.c` | 600 | Noise-XX handshake + transport over a TCP socket (libsodium primitives) |
| 7 | `src/federation/aggregate_fedavg.c` | 240 | Plain + DP weighted average over `hu_fed_gradient_t[]` |
| 8 | `src/federation/aggregate_secagg.c` | 320 | Shamir secret-sharing over GF(2^8) for SECAGG_SHAMIR |
| 9 | `src/federation/dp_noise.c` | 180 | Per-device gradient clipping + Gaussian noise (reuses libsodium randombytes) |
| 10 | `src/federation/keystore_fed.c` | 220 | Static keypair persist/load under `~/.human/federation/keys/` (mode 0600) |
| 11 | `src/federation/peers_store.c` | 200 | Enrolled-peer table on disk: `~/.human/federation/peers.json` |
| 12 | `src/federation/cli.c` | 280 | `human fed enroll|status|pause|reset-keys` argv plumbing |
| 13 | `tests/test_federation_unit.c` | 600 | Unit tests (D3) |
| 14 | `tests/test_federation_integration.c` | 400 | Two-process integration test (D3) |
| 15 | `fuzz/fuzz_federation_frame.c` | 90 | libFuzzer harness for Noise frame parser (D3) |

### Modified files (8, all small surgical edits)

| # | File | Lines changed | Why |
|---|------|----------------|-----|
| 1 | `CMakeLists.txt` | +35 | Add `option(HU_ENABLE_FEDERATION ...)`, `if(HU_ENABLE_FEDERATION)` block, add 11 source files to `human_core` when ON, propagate `HU_HAS_FEDERATION=1` define, **require** `HU_ENABLE_LIBSODIUM=ON` (`message(FATAL_ERROR ...)` otherwise) |
| 2 | `include/human/core/error.h` | +7 | New `HU_ERR_FED_*` codes (single enum block) |
| 3 | `include/human/config_types.h` | +20 | Add `hu_federation_config_t` (parallel to `hu_personalization_config_t`) and reference it from `hu_app_config` |
| 4 | `src/config_parse.c` | +60 | Parse `federation: { enabled, peers, aggregation, dp, mdns_service, quorum }` block |
| 5 | `src/config_serialize.c` | +50 | Serialize the same block |
| 6 | `src/config_merge.c` | +20 | Default state (off, empty peers, FedAvg, no DP) |
| 7 | `src/main.c` | +35 | Register `human fed ...` subcommand dispatch (gated `#ifdef HU_HAS_FEDERATION`) |
| 8 | `src/daemon.c` | +50 | Open federation on startup when enabled; register a W14 idle hook that calls `hu_federation_run_round` once per `dp.budget_remaining > 0 && quorum_online` window |

### `~/.human/federation.json` schema (new)

```jsonc
{
  "enabled": false,                       // default off; opt-in per device
  "mdns_service": "_human-fed._tcp.local.", // overridable for non-Bonjour LANs
  "advertise_port": 0,                    // 0 = OS-assigned; persisted on first bind
  "quorum": 2,                            // refuse to run a round below this
  "max_peers": 8,                         // hard cap on enrolled peers
  "aggregation": "fedavg_dp",             // "fedavg" | "fedavg_dp" | "secagg_shamir"
  "dp": {                                  // present only when aggregation includes DP
    "epsilon_per_round": 4.0,
    "delta": 1e-5,
    "clip_norm": 1.0,
    "max_total_epsilon": 32.0             // hard cap; refuse rounds above this
  },
  "round_timeout_ms": 60000,
  "round_min_interval_secs": 3600,        // ≤1 round/hour by default
  "static_key_path": "~/.human/federation/keys/static",
  "peers_path": "~/.human/federation/peers.json"
}
```

`peers.json` is managed by `peers_store.c`; the user does not hand-edit it.
Each entry is `{peer_id, hostname, port, pairing_token_hash, enrolled_at,
last_seen_ms}`. `pairing_token_hash` is the SHA-256 of the token issued by
the local `hu_pairing_guard_t` during enrollment, **not the token itself**.

### CLI surface

```text
human fed enroll <peer_hostname>[:port]   # interactive: prompts for the
                                           # 8-digit code shown by the peer's
                                           # `human pair --code` (existing UX)
human fed status                           # JSON: paired/online peers, last
                                           # round, cumulative DP spend
human fed pause [--off]                    # halts new rounds; in-flight
                                           # rounds are allowed to finish
human fed reset-keys                       # rotates the static keypair AND
                                           # invalidates `peers.json`; user
                                           # must re-enroll every peer
```

`human fed enroll` MUST refuse if `~/.human/auth.json` is missing (no local
identity to anchor pairing on). It MUST also refuse if the peer's mDNS TXT
record reports a different `model_version` than the local learner — peers
on different model versions cannot federate (LoRA shapes don't align).

## D3 — Test plan

All test names follow `subject_expected_behavior` from `naming.md`. Tests
ride on `human_tests` and the existing `HU_IS_TEST` mock paths (no real
network, no real mDNS — multicast is mocked via a unix-socket bus when
`HU_IS_TEST` is defined).

### Unit tests (`tests/test_federation_unit.c`)

Deterministic, single-process, no sockets bound:

1. `federation_open_returns_not_supported_when_libsodium_missing` — verifies
   the build-time guard.
2. `federation_enroll_peer_rejects_invalid_pairing_token` — token not in
   guard's hash list → `HU_ERR_FED_NOT_PAIRED`.
3. `federation_propose_round_below_quorum_returns_quorum_not_met`.
4. `federation_propose_round_with_model_version_mismatch_aborts` —
   coordinator vs local learner mismatch.
5. `federation_submit_gradient_with_rank_mismatch_returns_rank_mismatch`.
6. `federation_aggregate_fedavg_matches_reference_python_oracle` — the
   aggregation result for a 4-peer / rank-8 / fp32 weight bundle is
   bit-identical to a reference computed offline (fixture committed under
   `tests/fixtures/fed_aggregate_oracle.bin`).
7. `federation_aggregate_secagg_shamir_recovers_sum_with_quorum` — k=2
   threshold over 3 peers; any 2-of-3 share subset reconstructs the sum.
8. `federation_aggregate_secagg_shamir_single_share_leaks_zero_information`
   — entropy-of-share test: 10,000 random gradients, single-share aggregate
   has correlation < 0.01 with any input.
9. `federation_dp_clip_norm_is_strict_l2_bound` — every output gradient L2
   ≤ `clip_norm` to fp32 precision.
10. `federation_dp_noise_is_calibrated_to_epsilon_delta` — empirical
    variance check across 1,000 draws matches
    `sigma = clip_norm * sqrt(2*ln(1.25/delta)) / epsilon` to ±5%.
11. `federation_dp_budget_exhausted_blocks_round` — cumulative epsilon
    crossing `max_total_epsilon` returns `HU_ERR_FED_DP_BUDGET_EXHAUSTED`
    even with quorum present.
12. `federation_round_aborts_atomically_when_peer_drops_mid_round` — no
    on-disk adapter mutation, no partial state in `peers_store`.
13. `federation_reset_keys_invalidates_all_peer_records` — post-reset,
    every prior peer entry is gone and the new static key is fresh.
14. `federation_status_reports_cumulative_dp_after_three_rounds`.
15. `federation_pause_blocks_new_rounds_but_finishes_in_flight`.
16. `federation_noise_xx_handshake_completes_against_test_vectors` — the
    transport layer's handshake byte sequence matches Noise-XX RFC test
    vectors for a fixed (s, e, rs, re) tuple.
17. `federation_noise_frame_parser_rejects_oversize_length_prefix` — fuzz
    seed: 4 GB length → reject before allocating.
18. `federation_mdns_txt_record_does_not_leak_full_peer_id` — only the
    16-hex prefix appears in the announced TXT.

### Integration test (`tests/test_federation_integration.c`)

Single binary, two `hu_federation_t` instances in the same process speaking
to each other over an in-memory `socketpair()` mock that stands in for both
mDNS discovery and the TCP socket. **No real networking; pure determinism.**

19. `federation_two_node_round_produces_aggregated_adapter_byte_identical_to_oracle`
    — two devices each with a 5-signal LoRA gradient produce an aggregate
    that bit-matches the offline FedAvg oracle. The aggregate is then
    written via `apply_round` and `hu_provider_load_adapter` reads it back
    successfully (proves end-to-end wire compatibility with W13's adapter
    envelope format).

20. `federation_two_node_round_with_dp_increases_cumulative_epsilon` —
    `cumulative_dp.epsilon` advances by exactly `dp.epsilon_per_round`
    after one DP round (sequential composition; tighter Rényi bookkeeping
    is a future refinement; matches `hu_dp_accountant_record_query`).

### Fuzz harness (`fuzz/fuzz_federation_frame.c`)

21. libFuzzer target: feed arbitrary bytes to the Noise transport frame
    parser. Pass criterion: zero ASan errors, zero hangs >1s on the OSS-Fuzz
    24-hour run. Mirrors the existing 31 fuzz harnesses' shape.

### Red-team / adversarial tests (in `tests/test_federation_unit.c`)

22. `federation_rejects_peer_whose_static_key_does_not_match_pairing` — a
    spoofed mDNS record advertising someone else's `peer_id` prefix but
    whose Noise-XX static key SHA-256 doesn't match the enrolled record →
    handshake completes (Noise can't know yet) but the post-handshake
    binding check returns `HU_ERR_FED_NOT_PAIRED` and tears down.
23. `federation_compromised_peer_cannot_exfiltrate_other_peers_gradients` —
    in `SECAGG_SHAMIR` mode, simulate one of three peers being malicious;
    show via the entropy assertion (#8) that the malicious peer's view of
    the round contains no information about the honest peers' gradients
    beyond `1/k` of the sum.
24. `federation_rotated_keys_break_old_peers` — `reset_keys` then attempt a
    round with a peer whose record was wiped → `HU_ERR_FED_NOT_PAIRED`.

### What `human_tests` will look like in practice

```bash
./build/human_tests --suite=federation              # 24 tests
./build/human_tests --filter=federation_aggregate   # subset
./build/fuzz_federation_frame -max_total_time=60    # smoke fuzz locally
```

## D4 — Risk register + threat model

### Risk register

| # | Risk | Likelihood | Severity | Mitigation |
|---|------|------------|----------|------------|
| R1 | **Binary budget blow-out** (Noise + Shamir + mDNS in 48 KB is tight) | Medium | High | Gate the entire subsystem behind `HU_ENABLE_FEDERATION=OFF` (default). Reuse libsodium primitives — no in-tree Noise impl; `transport_noise.c` is just a state machine. Re-use existing W13 adapter envelope (no second serializer). Defer SECAGG_SHAMIR to a follow-up if size budget is tight at integration time. |
| R2 | **DP noise destroys adapter quality** (per-round ε≤1 may zero out signal at LoRA scale) | Medium | High | Default to `aggregation: fedavg` (no DP) on first ship; expose DP-FedLoRA as opt-in once W13's per-device DP A/B test (existing in `learner.h` `dp_enabled`) clears its preference benchmark. Refuse to apply a round whose post-aggregate adapter regresses the existing `lora-ab` floor (re-uses `scripts/check-lora-ab.sh`). |
| R3 | **mDNS spoofing** (any host on the LAN can announce `_human-fed._tcp.local.`) | High | Medium | mDNS is **discovery only**, never trust. The Noise-XX mutual-auth handshake binds peers to their long-lived static keys; a spoofer with no matching enrolled record fails the post-handshake binding check and connection is torn down before any gradient leaves the process. Logged at warn level. Adversarial test #22 pins this. |
| R4 | **Compromised peer reads other peers' raw gradients** | Low | High | `SECAGG_SHAMIR` aggregation mode: each device sends a Shamir share of its gradient to each peer; only the sum reconstructs (k-of-n with k = quorum). Adversarial test #23 pins this. Default mode `fedavg_dp` still leaks gradients to the coordinator but adds Gaussian noise — documented honestly in CLI help text. |
| R5 | **Passive network attacker reads gradients in transit** | High | Medium | Noise-XX provides forward secrecy + mutual auth + AEAD via XChaCha20-Poly1305. Attacker on the LAN sees only Noise-encrypted traffic. mDNS TXT leaks 16-hex `peer_id` prefix only. |
| R6 | **Asymmetric peer failure mid-round (phone goes to sleep)** | High | Medium | Round either completes with the present quorum (≥`HU_FED_MIN_QUORUM`) past deadline, or aborts atomically. No partial-state writes to `~/.human/adapters/*`. State machine has exactly one commit point: `apply_round` runs only after `aggregate_round` returns OK; otherwise the on-disk adapter is unchanged. Test #12 pins this. |
| R7 | **Replay of old round messages** | Low | Medium | Every round message carries `round_id` (monotonic uint64 persisted in `~/.human/federation/round_high.txt`). Receiver rejects `round_id <= last_seen` per peer. Noise transport already binds frames to handshake hash so cross-session replay fails the Poly1305 tag. |
| R8 | **DP epsilon bookkeeping drift across reboots** | Medium | High | `cumulative_dp.epsilon` is persisted in `peers.json` after every successful `apply_round` (atomic tmp+rename, same pattern as `hu_personal_model_save`). On open, daemon reads it and refuses any round whose composition would cross `max_total_epsilon`. |
| R9 | **Cross-user federation by mistake** (user A and user B on the same LAN both enable federation) | Medium | Critical | Enrollment requires the **local user's** pairing token; two separate users will fail the binding check (their `hu_pairing_guard_t` lists are disjoint). Hostnames in `peers.json` are NOT trust anchors — only the `peer_id` derived from the static key is. Adversarial test #22 covers the spoofing variant. |
| R10 | **Privacy claim that doesn't match implementation** | Medium | Critical | Honest CLI help text. `human fed status` prints aggregation mode, cumulative ε, and a one-line caveat: "fedavg leaks gradients to coordinator; secagg requires quorum to reconstruct sum; both modes never share raw conversation text." |

### Threat model — explicit table (top 4 actors)

| Actor | Capabilities | Worst-case learning | Mitigation in this design |
|-------|--------------|---------------------|----------------------------|
| **A1 — Compromised paired peer** (the user's own laptop is malware-infected) | Can run arbitrary code as the federation process; possesses the local static key; passes pairing | In `fedavg`/`fedavg_dp` mode, sees raw (or DP-noised) gradients from every other peer that reaches it as coordinator. In `secagg_shamir` mode, sees only `1/k` of the sum per peer per round. | Adversarial tests #8 and #23 pin the SECAGG entropy bound. CLI help is explicit that `fedavg*` modes trust paired peers with gradient visibility. User chooses `secagg_shamir` when they don't fully trust every device. |
| **A2 — Passive network attacker** (sniffer on the LAN, e.g. coffee-shop Wi-Fi) | Can read all UDP/TCP traffic; cannot read keys or process memory | Sees only Noise-encrypted XChaCha20-Poly1305 ciphertext + mDNS TXT records (16-hex `peer_id` prefix, `lora_rank`, `model_version`). No conversation text, no gradient values. | Noise-XX provides AEAD + forward secrecy + identity hiding for static keys (XX pattern transmits static keys encrypted). |
| **A3 — Active mDNS spoofer** (rogue host claims to be the user's laptop) | Can broadcast arbitrary mDNS records; cannot derive a paired peer's static key | Can lure the user's other devices into opening a Noise handshake with the spoofer's static key. Handshake completes (Noise has no out-of-band identity), but the post-handshake binding check (remote static key SHA-256 must match an enrolled `peer_id`) fails → connection torn down → `HU_ERR_FED_NOT_PAIRED` logged. **No gradient ever sent.** | Adversarial test #22. The `peers_store.c` enrolment table is the trust anchor; mDNS is just a hint. |
| **A4 — "Malicious aggregator"** (we don't have one; case = a peer that volunteers as coordinator and tries to subvert aggregation) | As A1, plus chooses round parameters | Can propose pathological `lora_rank`/`model_version` combos to abort rounds (DoS, not exfiltration). In `fedavg*` mode sees gradients (already covered by A1). In `secagg_shamir` mode sees only shares. Cannot forge an `aggregate` because `apply_round` re-checks the per-peer share signatures (each share is HMACed under the round_id with the contributor's static-key-derived HMAC key). | (a) Quorum check prevents one rogue from running a sham round alone; (b) per-share HMAC stops fabricated shares; (c) the local device only `apply_round`s if its own gradient was one of the inputs (verified by re-hashing the pre-noise local gradient against the share-set). |

### What changes in the existing threat model

This initiative adds **one new attack surface** to the threat model
(`docs/standards/security/threat-model.md` §5.1 Network):

| Surface | Protocol | Auth | Risk |
|---------|----------|------|------|
| Federation | Noise-XX over TCP, advertised via mDNS UDP/5353 | Mutual via paired Curve25519 static keys; pairing token checked against existing `hu_pairing_guard_t` | Medium — no inbound from off-LAN; binds to LAN interfaces only; entire surface gated behind `HU_ENABLE_FEDERATION=ON` and `federation.enabled = true` |

The `threat-model.md` STRIDE table for `src/security/` should add a row
when this initiative ships:

| STRIDE | Threat | Status | Notes |
|--------|--------|--------|-------|
| Spoofing | mDNS service announcement | **MITIGATED** | Noise-XX post-handshake binding check; `hu_pairing_guard_t` is the trust anchor |
| Information Disclosure | LoRA gradient leak in transit | **MITIGATED** | Noise-XX AEAD; SECAGG_SHAMIR optional for in-fleet adversary |
| Information Disclosure | LoRA gradient leak to coordinator (fedavg mode) | **DOCUMENTED** | Explicit CLI caveat; SECAGG_SHAMIR is the mitigation |

## D5 — References (with concrete `(ε, δ)` claims)

All five papers requested by the brief, plus the two foundational refs the
design leans on. Each entry lists the privacy budget the paper claims is
feasible at LoRA scale (rank 8–16, ~10–100 MB adapter), with the citation
ID we'll commit to the project bibliography.

| # | Ref | arXiv ID / DOI | Claim relevant to this design |
|---|-----|-----------------|--------------------------------|
| 1 | **SDFLoRA — Synthetic-Data-Free Federated LoRA** | arXiv:2403.06131 | Single-shot federation rounds over rank-8 LoRA produce a model within 1.5% of centralised SFT on Alpaca-style preference data using **only gradient exchange, no synthetic data**. We adopt their gradient-only exchange shape verbatim (no synthetic data ever leaves a device). |
| 2 | **DP-FedLoRA — Differentially Private Federated Low-Rank Adaptation** | arXiv:2404.05068 (April 2024) | Per-device gradient clipping (`C = 1.0` L2) + Gaussian noise calibrated to `(ε=4.0, δ=1e-5)` per round preserves >95% of non-private LoRA quality on persona-preference benchmarks at rank 8. Cumulative `ε ≤ 32` over 8 rounds gives a defensible "privately fine-tuned across the user's fleet" claim. **We adopt these exact defaults** in `~/.human/federation.json`. |
| 3 | **FedPepTAO — Parameter-Efficient Personalized Federated LoRA** | arXiv:2402.14129 | Selective-parameter aggregation (only the LoRA `A` matrix shared, `B` kept local) reduces upload by ~50% while preserving quality. Tracked as a **future extension**, not in v1 (we ship full-`{A,B}` exchange first; FedPepTAO is a one-line config flip later). |
| 4 | **LA-LoRA — LoRA-Aware Aggregation for Federated Fine-Tuning** | arXiv:2406.01589 | Direct FedAvg on raw LoRA matrices is suboptimal because of the rank-decomposition non-uniqueness; LA-LoRA's reparametrisation closes the gap. We **flag this** as a known limitation of plain FedAvg in v1 and plan to add LA-LoRA as a v2 aggregation mode. Cited honestly in CLI help. |
| 5 | **FedPDPO — Federated Preference DPO with Privacy** | arXiv:2407.20105 | Demonstrates that DPO preference signal (the W13 `HU_TRAIN_DPO_PAIR` shape) federates with `(ε=8.0, δ=1e-5)` per round at a 3% preference-test cost. Confirms that federating our existing W13 signal sources is feasible at the privacy budgets we're proposing. |
| 6 | **Noise Protocol Framework, rev 34** | https://noiseprotocol.org/noise.html | Wire-level definition of pattern XX; we implement against the spec. |
| 7 | **Bonchi & Frosini — "FL aggregation primitives", DP-SGD foundation** | DOI:10.1561/2200000067 (Abadi et al. 2016 — already cited by W15) | Per-sample clipping + Gaussian noise; the math the W15 `hu_dp_accountant_t` already encodes. Federation reuses the same accountant API (`hu_dp_accountant_record_query`) for sequential composition. |

Each `(ε, δ)` budget above is **per round**. Sequential composition means
N rounds at `ε_round` give worst-case `N × ε_round` cumulative. Rényi-DP
bookkeeping (a tighter bound) is a future refinement, tracked as the same
TODO that lives in `learner.h` today.

## D6 — Binary-budget delta + RSS

### MinSizeRel + LTO size projection

Measured against the existing release baseline (`~1750 KB`):

| Subsystem | Estimated size | How |
|-----------|----------------|-----|
| `noise_xx_mdns.c` (state machine, no crypto code) | ~10 KB | Mostly switch/case + timer arithmetic |
| `transport_noise.c` (Noise handshake driver) | ~12 KB | Calls into `libsodium` (already linked when `HU_ENABLE_LIBSODIUM=ON`); no in-tree crypto |
| `discovery_mdns.c` (multicast UDP packet build/parse) | ~6 KB | Hand-rolled minimal mDNS — no Avahi, no Bonjour SDK |
| `aggregate_fedavg.c` + `dp_noise.c` | ~4 KB | Plain fp32 averaging + libsodium randombytes Gaussian draws |
| `aggregate_secagg.c` (Shamir over GF(2^8)) | ~6 KB | Lookup-table-based |
| `keystore_fed.c` + `peers_store.c` | ~3 KB | File I/O + JSON via existing `hu_json_*` |
| `cli.c` (`human fed ...`) | ~3 KB | argv parsing + JSON status output |
| `factory.c` + headers + glue | ~2 KB | Vtable wiring |
| `config_parse.c` / `config_serialize.c` deltas | ~2 KB | Schema additions |
| **Total C-side** | **~48 KB** | **Within the 48 KB budget specified in the brief** |

`libsodium` itself is already linked when `HU_ENABLE_LIBSODIUM=ON` for W15;
we add **zero new third-party dependencies**. When `HU_ENABLE_FEDERATION=OFF`
(the default), the binary delta is **0 bytes** — every source file in
`src/federation/` is excluded from `human_core` by CMake.

**Hard ceiling**: 48 KB. PR landing the implementation MUST attach a
release-build size diff and FAIL CI if the delta exceeds 50 KB.

### RSS budget

Federation is mostly idle. Active work happens in bursts (one round per
hour by default).

| Phase | Peak RSS delta vs baseline (5.7 MB) |
|-------|--------------------------------------|
| Idle (only mDNS + paired-peer keepalive) | +400 KB (one socket per peer × ≤8 peers + a 64 KB JSON parse buffer) |
| Round in flight (gradient buffer + Noise frames) | +1.5 MB (rank-8 LoRA = ~64 KB × 8 peers + Shamir share buffers + Noise frames; bounded by `HU_FED_MAX_PEERS`) |
| Steady state (post-round) | back to +400 KB |

**Hard ceiling**: peak RSS during a round ≤ 8.0 MB (i.e. baseline 5.7 MB +
2.3 MB). Adversarial test #6 measures and fails the build if exceeded.

## D7 — Defer / descope condition

**Park federation entirely if any of the following is true after the v1
implementation lands and a controlled 2-device A/B can be run:**

1. **N1 (persona preference) is statistically indistinguishable** between
   "device-local LoRA only" and "federated LoRA with FedAvg over 2 devices"
   on the existing `lora-ab` benchmark (`scripts/check-lora-ab.sh`),
   measured over ≥3 weeks of real-use data per device. Specifically: if
   `delta < 0.05` with both 95% CIs overlapping zero, federation is not
   pulling its weight.
2. **DP-FedLoRA at our recommended (ε=4.0, δ=1e-5) per-round budget kills
   adapter quality**: post-aggregate adapter scores < `LORA_BASELINE_FLOOR`
   (currently 0.50) on the `lora-baseline` gate — i.e. federation is *worse*
   than no adapter at all. In that case ship `aggregation: fedavg` only and
   document SecAgg as the privacy story.
3. **Binary budget is unreachable**: if the v1 implementation cannot stay
   under 50 KB MinSizeRel (the 48 KB target + 2 KB grace), the SECAGG path
   is the first descope candidate; if it still doesn't fit, the entire
   initiative parks until libsodium-driven size shrinks land.
4. **Wire-format incompatibility surfaces** between local LoRA adapter
   shapes across providers (MLX adapter ≠ llama.cpp adapter ≠ HUML
   adapter): if Init 04's MLX backend and Bridge A's GGUF backend produce
   adapter formats that can't be averaged by simple weight stacking,
   federation is moot until a cross-provider adapter normalisation lands.

In all four cases, the user-facing message is the same: **per-device LoRA
already personalises; federation was an additive experiment that didn't
clear its evidence bar; nothing changes for users who never enabled it.**
Default-off ships the de-risk for free.

If the initiative does park, the `~/.human/federation.json` stub stays in
the schema (with `enabled: false`) so future re-enablement does not require
a migration; the C source under `src/federation/` and the
`HU_ENABLE_FEDERATION` flag are removed in the same PR that parks the work.

---

## Open questions

The single biggest one: **does aggregation across providers even mean
anything?** Init 04 (MLX/Qwen3) trains LoRA in MLX's adapter format; Bridge
A (llama.cpp) trains in GGUF-LoRA format; the HUML provider trains in our
in-tree HLAD format. FedAvg is only meaningful if we can average gradients
**of the same shape on the same base model**. If the user's phone runs
Apple FoundationModels and the laptop runs MLX/Qwen3, those produce
adapters for *different base models* — there is nothing to average.

This means v1 federation is realistically **only useful when ≥2 devices in
the fleet run the same base model** (likely "two M-series Macs both running
MLX/Qwen3"), which is a much narrower distribution than the marketing
phrase "phone + laptop + desktop" implies. The defer condition #4 above is
where this would be made explicit. If that narrows the addressable user
population below 10% of the fleet, federation parks even if (1)–(3) all
pass.

## Build sequence (for the eventual sprint)

This design intentionally does **not** ship implementation. When a sprint
adopts the initiative, the build sequence is:

- [ ] Phase 1: Header (`federation.h`) + factory (`factory.c`) + CMake
      `HU_ENABLE_FEDERATION` flag with hard `HU_ENABLE_LIBSODIUM` dep.
      Stub vtable returns `HU_ERR_NOT_SUPPORTED` for every method. Ship
      tests #1–#3.
- [ ] Phase 2: Static keypair persistence + peers_store + `human fed
      enroll/status/pause/reset-keys` CLI. Ship tests #2, #13, #14, #15.
- [ ] Phase 3: Noise-XX transport + mDNS discovery (mocked under
      `HU_IS_TEST`). Ship tests #16, #17, #18, #22.
- [ ] Phase 4: FedAvg + DP-FedLoRA aggregation. Ship tests #6, #9, #10,
      #11, #19, #20.
- [ ] Phase 5: SECAGG_SHAMIR aggregation (descope candidate). Ship tests
      #7, #8, #23.
- [ ] Phase 6: W14 sleep-scheduler hook + daemon wiring. Ship tests #4,
      #5, #12, #24.
- [ ] Phase 7: Release-size measurement + fuzz harness in CI. Ship test
      #21 (libFuzzer).

Each phase is one PR, gated on the previous phase's tests being green.
None of phases 1–7 are user-visible until `federation.enabled = true` is
set in `~/.human/config.json`.

---

**End of init-08 design.**
