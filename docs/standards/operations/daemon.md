---
title: Daemon Operations Standard
---

# Daemon Operations Standard

Standards for daemon service operation, worker pooling, job scheduling, and lifecycle management.

**Cross-references:** [monitoring.md](monitoring.md), [incident-response.md](incident-response.md), [../engineering/config-schema.md](../engineering/config-schema.md)

---

## Overview

The daemon (`src/daemon.c`) runs the service loop: polls messaging channels, dispatches messages to the agent, sends responses, and executes scheduled jobs. This standard covers worker pool configuration, job queueing, retry logic, graceful shutdown, and health monitoring.

**Related code:** `src/daemon*.c` (lifecycle, routing, cron), `include/human/daemon*.h`, `src/service_loop.c`.

---

## Daemon Startup and Configuration

### Configuration File

Daemon configuration lives in `.human/daemon-config.json` (or `$HU_DAEMON_CONFIG`):

```json
{
  "enabled": true,
  "max_parallel": 2,
  "auto_scale": true,
  "auto_scale_interval": 5,
  "max_workers": 8,
  "min_workers": 1,
  "worker_mem_gb": 4,
  "estimated_cost_per_job_usd": 5.0,
  "self_optimize": false,
  "auto_template": false,
  "max_retries": 2,
  "priority_lane": false,
  "tick_interval_ms": 1000,
  "channels": [
    { "name": "slack", "interval_ms": 5000 },
    { "name": "telegram", "interval_ms": 5000 }
  ]
}
```

### Configuration Fields

| Field | Type | Default | Purpose |
|-------|------|---------|---------|
| `enabled` | bool | true | Enable daemon (false = CLI only) |
| `max_parallel` | int | 2 | Static worker limit (overridden by auto_scale) |
| `auto_scale` | bool | false | Enable dynamic scaling based on resources |
| `max_workers` | int | 8 | Ceiling for auto-scaler |
| `min_workers` | int | 1 | Floor for auto-scaler |
| `worker_mem_gb` | int | 4 | Memory per worker (for scaling calculation) |
| `estimated_cost_per_job_usd` | float | 5.0 | Budget impact per job (for scaling) |
| `tick_interval_ms` | int | 1000 | Service loop tick frequency (1s default) |

---

## Worker Pool Configuration

### Static vs Auto-Scaled

**Static mode** (default):

```json
{
  "max_parallel": 2,
  "auto_scale": false
}
```

Always 2 workers. Simple, predictable. Use for low-traffic or cost-constrained deployments.

**Auto-scaled mode:**

```json
{
  "auto_scale": true,
  "max_workers": 8,
  "min_workers": 1,
  "worker_mem_gb": 4,
  "estimated_cost_per_job_usd": 5.0
}
```

Dynamic worker count based on resource constraints. Takes the **minimum of**:

1. **CPU:** 75% of available cores (e.g., 8 cores → max 6 workers)
2. **Memory:** available GB / `worker_mem_gb` (e.g., 16 GB / 4 GB/worker → 4 workers)
3. **Budget:** remaining daily budget / `estimated_cost_per_job_usd`
4. **Demand:** active + queued jobs (always ≥ 1)

**Example auto-scale calculation:**

```
CPU constraint: 8 cores * 0.75 = 6 workers
Memory constraint: 16 GB / 4 = 4 workers
Budget constraint: $10 remaining / $5 per job = 2 workers
Demand: 5 jobs queued

Worker count = min(6, 4, 2, 5) = 2 workers
```

### Rebalancing

If `worker_pool.enabled=true` in fleet config, the rebalancer redistributes workers across repos every `rebalance_interval_seconds` based on demand.

---

## Job Queueing and Priority

### Queue Structure

Jobs are queued per channel and globally:

```c
typedef struct hu_job_queue {
    hu_job_t *normal_priority;  // FIFO for regular messages
    hu_job_t *high_priority;    // FIFO for user-initiated actions
    size_t depth;               // Total queued jobs
} hu_job_queue_t;
```

### Priority Lanes

When `priority_lane: true`, one worker slot is reserved for urgent jobs:

```
Total workers: 4
Priority lane: 1 (reserved for urgent)
Normal lane: 3 (regular messages)

If priority lane filled, remaining urgent jobs queue behind (no starvation).
```

### Job Dispatch

