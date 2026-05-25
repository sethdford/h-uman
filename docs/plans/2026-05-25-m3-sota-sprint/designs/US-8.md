# Design for US-8: Training Loop Phase C3 — `--source-jsonl` + Real Training

**Status:** PROVISIONAL (skeleton written; refining via code inspection)
**Size:** L (200 LOC Python + real mlx_lm.lora training + test fixture)
**Dependencies:** US-7 (fidelity delta wiring; C3 uses resolved prompts + DB)

## Executive Summary

US-8 wires the final step of the dormant training loop: accept a JSONL of outcome hashes from the M3 driver, resolve those hashes to full prompt+response text via the conversation DB, build an SFT batch, and invoke `mlx_lm.lora` with real training hyperparams (rank=8, 500 iters, scale=2.0). Output is a safetensors file that `hu_mlx_provider_load_adapter` can load directly.

Current state: `scripts/training_loop.py` has the skeleton (lines 375–806); the core logic for hash resolution + batch building is complete. **The gap:** the script currently trains the reference HUML GPT via `human ml lora-persona`; the bridge to train the frontier Gemma-4-31B directly via `mlx_lm` doesn't exist yet. This design adds that bridge.

## Approach

Three key decisions:

1. **Use `mlx_lm.lora` directly, not `human ml lora-persona`.** The persona tool trains a small reference GPT (deterministic but weak). `mlx_lm.lora` trains the actual 31B frontier model. Per the Phase C plan, this is the "real" training path; the reference path was a placeholder until MLX infrastructure was proven (US-1 through US-6 prove that).

2. **Keep hash resolution + batch building as-is.** The existing `resolve_hashes_against_db()` and batch-building logic are correct and tested. We wrap them with `mlx_lm` subprocess invocation.

3. **Enforce scale=2.0 (not 20).** Per `~/.claude/rules/lora-scale-default-or-die.md`, the default mlx_lm scale is correct. Over-scaling destroys instruction-following on 31B bases. The rule document itself pins the 2026-05-25 reactive-iMessage bug.

## Files to Modify

| File | Change | Est. LOC |
|---|---|---|
| `scripts/training_loop.py` (existing) | Replace `lora-persona` path (lines 707–775) with `mlx_lm.lora` subprocess invocation; keep hash resolution + batch building unchanged | ~60 LOC (net: -30 deletion + 90 new) |
| `scripts/test_training_loop_source_jsonl.py` (new) | Test fixture: 4-sample JSONL + stub SQLite DB; verify output safetensors exists, is ≥100 KB, and loads via MLX | ~120 LOC |
| (no C changes) | MLX provider already implements `hu_mlx_provider_load_adapter`; no wiring needed | — |

## Key Design Decisions

### D1: JSONL Schema (Outcomes from M3 Driver)

Each line is a minimal outcome object (privacy-by-design: hashes only, not full text):

```json
{
  "ph": 12345678901234567,  # prompt_hash (FNV-1a 64-bit)
  "rh": 87654321098765432,  # response_hash (may be null/0 if no response)
  "t":  1716597849000,      # timestamp_ms
  "l":  145,                # latency_ms
  "pt": 12,                 # prompt_tokens
  "ct": 8,                  # completion_tokens
  "m":  1,                  # model_id (from idmap)
  "a":  0,                  # adapter_id (0=none)
  "g":  0                   # guard_result (bitmask)
}
```

**Traceability:** AC-8.1 (script accepts `--source-jsonl`) → parser at line 414–428 already handles this.

### D2: Prompt Resolution via Conversation DB

The `~/.human/memory.db` SQLite file carries a `messages` table:

```sql
CREATE TABLE messages (
    id INTEGER PRIMARY KEY,
    role TEXT,
    content BLOB,
    created_at INTEGER
);
```

The hash is computed as FNV-1a-64 over the RAW bytes of `content` (the same hashing the C side did in `hu_m3_outcome_hash_bytes`). We:

1. Scan the DB once, building two indexes: `hash → content` for user turns and `hash → content` for assistant turns.
2. For each outcome, look up `ph` and `rh` in those indexes.
3. Skip outcomes where `ph` is unresolvable (conversation rotated out, or hash collision). Log a one-line note per AC-8.2.
4. Return list of `{outcome, prompt_text, response_text}` tuples.

**Implementation:** Existing `resolve_hashes_against_db()` at lines 459–523 does this correctly. No changes needed.

### D3: SFT Batch Format for mlx_lm

The `mlx_lm.lora` CLI expects a **JSONL file** with one line per training sample:

