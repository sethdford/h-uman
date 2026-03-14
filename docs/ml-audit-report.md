# ML Training Subsystem Audit Report

**Date:** 2025-03-13  
**Scope:** `src/ml/` MuonAdamW optimizer, training loop, experiment loop  
**Reference:** Python autoresearch (sethdford/autoresearch), Keller Jordan Muon blog, NVIDIA Emerging Optimizers

---

## 1. MuonAdamW Correctness vs SOTA

### 1.1 CRITICAL: Newton-Schulz Orthogonalization Missing

**File:** `src/ml/muon_adamw.c` lines 114–124

**Issue:** The C implementation uses **row-normalization** (L2 norm per row) instead of the SOTA **Newton-Schulz iteration** for matrix orthogonalization.

**SOTA (Keller Jordan, Bernstein & Newhouse):**

- Normalize by Frobenius norm: `X = G / (||G||_F + eps)`
- Transpose if `rows > cols` for efficiency
- Newton-Schulz iteration (3–5 steps) with coefficients `(a, b, c) = (3.4445, -4.7750, 2.0315)`:
  - `A = X @ X.T`
  - `B = b*A + c*A@A`
  - `X = a*X + B@X`
- Optional spectral scaling: `sqrt(max(rows, cols))` for LR transferability from AdamW

**Current C implementation:**

```c
/* Row-normalize g (simplified orthogonalization) */
for (size_t r = 0; r < rows; r++) {
    float norm_sq = 0.0f;
    for (size_t c = 0; c < cols; c++) norm_sq += row[c] * row[c];
    float norm = sqrtf(norm_sq) + 1e-12f;
    for (size_t c = 0; c < cols; c++) row[c] /= norm;
}
```

Row normalization does **not** approximate the matrix sign function or polar factor. It is a different operation and will not match Muon’s convergence behavior.

**Confidence:** 100

---

### 1.2 Nesterov Momentum Formula May Be Incorrect

**File:** `src/ml/muon_adamw.c` lines 103–113

**Issue:** The Nesterov lookahead gradient may not match the standard formulation.

**Current code:**

```c
g->momentum_buf[i] = beta * g->momentum_buf[i] + (1.0f - beta) * grad_val;
g_buf[i] = g->grad[i] + beta * g->momentum_buf[i];
```

Standard Nesterov (Keller Jordan Muon): the effective update direction is `μ*v_t + (1-μ)*g_t` where `v_t = μ*v_{t-1} + g_t`. That is `g_t + μ*(v_t - g_t) = μ*v_t + (1-μ)*g_t`.

The current code uses `g_t + β*v_t`, which expands to `g_t + β*(β*v_{t-1} + (1-β)*g_t) = g_t + β²*v_{t-1} + β(1-β)*g_t`. This is not the same as `μ*v_t + (1-μ)*g_t`.

**Confidence:** 85

---

### 1.3 Muon Weight Decay Differs from SOTA

**File:** `src/ml/muon_adamw.c` lines 129–135

**Issue:** SOTA Muon uses **decoupled weight decay** (Frank-Wolfe style). The C code uses a custom mask: `mask = (g_buf * param >= 0)`.

**Current:** `param -= lr * wd * param * mask`  
**SOTA:** Decoupled weight decay applied separately, typically `param *= (1 - lr * wd)` or a constrained LMO step.

The mask logic is non-standard and may change optimization dynamics.

**Confidence:** 90

---

### 1.4 AdamW: Correct

**File:** `src/ml/muon_adamw.c` lines 60–89

- Bias correction: `bias1 = 1 - β1^t`, `bias2 = 1 - β2^t` ✓
- `adam_beta1`/`adam_beta2` from config ✓
- Weight decay skipped for embedding/unembedding ✓
- Weight decay applied as `param *= (1 - lr * wd)` for scalars ✓

**Confidence:** 95

---

### 1.5 Adam Beta Defaults

**File:** `include/human/ml/ml.h`, `src/ml/prepare.c`

Default `adam_beta1 = 0.8`, `adam_beta2 = 0.95` matches the stated autoresearch setup.

---

## 2. Training Loop Correctness

### 2.1 Gradient Scaling: Correct

**File:** `src/ml/train.c` lines 131–136

- `accum_scale = 1.0f / accum_steps` ✓
- `grad_scale = accum_scale / n_valid` ✓
- Effective scaling: `1 / (accum_steps * n_valid)` ✓

**Confidence:** 95

---

### 2.2 IMPORTANT: Gradient Scaling When n_valid = 0