1. **Normal:** FIFO within priority level
2. **Ordering:** Jobs from same channel maintain order
3. **Timeout:** Job max execution time = 5 minutes (configurable per job type)
4. **Cleanup:** Failed jobs remain on queue for retry

---

## Retry Logic and Exponential Backoff

### Retry Configuration

```json
{
  "max_retries": 2,
  "initial_backoff_ms": 1000,
  "max_backoff_ms": 30000,
  "backoff_multiplier": 2.0
}
```

### Backoff Algorithm

For attempt N (1-indexed):

```
delay_ms = min(
  initial_backoff_ms * (backoff_multiplier ^ (N - 1)),
  max_backoff_ms
)
+ random_jitter_ms(0, 100)
```

**Example (initial=1000, multiplier=2.0, max=30000):**

| Attempt | Delay | Jitter Range | Total |
|---------|-------|--------------|-------|
| 1 (initial failure) | 1000 ms | ±100 | 900–1100 ms |
| 2 (first retry) | 2000 ms | ±100 | 1900–2100 ms |
| 3 (second retry) | 4000 ms | ±100 | 3900–4100 ms |
| 4 (would exceed max) | 30000 ms | ±100 | 29900–30100 ms |

### Retry Conditions

Retry on:
- Network timeout (temporary issue)
- Provider rate limit (backoff + inform user)
- Transient agent error (e.g., token limit exceeded mid-turn)

Do NOT retry on:
- Authentication failure (agent auth issue, fix config)
- Invalid input (malformed message)
- Permanent provider error (unsupported model)

---

## Graceful Shutdown Protocol

### Signal Handling

Daemon traps `SIGTERM` and `SIGINT`:

```c
signal(SIGTERM, hu_daemon_shutdown_handler);
signal(SIGINT, hu_daemon_shutdown_handler);
```

### Shutdown Sequence

1. **Stop accepting new jobs** (close webhook listeners, pause channel polling)
2. **Wait for in-flight jobs** (timeout: 30 seconds)
3. **Save state** (persist job queue, session state, memory snapshots)
4. **Disconnect channels** (graceful close of WebSocket, etc.)
5. **Exit** (code 0 if clean, 1 if timeout/error)

**Timeout handling:**

If in-flight jobs don't complete within 30s:
- Log warnings for jobs still running
- Force-kill workers
- Attempt to save state (may be incomplete)
- Exit with code 1

### Monitoring Shutdown

Check daemon is shut down:

```bash
# PID file path (default: /tmp/human.pid)
if [ -f /tmp/human.pid ]; then
  kill -0 $(cat /tmp/human.pid) 2>/dev/null && echo "Running" || echo "Stopped"
fi
```

---

## Health Monitoring and Heartbeat

### Health Metrics

Daemon publishes health via `/api/health` endpoint:

```json
{
  "status": "healthy",
  "uptime_secs": 3600,
  "workers_active": 2,
  "workers_total": 4,
  "job_queue_depth": 5,
  "avg_job_duration_ms": 1250,
  "errors_last_hour": 3,
  "last_error": "rate limit on openai",
  "memory_rss_mb": 45,
  "memory_limit_mb": 128
}
```

### Status Codes

| Status | Meaning | Action |
|--------|---------|--------|
| `healthy` | All checks pass | Continue normal operation |
| `degraded` | Warnings (high queue, slow jobs) | Monitor; alert if persists |
| `unhealthy` | Critical failures (workers down) | Page on-call; auto-restart |
| `shutdown` | Graceful shutdown in progress | Wait for completion or force-kill |

### Heartbeat

Daemon sends heartbeat every 30 seconds:

```
Heartbeat format: JSON event with timestamp, status, metrics
Sent to: monitoring backend (OpenTelemetry, datadog, custom)
```

---

## Channel Polling Configuration

Channels poll on a configurable interval:

```json
{
  "channels": [
    { "name": "slack", "interval_ms": 5000 },
    { "name": "telegram", "interval_ms": 3000 },
    { "name": "cli", "interval_ms": 0 }
  ]
}
```

### Polling Strategy

- **0 ms:** Blocking (wait for input, e.g., CLI stdin)
- **> 0 ms:** Polling every N milliseconds
- **Jitter:** Add ±10% random jitter to avoid thundering herd

**Example:**

```
Interval 5000 ms → poll every 5000 ± 500 ms (4500–5500 ms)
```

---

## Cron Job Scheduling

