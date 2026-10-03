---
title: Local vision — HU_LOCAL_VISION gate, install, promotion and rollback
created: 2026-10-03
status: operator-facing
---

# Local vision (`HU_LOCAL_VISION`)

Under `local_only=enforce` ([local-only.md](local-only.md)) no image bytes may
leave the machine, so every photo a contact sends reaches the model as
`[They sent a photo]`. Local vision describes the photo on the machine
instead, over loopback only:

- **Caption:** Gemma 4 E2B 4-bit behind `mlx_vlm.server` on `127.0.0.1:8746`
  (OpenAI `/v1/chat/completions`, `image_url` data URI). It is asked for one
  plain sentence and told not to quote text.
- **Text:** the Apple Vision OCR helper `hu-vision-ocr`
  (`tools/hu-vision-ocr/main.swift`, JSON on stdout). It is the only source of
  quoted words. A quoted span in the caption that the OCR does not contain is
  cut together with its lead-in ("with the text", "that says"). In the
  2026-10-02 spike the VLM invented the word "ernest" on a photo that had no
  text.
- **Line the model sees:** `[They sent a photo: <caption>. Text in it: "<ocr>"]`.
  Either half may be missing. Brackets become parentheses and the OCR's double
  quotes become single quotes, so OCR'd text can't close the marker.
- **Budget:** caption and OCR run in parallel under one 8 s budget. Any
  failure (server down, helper missing, timeout, garbage reply, a URL that is
  not `http://127.0.0.1`) leaves today's placeholder in place.

Code lives in `src/context/local_vision.c`, with the contract in
`include/human/context/local_vision.h`. There is exactly one plug-in point:
`hu_daemon_local_photo` (`src/daemon/daemon_message_router.c`), called from the
per-message attachment branch in `src/daemon.c` in place of the local_only
placeholder. Each photo is described once, when it arrives, into the user turn.

`hu_daemon_describe_image` (the step-6 latest-attachment context) deliberately
does **not** run local vision and keeps refusing under local_only. Otherwise
the latest photo would be re-described on every later turn, uncached, and its
OCR text would land in the system-side "### Image Context" block. This path
also does not use `hu_vision_describe_image`, which is the cloud path and
carries the privacy kill-switch.

## Gate

| `HU_LOCAL_VISION` | Behaviour |
|---|---|
| `off` (default, also unset or unknown) | Nothing runs. The output is byte-identical to before (`off_is_byte_identical_to_placeholder`). |
| `shadow` | The pipeline runs on a detached background thread (one at a time; a photo arriving while one runs is logged `result=busy` and skipped). The reply is not delayed and the model gets exactly what it got before. One aggregate log line plus one owner-only sample row per photo. |
| `live` | The description is injected. Any failure falls back to the placeholder. |

Optional overrides:
- `HU_LOCAL_VISION_URL` must be loopback (`http://127.0.0.1[:port]`). Anything else is refused.
- `HU_LOCAL_VISION_MODEL`
- `HU_LOCAL_VISION_OCR` is the helper path. The default is `~/.local/bin/hu-vision-ocr`.

### Shadow and live log line

One line per photo goes to `~/.human/logs/service-loop-error.log`:

```
[HU_LOCAL_VISION shadow] ms=1240 caption_ms=1180 ocr_ms=290 caption_bytes=74 ocr_bytes=10 disagree=1 result=ok
```

The line holds timings, byte counts, the OCR/VLM disagreement flag and the
result only. It never contains the caption, the OCR text, the path or the
sender (`shadow_log_line_is_aggregate_only`).

Shadow never delays a reply: the caller gets the placeholder immediately
(`shadow_returns_placeholder_without_waiting`), and the pipeline finishes in
the background. `ms` is what LIVE would add to the reply.

### Owner-only sample store (shadow)

The promotion read needs the captions, which the log must never carry. In
shadow, each successful run appends one row to
`~/.human/local_vision_shadow.jsonl` (resolved through `hu_paths_state`, so
`HU_STATE_DIR` moves it):

```json
{"ts":1759460000,"path":"/Users/…/Attachments/…/IMG_1234.HEIC","caption":"…","ocr":"…","description":"…","disagree":false,"latency_ms":1240}
```

- The file is mode `0600` and keeps only the **latest 50** rows. It is
  rewritten through a temp file and a rename.
