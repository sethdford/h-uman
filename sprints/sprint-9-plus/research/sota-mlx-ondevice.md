# SOTA On-Device LLM Fine-Tuning, May 2026 — MLX & Apple Silicon

**Author:** research agent
**Date:** 2026-05-16
**Scope:** answers Sprint 9 questions about the MLX fine-tuning stack, alternative PEFT methods, Apple's first-party personalization story, and Gemma-4 speculative decode.

---

## 1. mlx-lm / mlx-lm-lora feature matrix (May 2026)

| Capability | `ml-explore/mlx-lm` (Apple, official) | `Goekdeniz-Guelmez/mlx-lm-lora` (third-party fork we use) | `ARahim3/mlx-tune` (Unsloth-API clone) |
|---|---|---|---|
| LoRA SFT | yes | yes | yes |
| QLoRA (4-bit base + LoRA) | yes (NF4-style, MLX-native) | yes | yes |
| DPO | **no native CLI** | **yes** (`--train-mode dpo`) | yes |
| ORPO, CPO, KTO, SimPO | no | yes (ORPO, CPO, KTO) | yes (incl. SimPO) |
| GRPO / Dr.GRPO / DAPO / GSPO | no | yes | yes (GRPO) |
| Online DPO / XPO / RLHF (PPO) | no | yes | partial |
| Vision / TTS / STT / OCR fine-tuning | no | partial | yes |
| `--lora-parameters` YAML config | partial (requires `-c`) | partial (Sprint-8 finding: requires `-c`) | yes |
| M5 GPU neural-accelerator path | yes (Apple ML blog, M5 piece) | inherits MLX backend | inherits |

The first-party `mlx-lm` stack still does **not** ship a DPO/RLHF CLI. The community forks `mlx-lm-lora` (Gülmez) and `mlx-tune` (Rahim) are where preference optimisation lives, and both target the same MLX backend Apple ships, so the perf cost of "third-party" is essentially nil. ([mlx-lm-lora][1], [mlx-tune][2], [Apple ML research][3])

[1]: https://github.com/Goekdeniz-Guelmez/mlx-lm-lora
[2]: https://github.com/ARahim3/mlx-tune
[3]: https://machinelearning.apple.com/research/exploring-llms-mlx-m5

## 2. Efficient fine-tuning techniques — what's actually winning

