---
title: "Prompt-Budget Compression — Design"
created: 2026-05-25
status: draft
sprint: TBD
reads: requirements.md
---

# Prompt-Budget Compression — Design

## Architecture

Single new module: `src/agent/prompt_budget.c` + public header
`include/human/agent/prompt_budget.h`. No vtable; the module is a
pure-function library called by `hu_prompt_build_system` and
exposed via a doctor check.

```
┌────────────────────────────────────┐
│ agent_turn.c                       │
│   build context (27 fields)        │
│        │                           │
│        ▼                           │
│ hu_prompt_build_system(            │  ← extended signature
│   fields, out_buf,                 │
│   hu_prompt_field_stats_t *stats)  │  ← new optional param
│        │                           │
│        ▼                           │
│ prompt_budget_observe(             │
│   global_budget, stats)            │
│        │                           │
│        ▼                           │
│   global_budget (process state)    │
└────────────────────────────────────┘
                 ▲
                 │ snapshot
                 │
   ┌─────────────┴────────────────┐
   │  doctor::prompt_budget_check  │
   │    reads global_budget,       │
   │    emits per-field JSON       │
   └───────────────────────────────┘
```

## Data Structures

```c
/* include/human/agent/prompt_budget.h */

#define HU_PROMPT_FIELD_COUNT 27 /* matches the audit's enumeration */

typedef struct hu_prompt_field_stat {
    const char *name;        /* borrowed; static string in builder */
    size_t bytes_contributed;
} hu_prompt_field_stat_t;

typedef struct hu_prompt_budget hu_prompt_budget_t; /* opaque */

hu_error_t hu_prompt_budget_init(hu_allocator_t *alloc,
                                 hu_prompt_budget_t **out);
void hu_prompt_budget_free(hu_prompt_budget_t *b);

/* Per-turn observation. Stats array is borrowed; the budget object
 * accumulates running statistics (count, sum, last-non-empty-at). */
void hu_prompt_budget_observe(hu_prompt_budget_t *b,
                              const hu_prompt_field_stat_t *stats,
                              size_t count);

/* Pure predicate: is this field DEAD given current statistics? */
bool hu_prompt_budget_field_is_dead(const hu_prompt_budget_t *b,
                                    const char *field_name,
                                    size_t min_bytes_threshold,
                                    size_t min_sample_count);

/* Snapshot for doctor reporting. Caller-owned array. Returns the
 * number of entries populated (capped at array_cap). */
size_t hu_prompt_budget_snapshot(const hu_prompt_budget_t *b,
                                 hu_prompt_field_stat_t *out_array,
                                 size_t array_cap);
```

The opaque struct holds:

```c
/* src/agent/prompt_budget.c */
struct hu_prompt_budget {
    hu_allocator_t *alloc;
    struct {
        const char *name;
        uint64_t total_bytes;
        uint64_t observation_count;
        uint64_t non_empty_count;
        int64_t  last_non_empty_at_ms; /* monotonic */
    } fields[HU_PROMPT_FIELD_COUNT];
    size_t field_count;
};
```

## Trimming Gate

`hu_prompt_build_system` gets a small extension. The builder reads
`cfg->prompt_budget.enabled`; when true AND
`hu_prompt_budget_field_is_dead(...)` returns true for a given
field, the appender call is SKIPPED. The trim is structural — no
content rewriting.

```c
/* src/agent/prompt.c — pseudocode */
for (size_t i = 0; i < HU_PROMPT_FIELD_COUNT; i++) {
    if (cfg->prompt_budget.enabled &&
        global_budget &&
        hu_prompt_budget_field_is_dead(global_budget,
                                       field_table[i].name,
                                       cfg->prompt_budget.dead_field_min_bytes,
                                       cfg->prompt_budget.dead_field_sample_count) &&
        !field_in_allowlist(cfg->prompt_budget.field_allowlist,
                            field_table[i].name)) {
        stats[i].bytes_contributed = 0;
        continue; /* skip the appender */
    }
    size_t before = out_buf->len;
    field_table[i].append(out_buf, ctx);
    stats[i].bytes_contributed = out_buf->len - before;
}
```

## Config Parsing

```c
/* src/config_parse.c (new function) */
static hu_error_t parse_prompt_budget(hu_allocator_t *a,
                                      hu_config_t *cfg,
                                      const hu_json_value_t *obj) {
    if (!obj || obj->type != HU_JSON_OBJECT) return HU_OK;
    cfg->prompt_budget.enabled = hu_json_get_bool(obj, "enabled", false);
    cfg->prompt_budget.dead_field_min_bytes =
        (size_t)hu_json_get_number(obj, "dead_field_min_bytes", 16);
    cfg->prompt_budget.dead_field_sample_count =
        (size_t)hu_json_get_number(obj, "dead_field_sample_count", 100);
    /* allowlist + denylist parsed via hu_json_get_string_array */
    return HU_OK;
}
```

