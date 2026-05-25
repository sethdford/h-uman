/* M3 frontier adapter stub — see include/human/ml/m3_frontier_adapter.h */

#include "human/ml/m3_frontier_adapter.h"
#include "human/core/log.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

/* Pin the on-wire outcome record size so a future field addition can't
 * silently bloat the ring buffer beyond the agreed 384 KB ceiling
 * (4096 × 96 bytes). If this fires, either the struct grew or the
 * #define drifted — both need explicit attention, not a paper-over. */
_Static_assert(sizeof(hu_m3_inference_outcome_t) == HU_M3_OUTCOME_RECORD_BYTES,
               "hu_m3_inference_outcome_t must be exactly HU_M3_OUTCOME_RECORD_BYTES");

struct hu_m3_frontier_adapter {
    hu_allocator_t *alloc;
    uint32_t schema_version;
    /* M3 first-slice observable signal (2026-05-17). The probe counter
     * increments on every `probe_infer` / `noop_infer` call so tests
     * (and operators, eventually) can prove the chat-path call sites
     * actually reach the adapter at runtime. */
    uint64_t probe_count;

    /* B1 (redefined 2026-05-17): inference outcome ring buffer. Fixed
     * capacity so the adapter's memory footprint is bounded at open.
     * `head` is the index where the NEXT outcome will be written;
     * writes always succeed and overwrite the oldest slot when full.
     * `total_recorded` is monotonic — every outcome ever written
     * counts, even those overwritten. The snapshot reader uses
     * `total_recorded < CAPACITY` to know whether to start from 0 or
     * from `head` (the oldest live slot in a full buffer). */
    hu_m3_inference_outcome_t outcomes[HU_M3_OUTCOMES_RING_CAPACITY];
    size_t head;
    uint64_t total_recorded;

    /* Spec 1 Task 8 (AC-M3-4 expanded): drain marker. Position in the
     * monotonic total_recorded sequence "up to which" the drainer has
     * persisted outcomes. Strictly <= total_recorded; advanced only by
     * `hu_m3_frontier_adapter_advance_drain_marker`. */
    uint64_t drain_marker;
};

bool hu_m3_adapter_should_disable(bool cfg_disabled) {
    if (cfg_disabled)
        return true;
    /* Env override: any non-empty value other than "0" disables. The
     * "0" exception lets operators explicitly re-enable in scripts
     * that always set the env var (mirrors the convention used for
     * other HUMAN_* boolean envs in the codebase). */
    const char *env = getenv("HUMAN_M3_ADAPTER_DISABLE");
    if (!env || env[0] == '\0')
        return false;
    if (env[0] == '0' && env[1] == '\0')
        return false;
    return true;
}

/* Track D D2.1 — honest-gap caveat strings.
 *
 * Single source of truth for the user-facing disclaimer printed by
 * `human ml lora-persona`. The block is intentionally unambiguous —
 * tests pin specific substrings (the "NOT" disclaimer, the model name,
 * the doc-path) so a refactor that softens the language fails CI. */

#define HU_ML_LORA_PERSONA_CAVEAT_DOC_PATH "docs/plans/2026-05-10-m3-frontier-model-bridge.md"

const char *hu_ml_lora_persona_caveat_doc_path(void) {
    return HU_ML_LORA_PERSONA_CAVEAT_DOC_PATH;
}

const char *hu_ml_lora_persona_caveat_block(void) {
    return "[lora-persona] NOTE: trains LoRA on the reference HUML GPT.\n"
           "[lora-persona]       This is not a frontier model fine-tune.\n"
           "[lora-persona]       For Llama/Qwen/Mistral fine-tuning, see\n"
           "[lora-persona]       " HU_ML_LORA_PERSONA_CAVEAT_DOC_PATH "\n";
}