**File:** `src/ml/train.c` lines 132–136

When `n_valid == 0`, gradients are not scaled (block is skipped). `d_logits` stays zero from `memset`. Backward receives zeros, so no gradient is added. An optimizer step is still taken with zero gradients, so only momentum/state is updated. This is a valid but potentially surprising edge case.

**Confidence:** 90

---

### 2.3 LR Schedule: Correct

**File:** `src/ml/train.c` lines 159–167, `src/ml/muon_adamw.c` lines 41–57

- Warmup / constant / warmdown phases ✓
- `hu_ml_lr_schedule` uses `warmup_ratio`, `warmdown_ratio`, `final_lr_frac` ✓
- Progress based on `elapsed / t_budget` ✓

**Confidence:** 95

---

### 2.4 Cross-Entropy Loss and Gradient: Correct

**File:** `src/ml/train.c` lines 108–116

- Softmax with max subtraction for numerical stability ✓
- `d_logits[target] -= 1.0f` (softmax - onehot) ✓
- Loss: `-log(p_target)` ✓

**Confidence:** 95

---

### 2.5 Time-Budget Termination: Correct

**File:** `src/ml/train.c` lines 176–178

- Check `elapsed >= t_budget` after each batch ✓

---

### 2.6 Ordering: Correct

**File:** `src/ml/train.c`

- `zero_grad` before loop ✓
- `forward` → loss/grad → `backward` → `step` ✓
- `zero_grad` after each optimizer step ✓

---

### 2.7 IMPORTANT: Flush Remaining Micro-Steps

**File:** `src/ml/train.c` lines 183–187

When the loop exits (time budget or dataloader end), any remaining accumulated gradients are applied with `step` and `zero_grad`. This is correct.

---

## 3. Experiment Loop Correctness

### 3.1 Config Mutation: Potential Division by Zero

**File:** `src/ml/experiment.c` line 113

```c
cfg->gpt.n_head = new_dim / cfg->gpt.head_dim;
```

If `head_dim == 0`, this divides by zero. Default config has `head_dim = 128`, and `head_dim` is not mutated, so this is unlikely in practice. `hu_gpt_create` rejects `head_dim == 0`, but the crash would occur in `mutate_config` before model creation.

**Confidence:** 75

---

### 3.2 Config Mutation: Invalid n_embd / n_head Consistency

**File:** `src/ml/experiment.c` lines 109–115

```c
size_t new_dim = cfg->gpt.n_embd + ((xorshift32(&seed) % 2) ? 128 : -128);
if (new_dim >= 128 && new_dim <= 2048) {
    cfg->gpt.n_embd = new_dim;
    cfg->gpt.n_head = new_dim / cfg->gpt.head_dim;
    cfg->gpt.n_kv_head = cfg->gpt.n_head;
}
```

If `new_dim` is not divisible by `head_dim`, `n_embd != n_head * head_dim` and `hu_gpt_create` will fail (line 748). Example: `new_dim = 200`, `head_dim = 128` → `n_head = 1`, `n_head * head_dim = 128 ≠ 200`.

**Fix:** Align `new_dim` to `head_dim`, e.g. `new_dim = (new_dim / head_dim) * head_dim`.

**Confidence:** 90

---

### 3.3 Keep/Discard/Crash Logic: Correct

**File:** `src/ml/experiment.c` lines 266–286

- CRASH: `result.status == HU_EXPERIMENT_CRASH` → no config update ✓
- KEEP: `val_bpb < best_bpb` → update best_config ✓
- DISCARD: `val_bpb >= best_bpb` → revert to best_config ✓

---

### 3.4 Convergence Detection: Correct

**File:** `src/ml/experiment.c` lines 294–296

- `convergence_threshold > 0` and `best_bpb <= threshold` → break ✓

---

### 3.5 IMPORTANT: Experiment Store Open Error Ignored

**File:** `src/ml/experiment.c` lines 250–251

```c
if (config->results_path)
    hu_experiment_store_open(alloc, config->results_path, &store);
```

The return value is ignored. If `hu_experiment_store_open` fails, `store` stays NULL. `hu_experiment_store_save(store, &result)` then returns `HU_ERR_INVALID_ARGUMENT` (also ignored). No crash, but results are not persisted and the caller is not informed.

**Confidence:** 95

---

### 3.6 Resource Cleanup: Correct

**File:** `src/ml/experiment.c` `run_single_experiment`