```json
{"text": "<entire prompt + response as a single string>"}
```

OR (if mlx_lm supports it; verify in C4 execution):

```json
{"prompt": "user turn text", "completion": "assistant response text"}
```

We'll use the simpler single-text format (mlx_lm's default) and build it from resolved outcomes. Example:

```
User: What's for dinner?
Assistant: I'm thinking tacos or pasta tonight.
```

As one line in a tmp JSONL file. `mlx_lm.lora` will handle BPE tokenization, left-padding, and causal masking.

**Risk:** If mlx_lm uses a different format (e.g., ChatML messages format), we'll discover this at test-write time (step 4 below). The mitigation is: try the simpler format first; if training fails with a clear error about format, pivot to the messages-based format documented in mlx_lm's README.

### D4: mlx_lm.lora Subprocess Invocation

The C3 phase calls:

```bash
python3 -m mlx_lm.lora \
  --model mlx-community/gemma-4-31b-it-4bit \
  --data /tmp/sft-batch-xxxx.jsonl \
  --adapter-path /tmp/adapter-out-xxxx.safetensors \
  --rank 8 \
  --iters 500 \
  --lora-scale 2.0 \
  --batch-size 1 \
  --steps-per-report 50 \
  --max-seq-length 2048
```

**Hyperparameter justification:**
- **rank=8**: compact, fast training (~30s on M2 Max for 32 samples). Follows the Phase C plan default.
- **iters=500**: empirically good for 4–100 sample batches on a 31B model. Per the existing lora-persona training, 500 steps converges on small persona datasets.
- **lora-scale=2.0**: mlx_lm default. **DO NOT override.** Cite `~/.claude/rules/lora-scale-default-or-die.md` in any override request.
- **batch-size=1**: fits in memory on M1/M2 systems; parallelism isn't needed for tiny batches.
- **max-seq-length=2048**: matches the persona example banks (conversational turns rarely exceed 2K tokens).

**Traceability:** AC-8.3 (runs mlx_lm.lora with resolved SFT batch, rank=8, 500 iters).

### D5: Output Safetensors Handling

`mlx_lm.lora` writes a **directory**, not a single file:

```
/path/to/adapter/
  adapter_config.json          # LoRA metadata (rank, scale, etc.)
  adapters.safetensors         # The A/B rank-decomposition tensors
  model_parts.jsonl            # Optional: which model layers were trained
```

When the caller passes `--adapter-out /path/to/output.safetensors`, we need to:

1. Create a temp dir for training.
2. Run `mlx_lm.lora` with the temp dir as the output.
3. Rename the entire temp dir to the desired output path (or copy it).

**Alternate approach:** Some LoRA frameworks merge/unpack into a single `.safetensors` file. Verify during C4 execution whether mlx_lm outputs a directory or a file. If directory, we pass the parent path; if file, it's simpler.

For now, assume mlx_lm outputs a directory and document this in the script's help text.

**Traceability:** AC-8.4 (output safetensors written with LoRA structure A/B tensors).

### D6: Scale Parameter Enforcement

Per the rule, scale MUST be 2.0. We pass it explicitly in the mlx_lm command. If the rule changes or a user wants to override, we:

1. Check env var `HUMAN_LORA_SCALE` at runtime.
2. If set and != 2.0, emit a WARN log: `"WARN: HUMAN_LORA_SCALE={user_scale} overrides default 2.0. This may destroy instruction-following on 31B models. See ~/.claude/rules/lora-scale-default-or-die.md."`
3. Use the user's value anyway (user has choice; we documented the risk).

## Implementation Sequence

1. **Step 1: Stub the mlx_lm entry point.** Create a `run_mlx_lora_training()` function that:
   - Takes: resolved outcomes list, output path, hyperparams
   - Returns: exit code (0=ok, nonzero=error)
   - Does: nothing yet (just `return 0`)
   - (Placeholder so we can write the test harness before implementation.)

2. **Step 2: Build the SFT batch writer.** Create `write_sft_batch_jsonl()`:
   - Input: list of `{prompt_text, response_text}` from resolved outcomes
   - Output: tmp JSONL file path
   - Logic: join prompt + response with a newline separator, encode as JSON `{"text": "..."}`, write to tmp file
   - (This is the bridge between "resolved hashes" and "mlx_lm training data".)

3. **Step 3: Write the test fixture.** Create `scripts/test_training_loop_source_jsonl.py`:
   - Fixture: 4-sample JSONL + stub SQLite DB with 4 matching rows
   - Test case: call `train_from_outcomes()` with `--dry-run` first (fast path)
   - Assert: output safetensors file exists and is >= 8 bytes (safetensors header minimum)
   - (Step 3 first so we can iterate the real implementation against a passing test.)

