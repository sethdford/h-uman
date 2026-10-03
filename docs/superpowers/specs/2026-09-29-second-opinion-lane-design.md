---
title: Second-opinion lane — a different model family checks h-uman's memory and replies overnight
date: 2026-09-29
status: draft (awaiting review)
---

# Second-opinion lane

## 1. Why

h-uman's own model (GLM-4.5-Air on `:8741`) currently grades its own work in three places, and none of them is measured by an independent model:

- **The curator's verifier.** GLM writes curator notes, and GLM's 3-vote verification decides which ones to keep. How often it keeps a note the texts don't support is unknown. That number gates `HU_INSIGHT_WIDE` shadow → live (see the sleep-time curator spec, §6).
- **The blind A/B inner-loop judge.** `scripts/blind_ab/synthetic_judge.py` shares a model family with the generator, so a low detection score is optimistic by construction (the script says so in its header).
- **Where replies fall short.** We have rating results, but no per-moment explanation of *what* a weak reply was missing.

A model from a different family gives a second opinion. Gemma 4 31B is from Google, GLM is from Zhipu, and Gemma runs locally. Kimi K3 was considered and rejected for now: its checkpoint is 1.56 TB against 183 GB of free disk, and a measured item takes about 55 minutes. It can be added later as another backend.

## 2. Decisions already made (with the user, 2026-09-29)

| Decision | Choice |
|---|---|
| Model | Gemma 4 31B instruction-tuned, 4-bit MLX: `mlx-community/gemma-4-31b-it-4bit` |
| Where it runs | Locally on the spare port `:8743`; started by the runner, stopped when it finishes (never always-on) |
| Cloud | `gemini-3.8-flash` via Vertex AI (ADC, project `johnb-2025`) is **opt-in only**, with `--backend vertex`. The default never sends message text off the Mac, which keeps the blind-A/B rule "never send real messages to a cloud judge". |
| Gold replies | Both. Where Seth replied, Gemma writes a **critique** of the h-uman reply against Seth's real one. Where h-uman replied and Seth did not, Gemma writes a **reference reply**, marked unrated. |
| Training | Out of scope. Nothing produced here is trained on, and nothing reaches live replies. |

Not usable: the on-disk `gemma-4-31b-seth-v3-fused`. It is fine-tuned on Seth, so it is not a neutral judge, and it was trained at LoRA scale 20.

## 3. Architecture

A new package, `scripts/second_opinion/`. Each unit has one job.

| Unit | Responsibility | Interface |
|---|---|---|
| `backend.py` | Model access. `GemmaBackend`: an OpenAI-compatible client for `http://127.0.0.1:8743`; it refuses any non-loopback URL. `serve_gemma()`: a context manager that starts `mlx_lm.server` from `~/Documents/gemma-realtime-1/.venv312` (mlx_lm 0.31.3, which has the `gemma4` architecture; verified 2026-09-29) on `:8743`, waits up to 10 min for `/health`, and always stops the server on exit. `VertexBackend`: `gemini-3.8-flash` with `thinkingConfig.thinkingBudget` always set explicitly; constructing it prints a one-line stderr notice that message excerpts leave the Mac. | `generate(system: str, user: str, max_tokens: int) -> str`; `.name` (e.g. `gemma-4-31b-it-4bit@local`, `gemini-3.8-flash@vertex`) |
| `store.py` | Its own SQLite file, `~/.human/second_opinion.db`, mode 0600 (not memory.db). Tables: `audits`, `critiques`, `reference_replies`, `runs`. Every row records `backend`, `prompt_version` and `created_at_ms`. | `open_store(path)`, plus insert and select helpers per table |
| `audit.py` | The verifier audit (§4.1). | `audit_pass(store, backend, mem_db, chat_db, limit, deadline) -> counts` |
| `judge.py` | The weekly judge calibration (§4.2). | `judge_pass(backend, sheet, key, out_dir) -> counts` |
| `gold.py` | Critiques and reference replies (§4.3). | `gold_pass(store, backend, sources, limit, deadline) -> counts` |
| `run_nightly.py` | The runner: a single-run lock, a deadline, the job rotation, serving Gemma, and a counts-only manifest. | CLI (§6) |

