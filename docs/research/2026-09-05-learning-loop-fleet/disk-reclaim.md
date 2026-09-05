---
title: Disk reclaim audit — ~/.human/training-data/adapters (203 GB, 63 GB free)
date: 2026-09-05
status: proposal
---

Read-only audit. **Nothing was executed.** Live server on :8741 is `mlx-server.py
--adapter-path .../seth-glm-air-v6-orpo-real-20260802-190128` (GLM-4.5-Air-4bit) —
confirmed via `ps -eo args | grep mlx` and `~/.human/config.json`.

## 1. Every top-level entry

Grouped where multiple entries share size/type/verdict (full names preserved).
KB = `du -sk`. GiB = KB/1,048,576.

| Entry | KB | GiB | mtime | Type | Content |
|---|---:|---:|---|---|---|
| `seth-lora-v6` | 61,970,924 | 59.10 | 2026-05-31 | dir | **FUSED gemma4 model** (12 model shards) + 31 LoRA checkpoints + `adapters.safetensors` |
| `seth-sprint8-prod-sft-e2b` | 9,678,432 | 9.23 | 2026-05-16 | dir | fused gemma4 model + adapter ckpts |
| `seth-dpo-early80` | 9,678,424 | 9.23 | 2026-05-16 | dir | fused gemma4 model + adapter ckpts |
| `seth-dpo-iter80plus` | 9,450,512 | 9.01 | 2026-05-16 | dir | fused gemma4 model + adapter ckpts |
| `seth-sprint8-sft-e2b`, `seth-sprint8-dpo-e2b` (2) | 9,122,904 ea | 8.70 ea | 2026-05-16 | dir | fused gemma4 model + `adapters.safetensors` (no per-iter ckpts) |
| `dpo-20260519-154309`,`-20260531-040020`,`-20260614-040018`,`-20260712-040017`,`-20260719-040018`,`-20260802-040017` (6) | 9,038,948 ea | 8.62 ea | May–Aug (see mtime col) | dir | fused gemma3 model (2 shards) + adapter ckpt — **legacy in-daemon nightly-LoRA output; that pipeline is retired** (`nightly_lora_enabled=false`, see `ai.human.nightly-retrain.plist` comment) |
| `seth-glm-air-v5-20260725-093742`, `-v62-sft-20260730-054131`, 6× `auto-17850…-glm` (8) | 3,259,708 ea | 3.11 ea | Jul 25–30 | dir | adapter-only (config.json=N, 4-6 safetensors) |
| `seth-glm-air-v6-orpo-real-20260802-190128` | 2,716,424 | 2.59 | 2026-08-02 | dir | adapter-only — **LIVE, serving now** |
| `seth-glm-air-v6-orpo-20260727-063621` | 2,716,424 | 2.59 | 2026-07-27 | dir | adapter-only — registry documents `lora_b` all-zero no-op |
| `auto-1785062238-glm`,`auto-1785063893-glm` (2) | 2,173,140 ea | 2.07 ea | Jul 26 | dir | adapter-only, **not in registry** |
| `seth-glm-air-v61-orpo-20260728-191306` | 1,629,856 | 1.55 | 2026-07-28 | dir | adapter-only — registry documents `lora_b` all-zero no-op |
| `seth-lora-v2`,`-v3`,`-v4`,`-v5`,`seth-lora` (bare) (5) | 1,312,008 ea | 1.25 ea | Apr 7 (v2-v5); May 31 relinked (bare) | dir | adapter-only, byte-identical sizes across v2–v5 |
| `seth-m3-outcomes-20260904-212919-glm`,`-20260905-030710-glm` (2, +2 top symlinks) | 1,086,572 ea | 1.04 ea | Sep 4/5 | dir+symlink | adapter-only, nightly retrain output, in registry |
| `auto-1785061303-glm` (+symlink) | 543,292 | 0.52 | Jul 26 | dir | adapter-only, **not in registry**, partial (2 shards) |
| `seth-lora-deliberation-dpo-v2` | 478,444 | 0.46 | 2026-07-05 | dir | adapter-only |
| `texting-llama-8b` | 450,828 | 0.43 | 2026-04-04 | dir | adapter-only (llama, unrelated base) |
| `seth-lora-v1` | 352,008 | 0.34 | 2026-04-07 | dir | adapter-only |
| `seth-lora-v4-repair-20260525-071921` | 224,004 | 0.21 | 2026-05-25 | dir | adapter-only, in registry |
| `seth-lora-v5-8bit-20260718-105251`, `auto-manual-1785053731-INVALID-scale20-gemma` (2) | 192,004 ea | 0.18 ea | Jul 18 / Jul 26 | dir | adapter-only; the INVALID one is explicitly named "trained at scale 20.0" (catastrophic, `lora-scale-default-or-die.md`) |
| `seth-lora-v6-8bit-20260725-114316` | 160,032 | 0.15 | 2026-07-25 | dir | adapter-only |
| `seth-orpo-replyfirst-20260530-174231` | 128,012 | 0.12 | 2026-05-30 | dir | adapter-only |
| `seth-sft-iter80`,`seth-dpo-iter20/40/60/80` (5) | 113,960 ea | 0.11 ea | 2026-05-16 | dir | adapter-only |
| `seth-voice-ideal-20260529-054706`,`-scale2-20260529-054946` (2) | 96,004 ea | 0.09 ea | 2026-05-29 | dir | adapter-only |
| `seth-lora-v2-w13` | 64,004 | 0.06 | 2026-05-10 | dir | adapter-only |
| `seth-dpo-full-072746` (+.log) | 27,432 | 0.03 | 2026-05-19 | dir+file | adapter-only |
| `gce-live-080621` | 14,464 | 0.01 | 2026-05-19 | dir | adapter-only |
| `seth-dpo-calibrated-073851` (+.log) | 13,724 | 0.01 | 2026-05-19 | dir+file | adapter-only |
| `seth-dpo-live-072531` (+.log) | 6,864 | <0.01 | 2026-05-19 | dir+file | adapter-only |
| `candidate-real.bin`,`baseline-empty.safetensors` | 264 | <0.01 | 2026-05-18 | file | test fixtures, referenced by `training_loop.py`/`live_fire_m3_full_loop.sh` |
| `registry.json` | 72 | <0.01 | 2026-09-05 | file | **the registry itself** |
| `seth-glm-air-v6-orpo-20260727-063342` | 4 | ~0 | 2026-07-27 | dir | empty/failed first attempt, superseded by `-063621` |
| `auto-1785061637-glm`,`auto-1785079845-glm` (2, +symlinks) | 8 / 4 | ~0 | Jul 26 | dir | empty/failed runs, **not in registry** |
| `seth-orpo-nothink-20260530-180729`,`seth-dpo-nothink-20260530-182410`,`seth-lora-deliberation-dpo`,`seth-dpo-live-072358`(+.log),`seth-sprint8-prod-dpo-e2b` | ≤4 ea | ~0 | May | dir | empty stubs |
| 7× `seth-m3-outcomes-*-glm.rejected-*` | 4 KB (349 B actual) ea | ~0 | Jul 31–Sep 3 | dir | quarantined fake `adapters.safetensors` (349 B) |
| 7× `seth-m3-outcomes-*.rejected-*` (top-level) + `seth-lora-current`, `auto-17850*` (11) + 2 current-cycle symlinks | 0 | — | various | **symlink** | all targets exist (verified `readlink`+`test -e`); **no dangling symlinks found** |