- Model, optimizer, train_dl, val_dl are freed on all error paths ✓
- Order: val_dl → train_dl → optimizer → model ✓

---

## 4. Memory Bugs

### 4.1 CRITICAL: Wrong Free in train.c on d_logits Allocation Failure

**File:** `src/ml/train.c` lines 91–95

```c
if (!d_logits) {
    alloc->free(alloc->ctx, logits, logits_size);
    hu_ml_batch_free(alloc, &batch);
    return HU_ERR_OUT_OF_MEMORY;
}
```

`logits` is produced by `model->vtable->forward` and is `output.data`. The model allocates it with its allocator. `hu_ml_train` receives `alloc` and uses it to free. If the model and `hu_ml_train` share the same allocator (as in `run_single_experiment`), this is fine. If a different allocator is ever passed to `hu_ml_train`, this free would be wrong. Currently the same allocator is used.

**Confidence:** 70 (correct for current usage; fragile if allocator contract changes)

---

### 4.2 step_muon: g_buf Leak on Early Return

**File:** `src/ml/muon_adamw.c` lines 108–139

`g_buf` is allocated at 108 and freed at 139. There is no early return between allocation and free, so no leak.

**Confidence:** 95

---

### 4.3 hu_param_group_t Allocation in add_param

**File:** `src/ml/muon_adamw.c` lines 267–276

On realloc failure, `new_groups` is NULL and the function returns without freeing the old `m->groups`. The realloc contract is implementation-dependent; some realloc implementations free the old block on failure, others do not. If the allocator’s realloc does not free on failure, this could leak.

**Confidence:** 75

---

## 5. Numerical Issues

### 5.1 Softmax/Cross-Entropy: Stable

**File:** `src/ml/train.c` lines 109–114

- Max subtraction before exp ✓
- `sum` in double ✓
- `di[target] -= 1.0f` after softmax ✓

---

### 5.2 Division by Zero in Muon Row Normalization

**File:** `src/ml/muon_adamw.c` line 120

```c
float norm = sqrtf(norm_sq) + 1e-12f;
```

Guard against zero norm is present ✓

---

### 5.3 AdamW Denom

**File:** `src/ml/muon_adamw.c` line 79

```c
float denom = sqrtf(g->exp_avg_sq[i] / bias2) + eps;
```

`bias2` can be 0 when `t == 0` (but `step_count` is incremented before use, so `t >= 1`). `bias2 = 1 - β2^t` is 0 only for `β2 = 1`, which is not used. Safe in practice.

---

### 5.4 NaN Propagation Check

**File:** `src/ml/train.c` lines 124–130

- `batch_loss != batch_loss` detects NaN ✓
- `batch_loss > 1e6` detects blow-up ✓
- Resources freed and loop exited ✓

---

## 6. Summary Table

| Severity  | File         | Line(s) | Issue                                                        |
| --------- | ------------ | ------- | ------------------------------------------------------------ |
| CRITICAL  | muon_adamw.c | 114–124 | Row normalization instead of Newton-Schulz orthogonalization |
| IMPORTANT | muon_adamw.c | 103–113 | Nesterov formula may not match SOTA                          |
| IMPORTANT | muon_adamw.c | 129–135 | Non-standard Muon weight decay                               |
| IMPORTANT | train.c      | 132–136 | n_valid=0 edge case (zero-grad step)                         |
| IMPORTANT | experiment.c | 250–251 | hu_experiment_store_open return value ignored                |
| IMPORTANT | experiment.c | 109–115 | n_embd mutation can violate n_embd = n_head \* head_dim      |
| MEDIUM    | experiment.c | 113     | Potential division by zero if head_dim == 0                  |
| MEDIUM    | muon_adamw.c | 267–276 | Possible leak on realloc failure                             |
| LOW       | train.c      | 91–95   | Free uses caller’s allocator (correct for current usage)     |

---

## 7. Recommendations

1. **Muon SOTA alignment:** Implement Newton-Schulz iteration with coefficients `(3.4445, -4.7750, 2.0315)`, Frobenius normalization, and optional spectral scaling.
2. **Nesterov:** Verify and, if needed, change to `g_buf = (1-β)*grad + β*momentum_buf` (or equivalent standard Nesterov form).
3. **Experiment store:** Check `hu_experiment_store_open` return value and propagate or handle errors.
4. **Config mutation:** Ensure `new_dim` is divisible by `head_dim`, or round to a valid value.
5. **head_dim guard:** Add a check in `mutate_config` to avoid `head_dim == 0`.
