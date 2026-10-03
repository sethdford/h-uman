---
title: Performance Testing
updated: 2026-03-13
---

# Performance Testing

Methodology for performance measurement, regression detection, and profiling across all human surfaces.

**Cross-references:** [performance.md](performance.md), [../operations/observability.md](../operations/observability.md), [../quality/ceremonies.md](../quality/ceremonies.md)

---

## 1. Performance Testing Tiers

| Tier                  | What                                | When                     | Gate                    |
| --------------------- | ----------------------------------- | ------------------------ | ----------------------- |
| **Micro-benchmark**   | Individual function timing          | Development              | Informational           |
| **Binary regression** | Size, startup, RSS                  | Every CI build           | Block if >5% regression |
| **Load test**         | Gateway concurrent connections, RPS | Pre-release              | Block if p99 >10s       |
| **UI performance**    | Lighthouse, Core Web Vitals         | Every PR (ui/, website/) | Block if Lighthouse <95 |
| **Profile**           | CPU flame graph, memory allocation  | On-demand / quarterly    | Informational           |

## 2. Binary Regression Testing

### 2.1 Metrics Tracked

Generated from [`docs/perf/footprint.json`](../../perf/footprint.json) and
[`docs/perf/footprint-budget.json`](../../perf/footprint-budget.json); see
[performance.md](performance.md#hard-limits-the-identity-metrics).

| Metric (release-size build) | Current | Hard budget | Measurement |
| --- | --- | --- | --- |
| Binary size | <!-- fp:binary_kb -->~2905 KB<!-- /fp --> | <!-- fp:budget_binary_kb -->3000 KB<!-- /fp --> | `stat` of `human` |
| Text section | <!-- fp:text_kb -->2024 KB<!-- /fp --> | none (part of binary size) | `size -m human` |
| Cold start | <!-- fp:startup_range -->8–18 ms<!-- /fp --> | <!-- fp:budget_startup_ms -->100 ms<!-- /fp --> median | 20 warm runs of `human --version` |
| Peak RSS | <!-- fp:version_rss_mb -->6.7 MB<!-- /fp --> | <!-- fp:budget_version_rss_mb -->8 MB<!-- /fp --> | `/usr/bin/time -l human --version` |
| Idle RSS | <!-- fp:idle_rss_mb -->8.4 MB<!-- /fp --> | none (tracked) | `ps` RSS of an idle `human mcp` |

### 2.2 CI Enforcement

The `release-size` job in `.github/workflows/ci.yml` builds the release-size configuration on
every push to `main` and measures it:

```bash
scripts/measure-build-footprint.sh release-size build --prebuilt > footprint-fresh.json
python3 scripts/footprint.py evaluate footprint-fresh.json   # exit 1 on a budget breach
```

A budget breach fails the job. A measurement that makes a published number untrue (more than
5% off, or outside a published bound or range) triggers the `footprint-heal` job, which
regenerates every footprint claim from it and proposes the change on the `footprint/refresh`
branch. `benchmark.yml` still reports size and startup for a different configuration (Linux,
curl on) as a PR comment; it is not the budget.

### 2.3 Justified Growth

When a feature legitimately increases binary size:

1. Measure the exact delta
2. Document the justification in the commit message
3. If it crosses a budget, raise that budget in `docs/perf/footprint-budget.json` with the measurement in its `why`
4. Ensure the feature is compile-flag gated if >10 KB

## 3. Gateway Load Testing

### 3.1 Test Scenarios

| Scenario         | Description                            | Target                        |
| ---------------- | -------------------------------------- | ----------------------------- |
| Single user      | 1 connection, sequential messages      | p99 <2s response              |
| Concurrent users | 10 connections, parallel messages      | p99 <5s response              |
| Burst            | 50 messages in 1 second                | No crashes, graceful queueing |
| Long session     | 1 connection, 100 messages over 10 min | No memory leak (RSS stable)   |
| Idle             | Gateway running 24h with no traffic    | RSS stable, no growth         |

### 3.2 Tools

| Tool                                   | Purpose                                    |
| -------------------------------------- | ------------------------------------------ |
| `wrk` / `wrk2`                         | HTTP load testing with constant throughput |
| `websocat`                             | WebSocket load testing (manual)            |
| Custom script (`scripts/load-test.sh`) | Gateway-specific test harness              |

### 3.3 Memory Leak Detection

```bash
# Run gateway for N requests, compare RSS before and after
initial_rss=$(ps -o rss= -p $PID)
# ... send 1000 requests ...
final_rss=$(ps -o rss= -p $PID)
growth=$((final_rss - initial_rss))
# Growth should be <1 MB for 1000 requests
```

## 4. UI Performance Testing

### 4.1 Lighthouse CI

Every PR touching `ui/` or `website/` runs Lighthouse CI:

| Metric         | Dashboard | Website |
| -------------- | --------- | ------- |
| Performance    | >= 95     | >= 95   |
| Accessibility  | >= 98     | >= 98   |
| Best Practices | >= 95     | >= 95   |
| SEO            | >= 90     | >= 95   |

### 4.2 Core Web Vitals

| Metric | Target | Stretch | Measurement             |
| ------ | ------ | ------- | ----------------------- |
| LCP    | <1.5s  | <0.5s   | Lighthouse + field data |
| CLS    | <0.05  | 0.00    | Lighthouse + field data |
| INP    | <200ms | <50ms   | Field data only         |
| FCP    | <1.0s  | <0.5s   | Lighthouse              |
| TBT    | <200ms | 0ms     | Lighthouse              |

### 4.3 Bundle Size Budget

| Chunk              | Budget         | Measurement         |
| ------------------ | -------------- | ------------------- |
| Initial bundle     | <150 KB (gzip) | `vite build` output |
| Lazy view chunk    | <100 KB (gzip) | `vite build` output |
| Total (all chunks) | <500 KB (gzip) | `vite build` output |

## 5. Profiling Workflow

### 5.1 CPU Profiling

**macOS (Instruments):**

```bash
xcrun xctrace record --template "Time Profiler" --launch ./human -- agent -m "test"
```

**Linux (perf):**

```bash
perf record -g ./human agent -m "test"
perf report
```

### 5.2 Memory Profiling

**ASan (always-on in dev):**

```bash
cmake --preset dev  # Enables ASan
./build/human_tests  # Reports leaks
```

**Valgrind (deep analysis):**

```bash
valgrind --tool=massif ./human agent -m "test"
ms_print massif.out.*
```

### 5.3 Web UI Profiling

```bash
# Browser DevTools Performance tab
# Or automated via Lighthouse CI user flows
npx lighthouse http://localhost:5173 --output=json --output-path=./lighthouse.json
```

## 6. Quarterly Performance Review

Per `docs/standards/quality/ceremonies.md`, the quarterly release gate includes:

1. Binary size within budget
2. Startup time within budget
3. RSS within budget
4. Lighthouse scores meet thresholds
5. No open memory leaks (ASan clean)
6. Load test scenarios pass

Document results in the quality scorecard (`docs/quality-scorecard.md`).

## Normative References

| ID           | Source                              | Version          | Relevance                             |
| ------------ | ----------------------------------- | ---------------- | ------------------------------------- |
| [WebVitals]  | Google Web Vitals                   | 2024 definitions | LCP, CLS, INP measurement methodology |
| [Gregg-SP]   | Brendan Gregg — Systems Performance | 2nd ed. (2020)   | Performance analysis methodology      |
| [Lighthouse] | Google Lighthouse                   | v12              | Automated web performance auditing    |
| [ASan]       | Google AddressSanitizer             | LLVM 18          | Memory error and leak detection       |
| [wrk2]       | wrk2 — HTTP benchmarking tool       | v4.2             | Constant-throughput HTTP load testing |