Sanity check: `df` shows 66,272,516 KB (63.2 GiB) free on `/System/Volumes/Data` (93% full).

## 2. Reference check (grep evidence)

| Adapter | registry.json | config.json | LaunchAgents | `:8741` ps args | scripts default (live code, not comment) | blind_ab_gate |
|---|---|---|---|---|---|---|
| `seth-glm-air-v6-orpo-real-20260802-190128` | promoted evidence entry | `mlx_local.adapter_path` + `personalization.lora_adapter_path` (`config.json:238,246-247`) | — | **YES** (`ps` output) | `scripts/blind_ab/test_rating_drip.py:270,273` | — |
| `seth-glm-air-v5-20260725-093742` | training/eval entries | — | — | — | `scripts/train-glm-adapter.sh:359` (`V5=...`) | `human.arm.adapter` (`docs/evaluation/blind_ab_gate.json:16`, `~/.human/blind_ab_gate.json`) |
| `seth-glm-air-v62-sft-20260730-054131` | training/eval entries (PENDING human gate) | — | — | — | referenced in own registry `smoke.adapter_b` | — |
| `seth-glm-air-v6-orpo-20260727-063621` | training entry — **documents `lora_b` 0/80, no-op** | — | — | — | — | — |
| `seth-glm-air-v61-orpo-20260728-191306` | training entry — **documents `lora_b` 0/80, no-op** | — | — | — | — | — |
| 6× `auto-17850…-glm` | training entry (metrics only, never promoted) | — | — | — | — | — |
| `seth-lora-v4-repair-20260525-071921`, `seth-lora-v5-8bit-20260718-105251` | training/eval entries | — | — | — | mentioned only in doc-comments/usage strings (`eval_fidelity_nightly.py:33`, historical) | — |
| `auto-manual-1785053731` (registry key) vs dir `auto-manual-1785053731-INVALID-scale20-gemma` | registry key present, **name doesn't exactly match the on-disk dir** | — | — | — | `scripts/training_loop.py:1098` comment: *"auto-manual-1785053731 trained at scale 20.0 and is dead weight"* | — |
| `seth-m3-outcomes-20260904-212919-glm`, `-20260905-030710-glm` | training entries (latest 2 nightly runs) | — | — | — | produced by `scripts/nightly-retrain.sh:450` | — |
| `seth-lora-current` → `seth-lora-v6` | — | — | — | — | `scripts/test_mlx_adapter_swap.py:30` (hardcoded test-only default, soft-fails if server unreachable); `scripts/m3_eval_adapter.py:34` is a **usage docstring only** — the real call site (`scripts/m3_loop_cycle.sh:198`) passes a dynamic `$PRIOR_ADAPTER`, not this symlink | — |
| `seth-lora` (bare) | — | — | — | — | **live code**: `scripts/training_loop.py:285,408,511-513,570,572` (`ADAPTER_PATH`, read+overwritten each legacy training cycle) — this loop predates the 2026-07-26 GLM flip (`training_loop.py` own comment) | — |
| `src/daemon.c:2705` `"%s/.human/ml/seth-lora-current"` | n/a | n/a | n/a | n/a | resolves to `~/.human/ml/seth-lora-current`, a **different, non-existent path** (`~/.human/ml/` only holds `training_data/`) — does NOT reach our adapters dir | — |
| All other entries in §1 | not a key in `registry.json`'s `adapters` object | not a path string in `config.json` | not matched by `grep -rli` over `~/Library/LaunchAgents/*.plist` | not in `ps` args | no non-comment/non-docstring hit in `scripts/*.py`/`*.sh` | not in either gate file | → **UNREFERENCED** |

