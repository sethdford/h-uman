---
title: Memory benchmarks on the reply path (LoCoMo, MSC, SOC-2508)
created: 2026-10-02
status: operator-facing
---

# Memory benchmarks on the reply path

"Remembers every conversation and detail" is a claim about what Seth's replies
contain. This guide measures it on the production reply path: public
long-conversation datasets are converted into replay turns, each turn goes
through `human replay` (the real-turn replay harness), and the replies are
scored for whether they recall the right detail.

This is not the W16 LoCoMo suite. That suite scores **retrieval** P@1 against the
memory store ([locomo-method.md](../evaluation/locomo-method.md)). This guide
scores **what the reply actually says**.

## What these benchmarks can and cannot tell you

- **They measure memory mechanics, not voice.** Every speaker is a
  crowd-sourced or LLM-generated stranger, not Seth. A probe tells you whether
  the pipeline retrieved, grounded and called back a detail. It says nothing
  about whether the reply sounds like Seth. Voice is the blind A/B's job.
- **Never train the persona on them.** Not for SFT, not for DPO, not as style
  examples, not as few-shot banks. They are other people's words. Each
  `PROVENANCE` file repeats this.
- **Never commit dataset files.** The fetch scripts write only to
  `~/.human/datasets/<name>/`. The fixtures in
  `tests/fixtures/memory_benchmarks/` are synthetic text written in each format,
  not samples.

## Datasets and licences

