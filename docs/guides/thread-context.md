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

The block sits between local-only markers (`include/human/providers/local_only.h`).

- The reliable provider strips every marked span from the request before any
  attempt on a provider that is not on-device. In prod that is the `gemini`
  extra the `mlx_local` primary fails over to: on a failed call, on a
  circuit-open window, and on the degradation and T4 cloud-model retries,
  which all go through the same provider. A truncated block (no end marker)
  is stripped to the end of the message.
- The block is not built at all when the reply provider's primary is not
  local (`local=0` in the log).
- Prod log 2026-09-19 → 2026-10-01: the circuit opened 25 times, and each
  opening routes every primary request to the gemini extra for 300 s. T4
  local→cloud retries: 0.

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