## 3. Reclaim tiers

**SAFE — 108.8 GiB** (unreferenced anywhere in §2, OR fused duplicate + >30 days + no registry entry, OR explicit quarantine/dead-weight marker):
- 14× `.rejected-*` fakes (349 B payload each): negligible bytes, but 14 inodes of clutter
- 12 fused-model dumps, no registry/config/plist/ps/live-script reference, all >30 days old: **96.6 GiB** (`seth-sprint8-prod-sft-e2b`, `seth-dpo-early80`, `seth-dpo-iter80plus`, `seth-sprint8-sft-e2b`, `seth-sprint8-dpo-e2b`, `seth-sprint8-prod-dpo-e2b`, 6× `dpo-2026*` legacy-pipeline dumps)
- 26 small/legacy adapter-only dirs, unreferenced or doc-only-referenced, >30 days: **12.2 GiB** (`seth-lora-v1/v2/v3/v4`, `seth-lora-v2-w13`, `seth-sft-iter80`, `seth-dpo-iter{20,40,60,80}`, `seth-dpo-live-*`, `seth-dpo-full-072746`, `seth-dpo-calibrated-073851`, `seth-dpo-nothink-*`, `seth-orpo-nothink-*`, `seth-orpo-replyfirst-*`, `seth-voice-ideal-*`×2, `gce-live-080621`, `texting-llama-8b`, `seth-lora-deliberation-dpo`(+v2), `auto-manual-1785053731-INVALID-scale20-gemma` (dead-weight, scale=20), `seth-lora-v6-8bit-*`, `auto-1785061303-glm`, `auto-1785061637-glm`, `auto-1785062238-glm`, `auto-1785063893-glm`, `auto-1785079845-glm`, `seth-glm-air-v6-orpo-20260727-063342` (failed dupe of `-063621`))

