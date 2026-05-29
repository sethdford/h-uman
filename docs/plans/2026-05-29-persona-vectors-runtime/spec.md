# Persona Vectors + KV-Cache Personalization — Serving-Layer Spec

> Item 3 of the SOTA roadmap. The two highest-impact May-2026 techniques
> (persona vectors / activation steering; KV-cache persona swap) live INSIDE the
> model forward pass / KV cache — i.e. in `mlx-server.py` (the local serving
> runtime), NOT the C agent. This spec designs that work so it can be executed +
> validated on Apple Silicon with a running model. It is **not implemented here**
> because it cannot be validated without the live runtime.

> **Priority note (2026-05-29):** the real data audit returned **THIN (254
> assistant messages)**. At that volume the evidence favors **RAG/hybrid over
> fine-tuning**, so wiring the RAG core (`src/persona/rag.c`) is the higher-
> priority SOTA move. Persona vectors steer the *fine-tuned* model's activations,
> so they're second priority until the corpus grows or the A/B favors LoRA.
> This spec is the frontier track, sequenced after RAG.

## Goal

Continuous, compositional, low-latency control of the local model's *voice
traits* (formality, humor, verbosity, warmth) at inference time — without
retraining or reloading adapters — plus sub-100ms in-conversation style
switching.

## Background (research leads — verify before building)

- **Activation steering / persona vectors:** personality dimensions encode
  ~linearly in hidden states; steering via `h_l ← h_l + α·v_l` on the residual
  stream controls traits at negligible inference cost. (PERSONA / activation-
  steering line, ICLR-2026 leads — verify arXiv IDs.)
- **KV-cache personalization:** store persona/concept state in the KV cache per
  layer; swap at inference for stateful personalization without LoRA reload
  (XKV / Jarvis leads — verify).

## Components (all in `mlx-server.py` / the MLX runtime)

1. **Persona-vector extraction** — derive trait direction vectors `v_l` per layer
   from contrastive prompt pairs (e.g. formal vs casual completions) OR from the
   trained Seth LoRA's weight delta. Cache to disk (`~/.human/persona_vectors/`).
2. **Residual-stream steering hook** — during generation, add `α·v_l` to the
   residual stream at each layer for the active trait set. `α` per trait from the
   request (so the C agent's existing persona-overlay formality/humor knobs map
   to steering coefficients).
3. **Steering API** — extend the OpenAI-compatible endpoint with an optional
   `steering: {formality: 0.5, humor: 0.8, ...}` field; default = no steering
   (backward compatible). The C agent's `maybe_override_to_mlx_local` path passes
   the persona overlay's trait values here.
4. **KV-cache persona slots (phase 2)** — keep per-persona KV state resident;
   route a request to a slot by persona/contact for sub-100ms switching.

## Data flow

1. C agent selects local model (AUTO routing) + computes persona-overlay traits.
2. Request includes `steering` coefficients derived from the overlay.
3. mlx-server applies residual-stream steering during decode; harmony filter +
   guards (already in place) still run on the output.
4. (Phase 2) persona KV slot selected by contact for stateful switching.

## Decisions

- **Steer at the runtime, expose coefficients over the existing HTTP contract**
  — keeps the cross-language boundary an HTTP API (per `cross-language-via-http`),
  no FFI. The C agent stays unchanged except for passing coefficients.
- **Default off / α=0** — zero behavior change until validated; opt-in per request.
- **Extract vectors from the LoRA delta first** — reuses the trained adapter;
  contrastive-pair extraction is the fallback.

## Validation (REQUIRES Apple Silicon + running mlx-server — cannot do in CI)

1. Unit: vector extraction is deterministic; steering with α=0 is a no-op
   (byte-identical to unsteered output).
2. Behavior: sweep α for one trait (e.g. formality) on 10 held-out prompts;
   confirm monotonic shift on the per-axis fidelity decomposition (W7-2) without
   base-capability collapse (the `lora-scale-default-or-die` lesson applies —
   over-steering destroys instruction-following).
3. Latency: first-token + per-token overhead vs unsteered; target <10% (steering
   is a vector add; KV-slot switch target <100ms).
4. A/B: steered-LoRA vs RAG (W7-3 harness) on real data — does steering close the
   gap RAG opened at THIN data volume?

## Risks

- **Over-steering collapses instruction-following** (same failure mode as
  LoRA scale=20). Mitigate: clamp α, validate base capability per `quality-gates`.
- **Thin data** (254 msgs) means the LoRA the vectors steer is itself weak —
  steering a weak adapter may not reach RAG's fidelity. Hence RAG-first.
- **MLX internals churn** across versions — keep the steering hook behind a
  capability probe; fall back to unsteered on any runtime mismatch.

## Sequencing

After: wire `src/persona/rag.c` (RAG-first, data-supported) + run the W7 A/B.
Then: this spec, IF the A/B or a grown corpus makes LoRA competitive again.
