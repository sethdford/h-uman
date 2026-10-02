---
title: Proposer context — HU_PROPOSER_CONTEXT gate, local pinning and promotion
created: 2026-10-01
status: operator-facing
---

# Proposer context (`HU_PROPOSER_CONTEXT`)

The per-contact proactive proposer decides "should I text this person now?".
Until this gate it decided without the thread with that person. The daemon
loaded the last 15 messages, used only the inbound text for event extraction,
and freed them before the proposer ran. The briefing also lacked the contact's
profile beyond relationship and Dunbar layer, how long it had been, and the
`contact_insights` stream.

This gate adds one pre-rendered block to the proposer's user message. It is
built by `src/daemon/daemon_proposer_context.c` and is at most
`HU_PROPOSER_CTX_BUDGET` = 3072 bytes, headers included:

| Field | Source | Cap |
|---|---|---|
| contact | `hu_contact_profile_build_context`, whole lines only (the `--- Contact profile for <number> ---` header is dropped) | 640 B |
| conversation | the last 10 messages of the real thread, as `[3d ago] them: …` and `[2h ago] Seth: …`, newest lines kept; the header carries days since the last message and since their last message | 1600 B |
| memory | `hu_contact_insights_render_for_query` with the thread as the query: 3 items, confidence ≥ 0.5, chosen exactly as the reactive path chooses them (`hu_contact_insights_select`). Each insight line is filtered by `hu_daemon_callback_content_is_safe` separately, so one unsafe line does not drop the rest | 640 B |

Commitments, due follow-ups, topic absences and callbacks are not repeated
here. They already reach the proposer through the `situation` and
`due_followups` sections.

## Where it is not used

The owner-facing Initiative tick (`daemon.c`, `hu_init_proposer_tick_with_provider`)
logs `context bundle: fields=2 … contact=0 conversation=0 …` and proposes to
`initiative.target_handle`. That bundle is a different path. Its `contact` and
`conversation` slots borrow `agent->contact_context` and
`agent->conversation_context`. Those are set per reactive batch and cleared
after it, so they are always empty at the tick. Its `memory`, `personal_model`,
`awareness` and `stm` slots were never implemented. This gate does not change
that tick.

## Privacy: the thread only reaches a local model

The block contains real message text, so it is never sent to a cloud model:

- It is built only when the agent's provider resolves to a **loopback**
  OpenAI-compatible backend (`hu_proposer_context_local_provider`). That means
  the provider itself, or the primary of the `reliable` wrapper, and only while
  the wrapper's circuit is closed (`hu_reliable_primary`). Otherwise nothing is
  captured. The history buffer is freed exactly as before. The circuit is
  checked again immediately before each local call, not only when the pass
  starts.
- The enriched call goes to that local provider **directly**, with no reliable
  chain and no Gemini fallback. If the local call fails, returns empty or
  returns unparseable output, the result is `local_unavailable`. The block is
  never retried elsewhere.
- The `[HU_PROPOSER_CONTEXT …]` line never contains message text, drafts,
  reasons, names or numbers.
- In LIVE, when the enriched block was used, the proposer's own log lines are
  redacted too. The `LLM verdict (ex, …)` line logs `reason_len=` instead of
  the reason, and the 14-day repeat rejection logs `draft_len=` instead of the
  first 60 characters of the draft. Without the block, both lines are
  unchanged.
- Two things outside the log still hold proposer text, as they do today:
  - **The decision table.** In LIVE, a FIRED decision writes the first 64
    bytes of the draft as `message_ref` in `proactive_decisions`, the local
    decision table.
  - **The DPO negatives file.** A draft the guard rejects is written to
    `~/.human` as a DPO negative, together with the **full user message,
    including the block**. Each JSONL line is capped at 8 KiB. An enriched
    prompt of about 5–7 KB can exceed that once JSON-escaped, and then the
    negative is dropped (`hu_response_guard_log_dpo_negative` returns
    `HU_ERR_IO`) rather than truncated. Both stay on this machine.

## Gate values

`HU_PROPOSER_CONTEXT`, parsed by `hu_gate_mode_from_env`; unset or unrecognized means OFF.

