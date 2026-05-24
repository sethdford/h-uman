# M3 Frontier-Bridge Runbook

How to fine-tune a LoRA adapter on YOUR data and serve it through the
running `human` daemon. This is the actual user-facing path for M3
("Private Learning — on-device ML personalization") — the strategic
mission the codebase has been building toward.

> **Status:** Bridge B is daemon-pattern proven. The MLX server can
> hot-swap adapters via `POST /v1/adapters/swap` (see
> `scripts/mlx-server.py`). The C side calls it via
> `hu_mlx_admin_swap_adapter`. The data-export piece (collector →
> JSONL) shipped in Sprint B C-loop. End-to-end fine-tune-from-real-
> reactions IS possible today; this doc shows how.

## What you need

1. **macOS 14+ on Apple Silicon.** MLX is the runtime; CPU-only / Linux
   paths exist but are reference-grade (toy GPT, not the frontier
   model you chat with).
2. **A running daemon with autoresponder OR reaction-collector wired.**
   The data this trains on is the `dpo_pairs` table written by the
   reaction-ingest pipeline. Without reactions arriving, the export
   produces an empty file and there's nothing to train on.
3. **A base MLX model.** This guide uses Gemma but any MLX-served
   model with adapter-swap support works.

## The four-step flow

```
┌───────────────┐  reactions  ┌───────────────┐  export   ┌──────────────┐  train   ┌─────────────┐  swap   ┌───────────────┐
│ daemon poll   │ ──────────▶ │ dpo_pairs in  │ ────────▶ │ pairs.jsonl  │ ────────▶│ adapter.npz │ ──────▶ │ MLX server    │
│ (cross-chan)  │             │ ~/.human/*.db │           │              │          │             │         │ serves replies│
└───────────────┘             └───────────────┘           └──────────────┘          └─────────────┘         └───────────────┘
   Sprint A.5/A.7              Sprint A foundation         Sprint B C-loop          mlx-lm-lora               Bridge B (D2)
```

### Step 1 — confirm dpo_pairs exists and is populating

The `dpo_pairs` table is part of `~/.human/memory.db` (the daemon's
main SQLite store), not a separate db file. The collector creates it
on first daemon start with reaction_collection enabled.

```bash
# How many preference pairs has the daemon collected?
sqlite3 ~/.human/memory.db 'SELECT COUNT(*) FROM dpo_pairs;'

# Recent pairs (last 7 days)?
sqlite3 ~/.human/memory.db \
  "SELECT COUNT(*) FROM dpo_pairs WHERE timestamp > strftime('%s','now','-7 days');"

# Sample one row to see what's there:
sqlite3 ~/.human/memory.db \
  "SELECT prompt, chosen, rejected FROM dpo_pairs ORDER BY timestamp DESC LIMIT 1;"
```

> **Verified live in production (2026-05-24 trusting-lamport
> session):** 279 pairs accumulated in `~/.human/memory.db`. First
> exported pair: `{"prompt":"hey what's up","chosen":"not much, just
> got home. you?","rejected":"Hey there! I'm doing great…"}` — the
> user's casual voice vs a generic assistant reply.

If the count is 0 or stale, check that reactions are flowing:

```bash
# The daemon should log per-reaction events. Look for "reaction_ingest":
grep -i "reaction" ~/.human/logs/service-loop.log | tail -10
```

If nothing — the reaction-collection subsystem isn't enabled.
Check `~/.human/config.json`:

```jsonc
{
  "reaction_collection": {
    "enabled": true
  }
}
```

Restart the daemon. Watch one full DPO cycle of reactions arrive
before continuing.

### Step 2 — export collector rows to JSONL

This is the Sprint B C-loop deliverable. The exporter walks
`~/.human/dpo_pairs.db`, JSON-escapes each prompt/chosen/rejected
field, and writes one JSONL line per pair to a path of your choice:

```bash
# All pairs to date (--since-days 0 disables the time filter):
./build/human export-dpo \
  --db ~/.human/memory.db \
  --out ~/.human/lora-pairs.jsonl \
  --since-days 0

# Verify shape:
head -3 ~/.human/lora-pairs.jsonl | jq .
```

You should see lines like:

```json
{"prompt":"hey are you free tonight?","chosen":"yeah! sushi at 7?","rejected":"sure.","ts":1700000000}
```

Pairs without a `rejected` sample emit the SFT shape:

```json
{"prompt":"...","chosen":"...","ts":1700000000}
```

> The `--out` file is overwritten on each invocation. If you want
> append-only or rolling exports, snapshot the file or use distinct
> `--out` paths per run.

### Step 3 — fine-tune the adapter

Use `mlx-lm-lora` (installed via pip or from source). The exact
invocation depends on your installed version; the canonical shape is:

```bash
mlx_lm.lora \
  --model mlx-community/gemma-2-2b-it-4bit \
  --train \
  --data ~/.human/lora-pairs.jsonl \
  --batch-size 4 \
  --iters 200 \
  --lora-layers 8 \
  --adapter-path ~/.human/adapters/v1
```

This produces `~/.human/adapters/v1/adapters.safetensors` plus a
small config JSON. Training time on M-series silicon: typically
~5-15 minutes for a few hundred pairs.