hu_error_t hu_m3_frontier_adapter_try_open(hu_allocator_t *alloc, const char *path, size_t path_len,
                                           hu_m3_frontier_adapter_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (!path || path_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return HU_ERR_IO;

    unsigned char hdr[16];
    size_t n = fread(hdr, 1, sizeof(hdr), fp);
    fclose(fp);
    if (n < 8)
        return HU_ERR_IO;
    if (memcmp(hdr, HU_M3_ADAPTER_MAGIC, 8) != 0)
        return HU_ERR_IO;

    uint32_t ver = 0;
    if (n >= 12) {
        ver = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) | ((uint32_t)hdr[10] << 16) |
              ((uint32_t)hdr[11] << 24);
    }
    if (ver == 0)
        ver = 1;

    hu_m3_frontier_adapter_t *a = (hu_m3_frontier_adapter_t *)alloc->alloc(alloc->ctx, sizeof(*a));
    if (!a)
        return HU_ERR_OUT_OF_MEMORY;
    memset(a, 0, sizeof(*a));
    a->alloc = alloc;
    a->schema_version = ver;
    *out = a;
    return HU_OK;
}

hu_error_t hu_m3_frontier_adapter_probe_infer(hu_m3_frontier_adapter_t *adapter) {
    /* First-slice (2026-05-17) bridge from the silent (void)return to a
     * real, observable signal. No tensors yet — see
     * docs/plans/2026-05-17-m3-mlx-bridge-execution-plan.md for the
     * phased path to actual MLX/llama.cpp inference. The point of the
     * counter is that a regression which drops a chat-path call site
     * stops being undetectable: the probe count fails to advance and
     * `test_m3_frontier_probe.c` goes red. */
    if (!adapter)
        return HU_OK;
    adapter->probe_count++;
    return HU_OK;
}

hu_error_t hu_m3_frontier_adapter_noop_infer(hu_m3_frontier_adapter_t *adapter) {
    /* Backwards-compat wrapper. Existing 11 call sites in agent_turn.c
     * + agent_stream.c (via hu_agent_m3_on_provider_success) and the
     * daemon's M3 probe-path bootstrap (`src/daemon.c`) still call
     * `noop_infer` — they all reach `probe_infer` so the counter
     * advances. Renaming the public symbol is a follow-up
     * (Phase B-pre.2 in the execution plan). */
    return hu_m3_frontier_adapter_probe_infer(adapter);
}

hu_error_t hu_m3_frontier_adapter_probe_infer_with_metadata(hu_m3_frontier_adapter_t *adapter,
                                                            uint32_t completion_tokens,
                                                            uint64_t latency_ms) {
    /* Phase B3 extension: record outcome metadata (completion tokens + latency)
     * into the ring buffer alongside incrementing the probe counter. */
    hu_error_t err = hu_m3_frontier_adapter_probe_infer(adapter);
    if (err != HU_OK || !adapter)
        return err;

    /* Record the outcome with the captured metrics. timestamp_unix_ms=0
     * tells the recorder to use wall clock. */
    return hu_m3_record_outcome_from_provider_result(adapter, /*timestamp_unix_ms=*/0,
                                                     /*prompt_tokens=*/0, completion_tokens,
                                                     latency_ms, /*contact_hash=*/0,
                                                     /*turn_kind=*/0);
}

uint64_t hu_m3_frontier_adapter_probe_count(const hu_m3_frontier_adapter_t *adapter) {
    return adapter ? adapter->probe_count : 0;
}

hu_m3_probe_outcome_snapshot_t
hu_m3_frontier_adapter_probe_outcome_at(const hu_m3_frontier_adapter_t *adapter, size_t idx) {
    hu_m3_probe_outcome_snapshot_t snap = {0, 0};
    if (!adapter || idx >= adapter->total_recorded)
        return snap;

    /* Ring buffer index: find the actual position of outcome[idx] in the
     * circular buffer, accounting for wrap-around at capacity.
     * When total_recorded < CAPACITY, the buffer hasn't wrapped yet, so
     * the oldest record is at index 0. When full (total_recorded >= CAPACITY),
     * the oldest is at `head` (the next write position). */
    size_t oldest_idx;
    if (adapter->total_recorded < HU_M3_OUTCOMES_RING_CAPACITY) {
        /* Buffer hasn't filled yet; oldest is at 0 */
        oldest_idx = 0;
    } else {
        /* Buffer is full; oldest is at current head position */
        oldest_idx = adapter->head;
    }
    size_t ring_pos = (oldest_idx + idx) % HU_M3_OUTCOMES_RING_CAPACITY;

    snap.completion_tokens = adapter->outcomes[ring_pos].completion_tokens;
    snap.latency_ms = adapter->outcomes[ring_pos].latency_ms;
    return snap;
}