| Dataset | Source, pinned | sha256 | Licence | Use here |
|---|---|---|---|---|
| **LoCoMo** (Maharana et al., ACL 2024) | `snap-research/locomo@3eb6f2c585f5e1699204e3c3bdf7adc5c28cb376`, `data/locomo10.json` | `79fa87e9…ea698ff4` | **CC BY-NC 4.0** (`LICENSE.txt`) | Internal evaluation only. Do not redistribute, publish derived files, or use it in anything sold. Non-commercial internal measurement is within the licence. |
| **Multi-Session Chat** (Xu et al., 2022) | ParlAI `msc_v0.1.tar.gz` from `dl.fbaipublicfiles.com` | `e640e37c…cb45e60` (the same value ParlAI's `parlai/tasks/msc/build.py` pins) | No dataset-specific licence file in the tarball. It is distributed through ParlAI, whose repo is MIT. | Research and internal evaluation. Re-check before any external use. |
| **SOC-2508** (optional prior) | Hugging Face `marcodsn/SOC-2508@8a95510c3a404edd428734cf9dbcb1671e609425`, `data.jsonl` (9.3 MB) | `44c5a797…1573bf67` | **CC BY 4.0** (dataset card) | Multi-message bursts and reply delays only. Attribute if anything derived is shown. The chats are synthetic (Qwen3-235B). |

Full checksums are the `PINNED_SHA256` constants in `scripts/datasets/fetch_*.sh`.

`scripts/fetch-evaluation-datasets.sh` (W16) used to describe LoCoMo as MIT and
pointed at the wrong org. Both comments are corrected in the same change.

## Commands

### 1. Fetch

The fetch is idempotent and checksum-verified. A present file with the right
sha256 is never downloaded again. A corrupted file is replaced. On a mismatch,
nothing is written and the script exits 2.

```bash
bash scripts/datasets/fetch_locomo.sh     # ~/.human/datasets/locomo/locomo10.json
bash scripts/datasets/fetch_msc.sh        # ~/.human/datasets/msc/session_5/{valid,test}.txt
bash scripts/datasets/fetch_soc2508.sh    # ~/.human/datasets/soc2508/data.jsonl
```

`HU_DATASETS_DIR` overrides the root. The tests point `HU_FETCH_TEST_URL` and
`HU_FETCH_TEST_SHA256` at `file://` fixtures. Those two variables are refused
unless both are set, so an override can never drop the checksum.

### 2. Convert

```bash
H=~/.human/datasets
python3 scripts/datasets/locomo_to_replay.py --input $H/locomo/locomo10.json \
    --out-dir $H/locomo/replay [--per-category 50]
python3 scripts/datasets/msc_to_replay.py \
    --input $H/msc/session_5/valid.txt $H/msc/session_5/test.txt --out-dir $H/msc/replay
python3 scripts/datasets/soc2508_to_replay.py --input $H/soc2508/data.jsonl \
    --out-dir $H/soc2508/replay
```

Each converter writes three files to its output directory:

- `turns.jsonl`: one replay turn per contact burst that Seth answered.
- `probes.jsonl`: memory probes. LoCoMo and MSC produce them. SOC writes an
  empty file.
- `stats.json`: the counts, also printed to stdout.

Useful flags:

| Flag | Default | Effect |
|---|---|---|
| `--seth-speaker` | LoCoMo `b`, MSC `2` | Which speaker plays Seth. SOC's equivalent is `--seth-persona` (default `2`). |
| `--window` | 25 | The history window the reply path sees. |
| `--probe-gap-hours` | 24 | When the question is asked, measured from the last message. |
| `--history-limit` | 0 (all) | Truncates the history. |
| `--photo-captions` | off | Renders images as `[Photo: caption]` instead of `[Photo]`. |
| `--per-category N` | 0 (all) | LoCoMo only: a deterministic sample of N probes per category. |

### 3. Replay (sibling branch `feat/replay-harness`)

Run each probe file through `human replay`, one process per arm. The runbook
lives in `docs/guides/replay-harness.md` on that branch. The flags below are
from its WIP header, `include/human/cli_replay.h`.

```bash
HU_STATE_DIR=<snapshot dir> HU_MEMORY_SQLITE_PATH=<snapshot memory.db> \
  human replay --in $H/locomo/replay/probes.jsonl --out base.jsonl --arm base \
  --endpoint http://127.0.0.1:<spare port>/v1
```

`human replay` refuses to start unless state and memory point at a private
snapshot outside `~/.human`, and it refuses any endpoint that is not loopback.
Use a spare server for long runs. **Do not run the full benchmark against the
production server on :8741.**

### 4. Score

```bash
python3 scripts/datasets/memory_probe_score.py --probes $H/locomo/replay/probes.jsonl \
    --arm base=base.jsonl --arm cand=cand.jsonl --json report.json
```

The scorer prints a table per arm. It shows hit rate by category (`single_hop`,
`multi_hop`, `temporal`, `open_domain`, `adversarial`) and by window
(`in_window` vs `beyond_window`), plus `missing` and `no_reply` counts. The
`--json` report adds `contains_rate`, `mean_f1`, `mean_recall` and a breakdown
by dataset.

The scorer refuses to write a report (exit 2) in two cases: an arm answered 0
probes, or the probe file is empty. A rate over an empty denominator is not a
measurement.

## Turn format

The converters emit what `hu_cli_replay_parse_turn` reads. Fields below were
checked against that parser and `scripts/blind_ab/replay_export_turns.py` in the
`replay-harness` worktree on 2026-10-02. That branch is unmerged, so re-check
the format if it changes.

```json
{"id": "locomo:conv-26:probe:0007", "contact_id": "locomo.conv-26@bench.invalid",
 "ts": "2023-10-23 14:12:00", "inbound_bubbles": ["wait when did I go to the support group?"],
 "history": [{"from_me": false, "text": "Hey Mel! ...", "ts": "2023-05-08 13:56:00"}, ...],
 "probe": {"dataset": "locomo", "category": "temporal", "gold_answers": ["7 May 2023"],
           "question_original": "When did Caroline go to ...", "evidence_history_idx": [2],
           "evidence_in_window": false, "window": 25, ...}}
```

- **Required by the parser:** `id`, a non-empty `contact_id`, a non-empty
  `inbound_bubbles` array, and `history` entries with a string `text`.
- **Turns** also carry `seth_action: "text"` and `seth_reply_bubbles`, which hold
  what the "Seth" speaker actually said next. SOC turns add
  `seth_reply_delay_seconds` when the source tagged a delay.
- **Contact ids** use the reserved `.invalid` TLD, so a benchmark turn cannot
  collide with a real handle.
- **Timestamps:**
  - LoCoMo: session dates come from `session_N_date_time`.
  - MSC: there are no dates. Session 5 is anchored at 2024-03-01 19:00, and
    earlier sessions are placed `time_back` before it ("6 days 12 hours ago").
  - SOC: the start is the epoch in `chat_id`, and `<delay/>` tags set the gaps.
  - Within a session, messages are 20–180 s apart, depending on length.
  - A turn never crosses a session boundary.

## Probes and scoring

- **LoCoMo QA** gives 1,986 probes. The upstream numeric categories are mapped
  to names using the official scorer's handling
  (`task_eval/evaluation.py`):

  | Upstream | Name | Official handling |
  |---|---|---|
  | 1 | `multi_hop` | gold split on commas |
  | 2 | `temporal` | |
  | 3 | `open_domain` | gold is the first `;` part |
  | 4 | `single_hop` | |
  | 5 | `adversarial` | correct = says it was never mentioned |

  The question is rewritten as the contact texting Seth. The contact's name
  becomes I/me/my, Seth's becomes you/your, and a rotating casual prefix is
  added ("wait …", "random q but …"). The rewrite is deterministic and
  best-effort ("her buddies" survives). The original is kept in
  `question_original`.
- **MSC callbacks** give 706 probes, derived without an LLM. A persona fact the
  Seth speaker established in sessions 1–4 becomes a probe when two conditions
  hold:
  - It matches a fixed template ("I work as a nurse" → "what do you do for work
    again?"; also live in, from, favorite X, drive, have N X, X's name).
  - The answer literally appears in one of Seth's own lines in the history.

  Generic answers ("the city", "a small town") are dropped. All MSC callbacks
  count as `single_hop`.
- **Matching is LLM-free and deterministic.**
  - **Normalization** follows LoCoMo's `normalize_answer`, with number words
    mapped to digits. One deviation: punctuation becomes a space, so
    `PC,Playstation` does not fuse into a single word.
  - **Metrics:**
    - `contains`: the whole gold phrase appears in the reply. For multi-hop,
      every part must appear.
    - `f1`: token F1. It uses a light stemmer in place of Porter, so numbers are
      not comparable with published LoCoMo F1.
    - `recall`: the share of gold content tokens that appear in the reply.
    - `date`: day and month must match, plus the year if the reply gives one.
  - **A hit is:**
    - `contains`, or `recall` ≥ 0.6;
    - for temporal golds with a calendar date: `date`, or `contains`;
    - for adversarial probes: the reply abstains or corrects ("idk", "never",
      "that was me") and does not assert the planted answer.
  - **Missing and empty replies are misses.** They stay in the denominator.
- **Sanity check, 2026-10-02:** two synthetic arms were scored on all 1,986
  LoCoMo probes. The arms were scored directly; nothing went through the reply
  path.
  - An arm that answers with each gold scores 1.000.
  - An arm that always answers "haha yeah totally, sounds good" scores 0.000.
  - The same gold-answer arm scores 706/706 on MSC.
- **Optional judge.** `--judge-url` adds a `judge_rate` column from a **local**
  OpenAI-compatible server. Any non-loopback URL is refused, because probe
  history must never leave the machine. There is no cloud judge. The string
  metrics are always reported as well.

## Counts (measured 2026-10-02, full conversion)

| Dataset | Conversations | Turns | Probes | Probes with evidence in the 25-message window |
|---|---:|---:|---:|---:|
| LoCoMo | 10 | 2,807 | 1,986 | 61 |
| MSC (session 5, valid + test) | 1,001 | 30,817 | 706 | 261 |
| SOC-2508 | 1,181 | 3,545 | 0 | — |

LoCoMo probes by category:

| Category | Probes |
|---|---:|
| `single_hop` | 841 |
| `adversarial` | 446 |
| `temporal` | 321 |
| `multi_hop` | 282 |
| `open_domain` | 96 |

`--per-category 50` gives a 250-probe LoCoMo set.

## Caveats you must read before quoting a number

1. **Most probes need memory, not context.** The daemon gives the reply path
   only the last 25 messages (`load_conversation_history(..., 25, ...)` in
   `src/daemon/daemon_reactive_context.c`). For 97% of LoCoMo probes (1,925 of
   1,986), the evidence is older than that.

   `human replay` serves `history` through a null channel and reads memory from
   the snapshot `HU_MEMORY_SQLITE_PATH`. That snapshot knows nothing about
   `*.bench.invalid` contacts. Unless the snapshot is first seeded with each
   conversation, the `beyond_window` column measures whether anything outside
   the window reaches the prompt. On today's harness, expect it near zero.
   Report `in_window` and `beyond_window` separately, always.

   No seeding tool exists yet. This is the main follow-up.
2. **History text is cut at 511 bytes** (`hu_channel_history_entry_t.text`).
   0 LoCoMo entries are affected, 349 MSC entries and 79 SOC entries.
3. **Perspective.** Seth's side of the history is the "Seth" speaker's words.
   A probe about "you" asks the pipeline to recall what that speaker said. That
   is the right mechanic, but the facts are not Seth's.
4. **String matching is strict in places and lenient in others.**
   - Relative temporal golds ("the Sunday before 25 May 2023") only match a
     reply that names the anchor date or the phrase.
   - Open-domain golds become their first `;` part, often "Yes" or "Likely no",
     which matches easily.
   - Use the local judge column to check either direction.
5. **Images are `[Photo]`.** This is what the iMessage channel shows. Some
   LoCoMo questions depend on image content. Use `--photo-captions` to give the
   pipeline the BLIP caption, and say so when you report.
6. **SOC delays are a weak prior.** The median tagged delay is 2.25 h and the
   p90 is 1 day. The chats were generated, not observed. Use SOC for burst shape
   (bubbles per message part, mostly 3–5) more than for timing.
7. **Size.** The full LoCoMo `probes.jsonl` is about 220 MB, because every probe
   carries its conversation's full history of up to 689 messages. At several
   seconds per reply, a full arm takes hours. Use `--per-category` for routine
   runs.

## Files

| Path | What |
|---|---|
| `scripts/datasets/fetch_lib.sh` | Pinned, checksum-verified, idempotent download plus `PROVENANCE`. |
| `scripts/datasets/fetch_{locomo,msc,soc2508}.sh` | Fetch scripts, one per dataset. |
| `scripts/datasets/{locomo,msc,soc2508}_to_replay.py` | Converters. |
| `scripts/datasets/replay_common.py` | Turn grouping, timestamps, casual phrasing, normalization. |
| `scripts/datasets/memory_probe_score.py` | Per-arm, per-category scorer. |
| `tests/test_memory_benchmarks_{converters,fetch}.py`, `tests/test_memory_probe_score.py` | Hermetic tests: synthetic fixtures, `file://` downloads, no network, no `~/.human`. |