**REVIEW — 60.4 GiB** (unreferenced by *production* but touched by non-live code, or unique/large enough to want a human call):
- `seth-lora-v6` (**59.1 GiB**) — only reference is a docstring example and a manual/CI-only regression test's hardcoded fallback (`test_mlx_adapter_swap.py`); not on the live serving path (GLM, not gemma) since 2026-07-26. Single biggest reclaim opportunity but it's the last pre-GLM-flip production checkpoint — confirm with Seth before deleting.
- `seth-lora` bare (**1.25 GiB**) — live variable in `training_loop.py`, but that script's own comments say the hardcoded-gemma-base assumption was superseded by the GLM flip. Confirm the script is retired before reclaiming.

**KEEP — referenced (see §2)**: `seth-glm-air-v6-orpo-real-20260802-190128` (2.59 GiB, live), `seth-glm-air-v5-20260725-093742` (3.11 GiB), `seth-glm-air-v62-sft-20260730-054131` (3.11 GiB), `seth-glm-air-v6-orpo-20260727-063621` (2.59 GiB, *registry-flagged no-op — safe to prune once Seth confirms*), `seth-glm-air-v61-orpo-20260728-191306` (1.55 GiB, *also registry-flagged no-op*), 6× `auto-17850…-glm` (18.66 GiB, registry-tracked but never promoted — low value, human call), `seth-lora-v4-repair-20260525-071921` (0.21 GiB), `seth-lora-v5-8bit-20260718-105251` (0.18 GiB), `seth-m3-outcomes-20260904-212919-glm`+`-20260905-030710-glm` (2.07 GiB, latest nightly pair), `registry.json`.

## 4. Nightly `seth-m3-outcomes-*` retention proposal

Current rate: ~1.09 GiB/night (real dir; the top-level symlink adds ~0 bytes) → **~32.7 GiB/month**, unbounded — `scripts/nightly-retrain.sh` and `scripts/rotate-logs.sh` currently have **no pruning logic** for `adapters/` (confirmed: `grep -n "m3-outcomes\|adapters" scripts/rotate-logs.sh` returns nothing).