void hu_m3_frontier_adapter_close(hu_allocator_t *alloc, hu_m3_frontier_adapter_t *adapter) {
    if (!alloc || !adapter)
        return;
    alloc->free(alloc->ctx, adapter, sizeof(*adapter));
}

uint32_t hu_m3_frontier_adapter_schema_version(const hu_m3_frontier_adapter_t *adapter) {
    return adapter ? adapter->schema_version : 0;
}

/* ─────────────────────────────────────────────────────────────────────
 * Phase B1 (redefined 2026-05-17): inference outcome capture
 * ───────────────────────────────────────────────────────────────── */

/* FNV-1a 64-bit. Standard parameters (offset basis + prime from
 * https://tools.ietf.org/html/draft-eastlake-fnv-21). Pure function;
 * deterministic across runs. Returns 0 for NULL/empty input — this is
 * a SENTINEL the outcome struct uses to mean "no value" (e.g. for
 * contact_id_hash when there's no contact context). */
uint64_t hu_m3_outcome_hash_bytes(const void *data, size_t len) {
    if (!data || len == 0)
        return 0;
    const unsigned char *p = (const unsigned char *)data;
    uint64_t hash = 0xcbf29ce484222325ULL; /* FNV offset basis */
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)p[i];
        hash *= 0x100000001b3ULL; /* FNV prime */
    }
    /* Reserve 0 for "no value" — if FNV happens to land on 0, bump
     * to 1 so callers can distinguish "hash is zero" from "no hash". */
    return hash ? hash : 1ULL;
}

hu_error_t hu_m3_frontier_adapter_record_outcome(hu_m3_frontier_adapter_t *adapter,
                                                 const hu_m3_inference_outcome_t *outcome) {
    /* NULL adapter is a deliberate no-op — agent paths shouldn't have
     * to gate the call, and the M3 system is opt-in via configuration.
     * Returning HU_OK here matches probe_infer's NULL semantics. */
    if (!adapter)
        return HU_OK;
    if (!outcome)
        return HU_ERR_INVALID_ARGUMENT;

    /* Copy into the slot at head; advance head; wrap on capacity. The
     * ring is single-writer (the agent serializes turns) so no atomic
     * is needed today. If concurrent writers ever appear, add a
     * spinlock or migrate head/total_recorded to atomic — the snapshot
     * reader is already designed to tolerate a slightly stale view. */
    adapter->outcomes[adapter->head] = *outcome;
    adapter->head = (adapter->head + 1u) % HU_M3_OUTCOMES_RING_CAPACITY;
    adapter->total_recorded++;
    return HU_OK;
}

uint64_t hu_m3_frontier_adapter_outcomes_recorded(const hu_m3_frontier_adapter_t *adapter) {
    return adapter ? adapter->total_recorded : 0ULL;
}