> **Hyperparameter tip:** 200 iters + lora_layers=8 is a conservative
> starting point. For very small datasets (<50 pairs) drop iters to
> avoid overfitting. For larger (>500), increase layers to 16.

### Step 4 — hot-swap the adapter on the live MLX server

The running MLX server (started by the daemon at boot via launchd)
exposes `POST /v1/adapters/swap`. The C side calls it via
`hu_mlx_admin_swap_adapter` from `src/agent/agent.c:1107` when a
per-contact adapter is configured.

For a one-off manual swap (e.g. to verify the new adapter loaded
without restarting the daemon):

```bash
# Direct HTTP call — same payload the C client sends:
curl -X POST http://127.0.0.1:8741/v1/adapters/swap \
  -H 'Content-Type: application/json' \
  -d '{"adapter_path":"/Users/seth/.human/adapters/v1/adapters.safetensors"}'
```

Expected response:

```json
{"ok": true, "adapter_path": "/Users/seth/.human/adapters/v1/..."}
```

To revert to base (no adapter):

```bash
curl -X POST http://127.0.0.1:8741/v1/adapters/swap \
  -H 'Content-Type: application/json' \
  -d '{"adapter_path": null}'
```

### Step 5 — verify behavior changed

The whole point is to feel the difference. Smoke-test by asking the
daemon (via any channel — iMessage, CLI, etc.) to draft a reply that
the OLD model would have written generically. With the new adapter
loaded, the draft should sound more like you.

```bash
# Generate a draft using whatever provider is configured. With the
# new adapter loaded, this should reflect your persona.
./build/human drafts --contact "+15551234567"
```

Compare against:

```bash
# Same prompt, base model (no adapter):
curl -X POST http://127.0.0.1:8741/v1/adapters/swap \
  -H 'Content-Type: application/json' \
  -d '{"adapter_path": null}'
./build/human drafts --contact "+15551234567"
```

If the difference is invisible: training didn't converge (try more
iters, more data) or the persona prompt is dominating (look for
"STYLE HINT:" / persona context lines and consider whether they're
overpowering the adapter's contribution).

## Honest caveats

**Adapter ≠ persona.** The adapter learns the SHAPE of your replies
(length, formality, common phrases). It doesn't replace the persona
prompt, the memory blocks, or the channel overlays — all of those
still gate behavior. The adapter compounds with them.

**Tiny datasets overfit fast.** Below ~30 pairs you risk the model
memorizing literal exchanges instead of generalizing. Wait until
the daemon has collected a few hundred pairs before doing a serious
fine-tune.

**Adapter swap is process-local.** The swap mutates the MLX server's
loaded weights in memory. If the MLX server crashes or restarts
(daemon restart, launchd kick), the previously-swapped adapter is
gone unless the daemon re-applies it. See
`src/agent/agent.c:1107` for the auto-load path triggered per turn.

**No remote-trained adapters.** Pull-from-cloud is intentionally not
supported. Adapters live in `~/.human/adapters/` and trained
locally; sharing them across machines would defeat the
"private-by-architecture" thesis.

**The reference HUML GPT path still exists** (`human ml lora-persona`)
and trains a toy model on the persona example bank. It's useful for
the engineering loop but does NOT affect the frontier model the
daemon uses for chat. The flow in this runbook is the real one.

## What the data-export covers

| Source | Captured | Notes |
|--------|----------|-------|
| iMessage reactions (tapback) | ✅ Sprint A | `reaction_ingest` source_hint |
| Slack reactions | ✅ Sprint A.6 | Cross-channel via reactions vtable |
| Discord MESSAGE_REACTION_ADD | ✅ | Same vtable |
| Telegram message_reaction diff | ✅ | Same vtable |
| WhatsApp / Matrix reactions | ✅ | Sprint A.5 |
| Direct text replies (no reaction) | Partial | production_outcomes table; not yet exported by this tool — TODO |
| Voice transcripts | ❌ | B5 audio-tone stub doesn't feed back into pairs yet |

The export today reads `dpo_pairs` only. A follow-up will join
`production_outcomes` (which carries reply-latency + reply-sentiment
signals) and produce richer pairs.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `0 pairs exported` | dpo_pairs.db missing or empty | Verify reaction_collection.enabled in config |
| `HU_ERR_NOT_SUPPORTED` | Built without HU_ENABLE_SQLITE or under HU_IS_TEST | Rebuild with `cmake --preset dev` |
| MLX swap returns `404` | mlx-server.py version predates adapter swap | Update `scripts/mlx-server.py` from main |
| MLX swap times out | Adapter file is huge or path is wrong | Verify the path is readable; check MLX server log |
| Adapter "loads" but behavior unchanged | Training didn't converge | Inspect loss curve; increase iters; verify data quality |

## Related

- `docs/plans/2026-05-17-m3-mlx-bridge-execution-plan.md` — Bridge B phasing
- `docs/plans/2026-05-10-m3-frontier-model-bridge.md` — Bridge B execution plan
- `include/human/ml/lora_export.h` — exporter API shipped this sprint
- `src/agent/agent.c:1107` — per-turn adapter auto-load site
- `scripts/mlx-server.py` — the MLX side of the bridge