Proposed rule (new `scripts/prune-m3-outcomes.sh`, invoked from `ai.human.nightly-retrain.plist`'s window after each run, NOT from `rotate-logs.sh` since that script only touches `*.log`):

```bash
# Keep: newest 3 non-rejected seth-m3-outcomes-* dirs, AND any dir whose id
# appears in registry.json under a "promoted" key (never delete a promoted adapter).
# Everything else non-rejected and older than the newest 3 -> delete.
# .rejected-* dirs: delete anything older than 14 days (quarantine already
# proved them useless; keeping 2 weeks is enough for post-mortem).
```
At 1 real dir/night, "keep newest 3" caps steady-state growth at ~3.3 GiB instead of unbounded — reclaims ~29 GiB/month vs. today's trajectory.

## 5. SAFE-tier commands (NOT executed — for Seth's review)

```bash
# --- 14 quarantined 349B fakes + their symlinks (~0 bytes, pure clutter) ---
# for f in seth-m3-outcomes-20260731-030708 seth-m3-outcomes-20260801-030706 \
#          seth-m3-outcomes-20260802-030705 seth-m3-outcomes-20260803-030709 \
#          seth-m3-outcomes-20260804-030706 seth-m3-outcomes-20260902-030710 \
#          seth-m3-outcomes-20260903-031457; do :; done
# rm -rf /Users/sethford/.human/training-data/adapters/seth-m3-outcomes-20260731-030708-glm.rejected-1788515706 /Users/sethford/.human/training-data/adapters/seth-m3-outcomes-20260731-030708.rejected-1788515706
# rm -rf /Users/sethford/.human/training-data/adapters/seth-m3-outcomes-20260801-030706-glm.rejected-1788515706 /Users/sethford/.human/training-data/adapters/seth-m3-outcomes-20260801-030706.rejected-1788515706
# rm -rf /Users/sethford/.human/training-data/adapters/seth-m3-outcomes-20260802-030705-glm.rejected-1788515706 /Users/sethford/.human/training-data/adapters/seth-m3-outcomes-20260802-030705.rejected-1788515706
# rm -rf /Users/sethford/.human/training-data/adapters/seth-m3-outcomes-20260803-030709-glm.rejected-1788515706 /Users/sethford/.human/training-data/adapters/seth-m3-outcomes-20260803-030709.rejected-1788515706
# rm -rf /Users/sethford/.human/training-data/adapters/seth-m3-outcomes-20260804-030706-glm.rejected-1788515667 /Users/sethford/.human/training-data/adapters/seth-m3-outcomes-20260804-030706.rejected-1788515667
# rm -rf /Users/sethford/.human/training-data/adapters/seth-m3-outcomes-20260902-030710-glm.rejected-1788515667 /Users/sethford/.human/training-data/adapters/seth-m3-outcomes-20260902-030710.rejected-1788515667
# rm -rf /Users/sethford/.human/training-data/adapters/seth-m3-outcomes-20260903-031457-glm.rejected-1788515667 /Users/sethford/.human/training-data/adapters/seth-m3-outcomes-20260903-031457.rejected-1788515667

# --- 96.6 GiB: fused-model dumps, legacy retired pipeline, unreferenced ---
# rm -rf /Users/sethford/.human/training-data/adapters/seth-sprint8-prod-sft-e2b
# rm -rf /Users/sethford/.human/training-data/adapters/seth-dpo-early80
# rm -rf /Users/sethford/.human/training-data/adapters/seth-dpo-iter80plus
# rm -rf /Users/sethford/.human/training-data/adapters/seth-sprint8-sft-e2b
# rm -rf /Users/sethford/.human/training-data/adapters/seth-sprint8-dpo-e2b
# rm -rf /Users/sethford/.human/training-data/adapters/seth-sprint8-prod-dpo-e2b
# rm -rf /Users/sethford/.human/training-data/adapters/dpo-20260519-154309
# rm -rf /Users/sethford/.human/training-data/adapters/dpo-20260531-040020
# rm -rf /Users/sethford/.human/training-data/adapters/dpo-20260614-040018
# rm -rf /Users/sethford/.human/training-data/adapters/dpo-20260712-040017
# rm -rf /Users/sethford/.human/training-data/adapters/dpo-20260719-040018
# rm -rf /Users/sethford/.human/training-data/adapters/dpo-20260802-040017

# --- 12.2 GiB: legacy/unreferenced small adapters ---
# rm -rf /Users/sethford/.human/training-data/adapters/seth-lora-v1 /Users/sethford/.human/training-data/adapters/seth-lora-v2 /Users/sethford/.human/training-data/adapters/seth-lora-v3 /Users/sethford/.human/training-data/adapters/seth-lora-v4
# rm -rf /Users/sethford/.human/training-data/adapters/seth-lora-v2-w13
# rm -rf /Users/sethford/.human/training-data/adapters/seth-sft-iter80 /Users/sethford/.human/training-data/adapters/seth-dpo-iter20 /Users/sethford/.human/training-data/adapters/seth-dpo-iter40 /Users/sethford/.human/training-data/adapters/seth-dpo-iter60 /Users/sethford/.human/training-data/adapters/seth-dpo-iter80
# rm -rf /Users/sethford/.human/training-data/adapters/seth-dpo-live-072358 /Users/sethford/.human/training-data/adapters/seth-dpo-live-072358.log
# rm -rf /Users/sethford/.human/training-data/adapters/seth-dpo-live-072531 /Users/sethford/.human/training-data/adapters/seth-dpo-live-072531.log
# rm -rf /Users/sethford/.human/training-data/adapters/seth-dpo-full-072746 /Users/sethford/.human/training-data/adapters/seth-dpo-full-072746.log
# rm -rf /Users/sethford/.human/training-data/adapters/seth-dpo-calibrated-073851 /Users/sethford/.human/training-data/adapters/seth-dpo-calibrated-073851.log
# rm -rf /Users/sethford/.human/training-data/adapters/seth-dpo-nothink-20260530-182410 /Users/sethford/.human/training-data/adapters/seth-orpo-nothink-20260530-180729
# rm -rf /Users/sethford/.human/training-data/adapters/seth-orpo-replyfirst-20260530-174231
# rm -rf /Users/sethford/.human/training-data/adapters/seth-voice-ideal-20260529-054706 /Users/sethford/.human/training-data/adapters/seth-voice-ideal-scale2-20260529-054946
# rm -rf /Users/sethford/.human/training-data/adapters/gce-live-080621
# rm -rf /Users/sethford/.human/training-data/adapters/texting-llama-8b
# rm -rf /Users/sethford/.human/training-data/adapters/seth-lora-deliberation-dpo /Users/sethford/.human/training-data/adapters/seth-lora-deliberation-dpo-v2
# rm -rf /Users/sethford/.human/training-data/adapters/auto-manual-1785053731-INVALID-scale20-gemma /Users/sethford/.human/training-data/adapters/auto-manual-1785053731
# rm -rf /Users/sethford/.human/training-data/adapters/seth-lora-v6-8bit-20260725-114316
# rm -rf /Users/sethford/.human/training-data/adapters/auto-1785061303-glm /Users/sethford/.human/training-data/adapters/auto-1785061303
# rm -rf /Users/sethford/.human/training-data/adapters/auto-1785061637-glm /Users/sethford/.human/training-data/adapters/auto-1785061637
# rm -rf /Users/sethford/.human/training-data/adapters/auto-1785062238-glm /Users/sethford/.human/training-data/adapters/auto-1785062238
# rm -rf /Users/sethford/.human/training-data/adapters/auto-1785063893-glm /Users/sethford/.human/training-data/adapters/auto-1785063893
# rm -rf /Users/sethford/.human/training-data/adapters/auto-1785079845-glm /Users/sethford/.human/training-data/adapters/auto-1785079845
# rm -rf /Users/sethford/.human/training-data/adapters/seth-glm-air-v6-orpo-20260727-063342
```
**Nothing above was executed.** Total if all SAFE-tier commands run: **~108.8 GiB freed** (would bring free space from 63 GiB to ~172 GiB).

## Grep commands actually run (verify-before-you-claim evidence)

```
python3 -c "... walk registry.json / config.json for path-like strings ..."
grep -rl -i 'lora|adapter|seth-dpo|sprint8|m3-outcomes|dpo-2026' ~/Library/LaunchAgents/*.plist
ps -eo pid,args | grep -i mlx
lsof -nP -iTCP:8741 -sTCP:LISTEN
grep -rl "<each adapter dirname>" scripts/ src/ docs/   # per-name loop, see §2
grep -n "seth-lora-current" src/daemon.c
grep -n "seth-lora-current" scripts/m3_eval_adapter.py scripts/test_mlx_adapter_swap.py
grep -n "m3_eval_adapter" scripts/m3_loop_cycle.sh; sed -n '190,205p' scripts/m3_loop_cycle.sh
grep -n "ADAPTER_PATH" scripts/training_loop.py
grep -n "auto-manual-1785053731" scripts/training_loop.py scripts/test_training_loop_serving_base.py
grep -n "m3-outcomes|adapters" scripts/rotate-logs.sh
grep -n "rejected" scripts/nightly-retrain.sh
find <every dir> -maxdepth 2 -name "*.safetensors" / -name "config.json" / -name "adapter_config.json"
readlink + test -e on every symlink in the directory
```
