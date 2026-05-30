# scripts/blind_ab — Blind Human A/B humanness test

The ground-truth humanness measurement for h-uman. See `PROTOCOL.md` for the why and the
pass criteria. This README is the runbook.

## Quickstart (with the synthetic example)

```bash
cd scripts/blind_ab
python3 make_rating_sheet.py example_triples.json --seed 42
#   -> rating_sheet.csv  (give to raters; no labels)
#   -> answer_key.json   (KEEP PRIVATE)
# ... raters fill the `choice` (A/B) and `confidence` (1-5) columns ...
python3 score.py rating_sheet.csv --key answer_key.json
python3 score.py --selftest        # verify the math without any data
```

## Real run — three steps

1. **Build `triples.json`** — the only data you must supply. Each item pairs a real
   conversation context, Seth's *actual* reply, and h-uman's reply for the same context.
   - Seth's real replies: from your own sent-message history (iMessage/etc.). This script
     never reads your messages — you assemble the JSON locally.
   - h-uman's replies: generate one per context with the built binary. The generation
     wiring is the **one open piece** (see below) — until then, paste h-uman replies by hand
     or from a short batch script.

2. **Make the sheet & recruit raters** — `make_rating_sheet.py triples.json`. Send
   `rating_sheet.csv` + `RATER_INSTRUCTIONS.txt` to 5–8 people who know Seth. Save one
   completed file per rater, e.g. `rating_sheet_alice.csv`.

3. **Score** — `python3 score.py rating_sheet_*.csv --key answer_key.json`. Prints the
   detection rate, 95% Wilson CI, confidence-weighted rate, per-rater breakdown, and
   `RESULT_blind_ab=PASS|FAIL`.

## Generating h-uman replies — WIRED (2026-05-30)

`gen_huuman_replies.py` is the generation glue. You supply `contexts.json`
(`[{id, context, seth_reply}, ...]` — your real sent history); it produces `triples.json`
by reusing the eval runner's generation path (persona 'seth', channel 'imessage' — the SAME
path the product uses), reading each reply back from `~/.human/memory.db`:

```bash
python3 gen_huuman_replies.py contexts.json --out triples.json
python3 make_rating_sheet.py triples.json
# ... raters fill rating_sheet.csv ...
python3 score.py rating_sheet_*.csv --key answer_key.json
```

The full chain (gen → sheet → score) is smoke-tested. Verified replies are convincingly
in-voice, e.g. context "hey you around this weekend?" → "yeah i should be around. what's up?"
(Seth's real reply was "yeah should be, what's up") — a preview of how hard the
discrimination will be. NOTE: requires the eval empty-output fix (`hu_eval_run_empty_invalid`,
branch `fix/eval-empty-output-guard`) — without it, `eval run` discarded slow generations and
this glue would emit empty replies.

## Files

| File | Role |
|---|---|
| `PROTOCOL.md` | the measurement, raters, pass criteria, anti-gaming |
| `make_rating_sheet.py` | triples.json -> shuffled blind sheet + private key |
| `score.py` | completed sheets + key -> detection rate, Wilson CI, verdict (has `--selftest`) |
| `RATER_INSTRUCTIONS.txt` | what each rater is told |
| `example_triples.json` | 3 synthetic items to smoke-test the pipeline |
