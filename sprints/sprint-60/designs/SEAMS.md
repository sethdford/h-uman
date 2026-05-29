# Cross-Story Integration Contract (SEAMS)

This document defines the exact symbols, signatures, and schemas that bind the four stories (US-101 through US-107) into a single closed learning loop. Each seam is bidirectional: both sides must agree on the contract, and tests must verify both sides implement it.

## Seam 1: Outcome Recording → Mining (US-101 ↔ US-102)

**Data flow**: Turn generates response → agent records `production_outcome` row → mining reads and processes.

**Schema (SQLite)**:
```sql
CREATE TABLE production_outcomes (
    id INTEGER PRIMARY KEY,
    channel TEXT NOT NULL,           -- e.g., "test", "imessage", "telegram"
    target TEXT NOT NULL,            -- contact ID or user identifier
    prompt TEXT NOT NULL,            -- user's original message
    chosen TEXT NOT NULL,            -- agent's response (the "chosen" outcome)
    response_id TEXT,                -- unique turn ID (optional but recommended)
    send_timestamp INTEGER NOT NULL  -- UNIX timestamp when response was sent
);
```

**C interface** (already exists per US-101):
```c
hu_error_t hu_agent_record_outcome(
    sqlite3 *db,
    const char *channel,
    const char *target,
    const char *prompt,
    const char *chosen,
    int64_t send_timestamp);
```

**Test assertion**: After Turn 1 agent call, query DB and verify exactly one row exists with all fields populated.

---

## Seam 2: Mining → DPO Pairs (US-102 ↔ US-105)

**Data flow**: Mining reads outcomes → produces DPO pairs → training consumes pairs.

**Schema (SQLite)**:
```sql
CREATE TABLE dpo_pairs (
    id INTEGER PRIMARY KEY,
    source TEXT NOT NULL,           -- "implicit", "user_feedback", "outbound_edit"
    prompt TEXT NOT NULL,           -- extracted from user message
    chosen TEXT NOT NULL,           -- agent's original response
    rejected TEXT NOT NULL,         -- alternate (mined or user-provided)
    created_at INTEGER NOT NULL     -- timestamp when pair was recorded
);
```

**C interface** (from US-102, used by US-105):
```c
hu_error_t hu_dpo_mine_corrections(
    hu_allocator_t *alloc,
    sqlite3 *db,
    const hu_dpo_mine_opts_t *opts,  // correction_window_sec, max_rows
    hu_dpo_mine_stats_t *stats);     // output: triples_examined, pairs_recorded, etc.
```

**Test assertion**: After mining, query DB and verify `pairs_recorded >= 1`, and each pair has distinct prompt/chosen/rejected.

---

## Seam 3: DPO Pairs → Training Input (US-105)

**Data flow**: Training subprocess reads JSON lines of DPO pairs → produces adapter checkpoint.

**JSON format** (input to `scripts/training_loop.py`):
```json
{"prompt": "Hi, how are you?", "chosen": "I'm doing well.", "rejected": "No idea.", "source": "implicit"}
{"prompt": "What time is it?", "chosen": "It's 3 PM.", "rejected": "I don't know.", "source": "implicit"}
```

**C interface** (US-105 implementation detail):
```c
hu_error_t hu_dpo_export_jsonl(
    hu_allocator_t *alloc,
    sqlite3 *db,
    const char *output_path);  // e.g., ~/.human/dpo/pairs.jsonl
```

**Command line** (AC-105.3):
```bash
scripts/training_loop.py \
    --input ~/.human/dpo/pairs.jsonl \
    --output ~/.human/adapters/lora-persona-<timestamp>.bin \
    --model mlx-community/gemma-4-31b-it-4bit \
    --iters 100 \
    --learning-rate 1e-5
```

**Test assertion**: Subprocess invocation captures arguments and verifies `--input`, `--output`, `--model`, `--iters`, `--learning-rate` match expected values.

---

## Seam 4: Training Checkpoint → Adapter File (US-105)

**Data flow**: Training script writes adapter checkpoint → swap verifies it exists.

**Filesystem contract**:
```
~/.human/adapters/lora-persona-<YYYY-MM-DD-HH-MM-SS>.bin  (e.g., lora-persona-2026-05-29-16-35-42.bin)
~/.human/training.log  (append: timestamp, pairs_count, adapter_path, training_status)
```