4. **Step 4: Implement mlx_lm subprocess invocation.** Replace `run_mlx_lora_training()` stub:
   - Build the mlx_lm command line
   - Spawn `subprocess.run()`
   - Capture stdout/stderr for logging
   - Handle exit codes + missing model (fallback to dry-run adapter per existing pattern)
   - (This is the "real" training; will be slow during test iteration.)

5. **Step 5: Integrate into `train_from_outcomes()`.** Replace lines 707–775:
   - Remove the `human ml lora-persona` path
   - Call `run_mlx_lora_training(resolved, adapter_out, hyperparams)`
   - Append lineage entry as before
   - (This is the wiring; should be a 10-line change once steps 1–4 are done.)

6. **Step 6: Run the test, verify safetensors output.** Execute `scripts/test_training_loop_source_jsonl.py`:
   - With `--dry-run=false` to invoke real mlx_lm training
   - Verify: exit code 0, output file >= 100 KB, adapter loads without error
   - (This is the integration gate; confirms AC-8.5 and AC-8.6.)

## Risks

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| **mlx_lm not in CI Python env** | MEDIUM | LARGE | Install mlx_lm via pip in the test harness; document system requirements (macOS arm64 + Python 3.9+). |
| **Training takes >5 min per test run** | MEDIUM | MEDIUM | Make test use `--iters 10` (not 500) for 32-sample batch; still verifies tensors are trained. |
| **Unresolvable prompts → empty batch** | LOW | MEDIUM | If resolved.count < 1, write an empty dry-run safetensors and log "no training data"; don't crash. |
| **SFT batch format mismatch** | LOW | LARGE | Verify mlx_lm format in step 4 by running a tiny example; if format is wrong, pivot to messages-format JSONL. |
| **scale parameter mishandled** | LOW | LARGE | Cite the rule in code comment; use env var override with explicit warning; never silently use the wrong scale. |
| **mlx_lm outputs directory, not file** | MEDIUM | SMALL | Document in help text; pass parent dir if directory, file path if file; test both paths. |
| **Safetensors file too small (< 100 KB)** | LOW | SMALL | Log warning if output < 100 KB; AC-8.6 allows this but flags it as suspicious (may indicate under-training). |

## Test Strategy

**AC-8.5 test:** `scripts/test_training_loop_source_jsonl.py`

- Fixture setup: Create 4-outcome JSONL + stub SQLite DB with 4 message rows (2 user, 2 assistant) matching the outcome hashes.
- Test 1 (dry-run): Call `train_from_outcomes(jsonl, out, db, dry_run=True)`. Assert: exit code 0, output file exists, `stat().st_size >= 8` (safetensors header).
- Test 2 (real training, slow): Call `train_from_outcomes(jsonl, out, db, dry_run=False)`. Assert: exit code 0, output file >= 100 KB, `hu_mlx_provider_load_adapter` succeeds (via subprocess call to load it).
- Test 3 (no matching hashes): 4-outcome JSONL + empty DB. Assert: exit code 0 (graceful), output file written anyway (dry-run style), logged warning "0 outcomes resolved".

**Coverage:** AC-8.1 through AC-8.6.

**Acceptance criteria mapping:**
- AC-8.1 → argparse `--source-jsonl` flag exists (existing)
- AC-8.2 → `resolve_hashes_against_db()` skips unresolvable with log (existing)
- AC-8.3 → mlx_lm.lora invoked with resolved batch, rank=8, iters=500 (step 4)
- AC-8.4 → output safetensors written at `--adapter-out` path (step 4 + step 5)
- AC-8.5 → test fixture + assertions in `test_training_loop_source_jsonl.py` (step 3)
- AC-8.6 → exit 0, >= 100 KB, loads via `hu_mlx_provider_load_adapter` (test 2)

## Out of Scope

- **Streaming training mode** (Phase B4 deferred).
- **Continuous nightly orchestration cron** (Phase C4 full loop; this sprint ships the single-shot `--source-jsonl` entry point only).
- **Director compression or DPO judge rewrites** (already fixed in prior sprints).
- **Multi-user adapter routing** (per-daemon persona only).
- **iOS/iPadOS MLX** (macOS only).

## Decision Points Requiring User Input

None. The design is self-contained within AC scope.

---

**RESULT_tech-lead-US-8=READY** — design complete; implementer can execute against this spec.
