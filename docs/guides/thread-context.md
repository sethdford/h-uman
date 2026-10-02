---
title: Thread context and history budget — gates and promotion measurement
created: 2026-10-01
status: operator-facing
---

# Thread context and history budget

On a reactive iMessage turn under `llm_decides` the reply model could not see
the conversation thread. Two causes, two gates. Both start `off`, and each one
moves forward only on a measurement.

Audit evidence (2026-10-01, `~/.human/logs/service-loop-error.log`): 350 of 944
provider calls (37%) carried `msgs=2`, the system prompt plus the current
message. Among reply turns (`tools=0`) it was 116 of 247 (47%).

## Gate 1: `HU_THREAD_CONTEXT` (recent thread block, daemon plist)

The daemon already loads the last 25 chat.db messages for the contact
(`load_conversation_history`). Under `llm_decides` they fed only emotion
detection, quality scoring and prospective memory. The model's history was the
session store, which only the daemon writes, so it never held a reply Seth
typed himself.

| Value | Effect |
|---|---|
| `off` (default) | No work. `conversation_context` is byte-identical. |
| `shadow` | Renders the block and logs one line per reactive turn: `[HU_THREAD_CONTEXT shadow] lines=N bytes=N seth_lines=N dropped=N skipped_current=0/1 applied=0 local=1`. The prompt is unchanged. |
| `live` | Appends the block to `conversation_context` (after the prospective note, before the director's scene direction), and logs the same line with `live` and `applied=1`. |

The block, at most 15 messages and 1,536 bytes, newest kept:

```
## Recent thread (you and Mike, oldest first)
[2d ago]
Mike: you around saturday?
you: ya should be
[5h later]
Mike: [photo]
## End of recent thread
```

- One line per message, labelled with the contact's first name or `you`.
- `[Nm/Nh/Nd ago]` before the first line, and `[… later]` before any gap of
  30 minutes or more.
- Attachments collapse to `[photo]`, `[voice memo]`, `[video]` or
  `[attachment]`. Message text is capped at 240 bytes.
- The trailing contact messages the model already receives as the current
  message are left out (`skipped_current=1`).
- No block for group chats (chat.db rows carry no sender) or for an empty
  thread.

It does not de-duplicate against session history. Dropping the thread lines
that also appear in the session store would leave holes that read as
unanswered messages. The block is capped at 1.5 KB instead.

### Never sent to a cloud model

The block sits between local-only markers. `include/human/providers/local_only.h`
is the one place that strips private spans: a table of headings, each closed
by an end line or by the first blank line. To protect another section, add a
row to it.

- **Every attempt is checked.** The reliable provider strips every span from
  any attempt, first or fallback, whose provider is not on-device **or** whose
  model name is a cloud model's (`gemini*`, `gpt-*`, `claude*`, ...). That
  covers:
  - the `gemini` extra that the `mlx_local` primary fails over to (failed
    call or circuit-open window);
  - the `agent_turn.c` routes that switch model by name on the same provider:
    the analytical tier to `gemini-3.1-pro-preview`, S3 to `fallback_model`,
    the on-device-failure retry to the reflexive model, and the degradation
    retry. Each one has a test in `tests/test_local_only.c`.
- **A truncated block is still stripped.** If there is no end marker, the
  strip runs to the end of the message. When stripping cannot allocate, the
  attempt fails rather than sending the unstripped text.
- **No local primary, no block.** The block is not built when the reply
  provider's primary is not local (`local=0` in the log).
- **How often fallback fired.** In the prod log from 2026-09-19 to 2026-10-01,
  the circuit opened 25 times. Each opening routes every primary request to
  the gemini extra for 300 s. 66 calls went out with
  `model=gemini-3.1-pro-preview` (the analytical route). The one inspected
  (2026-09-19 04:16:22) hit :8741 first and then went to Vertex
  `gemini-3.8-flash` with the full 19 KB prompt. There were 0 T4 local→cloud retries.

### Promotion: `shadow` → `live`

1. In shadow, over at least 3 days of reactive traffic:
   ```bash
   LOG=~/.human/logs/service-loop-error.log
   grep -o '\[HU_THREAD_CONTEXT shadow\] lines=[0-9]* bytes=[0-9]*' "$LOG" \
     | awk '{split($3,l,"="); n++; if (l[2] > 0) k++} END {print "turns", n, "non-empty", k, k/n}'
   grep -o '\[HU_THREAD_CONTEXT shadow\] lines=[1-9][0-9]* bytes=[0-9]*' "$LOG" \
     | sed 's/.*bytes=//' | sort -n | awk '{a[NR]=$1} END {print "median bytes", a[int((NR+1)/2)]}'
   ```
   Report the share of turns with a non-empty block and its median bytes. A
   non-empty share near 0 means the block cannot help, so stop there.
2. The n=40 blind A/B (`scripts/blind_ab/`, see its `PROTOCOL.md`), arm B with
   the block in context:
   ```bash
   cd scripts/blind_ab
   python3 export_seth_triples.py ...            # real contexts + Seth's replies
   python3 gen_huuman_replies.py contexts.json --out triples.json
   python3 make_rating_sheet.py triples.json --seed <seed>
   python3 score.py rating_sheet_*.csv --key answer_key.json --rater human
   ```
   Go `live` only on `RESULT_blind_ab=PASS` with a detection rate no worse than
   the current baseline. Caveat: `gen_huuman_replies.py` drives `human eval
   run`, not the daemon's reactive path, so it never calls this renderer. For
   the thread arm, each context must carry the rendered block.

Rollback: set `HU_THREAD_CONTEXT=off` (or delete the key) in
`~/Library/LaunchAgents/ai.human.service-loop.plist`, then
`launchctl bootout gui/$(id -u)/ai.human.service-loop && launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/ai.human.service-loop.plist`.

## Gate 2: `HU_HISTORY_BUDGET` (request history budget, daemon plist)

`hu_agent_internal_fit_history` (A1b, 2026-05-19) dropped the oldest history
until the system prompt plus history fit 20 KB. The system prompt is capped at
24 KB and often runs 22 KB, so that budget was spent before any history
counted, and every prior message was dropped. The prod log shows it:
`history truncated: dropped 6 oldest messages ... (now 2 msgs, 32836 bytes)`.

| Value | Effect |
|---|---|
| `off` (default) | Legacy policy, byte-identical messages. |
| `shadow` | Legacy policy is applied. One line per call: `[HU_HISTORY_BUDGET shadow] msgs_before=N msgs_after=N new_msgs_after=N sys_bytes=N hist_bytes=N new_keeps_more=0/1`. |
| `live` | The 20 KB counts history only, the current message always stays, and system plus history is capped at `HU_HISTORY_BUDGET_MAX_TOTAL_BYTES` (default 40,960, clamped to 20,480–98,304). Logs `[HU_HISTORY_BUDGET live] ... total_bytes=N max_total=N`. |

The old log-once `history truncated` warning is now a counter, logged on the
first truncation and every 25th: `history truncated (N so far, logged every 25)`.

The provider limit: GLM-4.5-Air's `max_position_embeddings` is 131,072
tokens, and the mlx server on :8741 sets no `max_kv_size`. So the 96 KB clamp
ceiling stays inside the window even at one byte per token. Latency is the
real constraint: the 2026-09-06 re-measure saw cold prefill at 15.0 s for
24 KB and 27.5 s for 40 KB. The 40 KB default is the largest prompt measured
to answer.

The stream path (`hu_agent_turn_stream_v2`) does not call this function. Prod
runs with `mlx_local.streaming_enabled=false`, so every reply goes through
`hu_agent_turn`.

### Promotion: `shadow` → `live`

1. Shadow share of calls where the new policy keeps more history:
   ```bash
   grep -o '\[HU_HISTORY_BUDGET shadow\].*new_keeps_more=[01]' ~/.human/logs/service-loop-error.log \
     | awk '{n++; if ($NF ~ /=1$/) k++} END {print "calls", n, "keeps_more", k, k/n}'
   ```
2. Cold prefill latency at the new totals. Check the `[HU_HISTORY_BUDGET live]`
   `total_bytes` distribution against the 2026-09-06 latency curve, and set
   `HU_HISTORY_BUDGET_MAX_TOTAL_BYTES` lower if p90 latency is unacceptable.
3. The same n=40 blind A/B as gate 1, with `HU_HISTORY_BUDGET=live` on the
   generating instance.

Rollback: `HU_HISTORY_BUDGET=off` (or delete the key) in the plist, then
bootout and bootstrap as above.