- `caption` is the raw VLM sentence. `description` is exactly what LIVE would
  inject, after quotes the OCR did not confirm are cut. `ocr` is the helper's
  text.
- Rows hold no contact handle or name, only the attachment path, so the owner
  can open the photo. Agents must not read this file.

## Install (owner, once)

```bash
scripts/install-local-vision.sh --dry-run   # render + validate the plist, write nothing
scripts/install-local-vision.sh             # build hu-vision-ocr, install + load ai.human.vision-server
```

The installer:
- refuses ports 8741 and 8743;
- refuses a plist that is not pinned to `127.0.0.1` or lacks `HF_HUB_OFFLINE=1`;
- refuses a model that is not already in the HF cache.

The server takes about 4.3 GB RSS. `scripts/nightly-retrain.sh` boots it out
for the training window and bootstraps it back afterwards
(`stop_vision` / `restore_vision`, tested by
`scripts/test_nightly_retrain_stop_vision.sh`).

Turn the gate on in the daemon's environment. `install-human-daemon.sh`
preserves operator-set keys across reinstalls.

```bash
P=~/Library/LaunchAgents/ai.human.service-loop.plist
/usr/libexec/PlistBuddy -c "Add :EnvironmentVariables:HU_LOCAL_VISION string shadow" "$P" ||
  /usr/libexec/PlistBuddy -c "Set :EnvironmentVariables:HU_LOCAL_VISION shadow" "$P"
launchctl bootout gui/$(id -u)/ai.human.service-loop; launchctl bootstrap gui/$(id -u) "$P"
```

## Promotion: SHADOW → LIVE

Run shadow for **7 days**, then measure all four of the following.

**1. Latency.** p50 and p95 of `ms`:

```bash
grep -h '\[HU_LOCAL_VISION shadow\]' ~/.human/logs/service-loop-error.log* |
  sed -E 's/.* ms=([0-9]+).*/\1/' | sort -n |
  awk '{a[NR]=$1} END {if (NR) print "n="NR, "p50="a[int(NR*0.5+0.5)], "p95="a[int(NR*0.95+0.5)]}'
```

**2. Failure rate.** The share of lines with `result=` not equal to `ok`:

```bash
grep -h '\[HU_LOCAL_VISION shadow\]' ~/.human/logs/service-loop-error.log* |
  sed -E 's/.*result=//' | sort | uniq -c
```

**3. Server RSS.** Check it after a week:

```bash
ps -o rss= -p "$(pgrep -f 'mlx_vlm.server.*8746')"
```

**4. Quality (owner only, agents never read the store).** Open the latest 20
rows and look at each photo next to its `description`:

```bash
tail -n 20 ~/.human/local_vision_shadow.jsonl |
  python3 -c 'import json,sys
for l in sys.stdin:
    r=json.loads(l); print(r["path"]); print("  ->", r["description"], "(disagree)" if r["disagree"] else "")'
open "$(tail -n 1 ~/.human/local_vision_shadow.jsonl | python3 -c 'import json,sys; print(json.load(sys.stdin)["path"])')"
```

For each one, ask: "would I be fine if the twin said this?" Count the ones you
accept. `disagree=true` rows are where the VLM quoted text the OCR did not
read; check that the cut left a sensible sentence.

**Go live when all of these hold:**
- at least 18 of 20 captions are acceptable;
- p95 is under 3 s;
- the failure rate is at most 5%;
- RSS stays under 5 GB.

If quality fails, swap the model to Qwen3-VL-4B (Apache-2.0, about 3 GB). That
is one change: `HU_VISION_MODEL` for the installer and `HU_LOCAL_VISION_MODEL`
for the daemon. Then re-run the shadow week.

## Rollback

Disable the gate. This is immediate and needs no rebuild:

```bash
/usr/libexec/PlistBuddy -c "Set :EnvironmentVariables:HU_LOCAL_VISION off" ~/Library/LaunchAgents/ai.human.service-loop.plist
launchctl bootout gui/$(id -u)/ai.human.service-loop; launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/ai.human.service-loop.plist
```

Then free the memory, and delete the samples once the read is done:

```bash
scripts/install-local-vision.sh --uninstall
rm -f ~/.human/local_vision_shadow.jsonl
```

With the gate off, nothing calls the server or the helper, so either can stay
installed.
