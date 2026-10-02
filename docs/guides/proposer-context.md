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
| memory | `hu_contact_insights_render` (6 items, confidence ≥ 0.5), filtered by `hu_daemon_callback_content_is_safe` | 640 B |

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
  captured. The history buffer is freed exactly as before.
- The enriched call goes to that local provider **directly**, with no reliable
  chain and no Gemini fallback. If the local call fails, returns empty or
  returns unparseable output, the result is `local_unavailable`. The block is
  never retried elsewhere.
- No log line contains message text, drafts, reasons, names or numbers.

## Gate values

`HU_PROPOSER_CONTEXT`, parsed by `hu_gate_mode_from_env`; unset or unrecognized means OFF.

| Value | Effect |
|---|---|
| `off` (default) | Today's `hu_init_proposer_tick_with_provider_ex` call, byte-identical. Nothing is captured. |
| `shadow` | Today's call decides and its result is what is used. Afterwards, if that call reached the model, the enriched prompt runs once on the local provider through `hu_init_proposer_decide_once`. That function has no governor, guard, decision row, send or logging of its own. This runs **at most once per contact per proactive pass**. One line is logged per contact-cycle. |
| `live` | The enriched prompt decides, on the local provider. If the local call returns `LLM_ERROR` or `PARSE_ERROR`, the decision is made by today's un-enriched call instead. That call may go through the reliable chain, exactly as today. |

The first proactive pass with the gate on logs one banner. The banner says
whether a loopback provider was found.

### The shadow line

```
[HU_PROPOSER_CONTEXT shadow] outcome=ok contact_idx=3 contact_bytes=412 conversation_bytes=980
  memory_bytes=310 block_bytes=1790 days_since=4 prod_result=9 local_err=0 should_propose=1
  confidence=0.880 reason_len=0
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
  `5` is FIRED, `8` is LOW_CONFIDENCE and `9` is NEGATIVE.

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
2. **Would-propose rate per contact-cycle.** This is
   `should_propose=1 / outcome=ok`. Compare it with today's rate on the *same*
   lines, where `prod_result ∈ {5, 8}`. Promotion requires the enriched rate to
   be at least 10 points higher, and no more than 60%. A proposer that wants to
   text every time is not more human.
3. **Distinct contacts with would-propose.** Count the distinct `contact_idx`
   with at least one `should_propose=1`. Promotion requires at least 3. Over 14
   days, today's path reached the proposer for only 2 distinct contacts.
4. **Blind check of about 20 shadow drafts.** The shadow line never logs
   drafts, so they come from an offline script run locally by the owner. This
   script is not built yet:
   - Sample 20 `outcome=ok, should_propose=1` events.
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
