#ifndef HU_CLI_REPLAY_H
#define HU_CLI_REPLAY_H

/* `human replay` — one arm of the real-turn replay harness.
 *
 *   human replay --in TURNS.jsonl --out RESULTS.jsonl [--arm NAME]
 *         [--endpoint http://127.0.0.1:8741/v1] [--provider mlx_local]
 *         [--model NAME] [--temperature T] [--delay-ms N] [--limit N]
 *         [--no-director] [--dump-requests DIR] [--seed N]
 *
 * Gate configuration is the process environment: run one process per arm
 * (scripts/blind_ab/replay_driver.py does). The command refuses to start
 * unless HU_STATE_DIR and HU_MEMORY_SQLITE_PATH name a private snapshot
 * outside ~/.human, refuses any non-loopback endpoint, and points every
 * libcurl proxy variable at a dead loopback port so nothing else in the
 * process can reach the network. Runbook: docs/guides/replay-harness.md. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/daemon/replay_turn.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hu_cli_replay_args {
    const char *in_path;
    const char *out_path;
    const char *arm;      /* label written into every row; default "default" */
    const char *endpoint; /* default http://127.0.0.1:8741/v1 */
    const char *provider; /* default mlx_local */
    const char *model;    /* default: config default_model */
    bool force_temperature;
    double temperature;
    uint32_t delay_ms;    /* pause between turns; default 1500 */
    size_t limit;         /* 0 = all turns */
    bool director;        /* default true; --no-director or HU_REPLAY_DIRECTOR=off */
    const char *dump_dir; /* write each reply request here (0600) */
    uint32_t seed;        /* shaping seed; default 1 */
} hu_cli_replay_args_t;

/* Parse argv[2..]. False (with a reason in `why`) on any unknown flag, a
 * missing value, or a missing --in/--out. */
bool hu_cli_replay_parse(int argc, char **argv, hu_cli_replay_args_t *out, char *why,
                         size_t why_cap);

/* --provider allowlist: local OpenAI-compatible servers only (mlx_local,
 * mlx-local, mlx_http, mlx-http, compatible, llamacpp, lmstudio, ollama).
 * The endpoint is still loopback-checked separately. */
bool hu_cli_replay_provider_allowed(const char *name);

/* True when the environment isolates this process from the live state:
 * HU_STATE_DIR names an existing directory that is not $HOME/.human, and
 * HU_MEMORY_SQLITE_PATH names an existing file outside $HOME/.human. False
 * with a reason otherwise. */
bool hu_cli_replay_isolation_ok(hu_allocator_t *alloc, char *why, size_t why_cap);

/* One parsed input line. Strings point into allocations owned by the turn. */
typedef struct hu_cli_replay_turn {
    char *id;
    char *contact_id;
    char *inbound; /* inbound_bubbles joined by '\n' */
    size_t inbound_len;
    hu_channel_history_entry_t *history;
    size_t history_count;
} hu_cli_replay_turn_t;

/* Parse {id, contact_id, inbound_bubbles[], history[{from_me, text, ts}]}.
 * HU_ERR_PARSE on anything missing or mistyped. */
hu_error_t hu_cli_replay_parse_turn(hu_allocator_t *alloc, const char *line, size_t len,
                                    hu_cli_replay_turn_t *out);
void hu_cli_replay_turn_free(hu_allocator_t *alloc, hu_cli_replay_turn_t *t);

/* One output JSON line (no trailing newline). Never includes contact_id. */
hu_error_t hu_cli_replay_format_result(hu_allocator_t *alloc, const char *id, const char *arm,
                                       uint64_t elapsed_ms, const hu_replay_turn_result_t *r,
                                       char **out, size_t *out_len);

#endif /* HU_CLI_REPLAY_H */
