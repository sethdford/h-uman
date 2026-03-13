# src/ml/ — ML Training Subsystem

On-device ML training for autonomous experimentation. Gated behind `HU_ENABLE_ML`.

## Architecture

```
tokenizer_bpe.c   BPE tokenizer (byte-pair encoding, train/save/load)
dataloader.c      Binary token data loader with BOS-aligned packing
prepare.c        Data preparation (tokenize files, build token_bytes lookup, default config)
evaluator.c      BPB (bits per byte) evaluation metric
experiment.c     Autonomous experiment loop (config mutation, train/eval, keep/discard)
gpt.c            GPT model: RMSNorm, RoPE, CausalSelfAttention, MLP, residual lambdas, logit soft-cap
muon_adamw.c     MuonAdamW optimizer: AdamW for scalars, Muon for matrices
train.c          Time-budgeted training loop
```

## Headers

All public headers in `include/human/ml/`:

- `ml.h` — core types (config structs, enums, `hu_ml_train_result_t`)
- `tokenizer_ml.h` — BPE tokenizer API
- `dataloader.h` — data loading API
- `model.h` — model vtable (`hu_model_t`) + `hu_gpt_create`
- `optimizer.h` — optimizer vtable (`hu_ml_optimizer_t`) + `hu_muon_adamw_create`
- `evaluator.h` — BPB evaluation
- `experiment.h` — experiment loop API
- `prepare.h` — data preparation utilities
- `train.h` — training loop API

## Rules

- All gated behind `HU_ENABLE_ML`
- CPU-only reference implementation (ggml integration planned for Phase 2+)
- All allocations use `hu_allocator_t` — never raw malloc/free
- BPE binary format: magic "HBPE" + version + vocab + merges
- Data format: flat `int32_t` arrays in `.bin` files
- Config-driven architecture: agent modifies `hu_experiment_config_t`, not source code
- Forward pass produces logits tensor; caller frees `output.data`
- Use `HU_IS_TEST` guards for operations with real file I/O
