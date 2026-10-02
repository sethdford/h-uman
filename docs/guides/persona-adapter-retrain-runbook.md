---
title: Persona Adapter Retrain Runbook (Chat-Template Fix)
created: 2026-10-01
status: operator-facing
---

# Persona Adapter Retrain Runbook (Chat-Template Fix)

How to retrain the GLM-4.5-Air persona adapter so it trains on the exact token
stream production serves, check it offline, and take it to promotion. Nothing
here runs automatically. Every step that loads a model is started by a human.

## Why this retrain exists

Production (`~/Documents/gemma-realtime-1/scripts/mlx-server.py`,
`prepare_prompt_lm`, `GEMMA_DISABLE_THINKING=1`) renders every prompt as:

```
[gMASK]<sop><|system|>\n…<|user|>\n…/nothink<|assistant|>\n<think></think>
```

The model is then expected to write `\n<reply>` and end its turn.

The serving adapter `seth-glm-air-mlxtune-orpo-20260905-0856-20260905-085655`
was trained differently. Despite the `mlxtune` in its name, it was trained by
`mlx_lm_lora` ORPO (`HU_RETRAIN_MLXTUNE_TRAINER=mlx_lm_lora`, see
`docs/plans/2026-09-02-persona-evolution/spec.md`), using
`~/.human/training-data/glm-v61-orpo-config.yaml` with the
`glm-v61-pref` corpus plus the casing and emoji rebalance. Its training rows
differed from production in three ways:

- `mlx_lm_lora`'s `ORPODataset` templates `{prompt, chosen}` without
  `enable_thinking=False`, so the user turn has no `/nothink`.
- The rows have no system prompt.
- Nothing follows the reply, so the end of the turn is never a training target.

`mlx_tune` (the nightly default trainer) is worse. It tokenizes the raw string
`prompt + chosen`, with no template at all.

Result: on classifier-style prompts the adapter ends the turn immediately on
about 13% of requests, which sends that traffic to cloud failover.

The fix lives in our data, not in site-packages:

| Piece | What it does |
|---|---|
| `scripts/chat_template_rows.py` | Renders each `{prompt, chosen, rejected}` row through the GLM tokenizer's own chat template (`add_generation_prompt=True, enable_thinking=False`). It appends `<|user|>` (id 151336) to each reply. Rows with template scaffold in them are dropped, as are empty `chosen` replies and rows longer than `max_seq_length`. Writes `chat_template_manifest.json`. Loads tokenizer files only. |
| `scripts/train-glm-adapter.sh` | For `--trainer mlx_tune` on a GLM base, templating is the last staging step, after the casing rebalance and depth upweight. Refuses `--trainer mlx_lm_lora` on GLM, because that trainer would template the rows a second time. Override with `HU_TRAIN_ALLOW_UNTEMPLATED=1`. |
| `scripts/mlx_tune_train.py` | Refuses a GLM run on raw rows before loading the 56 GB base. Once the trainer exists, it checks the trainer's own tokenization: exactly one leading `[gMASK]`, and the last token is `<|user|>`. |
| `scripts/eval_empty_reply_rate.py` | The offline empty-reply check below. |

### Stop-token evidence

From GLM-4.5-Air-4bit snapshot `60837794f3ca`:

- `config.json` and `generation_config.json` give `eos_token_id = [151329, 151336, 151338]`, which are `<|endoftext|>`, `<|user|>` and `<|observation|>`. `mlx_lm.load` stops generation on all three.
- `chat_template.jinja` has no end-of-assistant marker. In every multi-turn render, the token after an assistant reply is `<|user|>`.
- `tokenizer.json`'s post-processor is `ByteLevel` only, so `encode()` adds no BOS. `[gMASK]<sop>` comes from the template text itself.

`scripts/test_chat_template_rows.py` pins all of this. It also asserts
byte-equality with `prepare_prompt_lm`, imported from the production server
file.

## 0. Preconditions

- The branch `fix/adapter-chat-template` is merged, or you run from its worktree.
- Model-free tests pass:
  ```bash
  PY=~/.human/venvs/mlxtune312/bin/python
  $PY -m pytest scripts/test_chat_template_rows.py scripts/test_eval_empty_reply_rate.py scripts/test_mlx_tune_train.py -q
  bash scripts/test_train_chat_template_block.sh
  ```
- Memory: one 4-bit GLM-Air is about 57 GB on disk and about 44 GB wired when
  serving. Training peaks at about 64 GB (the 2026-09-05 ORPO run logged
  `peak_mem 63.745GB`). The machine has 128 GB, so prod and a trainer cannot
  both be resident. `train-glm-adapter.sh` stops `:8741` first, requires at
  least 70 GB free plus inactive memory, and restores prod from an EXIT trap.
  Also check that no other loader is resident: the `:8747` gemma spare, the
  arena on `:8743`, or `kto-train-window` at 04:40.
