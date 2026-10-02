---
title: Reply Latency — Purpose Tags, Priority, Post-Send Deferral
---

# Reply latency: purpose tags, priority, and `HU_POST_SEND_DEFER`

Three changes take work off the reply's critical path. The 2026-10-02 latency
profile measured processing (inbound to send, minus the director's deliberate
delay) at a p50 of 47.5 s.

| Change | Gate | Changes what is sent? |
| --- | --- | --- |
| `X-HU-Purpose` on every local LLM and embedding request | none | no |
| `X-HU-Priority: batch` on background work | none | no (queue order only) |
| Fact extraction and row embeddings run after the send | `HU_POST_SEND_DEFER` | yes, on some turns (see below) |

Code: `src/core/llm_purpose.c`, `src/core/post_send_defer.c`. Headers:
`include/human/core/llm_purpose.h`, `include/human/core/post_send_defer.h`.

## Purpose tags and priority (ungated)

mlx-server serves replies, proactive drafts, judges and embeddings on one
worker. Its admission queue reads `X-HU-Priority`. Only `batch` is low
priority; anything else, including no header, is live. Before this change the
non-stream reply call sent no priority header (`compatible_priority_header()`
was unused), and so did the proactive drafts. Both queued FIFO.

Now every request from the `compatible` provider and the HTTP embedder carries
`X-HU-Purpose: <name>`. The names match `[a-z_]{1,24}` and never contain message
text. `hu_llm_purpose_headers` is the single purpose-to-priority map:

| Purpose | Set at | Priority header |
| --- | --- | --- |
| `reply` | `hu_agent_turn`, when the caller left the thread untagged | none (live) |
| `guard_retry` | `response_guard_retry.c` slim retry | none |
| `planner` | W12 retrieval planner | none |
| `extract` | `hu_fact_extract_llm` | none inline; `batch` when deferred |
| `judge` | prospective-memory judge | none on the reply path; `batch` from proactive ticks |
| `embed` | `embedder_http.c` (every embedding) | `batch` (unchanged from before) |
| `proactive` | init_proposer drafts | `batch` |
| `background` | any reply-default call made inside the background lane | `batch` |
| `untagged` | anything not yet tagged | none |

The background lane is a thread-local depth. `hu_daemon_housekeeping_tick`
enters it, which covers cron turns, proactive check-ins, maintenance,
reflection and autodream. The post-send flush enters it too. Every request made
inside the lane carries `batch`, whatever its purpose.

`HU_LLM_PRIORITY` no longer matters. The stream path used to send
`X-HU-Priority: live` when it was set, but the server already treats a missing
header as live.

The server logs `purpose=<name>` (a sibling change). To check coverage there,
count requests per purpose and look for `untagged`.

## `HU_POST_SEND_DEFER=off|shadow|live` (default `off`)

The daemon arms a window on its reply thread just before the agent turn and
flushes it after `skip_send:`, once the reply is sent or the pre-send abort has
fired. Two kinds of work in that window can move to the flush:

1. **The LLM fact-extraction fallback** (`HU_LLM_FACT_EXTRACT`, about 1.4 s per
   turn). It runs inside `hu_personal_model_ingest` when the regex pass finds
   nothing.
2. **The semantic-index embedding** of every row stored into SQLite memory
   during the turn. The turn's deep-extract facts (`agent_turn.c`) and the
   conversation-summary facts (`daemon_comfort_summary.c`) both store, and so
   both embed.

| Mode | Behaviour |
| --- | --- |
| `off` | Today. Both run inline. Byte-identical (pinned by `off_gate_is_byte_identical_to_no_window`). |
| `shadow` | Both still run inline. The flush logs one line per turn: `[HU_POST_SEND_DEFER shadow] jobs= extract= embed= ran=0 inline_full= facts= facts_literal= flush_ms=`. Counts only. |
| `live` | Both run in the flush, FIFO, inside the background lane (`batch`). Each still runs exactly once per turn, with the same input and the same merge. Outside an armed window, for example in the CLI or the gateway, the work runs inline. A full queue (32) also runs inline. If a window is never flushed, the next `begin` runs its jobs. |

### Why this is gated

The 2026-10-02 profile said the reply never reads extraction's result. It does.
The extracted facts merge into `agent->personal_model` before the prompt is
built, and the same turn reads them back:

- `hu_personal_model_build_prompt` puts them in the "Key facts:" and "Avoid:"
  lines;
- `hu_personal_model_contradicts_user` (`agent_turn.c`) checks them.

The service log over all history (2026-09-24 to 10-02) holds 601 `live:
merging` lines against 907 `clean zero`, so roughly 40% of extractions change
that turn's prompt.

Under `live` those facts reach the prompt one turn later. Two smaller effects:

- A guard retry inside the same window cannot find this turn's new rows by
  vector. Keyword recall still finds them, because the row is stored
  immediately.
- A crash before the flush loses that turn's extraction. Inline, the facts
  would already have been saved with the model.

### Promotion: SHADOW to LIVE

1. Set `HU_POST_SEND_DEFER=shadow` in the service-loop plist and restart.
2. After at least 50 sent reply turns, run
   `python3 scripts/reply_latency_report.py --since <restart time>`. Its
   `post_send_defer` line sums the shadow counts.
3. Compute `non_literal = facts - facts_literal`. These are the facts that
   would reach the prompt one turn late and are not already in the inbound,
   which the prompt carries anyway.
   - If `non_literal / sent <= 0.10`, flip to `live`. That is at most one
     late-only fact per ten turns.
   - If it is higher, the delay changes the prompt often enough to need the
     blind A/B before flipping.
4. After `live`, run the report again. Processing p50 should fall by about the
   extraction time (~1.4 s when `extract=1`) plus the embedding time. If it
   does not, check that `ran` equals `extract + embed` and that `inline_full`
   is 0.

### Rollback

```bash
/usr/libexec/PlistBuddy -c "Set :EnvironmentVariables:HU_POST_SEND_DEFER off" \
  ~/Library/LaunchAgents/ai.human.service-loop.plist
launchctl bootout gui/$(id -u)/ai.human.service-loop
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/ai.human.service-loop.plist
```

The purpose tags and the `batch` header have no gate. To revert them, revert
the commit.

## Measuring

`scripts/reply_latency_report.py` reads the service log and prints counts and
timings only. It reports total, delay, processing, in-to-call, turn and tail
p50/p90, plus local chat, local embedding and cloud calls per turn. It never
prints handles or text. On the 2026-09-29 to 10-01 window it reproduces the
profile: 120 sent, processing p50 48 s. Run it before and after a deploy, with
`--since` set to the restart time.

## Related

- `docs/guides/length-policy.md`: another per-turn gate with the same OFF,
  SHADOW, LIVE shape.
- `.claude/rules/feature-gate-requires-measurement.md`: the activation
  contract.
