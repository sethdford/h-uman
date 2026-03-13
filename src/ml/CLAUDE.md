# src/ml/ — ML Training Subsystem

On-device ML training for autonomous experimentation. Gated behind `HU_ENABLE_ML`.

## Architecture

```
tokenizer_bpe.c   BPE tokenizer (byte-pair encoding, train/save/load)
dataloader.c       Binary token data loader with BOS-aligned packing
prepare.c          Data preparation (tokenize files, build token_bytes lookup)
evaluator.c        BPB (bits per byte) evaluation metric
experiment.c       Autonomous experiment loop (Phase 4: wired to agent system)
gpt.c              GPT model — CPU reference (Phase 1), ggml backend (Phase 2)
muon_adamw.c       MuonAdamW optimizer (Phase 3)
```

## Headers

All public headers in `include/human/ml/`:

- `ml.h` — core types (`hu_gpt_config_t`, `hu_experiment_config_t`, etc.)
- `tokenizer_ml.h` — BPE tokenizer API
- `dataloader.h` — data loading API
- `model.h` — model vtable (`hu_model_t`)
- `optimizer.h` — optimizer vtable (`hu_ml_optimizer_t`)
- `evaluator.h` — BPB evaluation
- `experiment.h` — experiment loop API
- `prepare.h` — data preparation utilities

## Rules

- All allocations use `hu_allocator_t` — never raw malloc/free
- Phase 2+ depends on ggml (optional dependency like SQLite)
- Use `HU_IS_TEST` guards for operations with real file I/O
- Binary data format: flat arrays of `int32_t` token IDs in `.bin` files
- BPE vocab format: magic "HBPE" + version + vocab entries + merge rules
