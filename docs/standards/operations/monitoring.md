---
title: Monitoring and Observability
---

# Monitoring and Observability

Standards for logging, health checks, alerting, and metrics collection across all human deployment contexts.

**Cross-references:** [incident-response.md](incident-response.md), [../ai/agent-architecture.md](../ai/agent-architecture.md), [../ai/evaluation.md](../ai/evaluation.md)

---

## Logging

### Structured Logging via hu_observer_t

All runtime logging goes through the `hu_observer_t` vtable. This enables pluggable backends (stderr, file, structured JSON, remote collectors) without changing application code.

```c
// Correct: structured via observer
hu_observer_log(observer, HU_LOG_INFO, "provider_response",
    "provider=%s model=%s latency_ms=%d tokens=%d",
    provider_name, model_name, latency_ms, total_tokens);

// Wrong: direct fprintf to stderr
fprintf(stderr, "Provider %s took %dms\n", name, ms);
```

### Log Levels

| Level | When                                         | Example                                                              |
| ----- | -------------------------------------------- | -------------------------------------------------------------------- |
| error | Something failed and needs attention         | Provider timeout, tool execution crash, memory backend unreachable   |
| warn  | Degraded but not broken                      | Retry succeeded, fallback provider activated, stale memory retrieved |
| info  | Normal operations worth recording            | Session started, tool executed, channel connected                    |
| debug | Detailed diagnostic info (off in production) | Full prompt content, raw provider response, memory search results    |

### Sensitive Data Rules

- Never log API keys, tokens, passwords, or secrets
- Never log full user messages in production -- log a hash or truncated preview
- Never log full AI responses at info level -- use debug only
- Log user/session IDs, not personal identifiers (names, emails, phone numbers)
- When logging tool arguments, redact file paths outside the allowed sandbox

---

## Health Checks

### Gateway Health Endpoint

The gateway server (`src/gateway/gateway.c`) exposes a health endpoint:

| Property | Requirement                                                 |
| -------- | ----------------------------------------------------------- |
| Path     | `GET /health`                                               |
| Auth     | None required                                               |
| Response | JSON: `{ "status": "ok", "version": "...", "uptime_s": N }` |
| Timeout  | Response within 5 seconds or considered unhealthy           |
| Content  | Include version, uptime, and basic subsystem status         |

### Subsystem Health

For deeper health assessment:

| Subsystem             | Health Signal                                              |
| --------------------- | ---------------------------------------------------------- |
| Provider connectivity | Can reach at least one configured provider                 |
| Memory backend        | SQLite file readable, or LanceDB/remote backend responsive |
| Channel connectivity  | Primary channel connected and authenticated                |
| Tool availability     | Core tools registered and responding                       |

---

## Alerting Thresholds

### Critical (immediate response required)

- Health check fails for > 2 minutes consecutively
- Error rate > 5% over 5-minute window
- Gateway process crash (exit code != 0)
- Provider returns 5xx for > 3 consecutive requests
- Binary startup time > 1 second (normally <!-- fp:startup_range -->3–6 ms<!-- /fp -->)

### Warning (investigate within the hour)

- P95 response latency exceeds 2x the provider's typical latency
- Daily AI token spend exceeds forecast by 50%
- Memory backend query latency P95 > 500ms
- Fallback provider activated (primary provider may be degraded)

### Informational (review in drift audit)

- Tool execution errors (may indicate misconfiguration)
- Channel reconnection events
- Memory compaction triggered
- Context window approaching capacity

---

## AI-Specific Metrics

Track via `hu_observer_t` on every provider invocation:

| Metric                       | Granularity                                  | Purpose                           |
| ---------------------------- | -------------------------------------------- | --------------------------------- |
| Token usage (input + output) | Per invocation, per provider, per model      | Cost tracking, budget alerting    |
| Response latency             | Per invocation (time-to-first-token + total) | Performance monitoring            |
| Fallback rate                | Per session                                  | Provider reliability assessment   |
| Tool call count per turn     | Per turn                                     | Detect runaway tool loops         |
| Error rate by type           | Per provider, per error code                 | Identify provider-specific issues |
| Context window utilization   | Per turn                                     | Predict compaction needs          |
| Memory retrieval latency     | Per search                                   | Memory backend performance        |
| Memory retrieval relevance   | Per search (if scoring available)            | RAG quality assessment            |

---

## Dashboards

Minimum observability surfaces for autonomous operation:

| Dashboard               | Metrics                                                        |
| ----------------------- | -------------------------------------------------------------- |
| **Runtime Health**      | Uptime, startup time, RSS, binary version, error rate          |
| **Provider Operations** | Token usage, latency by model, fallback rate, cost per session |
| **Channel Activity**    | Messages per channel, response times, connection status        |
| **Memory**              | Retrieval latency, storage size, compaction frequency          |
| **Security**            | Policy blocks, confirmation rates, denied tool executions      |

---

## Performance Baselines

Maintain these baselines and alert on regression:

| Metric                      | Baseline       | Alert If                          |
| --------------------------- | -------------- | --------------------------------- |
| Binary size (release-size build) | <!-- fp:binary_kb -->~2760 KB<!-- /fp --> | > <!-- fp:budget_binary_kb -->2800 KB<!-- /fp --> (CI fails the main build) |
| Cold start (`--version`)    | <!-- fp:startup_range -->3–6 ms<!-- /fp --> | > 50 ms                           |
| Peak RSS (`--version`)      | <!-- fp:version_rss_mb -->6.9 MB<!-- /fp --> | > <!-- fp:budget_version_rss_mb -->8 MB<!-- /fp --> (CI fails the main build) |
| Idle RSS (`human mcp`)      | <!-- fp:idle_rss_mb -->8.6 MB<!-- /fp --> | no alert; tracked only |
| Test throughput             | 700+ tests/sec | < 500 tests/sec                   |

---

## Anti-Patterns

```
WRONG -- Log everything at info level (noise drowns signal)
RIGHT -- Use appropriate log levels; debug for diagnostics, info for operations, warn/error for problems

WRONG -- Log the full system prompt on every request
RIGHT -- Log the prompt version identifier; full content at debug level only

WRONG -- No structured metadata on log entries
RIGHT -- Every log entry includes context: function name, provider, latency, error code

WRONG -- Monitor only errors (miss degradation)
RIGHT -- Monitor latency, fallback rate, and token usage too -- degradation precedes failure

WRONG -- Hard-code alert thresholds without baselines
RIGHT -- Establish baselines first, then set alerts relative to them

WRONG -- Log user secrets or personal data
RIGHT -- Log identifiers and hashes; never raw secrets, messages, or PII
```