Daemon supports cron-style job scheduling via config:

```json
{
  "cron_jobs": [
    {
      "name": "consolidate_memory",
      "schedule": "0 2 * * *",
      "agent_action": "memory.consolidate",
      "channel": "slack"
    },
    {
      "name": "daily_standup",
      "schedule": "0 9 * * 1-5",
      "agent_action": "send_summary",
      "channel": "slack"
    }
  ]
}
```

### Cron Expression Format

5-field format (minute hour day month weekday):

```
minute      (0-59)
hour        (0-23)
day         (1-31)
month       (1-12)
weekday     (0-6, 0=Sunday)

* = any value
0-5 = range
0,2,4 = list
*/5 = step (every 5)
0-9/2 = range + step (0, 2, 4, 6, 8)
```

**Examples:**

| Schedule | Meaning |
|----------|---------|
| `0 2 * * *` | 2:00 AM daily |
| `0 9 * * 1-5` | 9:00 AM Mon–Fri |
| `*/15 * * * *` | Every 15 minutes |
| `0 0 1 * *` | Midnight, 1st of month |

---

## Resource Limits

### Per-Job Limits

| Limit | Default | Purpose |
|-------|---------|---------|
| CPU time | 5 minutes | Prevent runaway agents |
| Memory | 512 MB (configurable) | Prevent OOM |
| Tool iterations | 10 | Prevent infinite loops |
| Token usage | Agent context limit | Prevent exhaustion |

### Per-Daemon Limits

| Limit | Default | Purpose |
|-------|---------|---------|
| Total memory | 500 MB | Hard cap on daemon process |
| Max workers | 8 | Worker count ceiling |
| Queue depth | 1000 jobs | Prevent memory explosion |
| Daily cost budget | $100 (configurable) | Control API spending |

If limits exceeded:
- New jobs rejected with `HU_ERR_LIMIT_EXCEEDED`
- Running jobs allowed to finish (graceful degradation)
- Alert sent to monitoring

---

## Daemon Service Templates

### systemd Service (Linux)

```ini
[Unit]
Description=human daemon
After=network.target

[Service]
Type=simple
User=human
WorkingDirectory=/home/human
ExecStart=/usr/local/bin/human daemon
Restart=on-failure
RestartSec=10
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

### launchd Service (macOS)

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" ...>
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>com.human.daemon</string>
  <key>Program</key>
  <string>/usr/local/bin/human</string>
  <key>ProgramArguments</key>
  <array>
    <string>daemon</string>
  </array>
  <key>StandardOutPath</key>
  <string>/var/log/human.log</string>
  <key>StandardErrorPath</key>
  <string>/var/log/human-error.log</string>
  <key>RunAtLoad</key>
  <true/>
</dict>
</plist>
```

---

## Testing Expectations

Daemon must pass test suite:

```bash
./human_tests --suite=Daemon         # Daemon lifecycle
./human_tests --suite=DaemonRouting  # Job routing
./human_tests --suite=DaemonCron     # Cron scheduling
```

Required tests:

- Startup and shutdown (clean exit)
- Worker pool scaling (min/max constraints)
- Job queueing and FIFO ordering
- Retry logic (backoff, max retries)
- Graceful shutdown (in-flight job completion)
- Health status reporting
- Cron schedule matching
- Resource limit enforcement
- Signal handling (SIGTERM, SIGINT)

---

## Monitoring Checklist

When running daemon in production:

- [ ] Health endpoint returning 200 OK
- [ ] Worker count matches expected pool size
- [ ] Job queue depth under 100
- [ ] Average job duration < 5 seconds
- [ ] Error rate < 1% (alert if > 5%)
- [ ] Memory usage stable (not growing)
- [ ] Disk space for logs (at least 10 GB)
- [ ] All channels polling successfully
- [ ] Cron jobs executing on schedule
- [ ] Graceful shutdown verified (test weekly)

---

## Key Paths

- Daemon entry: `src/daemon.c`
- Lifecycle: `src/daemon/daemon_lifecycle.c`
- Routing: `src/daemon/daemon_routing.c`
- Cron: `src/daemon/daemon_cron.c`
- Proactive: `src/daemon/daemon_proactive.c`
- Headers: `include/human/daemon*.h`
- Tests: `tests/test_daemon*.c`
- Config schema: `docs/standards/engineering/config-schema.md`