Added to `include/human/config.h`:

```c
typedef struct hu_prompt_budget_config {
    bool enabled;
    size_t dead_field_min_bytes;     /* default 16 */
    size_t dead_field_sample_count;  /* default 100 */
    char **field_allowlist;          /* NULL-terminated */
    char **field_denylist;           /* NULL-terminated */
} hu_prompt_budget_config_t;

/* in hu_config_t: */
hu_prompt_budget_config_t prompt_budget;
```

## Doctor Check

```c
/* src/doctor/check_prompt_budget.c (new file) */
static hu_doctor_check_result_t check_prompt_budget_run(
    hu_doctor_check_t *self, void *ctx) {
    /* ctx is hu_doctor_check_provider_ctx_t pattern — alloc + cfg.
     * Reads the global budget (singleton in src/agent/prompt.c) and
     * emits per-field stats. PASS if instrumented + at least 10
     * observations; NA if instrumentation disabled. */
    ...
}

hu_doctor_check_t hu_doctor_check_prompt_budget = {
    .name = "prompt_budget",
    .description = "Reports per-field prompt-budget statistics",
    .run = check_prompt_budget_run,
    .fix = NULL,
};
```

Registered in `src/doctor/registry.c::register_defaults` as the 13th
default check.

## Silent-Failure Diagnostic (AC-6)

When `cfg->prompt_budget.enabled=false` at first system-prompt build,
emit (one-shot per process):

```
prompt_budget: per-field instrumentation disabled by config
(prompt_budget.enabled=false). Set prompt_budget.enabled=true in
config.json to enable per-field byte accounting + DEAD-field detection.
Costs ~5µs/turn for the bookkeeping.
```

Pattern matches the LoRA diagnostic from commit `48372778` and the
rule at `~/.claude/rules/silent-config-gated-subsystems.md`.

## Test Strategy

| AC | Test file | Approach |
|---|---|---|
| AC-1 | `tests/test_prompt_field_stats.c` | Build prompt from fixture; assert stats[i].bytes_contributed = expected for each of 27 fields. |
| AC-2 | `tests/test_prompt_budget_dead.c` | Construct budget with synthetic counters; assert is_dead() returns true iff (mean < threshold) AND (sample_count ≥ min). |
| AC-3 | `tests/test_prompt_budget_trim.c` | Build twice (gate off / on); assert (len_off - len_on) == sum of bytes of DEAD-tagged fields. |
| AC-4 | `tests/test_config_prompt_budget.c` | Parse fixture JSON; assert defaults applied; assert allowlist parsed. |
| AC-5 | `tests/test_doctor_check_prompt_budget.c` | Register check; run with fixture budget; assert detail_json matches schema. |
| AC-6 | `tests/test_prompt_budget_disabled_warn.c` | Set enabled=false; first call emits one log line containing the config key. |
| AC-7 | `tests/test_prompt_budget_zero_change.c` | Build prompt with gate off; hash output; assert matches golden hash. Run again; assert identical. |

## Sequencing

Order matters because earlier ACs unblock later ones:

1. AC-1 (instrumentation) — extends `hu_prompt_build_system` signature
2. AC-7 (zero-change) — pin the baseline before any trimming logic
3. AC-2 (dead detection) — pure function over counters
4. AC-4 (config parsing) — pulls schema into the daemon
5. AC-3 (trim gate) — wires AC-2 into the builder
6. AC-5 (doctor check) — exposes the data to operators
7. AC-6 (silent-failure log) — defense-in-depth

Each AC is a small atomic commit. Estimated 1-2 days for a single
implementer.

## Out-of-Scope (Tracked Separately)

- **Audit B: Capture actual system-prompt content for 10 turns.**
  Should be a manual investigation; not a code task. Output: a doc
  in `docs/research/` with raw prompts + analysis.
- **Audit C: A/B test full vs stripped context.** Requires a fake
  provider that echoes the prompt. Separate spec.
- **Recency rearrangement experiment.** A different lever (reorder
  fields rather than drop them).

## Risks Revisited (from requirements.md)

- **R-1 (instrumentation overhead)** — mitigated by simple
  pointer-subtract for bytes_contributed. No string parsing. Add a
  microbenchmark in `tests/bench_prompt_build.c` (out of AC scope
  but valuable).
- **R-2 (false-positive DEAD)** — mitigated by allowlist (AC-4) and
  `last_non_empty_at_ms` snapshot (visible in doctor output).
- **R-3 (premature optimization)** — mitigated by Phase 1 being
  measurement-only. Trimming gate ships disabled-by-default.