| Value | Effect |
|---|---|
| `off` (default) | Today's `hu_init_proposer_tick_with_provider_ex` call, byte-identical. Nothing is captured. |
| `shadow` | Today's call decides and its result is what is used. Afterwards, if that call reached the model, **two** prompts run on the local provider through `hu_init_proposer_decide_once`: a **control** with today's un-enriched inputs, and the **enriched** prompt. Each answer goes through the same verdict pipeline production uses (`hu_init_proposer_final_verdict`: confidence threshold, response guard, 14-day repeat check), with no decision row, DPO capture or send. Comparing control with enriched on the same local model keeps the provider out of the comparison; production is mostly served by Gemini on fallback today. This runs **at most once per contact per proactive pass**. One line is logged per contact-cycle. **Cost: two local GPU calls per contact per pass, double the single-call design** (about 15 extra pass-contacts a day at today's volume, so about 30 local calls). |
| `live` | The enriched prompt decides, on the local provider. If the local call returns `LLM_ERROR` or `PARSE_ERROR`, the decision is made by today's un-enriched call instead. That call may go through the reliable chain, exactly as today. The failed local call writes no decision row (`defer_llm_failure_row`), so each contact-cycle has exactly one row, holding the final outcome. |

The first proactive pass with the gate on logs one banner. The banner says
whether a loopback provider was found.

### The shadow line

```
[HU_PROPOSER_CONTEXT shadow] outcome=ok contact_idx=3 contact_bytes=412 conversation_bytes=980
  memory_bytes=310 block_bytes=1790 days_since=4 prod_result=9 prod_provider=fallback
  ctrl=9 rich=5 ctrl_conf=0.300 rich_conf=0.880 local_err=0
```

- `outcome` is one of:
  - `ok`;
  - `local_unavailable` (no loopback provider, circuit open, or the local call failed; `local_err` is the `hu_error_t`);
  - `rate_limited` (already ran for this contact this pass);
  - `not_reached` (today's call was gated before the model);
  - `empty` (no profile, thread or insights).
- `contact_idx` is the contact's index in the persona's contact list. It is
  stable across restarts and is not a name or number.
- `prod_result` is today's decision, as a `hu_init_proposer_result_t` value.
  `5` is FIRED, `8` is LOW_CONFIDENCE, `9` is NEGATIVE and `10` is
  GUARD_REJECT.
- `prod_provider` says which provider served today's call: `primary` (the
  local model), `fallback` (Gemini), `direct` (not a reliable wrapper) or
  `none`.
- `ctrl` and `rich` are the final verdicts of the control and enriched
  prompts on the local model, after threshold, guard and repeat check. `-1`
  means not run. In LIVE, `rich` is the verdict that was used.

## Known blocker before any data arrives (measured 2026-10-01)

The local GLM does not answer the proposer prompt today. From 09-19 to 10-01:

- 193 of 200 per-contact verdicts, and 403 of 423 owner-path verdicts, had a
  POST to Vertex `gemini-3.8-flash` within the 60 s before them. There were
  3,488 such POSTs in total.
- The mlx-server log shows 2,099 replies of only `</think>`.
- h-uman strips that reply to empty, and the reliable wrapper's empty-reply
  failover sends the prompt to Gemini.

Until that is fixed, every shadow line will read `outcome=local_unavailable`.
That is the gate working, and it is not a measurement. A separate
investigation owns the GLM fix.

## Promotion: SHADOW → LIVE

Run shadow for **at least 3 days**, with the local proposer answering. Then
compute these from the `[HU_PROPOSER_CONTEXT shadow]` lines:

1. **Coverage.** At least 30 `outcome=ok` lines, from at least 3 distinct
   `contact_idx`. Report `local_unavailable` as a share of contact-cycles that
   reached the model. If it is above 20%, the result is INCONCLUSIVE, not PASS.
2. **Would-propose rate per contact-cycle.** Over the `outcome=ok` lines,
   compare `rich=5` (FIRED) with `ctrl=5`. Both are final verdicts from the
   same local model, so the difference is the context, not the provider or
   the threshold. Promotion requires the enriched rate to be at least 10
   points higher than the control, and no more than 60%. A proposer that
   wants to text every time is not more human. `prod_result` is reported
   alongside but is not the baseline, because it is mostly Gemini's answer.
3. **Distinct contacts with would-propose.** Count the distinct `contact_idx`
   with at least one `rich=5`. Promotion requires at least 3. Over 14 days,
   today's path reached the proposer for only 2 distinct contacts.
4. **Blind check of about 20 shadow drafts.** The shadow line never logs
   drafts, so they come from an offline script run locally by the owner. This
   script is not built yet:
   - Sample 20 `outcome=ok, rich=5` events.
   - Rebuild each event's enriched prompt from `chat.db` and `memory.db` as of
     that time, with the same builder.
   - Run it once against the **local** model only.
   - Show each draft blind, next to a real Seth-authored opener to the same
     person.
   - The owner rates each draft "would send / wrong / intrusive / stale".
   - PASS requires at least 16 of 20 rated "would send", and none with the
     wrong person's news or a repeated check-in.

LIVE also requires that the local proposer answers. If it does not, LIVE
silently reverts to today's behaviour on every call.

## Rollback

Unset the gate, or set `HU_PROPOSER_CONTEXT=off`, in the launchd plist with
`scripts/install-human-daemon.sh`. Never hand-edit the plist or `cp` over the
running binary. Then run `scripts/verify-deploy.sh`. OFF is exactly the
previous call.
