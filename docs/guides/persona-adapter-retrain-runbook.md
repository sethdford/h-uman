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

**The loss starts at the first reply token.** With batch size 1, mlx-tune's
ORPO and SimPO share the prompt's KV cache and train only `ids[prompt_length:]`.
Stock mlx-tune sets `prompt_length` to the common token prefix of the chosen
and rejected sequences. Every templated reply starts with `\n`, which is its own
token (`Ċ`) right after `</think>`. So stock mlx-tune never trained the first
reply token, nor any leading words both replies share. On glm-v61-pref all 426
rows lost 1–8 leading reply tokens, and that first position is exactly where
the served adapter emits EOS or `</think>`. `mlx_tune_train.py`'s
`pin_prompt_length` sets `prompt_length = len(encode(prompt))`, and the
pre-train contract check refuses to start if it does not hold.

### System prompt: the decision is NONE for this retrain

Production sends a persona system prompt with every request; the v6.1 rows have
none. Adding one is a trade-off, measured on glm-v61-pref with the tokenizer
(`chat_template_rows.py` reports these numbers on every run):

| Rows rendered with | system tokens | longest row | chosen reply's median share of its sequence |
|---|---|---|---|
| no system prompt | 0 | 369 | 0.333 |
| June persona head (`~/blind_ab_run/persona_prompt.txt`) | 886 | 1,257 | 0.014 |
| full production prompt (several thousand tokens) | ≥3,000 | > 2,048: every row drops | — |

mlx-tune's ORPO NLL term is the reply log-probability divided by the **full**
sequence length, prompt included. An 886-token prompt therefore shrinks the NLL
weight on the reply and its stop token about 23×. The odds-ratio term uses
summed reply log-probs and is unaffected. The full prompt also cannot fit
`max_seq_length 2048`. At 4096, memory is **unmeasured**: the 09-05 run peaked
at about 64 GB with rows ≤ 369 tokens, and activations grow with sequence
length.

**Default: no system prompt** (`HU_TRAIN_SYSTEM_PROMPT_FILE` unset). It keeps
the run like-for-like with 09-05, keeps the stop-token signal undiluted, and
fits 2048. The failure being fixed is the shape of the turn end after
`<think></think>`, which does not depend on the system prompt. The offline eval
in §3 measures the candidate on prompts that do carry system prompts, so a
failure to generalise shows up there before any A/B.

**Option B, only if §3 shows the no-system candidate fails on system-prompted
traffic:** a trimmed, representative persona head of about 400–800 tokens,
freshly dumped from production (the June file is stale), with
`max_seq_length` kept at 2048. 800 + 369 fits. Expect the NLL dilution above.
Consider raising `--beta` so the odds-ratio term carries more of the signal,
and watch `vm_stat` during the run. Never use the full production prompt.

`chat_template_rows.py` fails the staging step if fewer than 90% of the rows in
**either** train.jsonl or valid.jsonl survive (`--min-kept-frac 0.9`), so an
oversized system prompt stops the run instead of silently shrinking the corpus.

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
  - `template contract holds on N trainer-tokenized sequences (one leading [gMASK], last token '<|user|>'; loss starts at the first reply token (shared-prefix path))`.
    The run refuses to start, before any step, if the pin cannot take effect:
    mlx-tune is not 0.6.0, the trainer loop no longer calls the pinned
    tokenizer method, or the trainer is not on the native path.
  - `Training Mode: orpo`
  - `lora_parameters.scale = 2.0`
  - `lora_b non-zero 80/80`

  If any line is missing, the run is not a candidate.

## 3. Offline evaluation: empty-reply rate on a spare port

Run this on a spare instance on **:8748**, never :8741 or :8743. The script
refuses both. Prod must be stopped for this window, because two GLMs do not
fit. Run both arms on the same spare config, so the adapter is the only
difference.

**The spare server MUST run with `MLX_EMPTY_RETRY=0`.** Since gemma-realtime
`c02bd50`/`32cd6d1` (2026-10-01), mlx-server regenerates any empty adapter
reply on base weights by default. That would make both arms read about 0% and
hide exactly the failure this measures. `/health` does not expose the switch,
so `eval_empty_reply_rate.py` enforces it in two ways:

1. **Precondition.** It reads the listening process's own environment
   (`lsof` for the pid, `ps eww` for its env) and refuses unless
   `MLX_EMPTY_RETRY` is 0, false, no or off. The server reads `os.environ`
   and never sets it, so the exec-time env is the switch's value.
2. **Tripwire.** With `--server-log`, any new `[empty-retry]` line during the
   run voids it.

If the env cannot be read, the script refuses unless both
`--retry-disabled-confirmed` and `--server-log` are given.