### QLoRA (NF4 + double-quant + paged optimizer)
Still the workhorse. Slashes memory ~4x with 96–98% of full-precision quality on academic benchmarks; matches FP16 LoRA on most tasks. NF4 (information-theoretically optimal for normal weights) remains the recommended dtype. Production deployments on consumer hardware in 2025 routinely fine-tune 65–70B models on a single 48 GB GPU. ([Dettmers 2023, arXiv:2305.14314](https://arxiv.org/abs/2305.14314))

### DoRA — Weight-Decomposed Low-Rank Adaptation
Decomposes the pretrained weight into magnitude + direction, applies LoRA only to direction. Adds **~2-3% more trainable parameters** than LoRA, **closes ~93% of the LoRA-to-full-FT quality gap** at the same rank, and incurs **no inference overhead** (merge-back works). 2025 NVIDIA write-up confirms this is now a "costless replacement for LoRA". Cost: 10–20% wall-clock training slowdown. ([NVIDIA dev blog 2025](https://developer.nvidia.com/blog/introducing-dora-a-high-performing-alternative-to-lora-for-fine-tuning/), [FinLoRA arXiv:2505.19819](https://arxiv.org/html/2505.19819v1))

### GaLore — Gradient Low-Rank Projection
Full-parameter learning, not adapters. Projects gradients into a low-rank subspace so optimizer state shrinks ~65%, **enabling 7B pre-training on a 24 GB consumer GPU**. 8-bit GaLore further trims optimiser memory **up to 82.5%**. Fine-tuning RoBERTa-Base on GLUE at rank 4: **85.89 avg vs LoRA 85.61**. Best when you want true full-FT quality on tight memory. ([Zhao et al., arXiv:2403.03507](https://arxiv.org/abs/2403.03507))

### PiSSA / VeRA / LoftQ
Live in the "PEFT beyond LoRA" zoo. Useful when LoRA initialization hurts (PiSSA improves SVD warmstart) or when you need extreme parameter compression (VeRA shares random projections). Not yet the default. ([Spheron 2026 PEFT guide](https://www.spheron.network/blog/peft-methods-2026-dora-galore-pissa-vera-guide/))

## 3. Apple Intelligence on-device personalization — what's public

Apple published a developer-facing **Foundation Models Adapter Training Toolkit** (Python + Jupyter walkthrough, `examples/train_adapter.py`). It trains LoRA adapters against Apple's ~3B on-device model (AFM, internally `AFMTextV7`, 3.18B params, ~1 GB Apple Silicon footprint with 2-bit QAT) and exports a `.fmadapter` package consumed by Xcode and the Foundation Models framework. Architecture highlights from the 2025 tech report (arXiv:2507.13575): **2-bit QAT base + LoRA quality-recovery adapters**, KV-cache sharing (-37.5% cache memory), interleaved local/global attention with 4096 sliding window. Production deployment requires the Foundation Models Adapter Entitlement. ([Apple Developer](https://developer.apple.com/apple-intelligence/foundation-models-adapter/), [Apple ML report 2025](https://arxiv.org/abs/2507.13575), [AFMTextV7 reverse-eng analysis](https://github.com/fguzman82/apple-foundation-model-analysis))

**Takeaway for h-uman:** Apple's reference architecture is *exactly* what M3 is aiming at — a small base model + per-task LoRA adapter, with QAT for the base. The gap is that Apple's adapters fine-tune AFM, not whatever cloud LLM the user chats with — same architectural constraint we have. They route Adapter Entitlement through the system; we can't follow that path on iOS without entitlement approval. Mac is unconstrained.

## 4. iOS / iPadOS on-device training

Still emerging. Two relevant 2025/2026 entries:

- **MobileFineTuner** (arXiv:2512.08211) — C++ end-to-end framework, autodiff + full backprop on commodity phones, iOS + Android. Research-stage; not production-grade. ([arXiv](https://arxiv.org/html/2512.08211v1))
- **Tether QVAC Fabric LLM** (Dec 2025) — claims "first unified, portable, cross-platform" runtime supporting LoRA + instruction tuning on iOS/Android. Closed-source; vendor claims. ([Tether announcement](https://tether.io/news/tether-data-introduces-qvac-fabric-llm-the-edge-first-llm-inference-runtime-and-generalized-llm-lora-fine-tuning-framework-for-modern-ai-models-on-heterogeneous-gpus-smartphones-laptops-and-server/))

Battery/latency reality: iPhone-class chips can run **inference** at int4 comfortably, but a single LoRA epoch on a 2B model still bursts thermals; the practical pattern is **fine-tune on Mac → ship adapter → load on iOS**. Apple recommends "exhaust prompt engineering before adapter training" in their developer docs, which is industry consensus.

## 5. Gemma-4 family: speculative decode + LoRA

Google released the **Gemma-4 MTP drafter** on **2026-05-05** — a lightweight multi-token-prediction model that pairs with any Gemma-4 target via speculative decoding, reusing the target's KV cache. **Up to 3x decode speedup with no quality loss**, explicitly optimized for "NVIDIA GPUs, Apple Silicon via MLX, and Pixel TPU." E4B sustains ~38 tok/s decode at int4 on M2 Ultra via MLX. Red Hat EAGLE-3 weights for the 31B-as-target + 2B-as-draft pairing are on Hugging Face. ([Google blog](https://blog.google/innovation-and-ai/technology/developers-tools/multi-token-prediction-gemma-4/), [Build Fast With AI 2026 guide](https://www.buildfastwithai.com/blogs/gemma-4-mtp-drafter-faster-inference), [Red Hat AI on X](https://x.com/RedHat_AI/status/2042660544797110649))

**Caveat for h-uman:** the MTP drafter does *not* include LoRA adapters by default. Stacking a persona LoRA on the E2B *draft* path is unproven — the drafter must stay aligned with the target's distribution or speculation acceptance plummets. The safe pattern is **LoRA on the target (E4B/31B), vanilla draft (E2B + MTP head)**.

---

## Sprint 9 recommendations

1. **Keep `mlx-lm-lora` for SFT+DPO.** First-party `mlx-lm` still has no DPO CLI as of May 2026. Pin to `mlx-lm-lora>=2.1.0,<3` per Sprint-8 smoke run.
2. **Adopt DoRA as a default upgrade from LoRA** for the persona adapter, behind a config flag. `mlx-lm-lora` supports it via `--train-type dora` (verify in source). Expected gain: roughly half the remaining quality gap to full FT, no inference cost.
3. **Skip GaLore for now.** It's a pre-training/full-FT tool. Our story is adapters.
4. **M3 (Personal Model) — re-anchor against Apple's adapter architecture.** Their reference pipeline is QAT-base + LoRA-recovery + LoRA-task. Document explicitly in `docs/plans/2026-05-10-m3-frontier-model-bridge.md` that we are converging on Apple's pattern, but for non-AFM bases.
5. **iOS path — defer training to Mac.** Inference via MLX on iPad Pro M-series is fine; training on iPhone is research-grade. Honest M3 status: "train on Mac → ship `.npz` adapter → iOS loads at boot." Adopt `.fmadapter`-style sidecar packaging for forward compatibility.
6. **Gemma-4 speculative decode — pursue, but adapter-on-target only.** Plan a Sprint-10 spike: target = `gemma-4-e4b-it-4bit` + persona LoRA, draft = `gemma-4-e2b-mtp` (no adapter). Measure acceptance rate on persona-laden prompts.
7. **Address current Sprint-8 finding (`delta=0.000`).** Likely cause: SFT and DPO adapters trained at default rank=8/layers=8 don't move the needle on a 30-prompt held-out eval. DoRA + rank-16 + 200+ pref pairs is the next experimental step before declaring DPO ineffective.

---

RESULT_research=READY
