---
title: Privacy Primitives Gap Analysis 2026-05-16
---

# Privacy Primitives — Gap Analysis (2026-05-16)

The product thesis claims "privacy by architecture, not by settings."
That claim survives if-and-only-if a security-aware user examining the
codebase reaches the same conclusion. This doc inventories the gap
between **structural** privacy (we have it) and **verifiable**
privacy (we don't yet) so a future PR can close each item one at a time.

## What we have today

- **Local-first runtime.** The daemon runs on the user's hardware. The
  personal model at `~/.human/personal_model.bin` and persona at
  `~/.human/personas/*.json` never leave the device.
- **Cloud provider calls are opt-in.** The user explicitly configures
  Gemini / Claude / OpenAI / etc. When configured, traffic flows over
  HTTPS via [src/providers/provider_http.c](../../src/providers/provider_http.c).
- **Log scrubber.** [src/providers/scrub.c](../../src/providers/scrub.c)
  redacts API keys and PII before logging.
- **Memory engines support PII redaction** at the channel boundary
  (`hu_persona_banks_extract_from_history` strips PII before banks
  ship into training data).

This is structurally good. The gaps below are about **proving it** —
turning trust statements into verifiable properties.

## Gap 1 — DP-SGD is theater (high severity)

[src/ml/learner_mlx.c:309](../../src/ml/learner_mlx.c#L309) adds Gaussian
noise to the trained LoRA adapter *after* `mlx_lm.lora` finishes
training. The critic (2026-05-16 audit) flagged this:

> The DP noise injection in `learner_mlx.c:309` is post-hoc Gaussian
> noise appended after `mlx_lm.lora` trains without any gradient
> clipping. This does not satisfy DP-SGD; it satisfies neither the
> formal ε-δ guarantee nor the plan's privacy-by-architecture claim
> for the training path.

Why this matters: any claim that LoRA training is differentially-private
is false. A motivated adversary with the trained adapter can
reconstruct fragments of the training set with no formal bound on what
they can recover.

**Fix path:**
1. Replace post-hoc noise with proper DP-SGD: per-sample gradient
   clipping (clip norm typically 1.0) + Gaussian noise on the *clipped*
   gradient batch sum.
2. The hot-path change lives in the MLX subprocess invocation —
   `mlx_lm.lora` supports `--dp-noise-multiplier` and `--dp-l2-norm-clip`
   in recent versions. Wire those args from the existing config struct.
3. Pin an ε bound to documentation. Report (ε, δ) per training run in
   the daemon's training-log output.

**Effort:** 1-2 weeks. The Python-side flag wiring is small; the harder
part is auditing the ε-δ accounting matches mlx_lm's published shape.

## Gap 2 — No reproducible builds with signed attestation (medium-high)

Today: a user downloads a release binary, trusts it, runs it.

There is no way to verify the binary actually corresponds to the
public source tree. A compromised release pipeline (or upstream
dependency) could ship a binary that exfiltrates the personal model
without the source ever showing it.

**Fix path:**
1. Reproducible builds. `cmake --preset release` must produce
   bit-identical output across two clean machines given the same git SHA.
   Today: untested. Needs `--seed 0` everywhere randomness sneaks in
   (random function-name salting, timestamps in binary headers).
2. Signed attestation. The release CI workflow ([.github/workflows/release.yml](../../.github/workflows/release.yml))
   produces a binary + SHA256. Add a sigstore / Rekor signature.
3. User-facing verification: `human --verify-build` reads its own
   binary, computes the SHA256, compares to a signed claim at a known
   URL. Refuses to run if mismatched.

**Effort:** 2-3 weeks. Reproducibility audit is the bulk; signing is
~1 day with existing tooling.

## Gap 3 — Plaintext personal model on disk (low-medium)

[`hu_personal_model_save`](../../include/human/memory/personal_model.h#L333)
writes a binary blob in plaintext. Anyone with read access to the user's
home dir can dump it.

**Fix path:**
1. Symmetric encryption via the existing
   [`hu_crypto`](../../include/human/crypto.h) primitives. Key derivation
   from user passphrase or system keyring (Apple Keychain on macOS,
   libsecret on Linux). Default mode: encrypted; opt-out for
   debug builds.
2. The atomic-save path already proven by
   [`tests/test_personal_model_atomic_save.c`](../../tests/test_personal_model_atomic_save.c)
   handles tmp+rename — the encrypted version inserts encrypt-before-write.

**Effort:** 3-5 days. Atomic-save invariant must be preserved.

## Gap 4 — No federated learning / multi-device sync (large, blocks M4)

A single user with two devices today has TWO independent personal
models that never converge. The product thesis ("the assistant that's
yours, everywhere") requires sync. Cloud-relayed sync defeats the
privacy claim.

**Fix path (the hard option):** CRDT-based sync over local network
(Bonjour / mDNS discovery) with end-to-end encryption keyed to
device pairing.

**Fix path (the medium option):** explicit export/import via a
user-controlled encrypted file the user moves between devices. No
network, no automation, but preserves the property "data never leaves
the user."

**Effort:** 2-6 months depending on option. Out of scope for any
near-term PR.

## Gap 5 — No TEE / Secure Enclave story (low priority today)

On Apple Silicon, the Secure Enclave could host the personal model's
encryption key. On Linux, TPM 2.0 serves the same role. Neither is
wired today; the key (if/when Gap 3 lands) lives in the keychain.

**Effort:** 1-2 months per platform. Defer until Gap 3 is operational
and Gap 4 has a direction.

## Gap 6 — No user-facing privacy audit log (small, useful)

A user has no way to ask "in the last 24h, what data left this device?"
Cloud provider calls happen via [src/providers/provider_http.c](../../src/providers/provider_http.c);
the daemon could trivially log a structured outbound event per call.

**Fix path:** `~/.human/audit/outbound.log` — JSONL of every outbound
HTTP, with timestamp, provider, model, request_byte_count,
response_byte_count, NO content. UI surface in the dashboard that
streams this view.

**Effort:** 1 week. The hardest part is deciding what to redact in the
log itself (the request body would obviously leak everything; the byte
count is safe).

## Priority order (recommendation)

If forced to rank by leverage-per-week:

1. **Gap 6** (audit log) — 1 week, immediate user-visible trust signal.
2. **Gap 1** (real DP-SGD) — 1-2 weeks, removes a false privacy claim
   that's flagged by the security-review agent today.
3. **Gap 3** (encrypted on-disk model) — 3-5 days, eliminates a real
   attack on shared machines.
4. **Gap 2** (reproducible signed builds) — 2-3 weeks, longer but
   makes the entire trust story verifiable.
5. **Gap 4 / Gap 5** — deferred until distribution (M4) creates the
   pressure to solve them.

## Anti-pattern to watch

Do NOT add a "Privacy" tab to the dashboard before Gap 1 is fixed.
Surfacing a privacy story while the core training claim is theater
turns trust capital into a liability the day a security researcher
notices.