- Timing: avoid the nightly jobs (nightly-retrain at 03:07, orpo-watcher at
  03:17). The script itself refuses to run if conversation-arena (`:23` on
  hours 2, 6, 10, 14, 18 and 22) would overlap `--est-minutes` plus 15.

## 1. Dry run (no weights, prod untouched)

```bash
HU_TRAIN_REBALANCE_CASING=1 HU_TRAIN_MATCH_EMOJI=1 HU_TRAIN_UPWEIGHT_DEPTH=0 \
bash scripts/train-glm-adapter.sh --dry-run \
  --config ~/.human/training-data/glm-v61-orpo-config.yaml \
  --trainer mlx_tune --train-mode orpo --beta 0.05 \
  --tag mlxtune-orpo-tmpl-$(date +%Y%m%d-%H%M) --est-minutes 30
```

Expect `[chat-template] train.jsonl: kept 426, dropped 0` and
`valid.jsonl: kept 37, dropped 0`. The longest row is 369 tokens, against a
`max_seq_length` of 2048. Measured on 2026-10-01. The mlx_tune dry-run notes
that the source rows are raw and that they are templated at stage time.

## 2. Retrain (the exact command; stops prod for the dark window)

```bash
HU_TRAIN_REBALANCE_CASING=1 HU_TRAIN_MATCH_EMOJI=1 HU_TRAIN_UPWEIGHT_DEPTH=0 \
bash scripts/train-glm-adapter.sh \
  --config ~/.human/training-data/glm-v61-orpo-config.yaml \
  --trainer mlx_tune --train-mode orpo --beta 0.05 \
  --tag mlxtune-orpo-tmpl-$(date +%Y%m%d-%H%M) --est-minutes 30
```

This command uses the same corpus, casing and emoji pass, rank 8, scale 2.0,
8 layers, 400 iters, lr 5e-6 and beta 0.05 as the serving adapter.
`UPWEIGHT_DEPTH=0` because the 09-05 run predates depth upweighting. Two things
differ:

- **The template and stop token.** This is the fix.
- **The trainer.** mlx_tune's ORPO is `NLL(chosen) + beta * odds-ratio`. The
  `mlx_lm_lora` ORPO that built 09-05 had no NLL term.

The trainer change is forced: `mlx_lm_lora` cannot consume pre-templated rows.
Attribute any voice shift to both changes, not to the template alone.

Optional: `HU_TRAIN_SYSTEM_PROMPT_FILE=<file>` gives template rows that have no
system prompt the production persona prompt. The v6.1 corpus has none. Leave it
unset for a like-for-like run, and only set it from a freshly dumped production
prompt. `~/blind_ab_run/persona_prompt.txt` is from June and is stale.

- **Duration:** about 25–40 min of production downtime. That covers base load,
  400 ORPO steps, the guards, and the in-window base-capability smoke test.
  Reference timings: mlx_tune SimPO on 1,290 pairs took 7m20s including smoke
  (2026-09-12). `mlx_lm_lora` ORPO on these 426 pairs took about 19 min of
  training (2026-09-05). Templating adds about 10 tokens per row.
- **Outputs:**
  - Adapter: `~/.human/training-data/adapters/seth-glm-air-<TAG>-<STAMP>/` (`adapters.safetensors`, `adapter_config.json`).
  - Templated corpus and manifest: `~/.human/training-data/glm-v61-pref-casing-<STAMP>-tmpl-<STAMP>/`.
  - Log: `~/.human/logs/train-glm-<TAG>-<STAMP>.log`.
  - Smoke results: `~/.human/logs/v6-smoke-<STAMP>.json`.
- **Must appear in the log:**
  - `template contract holds on N trainer-tokenized sequences`
  - `Training Mode: orpo`
  - `lora_parameters.scale = 2.0`
  - `lora_b non-zero 80/80`

  If any line is missing, the run is not a candidate.

## 3. Offline evaluation: empty-reply rate on a spare port

Run this on a spare instance on **:8748**, never :8741 or :8743. The script
refuses both. Prod must be stopped for this window, because two GLMs do not
fit. Run both arms on the same spare config, so the adapter is the only
difference.

