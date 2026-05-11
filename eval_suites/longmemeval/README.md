# LongMemEval (W16) on-disk corpus

This directory holds an **example** LongMemEval-shaped JSON pack for the W16
evaluation backend (`hu_evaluation_longmemeval` + `hu_eval_lme_load`).

## Install location

The loader resolves:

`$HU_EVAL_DATA_DIR/longmemeval.json`

Default `HU_EVAL_DATA_DIR` is `~/.human/eval-datasets/`.

Copy this file there to exercise the real-corpus path locally:

```bash
mkdir -p ~/.human/eval-datasets
cp eval_suites/longmemeval/longmemeval.json ~/.human/eval-datasets/longmemeval.json
```

When the file is missing or malformed, the backend falls back to the built-in
synthetic 10-prompt split (see `src/evaluation/evaluation_longmemeval.c`).

## Schema

Top-level object:

- `name` (string): should be `longmemeval`
- `version` (number): currently `1`
- `items` (array): each element has `category`, `prompt`, `candidate_answer`,
  and `keywords` (array of strings). Categories must match the five W16
  buckets: `temporal`, `multi_hop`, `single_hop`, `abstention`,
  `knowledge_update`.