The lane only ever **reads** memory.db and chat.db (`mode=ro`). It writes only `second_opinion.db`, manifests and report sheets.

## 4. The three jobs

### 4.1 Verifier audit (nightly)

- **Sample.** Up to `--audit-limit` (default 25) live `contact_insights` rows (`retired_at_ms = 0`) not yet in `audits`, newest first, from both persona and `curator_wide` sources.
- **Resolve the evidence.**
  - Persona rows: `evidence_ids` is a JSON list of memory.db `messages.id`.
  - Wide rows: `evidence_ids` is `["chat:<rowid>"]`, resolved read-only against chat.db.
  - A row with no evidence, or evidence that no longer resolves, is skipped and counted (`skipped_no_evidence`), never guessed.
- **Ask.** Gemma sees only the note and its cited messages. It answers exactly one of `supported | unsupported | unclear`, then one short reason. Anything else is stored as `unclear` and counted (`unparseable`).
- **Store.** `audits(insight_id, source, verdict, reason, backend, prompt_version, created_at_ms)`. The message text is not copied into the store.
- **Weekly report** (`reports/audit-YYYYMMDD.json`, counts only):
  - disagreement rate = `unsupported / (supported + unsupported)`, with a Wilson 95% interval, split by source (persona vs wide);
  - `unclear` reported separately;
  - `n = 0` gives `"not measured"`, never 0.
- **Human calibration, once, after about 2 weeks.** `audit_sheet.py` writes `audit_check.csv`: 20 Gemma-`unsupported` rows and 10 Gemma-`supported` rows in random order. Each row has the note and its cited messages, with the verdict hidden. Seth marks each supported or not. `audit_score.py` then turns the disagreement rate into an estimated wrong-acceptance rate for the curator, with an interval, using Gemma's measured precision and recall on the 30.

### 4.2 Judge calibration (weekly, Sundays)

- Runs the existing `scripts/blind_ab/synthetic_judge.py` with `--endpoint http://127.0.0.1:8743/v1/chat/completions --model <gemma>` against the newest rating sheet. There are no changes to the judge's prompt; the existing `judge_model` stamp records who judged.
- Records the result with `score.py --rater synthetic`, so it can never overwrite the human verdict (`score.py` already enforces this).
- **Calibration number.** When a human-completed sheet exists for the same items, agreement = the share of items where Gemma and the human picked the same option as Seth's real reply, plus Cohen's κ. Fewer than 20 shared items gives `"not measured"`.
- The report is `reports/judge-YYYYMMDD.json`, counts only.

### 4.3 Gold: critiques and reference replies (nightly)

**Weakest moments**, in priority order:

1. Blind-A/B items where a human rater picked Seth's real reply with confidence ≥ 4 (h-uman's reply was spotted).
2. Items the Gemma judge (§4.2) detected.
3. For reference replies only: recent daemon replies (attribution label `huuman`) in threads where Seth sent no reply of his own within 24 h. These come from `eval_conversation_quality.attribute`.

**What Gemma produces:**

- **Critique** (Seth replied). Gemma sees the context, the h-uman reply and Seth's reply. It returns JSON: `{"gaps": [one or more of "specific_detail", "tone", "length", "question_vs_statement", "other"], "missing": "<one sentence>", "severity": 1-3}`. Output that won't parse is stored as `other` with `unparseable=1`.
- **Reference reply** (Seth did not reply). Gemma sees the context plus the contact's memory, read directly from memory.db exactly as `hu_contact_insights_render` selects it: live rows, `confidence >= 0.5`, newest 8, with `curator_wide` rows included only when `HU_INSIGHT_WIDE=live`. It writes one reply. The row is stored with `rated = NULL`.

**Stored in** `critiques(...)` and `reference_replies(...)`. The critique rows carry the item id, not the message text. Reference replies store the generated text, which is private and stays local.

**Export gate:** `gold_export.py` refuses to export any reference reply whose `rated` is not `1`. `gold_rate.py` writes a rating sheet for unrated reference replies. There is **no training export**; the export is a plain CSV for later decisions.

**Weekly report** `reports/gold-YYYYMMDD.json` (counts only): gap-category frequencies, critiques and reference replies written, and unparseable output.