```bash
launchctl bootout gui/501/ai.human.mlx-server          # prod down; wait until :8741 is gone
SRV=$HOME/Documents/gemma-realtime-1/scripts/mlx-server.py
PYS=$HOME/Documents/gemma-realtime-1/.venv312/bin/python3.12
SERVING=$HOME/.human/training-data/adapters/seth-glm-air-mlxtune-orpo-20260905-0856-20260905-085655
CAND=$HOME/.human/training-data/adapters/seth-glm-air-<TAG>-<STAMP>
eval_arm() {   # $1 = label, $2 = adapter dir; same server flags as prod (human-serve.sh)
  GEMMA_DISABLE_THINKING=1 HU_SELF_RAG_MODE=soft HU_SELF_RAG_STREAMING=1 \
    "$PYS" "$SRV" --model mlx-community/GLM-4.5-Air-4bit --port 8748 --realtime --kv-bits 8 \
    --adapter-path "$2" > "/tmp/spare-8748-$1.log" 2>&1 &
  SPID=$!
  until curl -sf localhost:8748/health >/dev/null; do kill -0 $SPID || return 1; sleep 5; done
  ~/.human/venvs/mlxtune312/bin/python scripts/eval_empty_reply_rate.py \
    --port 8748 --samples 5 --label "$1" --expect-adapter "$2" \
    --out "$HOME/.human/logs/empty-reply-$1-$(date +%Y%m%d).json"
  kill $SPID; wait $SPID                                 # fully reap before the next load
}
eval_arm serving "$SERVING"
eval_arm candidate "$CAND"
launchctl bootstrap gui/501 ~/Library/LaunchAgents/ai.human.mlx-server.plist   # prod back
```

The fixed set is `scripts/eval_data/empty_reply_prompts.jsonl`: 12
classifier-style prompts modeled on the daemon's "Return ONLY…" calls, plus 12
chat prompts. At 5 samples each, that is 60 classifier and 60 chat requests per
arm. Expect about 10–15 min per arm. Exit code 2 means the run was not measured
(unreachable server, a request error, or the wrong adapter loaded), and no
result file is written. Do not read a number from such a run.

To proceed to the A/B, all of these must hold:

- The candidate's classifier empty rate is at most 1/60.
- The candidate's chat empty rate is 0/60.
- The candidate is strictly below the SERVING arm, measured in the same window.

The SERVING arm should reproduce the roughly 13% failure. If it does not, the
prompt set does not exercise the failure; fix the set before trusting the
candidate's number.

## 4. Promotion path (human-gated)

1. **Register:** `python3 scripts/register_v6_adapter.py --adapter $CAND --log <train log> --smoke <smoke json> --corpus-manifest ~/.human/training-data/glm-v61-pref/manifest.json`. `--corpus-manifest` takes the SOURCE corpus manifest (counts, by_source, targets). The `chat_template_manifest.json` in the `-tmpl-` dir is the provenance for the template and stop token; keep it next to the adapter.
2. **Blind A/B gate:** use the `blind-ab-pipeline` skill on a spare port (not :8741 or :8743), with the real product system prompt (see the project memory "blind-A/B serving/routing"). Promotion needs the gate to PASS (`docs/evaluation/blind_ab_gate.json`).
3. **Promote:** `python3 scripts/m3_promote.py promote --adapter $CAND --yes`. This hot-swaps `:8741`. It enforces the scale ceiling, the smoke gate and the authorship gate, and records the swap in the lineage file. Roll back with `m3_promote.py rollback --yes`. Make the change persist across restarts with `mlx_local.adapter_path` in `~/.human/config.json`. Only the owner of that file edits it.
4. **After promotion:** watch the daemon's cloud-failover count for 24 h. The empty-reply share should fall from about 13% toward 0. If it does not, roll back.

## Known risks

- **Harvested pairs may carry scaffold.** DPO and KTO pairs mined from
  production text (`dpo_pairs`, `mine-corrections`) can contain `</think>`,
  `/nothink` or role tokens. The formatter drops such rows and counts them by
  reason in the manifest. A scan on 2026-10-01 found 0 such rows across
  glm-v6-pref, glm-v61-pref, glm-v6-merged-20260906 and the 09-05/09-06
  casing copies. Re-check the manifest for every new corpus.
- **No system prompt in the v6.1 corpus.** Production always sends one, so
  training is still off-distribution on that axis. See
  `HU_TRAIN_SYSTEM_PROMPT_FILE` above.
- **The nightly SFT path (`TRAINER=mlx_lm`, seth-sft-20260919) is unchanged.**
  `mlx_lm`'s chat dataset also renders without `enable_thinking=False`. It was
  out of scope here, but the same audit applies before any SFT adapter is
  promoted.
- **Template drift.** The manifest records a hash of the chat template.
  `test_template_matches_mlx_server_render` fails if the server's rendering
  diverges from the training rows.