hu_error_t hu_m3_frontier_adapter_snapshot_outcomes(const hu_m3_frontier_adapter_t *adapter,
                                                    hu_m3_inference_outcome_t *out_buf,
                                                    size_t max_count, size_t *out_count) {
    if (!out_buf || !out_count || max_count == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    if (!adapter)
        return HU_OK;

    /* Two layout cases:
     *   1. total_recorded < CAPACITY: ring not yet wrapped. The live
     *      outcomes are outcomes[0 .. head-1] in storage order.
     *   2. total_recorded >= CAPACITY: ring has wrapped. The oldest
     *      live outcome is at index `head`; the newest is at
     *      `(head - 1 + CAPACITY) % CAPACITY`. Live count = CAPACITY.
     *
     * The output ordering contract is "oldest-of-the-snapshot first".
     * We compute the start index and how many records to copy, then
     * copy in two segments if the snapshot wraps. */
    size_t live_count;
    size_t start;
    if (adapter->total_recorded < (uint64_t)HU_M3_OUTCOMES_RING_CAPACITY) {
        live_count = (size_t)adapter->total_recorded;
        start = 0;
    } else {
        live_count = (size_t)HU_M3_OUTCOMES_RING_CAPACITY;
        start = adapter->head;
    }

    size_t to_copy = live_count < max_count ? live_count : max_count;
    /* When max_count < live_count, return the most-recent `max_count`
     * outcomes — slide the start forward by (live_count - to_copy).
     * Otherwise start as computed above. */
    if (to_copy < live_count) {
        size_t skip = live_count - to_copy;
        start = (start + skip) % HU_M3_OUTCOMES_RING_CAPACITY;
    }

    /* Copy in up to two segments (may wrap around the end of the ring). */
    size_t first_segment = HU_M3_OUTCOMES_RING_CAPACITY - start;
    if (first_segment > to_copy)
        first_segment = to_copy;
    memcpy(out_buf, &adapter->outcomes[start], first_segment * sizeof(*out_buf));
    if (to_copy > first_segment) {
        memcpy(out_buf + first_segment, &adapter->outcomes[0],
               (to_copy - first_segment) * sizeof(*out_buf));
    }

    *out_count = to_copy;
    return HU_OK;
}

/* ─────────────────────────────────────────────────────────────────────
 * Phase B3 v0 — JSONL serializer
 *
 * Each outcome becomes one line:
 *   {"t":TS,"l":LAT,"ph":PHASH,"rh":RHASH,"ch":CHASH,"pt":PT,"ct":CT,
 *    "m":MID,"a":AID,"g":GUARD,"k":KIND}
 *
 * Field names are abbreviated to keep the JSONL compact — for 4096
 * outcomes the long-name version is ~1.5 MB, the short-name version
 * is ~720 KB. The training script knows the schema.
 * ───────────────────────────────────────────────────────────────── */

/* Global accessor: single pointer set by the daemon at boot, read by
 * the gateway endpoint. Single-writer / many-reader pattern; a `volatile`
 * qualifier is enough for x86_64 + arm64 word-sized loads/stores under
 * the C memory model assumptions we make elsewhere in the codebase. */
static hu_m3_frontier_adapter_t *volatile s_global_outcomes_adapter = NULL;

void hu_m3_outcomes_register_global_adapter(hu_m3_frontier_adapter_t *adapter) {
    s_global_outcomes_adapter = adapter;
}

hu_m3_frontier_adapter_t *hu_m3_outcomes_global_adapter(void) {
    return s_global_outcomes_adapter;
}

/* Worst-case bytes per outcome line, used to size the initial buffer.
 * Each uint64 is at most 20 chars; uint32 at most 10; uint8 at most 3.
 * 5 × 22 (uint64) + 2 × 12 (uint32) + 4 × 5 (uint16/uint8) + brackets
 * + commas + field names ≈ 250 bytes/outcome. Round up to 320 for
 * safety; cheap given the buffer is freed immediately. */
#define HU_M3_OUTCOMES_JSONL_BYTES_PER_OUTCOME 320u

hu_error_t hu_m3_outcomes_to_jsonl(hu_allocator_t *alloc, const hu_m3_frontier_adapter_t *adapter,
                                   const hu_m3_outcomes_filter_t *filter, char **out_buf,
                                   size_t *out_len, size_t *out_cap) {
    if (!alloc || !out_buf || !out_len || !out_cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_buf = NULL;
    *out_len = 0;
    *out_cap = 0;

    if (!adapter)
        return HU_OK; /* No adapter → empty result is OK, not an error. */

    /* Snapshot under the same path the public API uses so we don't
     * duplicate the wrap-aware copy logic. */
    size_t max_count =
        (filter && filter->max_count > 0 && filter->max_count < HU_M3_OUTCOMES_RING_CAPACITY)
            ? filter->max_count
            : HU_M3_OUTCOMES_RING_CAPACITY;

    hu_m3_inference_outcome_t *snap =
        (hu_m3_inference_outcome_t *)alloc->alloc(alloc->ctx, sizeof(*snap) * max_count);
    if (!snap)
        return HU_ERR_OUT_OF_MEMORY;

    size_t snap_count = 0;
    hu_error_t err =
        hu_m3_frontier_adapter_snapshot_outcomes(adapter, snap, max_count, &snap_count);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, snap, sizeof(*snap) * max_count);
        return err;
    }
    if (snap_count == 0) {
        alloc->free(alloc->ctx, snap, sizeof(*snap) * max_count);
        return HU_OK; /* Empty result. */
    }

    /* Pre-size the output buffer for the worst case. We'll truncate the
     * cap at the end if we want — but for a one-shot serialize-and-free
     * pattern, slight over-allocation is fine. */
    size_t cap = HU_M3_OUTCOMES_JSONL_BYTES_PER_OUTCOME * snap_count + 1u;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        alloc->free(alloc->ctx, snap, sizeof(*snap) * max_count);
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t off = 0;
    size_t emitted = 0;
    for (size_t i = 0; i < snap_count; i++) {
        const hu_m3_inference_outcome_t *o = &snap[i];
        /* Apply filters. */
        if (filter && filter->turn_kind != 0 && o->turn_kind != filter->turn_kind)
            continue;
        if (filter && filter->since_ms != 0 && o->timestamp_unix_ms < filter->since_ms)
            continue;

        /* Emit one compact JSON object + newline. Field names are
         * short (1-2 chars) — the training script knows the schema.
         * Format strings use the most-permissive integer types so a
         * future struct widening doesn't silently overflow. */
        int n = snprintf(
            buf + off, cap - off,
            "%s{\"t\":%llu,\"l\":%llu,\"ph\":%llu,\"rh\":%llu,\"ch\":%llu,"
            "\"pt\":%u,\"ct\":%u,\"m\":%u,\"a\":%u,\"g\":%u,\"k\":%u}",
            emitted == 0 ? "" : "\n", (unsigned long long)o->timestamp_unix_ms,
            (unsigned long long)o->latency_ms, (unsigned long long)o->prompt_hash,
            (unsigned long long)o->response_hash, (unsigned long long)o->contact_id_hash,
            (unsigned)o->prompt_tokens, (unsigned)o->completion_tokens, (unsigned)o->model_id,
            (unsigned)o->adapter_id, (unsigned)o->guard_decision, (unsigned)o->turn_kind);
        if (n < 0 || (size_t)n >= cap - off) {
            /* Either snprintf reported an error or we'd overflow. The
             * pre-sizing should make this unreachable, but bail safely
             * if it ever happens (e.g. a future struct field is added
             * without bumping HU_M3_OUTCOMES_JSONL_BYTES_PER_OUTCOME). */
            alloc->free(alloc->ctx, buf, cap);
            alloc->free(alloc->ctx, snap, sizeof(*snap) * max_count);
            return HU_ERR_OUT_OF_MEMORY;
        }
        off += (size_t)n;
        emitted++;
    }

    alloc->free(alloc->ctx, snap, sizeof(*snap) * max_count);

    if (emitted == 0) {
        /* All snapshotted outcomes were filtered out. */
        alloc->free(alloc->ctx, buf, cap);
        return HU_OK;
    }

    buf[off] = '\0';
    *out_buf = buf;
    *out_len = off;
    *out_cap = cap;
    return HU_OK;
}

