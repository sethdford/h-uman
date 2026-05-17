# src/ml/ — ML Training Subsystem

On-device ML training for autonomous experimentation. Gated behind `HU_ENABLE_ML`.

## Testing

- `test_ml.c`

## Architecture

```
tokenizer_bpe.c     BPE tokenizer (byte-pair encoding, train/save/load)
dataloader.c        Binary token data loader with BOS-aligned packing
prepare.c           Data preparation (tokenize files, build token_bytes lookup, default config)
evaluator.c         BPB (bits per byte) evaluation metric
experiment.c        Autonomous experiment loop (config mutation, train/eval, keep/discard)
experiment_store.c  Experiment result persistence
gpt.c              GPT model: RMSNorm, RoPE, CausalSelfAttention, MLP, residual lambdas, logit soft-cap
muon_adamw.c       MuonAdamW optimizer: AdamW for scalars, Muon for matrices
train.c            Time-budgeted training loop
training_data.c    Training data management and loading
dpo.c              Direct Preference Optimization
rl_trainer.c       RL trainer vtable dispatcher (US-7.10, Init #06 phase 1)
rl_trainer_simpo.c SimPO loss head registered behind hu_rl_trainer_t
lora.c             Low-Rank Adaptation fine-tuning
agent_trainer.c    Agent-driven training orchestration
checkpoint.c       Model checkpoint save/load
cli.c              ML CLI subcommands
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

## CLI Subcommands (`human ml <subcommand>`)

| Subcommand | Handler | Description |
| --- | --- | --- |
| `train` | `hu_ml_cli_train` | Time-budgeted training on tokenized data |
| `experiment` | `hu_ml_cli_experiment` | Autonomous experiment loop (config mutation, train/eval, keep/discard) |
| `prepare` | `hu_ml_cli_prepare` | Tokenize files into binary training data |
| `prepare-conversations` | `hu_ml_cli_prepare_conversations` | Prepare conversation history as ML training data |
| `dpo-train` | `hu_ml_cli_dpo_train` | Train with Direct Preference Optimization on collected pairs |
| `lora-persona` | `hu_ml_cli_lora_persona` | Fine-tune LoRA adapter on persona example bank |
| `train-feed-predictor` | `hu_ml_cli_train_feed_predictor` | Train topic/trend predictor on feed item data |
| `status` | `hu_ml_cli_status` | Show ML subsystem status |

All handlers declared in `include/human/ml/cli.h`. Routed from `cmd_ml()` in `src/main.c`.

## Rules

- All gated behind `HU_ENABLE_ML`
- CPU-only reference implementation (ggml integration planned for Phase 2+)
- All allocations use `hu_allocator_t` — never raw malloc/free
- BPE binary format: magic "HBPE" + version + vocab + merges
- Data format: flat `int32_t` arrays in `.bin` files
- Config-driven architecture: agent modifies `hu_experiment_config_t`, not source code
- Forward pass produces logits tensor; caller frees `output.data`
- Use `HU_IS_TEST` guards for operations with real file I/O