```bash
launchctl bootout gui/501/ai.human.mlx-server          # prod down; wait until :8741 is gone
SRV=$HOME/Documents/gemma-realtime-1/scripts/mlx-server.py
PYS=$HOME/Documents/gemma-realtime-1/.venv312/bin/python3.12
SERVING=$HOME/.human/training-data/adapters/seth-glm-air-mlxtune-orpo-20260905-0856-20260905-085655
CAND=$HOME/.human/training-data/adapters/seth-glm-air-<TAG>-<STAMP>
eval_arm() {   # $1 = label, $2 = adapter dir; same server flags as prod (human-serve.sh)
  MLX_EMPTY_RETRY=0 GEMMA_DISABLE_THINKING=1 HU_SELF_RAG_MODE=soft HU_SELF_RAG_STREAMING=1 \
    "$PYS" "$SRV" --model mlx-community/GLM-4.5-Air-4bit --port 8748 --realtime --kv-bits 8 \
    --adapter-path "$2" > "/tmp/spare-8748-$1.log" 2>&1 &
  SPID=$!
  until curl -sf localhost:8748/health >/dev/null; do kill -0 $SPID || return 1; sleep 5; done
  ~/.human/venvs/mlxtune312/bin/python scripts/eval_empty_reply_rate.py \
    --port 8748 --samples 5 --label "$1" --expect-adapter "$2" \
    --server-log "/tmp/spare-8748-$1.log" \
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
result file is written. Do not read a number from such a run. The causes are
an unreachable server, a request error, the wrong adapter loaded, empty-retry
on, or an `[empty-retry]` line in the log.

"Empty" here is a **superset** of the server's `_is_empty_generation`: any reply
whose visible text is empty after stripping scaffold counts. That includes long
generations a server guard emptied, which the server's retry deliberately
skips. Both arms use the same definition, so the comparison holds. Don't
compare the absolute rate with the server's own `[empty-retry]` counts.

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

## 5. The nightly retrain (`scripts/nightly-retrain.sh`, 03:07)

The launchd job `ai.human.nightly-retrain` runs the main checkout's
`scripts/nightly-retrain.sh`. Its plist pins `HU_RETRAIN_MLXTUNE_TRAINER=mlx_lm`,
`HU_RETRAIN_MLXTUNE_MODE=sft` and the `seth-sft-20260919` corpus.

### What the stock SFT path trained (measured 2026-10-02)

Stock `mlx_lm.lora` templates `{prompt, completion}` itself. On the GLM tokenizer
it trains these targets for one row, read off mlx_lm's own `iterate_batches` and
`default_loss` mask:

```
\n <think> </think> \n <reply...> !
```

- The user turn has no `/nothink`.
- `<think></think>` are **targets**, though production already puts them in the prompt.
- The last target is mlx_lm's pad id 0 (`!`): `default_loss` masks `steps <= length`,
  one past the last real token. No stop token is ever a target.

### What it trains now

For a GLM base, `train-glm-adapter.sh --trainer mlx_lm` templates the rows with
`chat_template_rows.py` (`{prompt, completion}` rows are supported) and trains
through `scripts/mlx_lm_sft_templated.py`:

```
\n <reply...> <|user|>
```

- `PretemplatedCompletionsDataset` masks every prompt token (offset =
  `len(encode(prompt))`).
- `turn_end_mask` ends the loss at the last real token, so `<|user|>` is the
  last target and the pad is never trained. Everything else is stock mlx_lm.lora:
  same config, optimizer, iters, lr, rank and scale.
- Before step 1 the driver runs the contract check on every row of the exact
  train/valid sets: one leading `[gMASK]`, last token `<|user|>`, the row fits
  `max_seq_length`, and the mask starts at the first reply token. It prints
  `template contract holds on N`, and `train-glm-adapter.sh` refuses the run if
  that line is missing.
- The hooks are pinned to mlx-lm 0.31.3 (`assert_mlx_lm_preconditions`); any
  other version refuses to train.

On `seth-sft-20260919` (2026-10-02, tokenizer only): train kept 1164/1164,
valid 63/63, longest row 518 tokens against `max_seq_length` 1024, and the
contract holds on all 1227 rows.

Why not mlx_tune for SFT: mlx-tune 0.6.0's `SFTTrainer` writes the rows back out
and loads them with mlx_lm's own dataset code, so it is the same stock path with
no `prompt_length` pin. ORPO/SimPO/KTO candidates keep the mlx_tune path from §2.

The nightly candidate stage also refuses to **score** a candidate whose train log
lacks the contract line. It writes `UNTEMPLATED` beside the adapter.

**Rollback:** `HU_TRAIN_ALLOW_UNTEMPLATED=1` in the nightly plist restores the
stock SFT path and lets the stage score such a candidate (both are logged).

### Offline empty-reply eval stage: `HU_RETRAIN_EMPTY_EVAL=off|shadow|live`

Default **off**: nothing runs and nothing is printed.

After training and LUAR scoring, while `:8741` is still down, the stage serves the
**current** adapter and then the **candidate** on spare port `:8748`. It runs one
server at a time, with prod's flags and `MLX_EMPTY_RETRY=0`. It measures both
with `eval_empty_reply_rate.py` and writes `<candidate>/promotion_manifest.json`
through `scripts/empty_reply_gate.py`.

| Mode | Effect |
|---|---|
| `off` | No stage. Byte-identical to before. |
| `shadow` | Measured and recorded (`empty_reply.enforce: false`). The promotion verdict is the authorship verdict, unchanged. |
| `live` | `promotion_gate` also needs `rate(candidate) <= rate(serving)` on the overall rate. `m3_promote.py` refuses the swap otherwise (exit 7; `--skip-empty-reply-gate` overrides and is recorded). |

An arm that cannot run is recorded as `INCONCLUSIVE`, which blocks in `live`.
That covers serving not stopped, a busy or forbidden port, a resident model,
a spare that never becomes healthy, the deadline, or an unmeasured run.

The gate fails closed when the gate script itself fails. In `live` the stage
writes an enforced `INCONCLUSIVE` manifest at stage START, before the first
arm loads a model. A kill or reboot mid-arm therefore leaves a manifest that
blocks. Because it says `enforce: true`, `m3_promote.py` blocks on it even
when the promoter's shell has no `HU_RETRAIN_EMPTY_EVAL`. If that write fails,
no arm runs and the stage returns non-zero. The manifest is rewritten if
`empty_reply_gate.py` exits non-zero. `register_v6_adapter.py` only records
the registry row; the swap and its gates are `m3_promote.py promote`. With `HU_RETRAIN_EMPTY_EVAL=live` set
when you run `m3_promote.py promote`, a missing or unreadable manifest, or an
`empty_reply_gate` module that fails to import, also refuses the swap (exit 7).
In `off`/`shadow`, an import failure prints a `WARNING` and promotion continues.

**Memory.** Prod plus a second GLM is about 114 GB of 128. That co-residency
rebooted the box on 2026-07-26, so the eval runs while prod is still down. Each
arm is preceded by `check-no-resident-model.sh` and reaped before the next.
`restore_serving` stops any spare before it brings prod back.

**Added downtime.** One arm is a server load (16–59 s restart-to-healthy,
2026-09-29..10-02) plus 24 prompts × `HU_RETRAIN_EVAL_SAMPLES` (default 3)
requests at about 4.1 s each (prod log mean, n=22,106). That is about 6–7 min per
arm, **about 13–15 min for both**. Prod has come back at 04:13–04:18 on recent
nights, so expect about 04:30.

An arm only starts if it can finish (`HU_RETRAIN_EVAL_ARM_MAX_MIN`, default 12)
before `HU_RETRAIN_EVAL_DEADLINE` (default `04:35`, ahead of `kto-train-window`
at 04:40). The stage logs `added prod-down time Ns` every night. A manual run
outside the window needs `HU_RETRAIN_EVAL_DEADLINE=none`.

**SHADOW → LIVE.** Promote when three shadow nights have both arms measured, and
the serving arm reproduces the known failure: classifier empty rate ≥ 5%
(measured 13–18%). If it does not, the prompt set is not exercising the failure.

**Rollback:** remove `HU_RETRAIN_EMPTY_EVAL` from the plist (or set `off`).

## Known risks

- **Harvested pairs may carry scaffold.** DPO and KTO pairs mined from
  production text (`dpo_pairs`, `mine-corrections`) can contain `</think>`,
  `/nothink` or role tokens. The formatter drops such rows and counts them by
  reason in the manifest. A scan on 2026-10-01 found 0 such rows across
  glm-v6-pref, glm-v61-pref, glm-v6-merged-20260906 and the 09-05/09-06
  casing copies. Re-check the manifest for every new corpus.
- **No system prompt in the v6.1 corpus.** Production always sends one, so
  training is still off-distribution on that axis. This is a deliberate default;
  see "System prompt" above for the measured trade-off and option B.
- **The stock mlx-tune shared-prefix boundary.** `pin_prompt_length` shadows
  the trainer's tokenize method on the instance. If an mlx-tune upgrade renames
  `_tokenize_preference_pair` or `_tokenize_pair`, the pin raises
  `AttributeError` at start-up rather than silently training without it. The
  contract check refuses any `prompt_length` that differs from the prompt.
- **The nightly SFT path is templated as of 2026-10-02.** See §5.
- **Template drift.** The manifest records a hash of the chat template.
  `test_template_matches_mlx_server_render` fails if the server's rendering
  diverges from the training rows.