/* ─────────────────────────────────────────────────────────────────────
 * Spec 1 Task 7 (AC-M3-4): outcome-ring population helper.
 * ───────────────────────────────────────────────────────────────── */

hu_error_t hu_m3_record_outcome_from_provider_result(
    hu_m3_frontier_adapter_t *adapter, uint64_t timestamp_unix_ms, uint32_t prompt_tokens,
    uint32_t completion_tokens, uint64_t latency_ms, uint64_t contact_hash, uint8_t turn_kind) {
    if (!adapter)
        return HU_OK;

    hu_m3_inference_outcome_t outcome;
    memset(&outcome, 0, sizeof(outcome));
    if (timestamp_unix_ms == 0) {
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
            timestamp_unix_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
        }
    }
    outcome.timestamp_unix_ms = timestamp_unix_ms;
    outcome.latency_ms = latency_ms;
    outcome.contact_id_hash = contact_hash;
    outcome.prompt_tokens = prompt_tokens;
    outcome.completion_tokens = completion_tokens;
    outcome.turn_kind = turn_kind;
    return hu_m3_frontier_adapter_record_outcome(adapter, &outcome);
}

/* ─────────────────────────────────────────────────────────────────────
 * Spec 1 Task 8 (AC-M3-4 expanded): drain marker + drainer.
 * ───────────────────────────────────────────────────────────────── */