**C interface** (US-105 implementation):
```c
hu_error_t hu_daemon_nightly_lora_run(
    hu_allocator_t *alloc,
    sqlite3 *db,
    const hu_lora_nightly_config_t *cfg,
    hu_lora_nightly_result_t *out);  // out->new_adapter_path, out->status

typedef struct hu_lora_nightly_result {
    char new_adapter_path[512];      // absolute path to checkpoint, or empty if failed
    hu_error_t training_status;      // HU_OK, or HU_ERR_IO, etc.
    size_t pairs_mined;              // count of DPO pairs used for training
    int64_t training_duration_ms;    // wall-clock time training took
} hu_lora_nightly_result_t;
```

**Training.log schema** (plain text, append):
```
[2026-05-29 16:35:42] source=nightly_lora pairs=12 adapter=~/.human/adapters/lora-persona-2026-05-29-16-35-42.bin status=success duration_ms=4320
```

**Test assertion**: After training mock, verify checkpoint file exists at expected path and training.log contains one line with all fields.

---

## Seam 5: Cooldown Gate (US-105)

**State contract**: After a successful nightly training run, no second run for 12 hours.

**C interface** (US-105 implementation):
```c
typedef struct hu_lora_nightly_cooldown {
    int64_t last_successful_timestamp_us;  // UNIX microseconds
    static atomic_bool cooldown_active;    // (internal state)
} hu_lora_nightly_cooldown_t;

bool hu_lora_nightly_in_cooldown(int64_t now_us);
hu_error_t hu_lora_nightly_mark_success(int64_t now_us);
```

**Test assertion**: After marking success, verify next call to `in_cooldown()` returns true. Advance time by 12 hours, verify `in_cooldown()` returns false.

---

## Seam 6: Adapter Swap Request → MLX Server (US-106)

**Data flow**: After training succeeds → invoke swap → MLX server loads new adapter → next turn uses new persona.

**C interface** (existing, from `include/human/ml/mlx_admin.h`):
```c
hu_error_t hu_mlx_admin_swap_adapter(
    hu_allocator_t *alloc,
    const char *base_url,             // "http://127.0.0.1:8741/v1"
    size_t base_url_len,
    const char *adapter_path,         // absolute or ~-expandable path
    size_t adapter_path_len,
    hu_mlx_admin_swap_result_t *result);  // output: status_code, tensors_loaded, resolved_adapter_path

typedef struct hu_mlx_admin_swap_result {
    long status_code;                 // 200 = success, 4xx/5xx = failure
    size_t tensors_loaded;            // how many tensors server applied
    char *resolved_adapter_path;      // server's canonical resolved path
    size_t resolved_adapter_path_len;
} hu_mlx_admin_swap_result_t;
```

**HTTP contract** (MLX server expects POST):
```
POST http://127.0.0.1:8741/v1/adapters/swap
Content-Type: application/json

{"adapter_path": "/home/user/.human/adapters/lora-persona-2026-05-29-16-35-42.bin"}

Response (200 OK):
{
  "status": "success",
  "tensors_loaded": 4,
  "resolved_adapter_path": "/home/user/.human/adapters/lora-persona-2026-05-29-16-35-42.bin"
}
```

**Test assertion**: Call swap with mock adapter path → verify `result.status_code == 200` → verify `result.resolved_adapter_path` set to input path.

---

## Seam 7: Swap → Next Turn Uses New Persona (US-106 ↔ US-107 E2E)

**Contract**: After successful swap, the next call to `hu_agent_turn()` uses the new adapter's weights, producing measurably different output.

**Test assertion** (key to e2e closure): 
1. Turn 1 agent call → capture output O1 with signature S1 (derived from current adapter).
2. Training + swap → new adapter loaded with different weights.
3. Turn 3 agent call with same prompt → capture output O3 with signature S3.
4. Verify S3 ≠ S1 (persona changed).

**Mock model contract**: Output format includes adapter signature:
```c
// Under HU_IS_TEST, mock provider's chat_with_system returns:
// "e2e_sig_<hex_encoded_adapter_signature>"
// where hex is derived from current_adapter_id (changed after swap).
```

**Implementation detail**: Mock adapter ID is global; swap increments it; mock model output is deterministic from adapter ID.

---

## Seam 8: Error Handling Contracts (US-105, US-106, US-107)

**Mining failure**: If `hu_dpo_mine_corrections()` returns error, training does not run. Logged but does not crash daemon.

**Training failure**: If subprocess returns error code, no checkpoint is created. Logged, cooldown is NOT set (so retry can happen sooner). System continues with previous adapter.

**Swap failure**: If `hu_mlx_admin_swap_adapter()` returns error or status != 200, system logs error + continues. Previous adapter remains active. Next turn uses old persona.

**Contract table**:

