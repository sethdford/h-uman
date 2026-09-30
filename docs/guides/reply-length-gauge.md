---
title: Reply-Length Gauge
---

# Reply-Length Gauge

`scripts/reply_length_gauge.py` measures, per contact and overall, how the
daemon's sent 1:1 replies compare in length with Seth's own replies to the
same contact -- a standing, YapBench-style (arXiv 2601.00624) check for both
directions of the failure: too brief (the problem the 2026-09-22 specificity
measurement found -- see `project_h-uman_specificity_length_confound_2026-09-22`
memory) and too long (YapBench's own concern).

## Why

The 2026-09-22 specificity measurement found that 74% of the apparent
specificity gap between the daemon's replies and Seth's own was explained by
reply LENGTH alone: the daemon averaged 37.7 characters per reply against
Seth's 71.3. That was a one-off audit. This gauge makes the same comparison a
standing, nightly measurement instead of something that has to be
rediscovered.

## Attribution

Reuses `eval_conversation_quality.attribute()` exactly as
`scripts/measure_contact_reply_lengths.py` does: every 1:1 outbound message
is labeled `"seth"`, `"huuman"`, or `"ambiguous"` (a delivered text that
doesn't match what the daemon logged). Only `"seth"` and `"huuman"` count
toward this gauge -- an ambiguous send can never be credited to, or blamed
on, the daemon.

Lengths are **characters** of the text (Python `len(str)`); a `bytes_p50`
field (UTF-8 encoded) is reported alongside every side's percentiles for
continuity with the daemon's own reply-length cap, which compares bytes.

## Metrics

Per contact with at least `--min-n` (default 10) of **both** seth- and
huuman-labeled replies in the window:

| Field | Meaning |
| --- | --- |
| `seth.p10` / `p50` / `p90` | Seth's own reply-length distribution to this contact |
| `huuman.p10` / `p50` / `p90` | the daemon's reply-length distribution to the same contact |
| `median_ratio` | `huuman.p50 / seth.p50` -- 1.0 means the daemon's typical reply is exactly as long as Seth's |
| `brevity_rate` | share of the daemon's replies shorter than **this contact's own** `seth.p10` |
| `excess_rate` | share longer than this contact's own `seth.p90` |
| `excess_chars_mean` | mean, over the daemon's replies, of `max(0, len - seth.p90)` -- the YapScore-style overshoot |
| `deficit_chars_mean` | mean of `max(0, seth.p10 - len)` -- the undershoot |

`overall` pools the same metrics across every measured contact, weighting
each **reply** equally (not each contact), plus `contacts_measured` and
`contacts_skipped_min_n` (contacts with real 1:1 traffic that didn't clear
`--min-n` on one or both sides).

## Refusals

Per `.claude/rules/no-number-without-a-measurement.md`, the gauge refuses
(exit 2, writes nothing) rather than emit a number it didn't measure:

- `chat.db` or `memory.db` is unreadable.
- Zero contacts meet `--min-n` on both sides.

## Output

Counts and metrics only -- never message text, handles, or contact names.
Per-contact entries are keyed by an index (`c1`, `c2`, ...) sorted by total
reply volume, never by handle. Written atomically at mode `0600` to
`~/.human/logs/reply-length-YYYYMMDD.json`, following the same private-write
pattern as `scripts/curator_names.py`.

## Usage

```bash
python3 scripts/reply_length_gauge.py [--days 14] [--min-n 10]
```

Prints a one-line summary (overall `median_ratio`, `brevity_rate`,
`excess_rate`, `contacts_measured`) and, on success, the path it wrote.

## Tests

`tests/test_reply_length_gauge.py` (hermetic pytest, CI: see the
"Insight-stream + curator + second-opinion pins" step in
`.github/workflows/ci.yml`): metric values against a hand-computed fixture,
UTF-8 byte percentiles for multibyte text, ambiguous-send exclusion, the
per-contact `--min-n` skip, both refusal paths (unreadable db, zero measured
contacts), and that the written file contains no planted text or handle and
is mode `0600`.

## Related

- `scripts/measure_contact_reply_lengths.py` -- the sibling tool this reuses
  attribution from; measures only Seth's own reply lengths, to set the
  per-contact floor on the daemon's reply cap.
- `scripts/eval_conversation_quality.py` -- the source of `attribute()`.
- `scripts/curator_names.py` -- the private atomic-write pattern this gauge
  follows for its own output file.