uint64_t hu_m3_frontier_adapter_drain_marker(const hu_m3_frontier_adapter_t *adapter) {
    return adapter ? adapter->drain_marker : 0ULL;
}

void hu_m3_frontier_adapter_advance_drain_marker(hu_m3_frontier_adapter_t *adapter,
                                                 uint64_t recorded_through) {
    if (!adapter)
        return;
    if (recorded_through <= adapter->drain_marker)
        return;
    if (recorded_through > adapter->total_recorded)
        recorded_through = adapter->total_recorded;
    adapter->drain_marker = recorded_through;
}

#ifdef HU_ENABLE_SQLITE

hu_error_t hu_m3_outcomes_init_table(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql_table = "CREATE TABLE IF NOT EXISTS m3_outcomes("
                            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                            "timestamp_unix_ms INTEGER NOT NULL, "
                            "latency_ms INTEGER NOT NULL, "
                            "prompt_hash INTEGER NOT NULL, "
                            "response_hash INTEGER NOT NULL, "
                            "contact_id_hash INTEGER NOT NULL, "
                            "prompt_tokens INTEGER NOT NULL, "
                            "completion_tokens INTEGER NOT NULL, "
                            "model_id INTEGER NOT NULL, "
                            "adapter_id INTEGER NOT NULL, "
                            "guard_decision INTEGER NOT NULL, "
                            "turn_kind INTEGER NOT NULL, "
                            "drained_ts_ms INTEGER NOT NULL"
                            ");";
    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql_table, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg)
            sqlite3_free(err_msg);
        return HU_ERR_IO;
    }
    const char *sql_idx = "CREATE INDEX IF NOT EXISTS idx_m3_outcomes_timestamp "
                          "ON m3_outcomes(timestamp_unix_ms);";
    rc = sqlite3_exec(db, sql_idx, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg)
            sqlite3_free(err_msg);
        return HU_ERR_IO;
    }
    return HU_OK;
}