## 5. Error handling

| Condition | Behaviour |
|---|---|
| Gemma is not healthy within 10 min | Refuse: exit 2, write nothing, stop the server |
| chat.db or memory.db is unreadable | Refuse: exit 2, write nothing |
| Non-loopback Gemma URL | Refuse at construction |
| `--backend vertex` without the flag, or ADC is missing | Refuse: exit 2 |
| One item fails (timeout, HTTP error, unparseable output) | Count it, skip it, continue |
| Every attempted item fails | Exit 3; the manifest is still written (it is the evidence) |
| Deadline reached | Stop between items; the manifest records `stopped_at_deadline` |
| Another run holds the lock | Exit 0 with a message; do nothing |
| A rate would come from `n = 0` (or `n < 20` for κ) | Report `"not measured"` |

The runner always stops the Gemma server: on success, failure, deadline, or KeyboardInterrupt.

## 6. CLI and schedule

```
python3 -m second_opinion.run_nightly [--jobs audit,gold] [--judge-sheet PATH]
    [--backend gemma|vertex] [--deadline HH:MM] [--audit-limit N] [--gold-limit N]
    [--store PATH] [--manifest-dir PATH] [--dry-run]
```

- The manifest is `~/.human/logs/second-opinion-YYYYMMDD.json`: counts, backend, elapsed time, exit reason. It is written atomically. It never contains message text, note text or handles.
- `--dry-run` calls the model but writes no rows. Its manifest file gets a `-dryrun` suffix.
- The deadline uses the curator's rule. It rolls to tomorrow only if the time passed more than 12 h ago; otherwise the window is closed and nothing is written.
- launchd: `ai.human.second-opinion` at **07:40** daily, after the curator's 07:30 deadline, with `--deadline 09:00`. Jobs: `audit,gold` every day, `judge` on Sundays. This is an operator step after merge, not in the repo, and it runs from the shared main checkout like the curator.

## 7. Testing

`tests/test_second_opinion_*.py` (pytest, hermetic: a fake backend with scripted outputs, temporary SQLite files, no chat.db, no network, no real `~/.human`). They join the existing CI step for the curator tests. Required cases:

- every refusal path in §5 writes nothing;
- a non-loopback Gemma URL is refused; Vertex without the flag is refused;
- `serve_gemma` stops the server on exception (a fake process);
- an unparseable verdict or critique becomes `unclear` / `other` and is counted;
- `n = 0` and `n < 20` produce `"not measured"`;
- Wilson interval math is pinned against known values;
- the judge result is recorded under `synthetic`, never `human`;
- the manifest and weekly reports contain no message text, note text or handles;
- `gold_export` refuses unrated reference replies;
- persona and wide evidence both resolve; unresolvable evidence is skipped and counted.

**Live smoke test (manual, not CI):** download the model, run `--jobs audit --audit-limit 3`, check 3 rows plus the manifest, and confirm the server is gone afterwards.

## 8. What "working" means

| Metric | Decides |
|---|---|
| Curator wrongly-accepted rate (after the one-time calibration), with its interval | `HU_INSIGHT_WIDE` shadow → live requires < 10% and an interval upper bound < 20%, alongside the curator spec's §6 targets |
| Gemma vs human agreement (κ) on the blind A/B | κ ≥ 0.4: the Gemma judge may be used as a between-rounds regression signal. Below that it is a warning only. It never certifies. |
| Gap-category frequencies from critiques | The ranked input for the next humanness fix |

## 9. Out of scope

- Training on anything produced here.
- Anything that reaches the live reply path.
- K3 (a later backend; `backend.py`'s interface is its seam).
- Changing `synthetic_judge.py`'s prompt.
- An always-on Gemma server.

## 10. Rollout

1. Merge the package, tests and CI step.
2. Download `mlx-community/gemma-4-31b-it-4bit` into the Hugging Face cache (about 18 GB; 183 GB is free).
3. Run the live smoke test (§7).
4. Operator: add the `ai.human.second-opinion` launchd job (§6), backing up any existing plist first.
5. After about 2 weeks: generate `audit_check.csv` for Seth's one-time 30-row check.