| Failure point | Return value | Daemon behavior | Next turn adapter |
|---|---|---|---|
| Mining fails | HU_ERR_IO | Log error, skip training | (no change) |
| Training fails | HU_ERR_IO | Log error, don't set cooldown | previous |
| Swap fails (transport) | HU_ERR_IO | Log error | previous |
| Swap fails (HTTP 404) | HU_OK + status_code=404 | Log error | previous |
| Swap fails (HTTP 500) | HU_OK + status_code=500 | Log error | previous |

**Test assertions**: 
- `test_nightly_lora_handles_mining_error`: mining returns error, training not invoked.
- `test_nightly_lora_handles_training_error`: training returns error, cooldown not set.
- `test_adapter_swap_handles_http_error`: swap returns error, previous adapter used.
- `test_e2e_learning_loop_fallback_on_training_error`: full loop recovers from training error.

---

## Seam 9: Telemetry + Logging (AC-105.5, AC-106.6)

**Training log** (written by US-105, read/parsed by tests):
```
~/.human/logs/training.log
[2026-05-29 16:35:42] source=nightly_lora pairs=12 adapter=~/.human/adapters/lora-persona-2026-05-29-16-35-42.bin status=success duration_ms=4320
```

**Adapter swap telemetry** (written by US-106, read/parsed by tests):
```
~/.human/logs/service.log (or wherever hu_log_info goes)
[2026-05-29 16:35:45] adapter_swap old=/prev/lora-persona-old.bin new=/new/lora-persona-new.bin status=success latency_ms=42 tensors=4
```

**Counters** (per `include/human/ml/mlx_admin.h`):
```c
uint64_t hu_mlx_admin_swap_failure_counter(hu_mlx_swap_failure_reason_t reason);
// reason = HU_MLX_SWAP_FAILURE_TRANSPORT, _HTTP_4XX, _HTTP_5XX, _MISSING_ENDPOINT, _OTHER
```

**Test assertions**:
- Parse training.log, verify >= 1 entry with all fields after nightly run.
- Parse service.log or use counter API, verify swap telemetry recorded with correct paths/status.

---

## Seam 10: Test Fixture Shared Mocks

**Mock provider** (shared by US-105, US-106, US-107 tests):
```c
typedef struct {
    const char *current_adapter_id;  // "base" or "lora-persona-<timestamp>"
    // Mock model signature is deterministic from current_adapter_id
} hu_test_mock_provider_state_t;

hu_error_t hu_test_mock_chat(
    void *ctx,
    hu_allocator_t *alloc,
    const char *system_prompt, size_t system_prompt_len,
    const char *message, size_t message_len,
    const char *model, size_t model_len,
    double temperature,
    char **out, size_t *out_len);
    // out = "e2e_sig_<hex_of_adapter_id>"
```

**Mock subprocess** (shared by US-105, US-107):
```c
// Under HU_IS_TEST, hu_daemon_nightly_lora_run() calls:
hu_error_t hu_test_mock_training_subprocess(
    hu_allocator_t *alloc,
    const char *input_jsonl_path,
    const char *output_adapter_path);
    // Writes synthetic adapter file, increments mock_adapter_id counter
```

**Mock MLX swap** (shared by US-106, US-107):
```c
// Under HU_IS_TEST, hu_mlx_admin_swap_adapter() returns:
//   status_code = 200, tensors_loaded = 4, resolved_path = input path
// and increments internal current_adapter_id counter
```

**Reset helpers** (called before each test):
```c
void hu_test_reset_mock_state(void);  // Clear all mock state
void hu_test_set_mock_training_error(hu_error_t err);  // Inject error on next training call
void hu_test_set_mock_swap_error(long http_status);    // Inject HTTP error on next swap call
```

---

## Verification checklist

Before marking any story done, verify all relevant seams:

- [ ] Outcome recording schema matches Seam 1
- [ ] Mining interface matches Seam 2
- [ ] Training JSON export matches Seam 3
- [ ] Checkpoint file path matches Seam 4
- [ ] Cooldown gate is testable (Seam 5)
- [ ] Swap call matches exact signature (Seam 6)
- [ ] Mock provider signature changes deterministically (Seam 7)
- [ ] Error handling matches table (Seam 8)
- [ ] Telemetry logged to correct paths (Seam 9)
- [ ] Mock helpers exist and are reset before each test (Seam 10)

---

## Integration test ordering

The e2e test (US-107) depends on all unit tests passing:

1. US-101 unit tests (outcome recording).
2. US-102 unit tests (mining).
3. US-103 unit tests (reward model — if applicable).
4. US-104 unit tests (learning loop state — if applicable).
5. US-105 unit tests (nightly gate, mining, training mock, logging).
6. US-106 unit tests (adapter swap, error handling, telemetry).
7. **US-107 e2e test** (full loop, all components integrated).

The e2e test must run LAST and must pass all prior unit tests.
