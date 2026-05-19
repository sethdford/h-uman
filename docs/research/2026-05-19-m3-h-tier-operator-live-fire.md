# M3 H-tier Operator-in-the-Loop Live-Fire — 2026-05-19

First end-to-end live-fire of the M3 active-learning probe with a REAL
operator-supervised round trip. This was the irreducible last mile from
the post-ship audit: the only path that could not be proven by mocks.

## What was tested

The full H3 → H3b loop with the real osascript dispatch wire and real
chat.db poll wire:

  1. `m3_active_probe.py --delivery=queue` — queue a probe from the
     live corpus (`~/.human/training-data/m3-corpus.jsonl`, 574 records)
  2. `m3_probe_collector.py --mode=dispatch --operator-handle=<X>
     --confirm-real-send` — invoke `osascript` to send the probe to
     Messages.app
  3. Operator (Seth) reads the probe on his other Apple device and
     replies with a letter
  4. `m3_probe_collector.py --mode=poll --operator-handle=<X>` —
     scan chat.db for the reply, convert to Alpaca-DPO pairs, mark done

## Timeline (real UTC timestamps from this run)

| Step | Time | What |
|------|------|------|
| Queue write | 07:08:18 | H3 queued one probe from corpus; user_message + candidates serialized |
| osascript send | 07:08:19.063 | dispatch wire invoked, queue flipped to `status=sent` |
| chat.db write | 07:08:19.387 | Apple's Messages framework persisted the outbound probe (Δ=324 ms) |
| Operator reply | 07:10:15.411 | Seth typed "C" on his other device, synced into chat.db (Δ from send=116 s) |
| Poll | 07:13:01.626 | collector found the reply, ran `response_to_pairs`, wrote 2 pairs, marked `status=done` |

## What landed

`~/.human/training-data/m3-active-probe-pairs.jsonl` (2 entries):

  pair[0] `_source=active_probe`
    prompt:    'Okay sometimes it sounds ai haha'
    chosen:    "Thanks for sharing. Noted about 'Okay sometimes it sounds ai haha'; got it."
    rejected:  'ok'

  pair[1] `_source=active_probe`
    prompt:    'Okay sometimes it sounds ai haha'
    chosen:    "Thanks for sharing. Noted about 'Okay sometimes it sounds ai haha'; got it."
    rejected:  'yeah, makes sense — thanks for the heads up'

These are the first GOLD-LABEL preference pairs M3 has ever generated.
Every prior pair was either a synthetic counterfactual (H2) or a
simulated reply (`--simulate-response` in tests). This is the real
signal the personalization loop was built to consume.

## Queue entry final state

  status:                  'done'
  operator_reply:          'C'
  operator_reply_ts_ms:    1779189015411
  pairs_written:           2
  done_ts_ms:              1779189181626

## What this proves

  - The osascript dispatch wire actually delivers an iMessage on macOS
  - The chat.db poll wire actually finds the inbound reply
  - The pair-emission contract (`response_to_pairs`) converts gold-label
    operator input into the same Alpaca-DPO shape that H2 produces
  - The `--confirm-real-send` safety gate works as advertised: dry-run
    leaves the queue unchanged; only `--confirm-real-send` flips status

## Finding: attributedBody decoding is a follow-up

The probe MESSAGE Apple stored as `text=NULL` + `attributedBody=450
bytes` (NSAttributedString typedstream — multi-line, emoji-bearing
content). Seth's REPLY ("C") was short enough that Apple stored it as
`text='C'` + `attributedBody=176 bytes` (still allocated but with the
plain string also in `text`).

Implication for production:
  - Short replies (letter picks A/B/C, brief freetext) are reliably
    captured by the current poll filter (`WHERE m.text IS NOT NULL`)
  - LONG freetext replies, especially with emoji or formatting, would
    go into attributedBody only — and our poll would miss them
  - Same gap exists in H1's `extract_imessage` for the Seth-side
    corpus building

This warrants its own slice: a Python typedstream decoder that
extracts the visible body from `attributedBody` when `text` is NULL.
Without a third-party dep, the cleanest path is a small byte-scanner
that finds the body chunk between class-registry markers and the
attribute dictionary.

## What this loop cost vs synthetic data

  - 1 probe sent
  - ~3 minutes operator latency (Seth's reply time)
  - 1 person-second of operator effort (tapping "C")
  - 2 GOLD-LABEL preference pairs

At that rate, 100 probes per week (auto-queued by m3_loop_cycle.sh) +
1-tap replies on the phone = ~200 gold pairs/week. That's the moat.

## Reproducing this run

  # Refresh the corpus (one-time per cycle)
  make m3-extract

  # Queue one probe
  make m3-probe

  # Dry-run dispatch (no iMessage sent — validation only)
  M3_OPERATOR_HANDLE=<your-icloud> make m3-collect MODE=dispatch

  # LIVE dispatch (sends one real iMessage)
  M3_OPERATOR_HANDLE=<your-icloud> python3 scripts/m3_probe_collector.py \
      --mode dispatch --confirm-real-send \
      --operator-handle <your-icloud>

  # ... reply on phone with A/B/C or freetext ...

  # Poll for the reply
  M3_OPERATOR_HANDLE=<your-icloud> make m3-collect MODE=poll