hu_error_t hu_m3_drain_outcomes_to_sqlite(hu_m3_frontier_adapter_t *adapter, sqlite3 *db,
                                          int64_t now_ts_ms, hu_allocator_t *alloc,
                                          size_t max_outcomes, int64_t *out_drained) {
    if (out_drained)
        *out_drained = 0;
    if (!db || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!adapter)
        return HU_OK;

    uint64_t total = adapter->total_recorded;
    uint64_t marker = adapter->drain_marker;
    if (total <= marker)
        return HU_OK;

    /* The ring holds up to CAPACITY records. If unread > CAPACITY,
     * records have been dropped — advance the marker past them anyway
     * to keep progress (operator sees gap via persisted row count vs
     * total_recorded). */
    uint64_t unread = total - marker;
    if (unread > (uint64_t)HU_M3_OUTCOMES_RING_CAPACITY)
        unread = (uint64_t)HU_M3_OUTCOMES_RING_CAPACITY;
    size_t want = (size_t)unread;
    if (max_outcomes == 0)
        max_outcomes = HU_M3_OUTCOMES_RING_CAPACITY;
    if (want > max_outcomes)
        want = max_outcomes;
    if (want == 0)
        return HU_OK;

    hu_m3_inference_outcome_t *snap =
        (hu_m3_inference_outcome_t *)alloc->alloc(alloc->ctx, sizeof(*snap) * want);
    if (!snap)
        return HU_ERR_OUT_OF_MEMORY;

    size_t snap_count = 0;
    hu_error_t err = hu_m3_frontier_adapter_snapshot_outcomes(adapter, snap, want, &snap_count);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, snap, sizeof(*snap) * want);
        return err;
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "INSERT INTO m3_outcomes("
                                "timestamp_unix_ms, latency_ms, prompt_hash, response_hash, "
                                "contact_id_hash, prompt_tokens, completion_tokens, model_id, "
                                "adapter_id, guard_decision, turn_kind, drained_ts_ms) "
                                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        alloc->free(alloc->ctx, snap, sizeof(*snap) * want);
        return HU_ERR_IO;
    }

    (void)sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    hu_error_t insert_err = HU_OK;
    int64_t inserted = 0;
    for (size_t i = 0; i < snap_count; i++) {
        const hu_m3_inference_outcome_t *o = &snap[i];
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, (int64_t)o->timestamp_unix_ms);
        sqlite3_bind_int64(stmt, 2, (int64_t)o->latency_ms);
        sqlite3_bind_int64(stmt, 3, (int64_t)o->prompt_hash);
        sqlite3_bind_int64(stmt, 4, (int64_t)o->response_hash);
        sqlite3_bind_int64(stmt, 5, (int64_t)o->contact_id_hash);
        sqlite3_bind_int(stmt, 6, (int)o->prompt_tokens);
        sqlite3_bind_int(stmt, 7, (int)o->completion_tokens);
        sqlite3_bind_int(stmt, 8, (int)o->model_id);
        sqlite3_bind_int(stmt, 9, (int)o->adapter_id);
        sqlite3_bind_int(stmt, 10, (int)o->guard_decision);
        sqlite3_bind_int(stmt, 11, (int)o->turn_kind);
        sqlite3_bind_int64(stmt, 12, now_ts_ms);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            insert_err = HU_ERR_IO;
            break;
        }
        inserted++;
    }
    sqlite3_finalize(stmt);

    if (insert_err != HU_OK) {
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        alloc->free(alloc->ctx, snap, sizeof(*snap) * want);
        return insert_err;
    }
    (void)sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);

    /* Marker = total at function entry. Concurrent records that land
     * after this point are still > total and remain to be drained next
     * tick. */
    hu_m3_frontier_adapter_advance_drain_marker(adapter, total);

    alloc->free(alloc->ctx, snap, sizeof(*snap) * want);
    if (out_drained)
        *out_drained = inserted;
    return HU_OK;
}

static atomic_bool g_warned_m3_drain_enabled = false;
static atomic_bool g_warned_m3_drain_null_db = false;

#if HU_IS_TEST
void hu_daemon_tick_m3_outcome_drain_reset_warn_guards_for_test(void) {
    atomic_store(&g_warned_m3_drain_enabled, false);
    atomic_store(&g_warned_m3_drain_null_db, false);
}
#endif

hu_error_t hu_daemon_tick_m3_outcome_drain(hu_m3_frontier_adapter_t *adapter, sqlite3 *db,
                                           int64_t now_ts_ms, int64_t *last_run_ts_ms_inout,
                                           int64_t interval_seconds, hu_allocator_t *alloc,
                                           int64_t *out_drained) {
    if (out_drained)
        *out_drained = 0;
    if (!last_run_ts_ms_inout || !alloc)
        return HU_ERR_INVALID_ARGUMENT;

    if (!db) {
        hu_log_info_once(&g_warned_m3_drain_null_db, "daemon", NULL,
                         "m3 outcome drainer tick called with NULL sqlite3 db — "
                         "outcome ring will wrap silently. Open the daemon's primary "
                         "memory db before scheduling the drain tick.");
        return HU_OK;
    }

    if (interval_seconds <= 0)
        interval_seconds = 300;

    int64_t interval_ms = interval_seconds * 1000;
    if (*last_run_ts_ms_inout > 0 && now_ts_ms - *last_run_ts_ms_inout < interval_ms)
        return HU_OK;

    hu_log_info_once(&g_warned_m3_drain_enabled, "daemon", NULL,
                     "m3 outcome drainer tick enabled — draining outcome ring every %lld "
                     "seconds into m3_outcomes table",
                     (long long)interval_seconds);

    int64_t drained = 0;
    hu_error_t err = hu_m3_drain_outcomes_to_sqlite(adapter, db, now_ts_ms, alloc, 0, &drained);
    *last_run_ts_ms_inout = now_ts_ms;
    if (out_drained)
        *out_drained = drained;
    return err;
}

#endif /* HU_ENABLE_SQLITE */
