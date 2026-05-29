---
title: Sentinel-Boundary Reply-First LoRA — Implementation Plan
---

# Sentinel-Boundary Reply-First LoRA Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Train `seth-lora-v5-replyfirst`, a LoRA that makes the seth-voice model emit the user-facing reply FIRST (then a `<|channel|>` sentinel + deliberation) so streamed tiers win first-token latency (`streaming_beneficial:true`) without regressing the +27pp persona voice.

**Architecture:** Self-distill v4-repair offline (`mlx_lm.generate`), split each generation into `(deliberation, reply)`, reorder to `reply + sentinel + deliberation`, train a fresh scale=2.0 LoRA on `mlx-community/gemma-4-31b-it-4bit`, then gate on ordering (≥90% reply-first) AND fidelity (Δ≥+22pp). All distillation/eval is offline — never the shared `:8741` prod server.

**Tech Stack:** Python 3 (stdlib only for logic + tests), `mlx_lm` 0.31.2 CLI for generation + LoRA training, plain-runner test pattern (`scripts/test_*.py`, no pytest), JSONL corpora under `~/.human/training-data`.

**Spec:** `docs/superpowers/specs/2026-05-29-sentinel-boundary-replyfirst-lora-design.md`

---

## File Structure

| File | Responsibility |
|------|----------------|
| `scripts/spike_replyfirst_distill.py` | **Task 0 spike.** Probe ~20 prompts through v4-repair offline; characterize output structure; pin the exact sentinel bytes. Throwaway-ish (kept for reproducibility). |
| `scripts/replyfirst_corpus_lib.py` | Pure logic: `split_deliberation_reply`, `reorder_to_replyfirst`, `build_target`, `format_sft_example`. No I/O, no model. Fully unit-tested. |
| `scripts/build_replyfirst_corpus.py` | CLI glue: read m3 prompts, call `generate()` (offline mlx_lm), apply `replyfirst_corpus_lib`, write train + heldout JSONL + `parse_failures.jsonl`. |
| `scripts/train_replyfirst.py` | mlx_lm lora wrapper: scale=2.0 explicit, `assert_scale_2(adapter_config)` post-train hard-fail, writes adapter + lineage. |
| `scripts/eval_ordering.py` | New gate: ordering metrics (`is_reply_first`, `first_reply_token_index`) + verdict JSON. |
| `scripts/replyfirst_ship_gate.py` | Pure logic + CLI: read fidelity + ordering verdict JSONs, apply +0.22 fidelity floor & ordering gate, emit combined ship verdict. |
| `scripts/eval_replyfirst_stream_e2e.py` | Live E2E proof: stream a casual prompt, assert first-token-early + no leak + `streaming_beneficial:true`. Apple Silicon only. |
| `scripts/test_replyfirst_corpus_lib.py` | Unit tests for `replyfirst_corpus_lib` (stdlib, no model). |
| `scripts/test_eval_ordering.py` | Unit tests for ordering metrics + verdict assembly (stdlib, no model). |
| `scripts/test_replyfirst_ship_gate.py` | Unit tests for ship-gate logic (stdlib). |
| `scripts/test_train_replyfirst.py` | Unit test for `assert_scale_2` (stdlib, mocked adapter_config dict). |

Reused unchanged: `scripts/eval_fidelity_nightly.py` (fidelity gate — invoked, not modified), `~/.claude/rules/lora-scale-default-or-die.md`, `src/util/harmony_filter.c`.

---

## Task 0: Marker/parse spike (de-risk the splitter)

**This task is a SPIKE, not TDD.** Its output is findings that confirm/adjust Tasks 1–2. Do not proceed to Task 1 until the spike's three questions are answered in writing.

**Files:**
- Create: `scripts/spike_replyfirst_distill.py`

- [ ] **Step 1: Write the probe script**

```python
#!/usr/bin/env python3
"""Task-0 spike: characterize v4-repair output structure for reply-first distillation.

Runs ~20 m3 prompts through v4-repair OFFLINE (mlx_lm.generate), prints each raw
generation with markers made visible, and tallies how many contain a recognizable
Harmony channel marker. Answers three questions before we build the corpus:
  Q1. Does v4-repair emit ANY channel marker natively, or is it always markerless?
  Q2. Where does the user-facing reply sit relative to the deliberation?
  Q3. What exact byte sequence should the reply-first sentinel be?

NOT a production tool. Offline only — never touches the :8741 prod server.
Run: python3 scripts/spike_replyfirst_distill.py
"""
import json
import subprocess
import sys
from pathlib import Path

MODEL_ID = "mlx-community/gemma-4-31b-it-4bit"
ADAPTER = Path.home() / ".human/training-data/adapters/seth-lora-v4-repair-20260525-071921"
CORPUS = Path.home() / ".human/training-data/m3-corpus.jsonl"
# Harmony markers the filter recognizes (from include/human/util/harmony_filter.h)
MARKERS = ["<|channel|>", "<|channel>", "<|message|>", "<|thought|>", "<|return|>",
           "thought", "analysis", "commentary", "final"]


def generate(prompt: str, max_tokens: int = 200) -> str:
    cmd = [sys.executable, "-m", "mlx_lm", "generate", "--model", MODEL_ID,
           "--adapter-path", str(ADAPTER), "--prompt", prompt,
           "--max-tokens", str(max_tokens), "--temp", "0.0"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    out = r.stdout
    lines = [l for l in out.splitlines()
             if l and not l.startswith("==") and not l.startswith("Prompt")
             and "tokens-per-sec" not in l and "Peak memory" not in l]
    return "\n".join(lines).strip()


def main() -> int:
    prompts = []
    for line in CORPUS.read_text().splitlines():
        rec = json.loads(line)
        if rec.get("role") == "user" and rec.get("content", "").strip():
            prompts.append(rec["content"])
        if len(prompts) >= 20:
            break
    marker_hits = 0
    for i, p in enumerate(prompts):
        raw = generate(p)
        has_marker = any(m in raw for m in MARKERS[:5])  # only the <|...|> forms
        marker_hits += int(has_marker)
        print(f"\n===== PROMPT {i} =====\n{p[:120]}")
        print(f"----- RAW (markers literal) -----\n{raw[:600]}")
        print(f"----- has_marker={has_marker} len_tokens={len(raw.split())} -----")
    print(f"\n\nSUMMARY: {marker_hits}/{len(prompts)} generations contained a "
          f"<|...|> marker natively.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Run the spike offline**

Run: `cd /Users/sethford/Projects/h-uman && python3 scripts/spike_replyfirst_distill.py 2>&1 | tee /tmp/replyfirst_spike.log`
Expected: 20 raw generations printed; a SUMMARY line with the marker-hit count. Takes several minutes on Apple Silicon.

- [ ] **Step 3: Record findings in the spec's Open Questions section**

Edit `docs/superpowers/specs/2026-05-29-sentinel-boundary-replyfirst-lora-design.md`, replacing the "Open Questions" section with answers:
- Q1: "v4-repair emits `<|...|>` markers in N/20 generations" (N from the SUMMARY line).
- Q2: One sentence on where the reply sits (e.g. "reply is the final paragraph after a deliberation block" or "interleaved").
- Q3: The chosen sentinel byte string. **Default decision if no native marker appears: use `<|channel|>thought\n` as the inserted sentinel** (the closed-marker form the harmony filter strips). If v4-repair natively emits a different marker the filter recognizes, prefer that.

- [ ] **Step 4: Confirm the server discard-mode contract**

Run: `grep -n 'discard\|StreamThoughtFilter\|def _resolve_should_buffer\|channel' /Users/sethford/Documents/gemma-realtime-1/scripts/mlx-server.py | head -30`
Expected: confirm the server's StreamThoughtFilter has a "discard" mode that drops content from the thought-channel marker onward. Record one line in the spec: "Server discard mode drops from `<sentinel>` to end — confirmed at mlx-server.py:LINE."

- [ ] **Step 5: Decision gate**

If Q2 reveals the reply is NOT cleanly separable (deliberation and reply semantically interleaved with no boundary), STOP and escalate — the self-distillation approach needs rethinking (fall back to teacher-generated or reply-only). Otherwise proceed to Task 1 with the confirmed sentinel string.

- [ ] **Step 6: Commit**

```bash
cd /Users/sethford/Projects/h-uman
git add scripts/spike_replyfirst_distill.py docs/superpowers/specs/2026-05-29-sentinel-boundary-replyfirst-lora-design.md
git commit -m "spike(voice): Task-0 reply-first distillation marker/parse findings"
```

---

## Task 1: Corpus reorder logic (pure, fully tested)

**Files:**
- Create: `scripts/replyfirst_corpus_lib.py`
- Test: `scripts/test_replyfirst_corpus_lib.py`

**Note:** Use the sentinel string confirmed in Task 0. This plan uses `<|channel|>thought` as the default; if Task 0 chose a different one, substitute it consistently everywhere `SENTINEL` appears.

- [ ] **Step 1: Write the failing tests**

```python
#!/usr/bin/env python3
"""Unit tests for replyfirst_corpus_lib. Plain-runner, stdlib only, no model.
Run: python3 scripts/test_replyfirst_corpus_lib.py
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import replyfirst_corpus_lib as lib


def test_split_marker_present():
    # deliberation, then a marker, then the reply (one v4-repair shape)
    raw = "Let me think about this.\n<|channel|>final\nYeah, I'm around — what's up?"
    delib, reply = lib.split_deliberation_reply(raw, marker="<|channel|>")
    assert delib == "Let me think about this.", repr(delib)
    assert reply == "Yeah, I'm around — what's up?", repr(reply)
    print("✓ split_marker_present")


def test_split_markerless_fallback_last_paragraph_is_reply():
    raw = "I should keep this casual and short.\n\nHaha yeah totally, let's do it."
    delib, reply = lib.split_deliberation_reply(raw, marker="<|channel|>")
    assert reply == "Haha yeah totally, let's do it.", repr(reply)
    assert delib == "I should keep this casual and short.", repr(delib)
    print("✓ split_markerless_fallback_last_paragraph_is_reply")


def test_split_pure_reply_no_deliberation():
    raw = "yeah what's up"
    delib, reply = lib.split_deliberation_reply(raw, marker="<|channel|>")
    assert delib == "", repr(delib)
    assert reply == "yeah what's up", repr(reply)
    print("✓ split_pure_reply_no_deliberation")


def test_reorder_puts_reply_first_then_sentinel_then_delib():
    out = lib.reorder_to_replyfirst("the deliberation", "the reply", sentinel="<|channel|>thought")
    assert out == "the reply<|channel|>thought\nthe deliberation", repr(out)
    print("✓ reorder_puts_reply_first_then_sentinel_then_delib")


def test_reorder_empty_deliberation_keeps_boundary():
    out = lib.reorder_to_replyfirst("", "just the reply", sentinel="<|channel|>thought")
    assert out == "just the reply<|channel|>thought\n", repr(out)
    print("✓ reorder_empty_deliberation_keeps_boundary")


def test_build_target_returns_none_on_empty_reply():
    # marker at very start → no reply text → parse failure
    raw = "<|channel|>final\n"
    assert lib.build_target(raw, marker="<|channel|>", sentinel="<|channel|>thought") is None
    print("✓ build_target_returns_none_on_empty_reply")


def test_format_sft_example_text_schema():
    ex = lib.format_sft_example("USER: hi", "hey<|channel|>thought\n")
    assert ex == {"text": "USER: hi\nhey<|channel|>thought\n"}, repr(ex)
    print("✓ format_sft_example_text_schema")


def run():
    test_split_marker_present()
    test_split_markerless_fallback_last_paragraph_is_reply()
    test_split_pure_reply_no_deliberation()
    test_reorder_puts_reply_first_then_sentinel_then_delib()
    test_reorder_empty_deliberation_keeps_boundary()
    test_build_target_returns_none_on_empty_reply()
    test_format_sft_example_text_schema()
    print("\nALL replyfirst_corpus_lib TESTS PASSED")


if __name__ == "__main__":
    run()
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/sethford/Projects/h-uman && python3 scripts/test_replyfirst_corpus_lib.py`
Expected: FAIL — `ModuleNotFoundError: No module named 'replyfirst_corpus_lib'`.

- [ ] **Step 3: Write the implementation**

```python
#!/usr/bin/env python3
"""Pure reorder logic for the reply-first corpus. No I/O, no model — unit-tested.

split_deliberation_reply: given a v4-repair raw generation, return (deliberation, reply).
reorder_to_replyfirst:     produce the reply-first training target.
build_target:              full pipeline for one generation; None on parse failure.
format_sft_example:        wrap (prompt, target) into the {"text": ...} SFT schema.
"""
from __future__ import annotations

DEFAULT_SENTINEL = "<|channel|>thought"


def split_deliberation_reply(raw: str, marker: str = "<|channel|>") -> tuple[str, str]:
    """Split a raw generation into (deliberation, reply).

    1. If a channel marker is present, the reply is everything AFTER the last marker
       line; deliberation is everything before it.
    2. Else fall back to paragraph split: last non-empty paragraph = reply.
    3. Single paragraph → pure reply, empty deliberation.
    """
    raw = raw.strip()
    if marker in raw:
        # take text after the final marker occurrence; drop a trailing channel word
        tail = raw.rsplit(marker, 1)[1]
        # strip a leading channel value like "final\n" / "thought\n"
        for word in ("final", "thought", "analysis", "commentary"):
            if tail.lstrip().startswith(word):
                tail = tail.lstrip()[len(word):]
                break
        reply = tail.strip()
        delib = raw.rsplit(marker, 1)[0].strip()
        return delib, reply
    paras = [p.strip() for p in raw.split("\n\n") if p.strip()]
    if len(paras) <= 1:
        return "", raw
    return "\n\n".join(paras[:-1]), paras[-1]


def reorder_to_replyfirst(deliberation: str, reply: str,
                          sentinel: str = DEFAULT_SENTINEL) -> str:
    """Produce target: <reply><sentinel>\\n<deliberation>. Boundary always present."""
    deliberation = deliberation.strip()
    return f"{reply}{sentinel}\n{deliberation}" if deliberation else f"{reply}{sentinel}\n"


def build_target(raw: str, marker: str = "<|channel|>",
                 sentinel: str = DEFAULT_SENTINEL) -> str | None:
    """Full per-generation pipeline. Returns None on parse failure (empty reply)."""
    delib, reply = split_deliberation_reply(raw, marker=marker)
    if not reply.strip():
        return None
    return reorder_to_replyfirst(delib, reply, sentinel=sentinel)


def format_sft_example(prompt: str, target: str) -> dict:
    """Wrap into the {"text": ...} schema training_loop.py expects (see :611-634)."""
    return {"text": f"{prompt}\n{target}"}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd /Users/sethford/Projects/h-uman && python3 scripts/test_replyfirst_corpus_lib.py`
Expected: `ALL replyfirst_corpus_lib TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
cd /Users/sethford/Projects/h-uman
git add scripts/replyfirst_corpus_lib.py scripts/test_replyfirst_corpus_lib.py
git commit -m "feat(voice): reply-first corpus reorder logic + unit tests"
```

---

## Task 2: Corpus builder CLI (offline distillation glue)

**Files:**
- Create: `scripts/build_replyfirst_corpus.py`

This task is CLI glue around the tested logic. The model-touching part (`generate`) is copied verbatim from `eval_fidelity_nightly.py:55-102` (proven). No new unit test for the glue beyond a `--dry-run` smoke check (Step 3) — the logic is covered by Task 1.

- [ ] **Step 1: Write the builder**

```python
#!/usr/bin/env python3
"""Build the reply-first SFT corpus by self-distilling v4-repair OFFLINE.

For each m3 user prompt: generate with v4-repair (mlx_lm.generate, offline), split
into (deliberation, reply), reorder to reply-first, write SFT JSONL. Holds out a
disjoint split for ordering eval. Parse failures are logged, never emitted.

NEVER touches the :8741 prod server. Run on Apple Silicon.
Run: python3 scripts/build_replyfirst_corpus.py --limit 400
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import replyfirst_corpus_lib as lib

MODEL_ID = "mlx-community/gemma-4-31b-it-4bit"
ADAPTER = Path.home() / ".human/training-data/adapters/seth-lora-v4-repair-20260525-071921"
CORPUS_IN = Path.home() / ".human/training-data/m3-corpus.jsonl"
OUT_DIR = Path.home() / ".human/training-data"
SENTINEL = lib.DEFAULT_SENTINEL  # substitute Task-0's choice if different


def generate(prompt: str, max_tokens: int = 256) -> str:
    """Offline mlx_lm.generate. Copied from eval_fidelity_nightly.py:55-102."""
    cmd = [sys.executable, "-m", "mlx_lm", "generate", "--model", MODEL_ID,
           "--adapter-path", str(ADAPTER), "--prompt", prompt,
           "--max-tokens", str(max_tokens), "--temp", "0.0"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return "[timeout]"
    if r.returncode != 0:
        return "[gen_err]"
    lines = [l for l in r.stdout.splitlines()
             if l and not l.startswith("==") and not l.startswith("Prompt")
             and not l.startswith("Generation:") and "tokens-per-sec" not in l
             and "Peak memory" not in l]
    return "\n".join(lines).strip()


def load_user_prompts(limit: int) -> list[str]:
    prompts = []
    for line in CORPUS_IN.read_text().splitlines():
        rec = json.loads(line)
        if rec.get("role") == "user" and rec.get("content", "").strip():
            prompts.append(rec["content"].strip())
        if len(prompts) >= limit:
            break
    return prompts


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=400, help="max prompts to distill")
    ap.add_argument("--heldout-frac", type=float, default=0.15)
    ap.add_argument("--dry-run", action="store_true",
                    help="skip generation; emit nothing; just validate prompt loading")
    args = ap.parse_args()

    prompts = load_user_prompts(args.limit)
    print(f"[corpus] loaded {len(prompts)} user prompts", flush=True)
    if args.dry_run:
        print("[corpus] dry-run: skipping generation"); return 0

    examples, failures = [], []
    for i, p in enumerate(prompts):
        raw = generate(p)
        if raw.startswith("[") and raw.endswith("]"):  # [timeout]/[gen_err]
            failures.append({"prompt": p, "raw": raw}); continue
        target = lib.build_target(raw, sentinel=SENTINEL)
        if target is None:
            failures.append({"prompt": p, "raw": raw}); continue
        examples.append(lib.format_sft_example(p, target))
        if i % 20 == 0:
            print(f"[corpus] {i}/{len(prompts)} ok={len(examples)} fail={len(failures)}", flush=True)

    n_heldout = max(1, int(len(examples) * args.heldout_frac))
    heldout, train = examples[:n_heldout], examples[n_heldout:]

    (OUT_DIR / "replyfirst-train.jsonl").write_text(
        "\n".join(json.dumps(e) for e in train) + "\n")
    (OUT_DIR / "replyfirst-heldout.jsonl").write_text(
        "\n".join(json.dumps(e) for e in heldout) + "\n")
    (OUT_DIR / "replyfirst-parse-failures.jsonl").write_text(
        "\n".join(json.dumps(f) for f in failures) + ("\n" if failures else ""))

    print(f"[corpus] DONE train={len(train)} heldout={len(heldout)} "
          f"failures={len(failures)}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Verify it imports + parses args**

Run: `cd /Users/sethford/Projects/h-uman && python3 -c "import ast; ast.parse(open('scripts/build_replyfirst_corpus.py').read()); print('syntax ok')"`
Expected: `syntax ok`.

- [ ] **Step 3: Dry-run smoke (no model)**

Run: `cd /Users/sethford/Projects/h-uman && python3 scripts/build_replyfirst_corpus.py --dry-run --limit 5`
Expected: `[corpus] loaded N user prompts` then `[corpus] dry-run: skipping generation`. No JSONL written, no model call.

- [ ] **Step 4: Commit**

```bash
cd /Users/sethford/Projects/h-uman
git add scripts/build_replyfirst_corpus.py
git commit -m "feat(voice): offline reply-first corpus builder CLI"
```

---

## Task 3: Train wrapper + scale=2.0 assertion

**Files:**
- Create: `scripts/train_replyfirst.py`
- Test: `scripts/test_train_replyfirst.py`

- [ ] **Step 1: Write the failing test**

```python
#!/usr/bin/env python3
"""Unit test for train_replyfirst.assert_scale_2. Stdlib only, no model.
Run: python3 scripts/test_train_replyfirst.py
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import train_replyfirst as tr


def test_scale_2_passes():
    tr.assert_scale_2({"lora_parameters": {"scale": 2.0, "rank": 8}})  # no raise
    print("✓ scale_2_passes")


def test_scale_20_hard_fails():
    try:
        tr.assert_scale_2({"lora_parameters": {"scale": 20.0}})
    except SystemExit as e:
        assert "2.0" in str(e), str(e)
        print("✓ scale_20_hard_fails"); return
    raise AssertionError("expected SystemExit on scale=20.0")


def test_missing_scale_hard_fails():
    try:
        tr.assert_scale_2({"lora_parameters": {}})
    except SystemExit:
        print("✓ missing_scale_hard_fails"); return
    raise AssertionError("expected SystemExit on missing scale")


def run():
    test_scale_2_passes()
    test_scale_20_hard_fails()
    test_missing_scale_hard_fails()
    print("\nALL train_replyfirst TESTS PASSED")


if __name__ == "__main__":
    run()
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/sethford/Projects/h-uman && python3 scripts/test_train_replyfirst.py`
Expected: FAIL — `ModuleNotFoundError: No module named 'train_replyfirst'`.

- [ ] **Step 3: Write the implementation**

```python
#!/usr/bin/env python3
"""Train seth-lora-v5-replyfirst via mlx_lm lora. scale=2.0 enforced.

Hyperparameters match the v4-repair recipe (lora-scale-default-or-die.md): rank=8,
scale=2.0 EXPLICIT, dropout=0.0, lr=1e-5, batch=1, num_layers=8, max_seq=2048.
Post-train: read adapter_config.json and HARD-FAIL if scale != 2.0.
Run on Apple Silicon: python3 scripts/train_replyfirst.py --iters 500
"""
import argparse
import json
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

MODEL_ID = "mlx-community/gemma-4-31b-it-4bit"
DATA_DIR = Path.home() / ".human/training-data"
ADAPTERS_DIR = DATA_DIR / "adapters"
LINEAGE = DATA_DIR / "adapter_lineage.jsonl"


def assert_scale_2(adapter_config: dict) -> None:
    """HARD-FAIL unless lora scale is exactly 2.0 (mlx_lm 0.31.2 default is 20.0)."""
    scale = adapter_config.get("lora_parameters", {}).get("scale")
    if scale != 2.0:
        raise SystemExit(
            f"FATAL: adapter scale={scale!r}, expected 2.0 "
            f"(lora-scale-default-or-die.md). Refusing to ship.")


def write_lora_config(cfg_path: Path, data_dir: Path, adapter_dir: Path,
                      iters: int) -> None:
    """mlx_lm reads scale from a YAML config; write it explicitly."""
    cfg = f"""model: "{MODEL_ID}"
train: true
data: "{data_dir}"
adapter_path: "{adapter_dir}"
iters: {iters}
batch_size: 1
num_layers: 8
learning_rate: 1.0e-5
max_seq_length: 2048
lora_parameters:
  rank: 8
  scale: 2.0
  dropout: 0.0
"""
    cfg_path.write_text(cfg)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--iters", type=int, default=500)
    args = ap.parse_args()

    runid = datetime.now().strftime("%Y%m%d-%H%M%S")
    adapter_dir = ADAPTERS_DIR / f"seth-lora-v5-replyfirst-{runid}"
    adapter_dir.mkdir(parents=True, exist_ok=True)

    # mlx_lm lora expects train.jsonl/valid.jsonl in a data dir; symlink our splits.
    data_dir = DATA_DIR / f"replyfirst-data-{runid}"
    data_dir.mkdir(exist_ok=True)
    (data_dir / "train.jsonl").write_text((DATA_DIR / "replyfirst-train.jsonl").read_text())
    (data_dir / "valid.jsonl").write_text((DATA_DIR / "replyfirst-heldout.jsonl").read_text())

    cfg_path = adapter_dir / "lora_train_config.yaml"
    write_lora_config(cfg_path, data_dir, adapter_dir, args.iters)

    t0 = time.time()
    r = subprocess.run([sys.executable, "-m", "mlx_lm", "lora", "--config", str(cfg_path)])
    if r.returncode != 0:
        raise SystemExit(f"FATAL: mlx_lm lora exited {r.returncode}")

    # Post-train scale verification (lora-scale-default-or-die.md)
    ac_path = adapter_dir / "adapter_config.json"
    adapter_config = json.loads(ac_path.read_text())
    assert_scale_2(adapter_config)

    with LINEAGE.open("a") as f:
        f.write(json.dumps({
            "adapter": adapter_dir.name, "runid": runid, "base": MODEL_ID,
            "iters": args.iters, "scale": 2.0, "purpose": "reply-first ordering",
            "elapsed_sec": round(time.time() - t0, 1),
            "timestamp": datetime.now().isoformat(),
        }) + "\n")
    print(f"[train] DONE adapter={adapter_dir} scale=2.0 verified", flush=True)
    print(adapter_dir)  # last line = adapter path (consumed by the run task)
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run to verify the test passes**

Run: `cd /Users/sethford/Projects/h-uman && python3 scripts/test_train_replyfirst.py`
Expected: `ALL train_replyfirst TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
cd /Users/sethford/Projects/h-uman
git add scripts/train_replyfirst.py scripts/test_train_replyfirst.py
git commit -m "feat(voice): v5 reply-first train wrapper + scale=2.0 assertion"
```

---

## Task 4: Ordering eval gate

**Files:**
- Create: `scripts/eval_ordering.py`
- Test: `scripts/test_eval_ordering.py`

- [ ] **Step 1: Write the failing tests**

```python
#!/usr/bin/env python3
"""Unit tests for eval_ordering metrics + verdict. Stdlib only, no model.
Run: python3 scripts/test_eval_ordering.py
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import eval_ordering as eo

S = "<|channel|>thought"


def test_reply_first_true_when_reply_precedes_sentinel():
    assert eo.is_reply_first("yeah what's up" + S + "\nthinking", sentinel=S) is True
    print("✓ reply_first_true_when_reply_precedes_sentinel")


def test_reply_first_true_when_no_sentinel():
    assert eo.is_reply_first("yeah what's up", sentinel=S) is True
    print("✓ reply_first_true_when_no_sentinel")


def test_reply_first_false_when_sentinel_at_start():
    assert eo.is_reply_first(S + "\ndeliberating first", sentinel=S) is False
    print("✓ reply_first_false_when_sentinel_at_start")


def test_first_reply_token_index_zero_for_reply_first():
    assert eo.first_reply_token_index("hey there" + S + "\nx", sentinel=S) == 0
    print("✓ first_reply_token_index_zero_for_reply_first")


def test_first_reply_token_index_penalty_for_delib_first():
    g = S + "\none two three four five"
    assert eo.first_reply_token_index(g, sentinel=S) >= 5
    print("✓ first_reply_token_index_penalty_for_delib_first")


def test_build_verdict_pass_when_thresholds_met():
    gens = ["hi" + S + "\nx", "yo" + S + "\ny", "sup"]  # 3/3 reply-first
    v = eo.build_verdict(gens, sentinel=S, adapter_path="/x", floor=0.90, max_idx=8)
    assert v["pct_reply_first"] == 1.0, v
    assert v["gate"]["ordering_pass"] is True, v
    assert v["verdict"] == "PASS" and v["exit_code"] == 0, v
    print("✓ build_verdict_pass_when_thresholds_met")


def test_build_verdict_fail_when_below_floor():
    gens = [S + "\nbad", S + "\nbad", "good" + S + "\nx"]  # 1/3 reply-first
    v = eo.build_verdict(gens, sentinel=S, adapter_path="/x", floor=0.90, max_idx=8)
    assert v["gate"]["ordering_pass"] is False, v
    assert v["verdict"] == "FAIL" and v["exit_code"] == 1, v
    print("✓ build_verdict_fail_when_below_floor")


def run():
    test_reply_first_true_when_reply_precedes_sentinel()
    test_reply_first_true_when_no_sentinel()
    test_reply_first_false_when_sentinel_at_start()
    test_first_reply_token_index_zero_for_reply_first()
    test_first_reply_token_index_penalty_for_delib_first()
    test_build_verdict_pass_when_thresholds_met()
    test_build_verdict_fail_when_below_floor()
    print("\nALL eval_ordering TESTS PASSED")


if __name__ == "__main__":
    run()
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/sethford/Projects/h-uman && python3 scripts/test_eval_ordering.py`
Expected: FAIL — `ModuleNotFoundError: No module named 'eval_ordering'`.

- [ ] **Step 3: Write the implementation**

```python
#!/usr/bin/env python3
"""Ordering gate for the reply-first adapter.

Metric: does the user-facing reply come BEFORE the deliberation? Generates each
held-out casual prompt with the v5 adapter (offline) and measures % reply-first +
first-reply-token index. PASS iff pct_reply_first >= floor AND median index <= max_idx.

Exit: 0 = PASS, 1 = FAIL, 2 = SKIP/DEFERRED (mlx_lm unavailable / adapter missing).
Run: python3 scripts/eval_ordering.py --adapter-path <dir> --output-json out.json
"""
import argparse
import json
import statistics
import subprocess
import sys
from datetime import datetime
from pathlib import Path

MODEL_ID = "mlx-community/gemma-4-31b-it-4bit"
DEFAULT_SENTINEL = "<|channel|>thought"


def split_on_sentinel(generated: str, sentinel: str) -> tuple[str, str]:
    idx = generated.find(sentinel)
    if idx == -1:
        return generated, ""
    return generated[:idx], generated[idx + len(sentinel):]


def is_reply_first(generated: str, sentinel: str) -> bool:
    pre, _ = split_on_sentinel(generated, sentinel)
    return len(pre.strip()) > 0


def first_reply_token_index(generated: str, sentinel: str) -> int:
    """0 if reply streams first; large penalty if deliberation precedes the reply."""
    pre, _ = split_on_sentinel(generated, sentinel)
    if len(pre.strip()) > 0:
        return 0
    return len(generated.split())


def build_verdict(generations: list[str], sentinel: str, adapter_path: str,
                  floor: float = 0.90, max_idx: int = 8) -> dict:
    flags = [is_reply_first(g, sentinel) for g in generations]
    idxs = [first_reply_token_index(g, sentinel) for g in generations]
    pct = sum(flags) / len(flags) if flags else 0.0
    median_idx = statistics.median(idxs) if idxs else 1e9
    ordering_pass = pct >= floor and median_idx <= max_idx
    return {
        "timestamp": datetime.now().isoformat(),
        "verdict": "PASS" if ordering_pass else "FAIL",
        "exit_code": 0 if ordering_pass else 1,
        "n_prompts": len(generations),
        "adapter_path": adapter_path,
        "pct_reply_first": round(pct, 4),
        "median_first_reply_token_idx": median_idx,
        "gate": {"ordering_pass": ordering_pass, "floor": floor, "max_idx": max_idx},
    }


def generate(prompt: str, adapter_path: str, max_tokens: int = 80) -> str:
    cmd = [sys.executable, "-m", "mlx_lm", "generate", "--model", MODEL_ID,
           "--adapter-path", adapter_path, "--prompt", prompt,
           "--max-tokens", str(max_tokens), "--temp", "0.0"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired:
        return "[timeout]"
    if r.returncode != 0:
        return "[gen_err]"
    lines = [l for l in r.stdout.splitlines()
             if l and not l.startswith("==") and not l.startswith("Prompt")
             and not l.startswith("Generation:") and "tokens-per-sec" not in l
             and "Peak memory" not in l]
    return "\n".join(lines).strip()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--adapter-path", type=Path, required=True)
    ap.add_argument("--prompts", type=Path,
                    default=Path.home() / ".human/training-data/replyfirst-heldout.jsonl")
    ap.add_argument("--sentinel", default=DEFAULT_SENTINEL)
    ap.add_argument("--output-json", type=Path)
    args = ap.parse_args()

    if not args.adapter_path.exists():
        v = {"timestamp": datetime.now().isoformat(), "verdict": "SKIP",
             "reason": f"adapter not found: {args.adapter_path}", "exit_code": 2}
        print(f"[SKIP] {v['reason']}")
        if args.output_json:
            args.output_json.write_text(json.dumps(v, indent=2))
        return 2

    # held-out examples are SFT {"text": prompt + "\n" + target}; recover the prompt
    prompts = []
    for line in args.prompts.read_text().splitlines():
        if not line.strip():
            continue
        text = json.loads(line)["text"]
        prompts.append(text.rsplit("\n", 1)[0] if "\n" in text else text)

    gens = [generate(p, str(args.adapter_path)) for p in prompts]
    verdict = build_verdict(gens, args.sentinel, str(args.adapter_path))
    print(json.dumps(verdict, indent=2))
    if args.output_json:
        args.output_json.write_text(json.dumps(verdict, indent=2))
    return verdict["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run to verify the tests pass**

Run: `cd /Users/sethford/Projects/h-uman && python3 scripts/test_eval_ordering.py`
Expected: `ALL eval_ordering TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
cd /Users/sethford/Projects/h-uman
git add scripts/eval_ordering.py scripts/test_eval_ordering.py
git commit -m "feat(voice): reply-first ordering eval gate + unit tests"
```

---

## Task 5: Ship-decision gate (combine fidelity + ordering)

**Files:**
- Create: `scripts/replyfirst_ship_gate.py`
- Test: `scripts/test_replyfirst_ship_gate.py`

This applies the project's **+0.22 fidelity floor** against the fidelity verdict's `delta.mean` WITHOUT modifying the shared `eval_fidelity_nightly.py` (whose default floor stays 0.05 for the nightly gate).

- [ ] **Step 1: Write the failing tests**

```python
#!/usr/bin/env python3
"""Unit tests for replyfirst_ship_gate. Stdlib only.
Run: python3 scripts/test_replyfirst_ship_gate.py
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import replyfirst_ship_gate as sg


def _fid(delta):  # minimal fidelity verdict
    return {"delta": {"mean": delta}, "verdict": "PASS"}


def _ord(pct, idx):
    return {"pct_reply_first": pct, "median_first_reply_token_idx": idx}


def test_ship_true_when_both_pass():
    d = sg.ship_decision(_fid(0.25), _ord(0.95, 0))
    assert d["ship"] is True and d["fidelity_pass"] and d["ordering_pass"], d
    print("✓ ship_true_when_both_pass")


def test_no_ship_when_fidelity_below_022():
    d = sg.ship_decision(_fid(0.18), _ord(0.95, 0))
    assert d["ship"] is False and d["fidelity_pass"] is False, d
    print("✓ no_ship_when_fidelity_below_022")


def test_no_ship_when_ordering_below_090():
    d = sg.ship_decision(_fid(0.25), _ord(0.80, 0))
    assert d["ship"] is False and d["ordering_pass"] is False, d
    print("✓ no_ship_when_ordering_below_090")


def test_no_ship_when_median_idx_too_high():
    d = sg.ship_decision(_fid(0.25), _ord(0.95, 12))
    assert d["ship"] is False and d["ordering_pass"] is False, d
    print("✓ no_ship_when_median_idx_too_high")


def run():
    test_ship_true_when_both_pass()
    test_no_ship_when_fidelity_below_022()
    test_no_ship_when_ordering_below_090()
    test_no_ship_when_median_idx_too_high()
    print("\nALL replyfirst_ship_gate TESTS PASSED")


if __name__ == "__main__":
    run()
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/sethford/Projects/h-uman && python3 scripts/test_replyfirst_ship_gate.py`
Expected: FAIL — `ModuleNotFoundError: No module named 'replyfirst_ship_gate'`.

- [ ] **Step 3: Write the implementation**

```python
#!/usr/bin/env python3
"""Combine fidelity + ordering verdicts into a single ship decision.

Applies THIS project's strict +0.22 fidelity floor (not the shared 0.05 nightly
floor) against the fidelity verdict's delta.mean, AND the ordering gate.

Exit: 0 = SHIP, 1 = NO-SHIP.
Run: python3 scripts/replyfirst_ship_gate.py --fidelity f.json --ordering o.json
"""
import argparse
import json
import sys
from datetime import datetime
from pathlib import Path

FIDELITY_FLOOR = 0.22
ORDERING_FLOOR = 0.90
MAX_FIRST_IDX = 8


def ship_decision(fidelity_verdict: dict, ordering_verdict: dict,
                  fidelity_floor: float = FIDELITY_FLOOR,
                  ordering_floor: float = ORDERING_FLOOR,
                  max_idx: int = MAX_FIRST_IDX) -> dict:
    fid_delta = fidelity_verdict.get("delta", {}).get("mean", 0.0)
    fid_pass = fid_delta >= fidelity_floor
    pct = ordering_verdict.get("pct_reply_first", 0.0)
    median_idx = ordering_verdict.get("median_first_reply_token_idx", 1e9)
    ord_pass = pct >= ordering_floor and median_idx <= max_idx
    return {
        "timestamp": datetime.now().isoformat(),
        "ship": bool(fid_pass and ord_pass),
        "fidelity_pass": bool(fid_pass),
        "fidelity_delta": fid_delta,
        "fidelity_floor": fidelity_floor,
        "ordering_pass": bool(ord_pass),
        "pct_reply_first": pct,
        "median_first_reply_token_idx": median_idx,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fidelity", type=Path, required=True)
    ap.add_argument("--ordering", type=Path, required=True)
    ap.add_argument("--output-json", type=Path)
    args = ap.parse_args()

    d = ship_decision(json.loads(args.fidelity.read_text()),
                      json.loads(args.ordering.read_text()))
    print(json.dumps(d, indent=2))
    if args.output_json:
        args.output_json.write_text(json.dumps(d, indent=2))
    return 0 if d["ship"] else 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run to verify the tests pass**

Run: `cd /Users/sethford/Projects/h-uman && python3 scripts/test_replyfirst_ship_gate.py`
Expected: `ALL replyfirst_ship_gate TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
cd /Users/sethford/Projects/h-uman
git add scripts/replyfirst_ship_gate.py scripts/test_replyfirst_ship_gate.py
git commit -m "feat(voice): reply-first ship-decision gate (fidelity+ordering) + tests"
```

---

## Task 6: E2E streaming proof (live, Apple Silicon)

**Files:**
- Create: `scripts/eval_replyfirst_stream_e2e.py`

This is the SOTA proof artifact. It serves the v5 adapter on a **non-prod ephemeral port** (NOT `:8741`) and streams a casual prompt, asserting first-token-early + no leak + `streaming_beneficial:true`. No unit test — it is itself a live verification harness; its pure sub-logic (`streaming_beneficial`) gets one inline assertion test in Step 2.

- [ ] **Step 1: Write the proof harness**

```python
#!/usr/bin/env python3
"""E2E reply-first streaming proof. Apple Silicon, NON-PROD port (never :8741).

Streams a casual prompt through an ephemeral mlx-server serving the v5 adapter,
measures first-token latency vs full-response latency, and checks the trailing
deliberation is stripped (no <|channel> leak). Emits a proof JSON.

Run: python3 scripts/eval_replyfirst_stream_e2e.py --adapter-path <dir> \\
        --output-json proof.json
"""
import argparse
import json
import sys
import time
from datetime import datetime
from pathlib import Path

CASUAL_PROMPTS = ["hey, you around?", "yo what's the move tonight", "did you eat yet"]
LEAK_MARKERS = ["<|channel", "<|thought", "<|message", "<|return"]


def streaming_beneficial(first_token_ms: float, full_ms: float,
                         threshold_frac: float = 0.5) -> bool:
    """True if the first reply token arrives meaningfully before the full response."""
    if full_ms <= 0:
        return False
    return first_token_ms <= threshold_frac * full_ms


def _selftest():
    assert streaming_beneficial(100.0, 1000.0) is True
    assert streaming_beneficial(900.0, 1000.0) is False
    assert streaming_beneficial(100.0, 0.0) is False
    print("✓ streaming_beneficial self-test passed")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--adapter-path", type=Path, required=True)
    ap.add_argument("--port", type=int, default=8799, help="ephemeral, NOT 8741")
    ap.add_argument("--output-json", type=Path)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        _selftest(); return 0

    assert args.port != 8741, "refuse to use the production port"
    # NOTE: serving + SSE consumption uses the same harness as
    # scripts/eval_streaming_smoke.py (the existing tripwire). Reuse its
    # server-spawn + SSE-read helpers; do not reimplement. This step assumes
    # an mlx-server is started on args.port with the v5 adapter, then:
    import urllib.request

    results = []
    for prompt in CASUAL_PROMPTS:
        body = json.dumps({
            "messages": [{"role": "user", "content": prompt}],
            "stream": True, "max_tokens": 80, "stream_strip": False,  # casual=incremental
        }).encode()
        req = urllib.request.Request(
            f"http://127.0.0.1:{args.port}/v1/chat/completions", data=body,
            headers={"Content-Type": "application/json"})
        t0 = time.time()
        first_token_ms = None
        chunks = []
        with urllib.request.urlopen(req, timeout=120) as resp:
            for raw in resp:
                line = raw.decode().strip()
                if not line.startswith("data:"):
                    continue
                payload = line[len("data:"):].strip()
                if payload == "[DONE]":
                    break
                delta = json.loads(payload)["choices"][0]["delta"].get("content", "")
                if delta and first_token_ms is None:
                    first_token_ms = (time.time() - t0) * 1000
                chunks.append(delta)
        full_ms = (time.time() - t0) * 1000
        text = "".join(chunks)
        leaked = any(m in text for m in LEAK_MARKERS)
        results.append({
            "prompt": prompt, "reply": text[:200],
            "first_token_ms": round(first_token_ms or full_ms, 1),
            "full_ms": round(full_ms, 1),
            "leaked": leaked,
            "streaming_beneficial": streaming_beneficial(first_token_ms or full_ms, full_ms),
        })

    proof = {
        "timestamp": datetime.now().isoformat(),
        "adapter_path": str(args.adapter_path),
        "n_prompts": len(results),
        "all_beneficial": all(r["streaming_beneficial"] for r in results),
        "any_leak": any(r["leaked"] for r in results),
        "results": results,
    }
    proof["verdict"] = "PASS" if (proof["all_beneficial"] and not proof["any_leak"]) else "FAIL"
    print(json.dumps(proof, indent=2))
    if args.output_json:
        args.output_json.write_text(json.dumps(proof, indent=2))
    return 0 if proof["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Run the pure-logic self-test (no model)**

Run: `cd /Users/sethford/Projects/h-uman && python3 scripts/eval_replyfirst_stream_e2e.py --selftest`
Expected: `✓ streaming_beneficial self-test passed`.

- [ ] **Step 3: Commit**

```bash
cd /Users/sethford/Projects/h-uman
git add scripts/eval_replyfirst_stream_e2e.py
git commit -m "feat(voice): reply-first E2E streaming proof harness"
```

---

## Task 7: First live run — distill, train, gate (Apple Silicon)

**This task runs real compute (hours).** No new code. Produces the verdict artifacts. Do NOT touch the shared `:8741` server.

**Files:**
- Create (outputs): `~/.human/training-data/replyfirst-{train,heldout,parse-failures}.jsonl`, `~/.human/training-data/adapters/seth-lora-v5-replyfirst-<runid>/`, `docs/superpowers/plans/results/replyfirst-{fidelity,ordering,ship,e2e}-verdict.json`

- [ ] **Step 1: Build the corpus**

Run: `cd /Users/sethford/Projects/h-uman && python3 scripts/build_replyfirst_corpus.py --limit 400 2>&1 | tee /tmp/replyfirst_build.log`
Expected: `[corpus] DONE train=N heldout=M failures=F` with F well under N (if F > 25% of prompts, STOP — the splitter is unreliable; revisit Task 0/1).

- [ ] **Step 2: Train v5**

Run: `cd /Users/sethford/Projects/h-uman && python3 scripts/train_replyfirst.py --iters 500 2>&1 | tee /tmp/replyfirst_train.log`
Expected: last line is the adapter path; `[train] DONE ... scale=2.0 verified`. Capture the path:
`ADAPTER=$(tail -1 /tmp/replyfirst_train.log)`

- [ ] **Step 3: Run the ordering gate**

Run:
```bash
cd /Users/sethford/Projects/h-uman
mkdir -p docs/superpowers/plans/results
python3 scripts/eval_ordering.py --adapter-path "$ADAPTER" \
  --output-json docs/superpowers/plans/results/replyfirst-ordering-verdict.json
```
Expected: verdict JSON printed; exit 0 if `pct_reply_first >= 0.90`.

- [ ] **Step 4: Run the fidelity gate (reuse existing harness, unchanged)**

Run:
```bash
cd /Users/sethford/Projects/h-uman
python3 scripts/eval_fidelity_nightly.py --adapter-path "$ADAPTER" \
  --output-json docs/superpowers/plans/results/replyfirst-fidelity-verdict.json
```
Expected: verdict JSON with `delta.mean`. (The shared script's own gate uses floor 0.05; the +0.22 project floor is applied in Step 5.)

- [ ] **Step 5: Combined ship decision**

Run:
```bash
cd /Users/sethford/Projects/h-uman
python3 scripts/replyfirst_ship_gate.py \
  --fidelity docs/superpowers/plans/results/replyfirst-fidelity-verdict.json \
  --ordering docs/superpowers/plans/results/replyfirst-ordering-verdict.json \
  --output-json docs/superpowers/plans/results/replyfirst-ship-verdict.json
```
Expected: `"ship": true` only if BOTH gates pass. Record the result.

- [ ] **Step 6: Commit the verdicts**

```bash
cd /Users/sethford/Projects/h-uman
git add docs/superpowers/plans/results/replyfirst-*.json
git commit -m "test(voice): first v5 reply-first distill+train+gate verdicts"
```

---

## Task 8: Ship decision + contingent follow-ups

- [ ] **Step 1: Branch on the ship verdict**

Read `docs/superpowers/plans/results/replyfirst-ship-verdict.json`.

- **If `ship: true`:** proceed to Step 2 (E2E proof + wire the adapter).
- **If `ship: false` because fidelity < +0.22:** the reorder cost too much voice. Remediation: increase `--iters` to 800, rebuild corpus with `--limit 800`, re-run Task 7. Spawn a follow-up chip if deferring.
- **If `ship: false` because ordering < 0.90:** the model isn't learning reply-first. Remediation: inspect `replyfirst-train.jsonl` for malformed targets; verify the sentinel string matches Task 0; consider raising iters. Spawn a follow-up chip if deferring.

- [ ] **Step 2 (only if ship:true): Run the E2E streaming proof**

Start an ephemeral mlx-server on port 8799 with `$ADAPTER` (reuse the `scripts/eval_streaming_smoke.py` spawn helper), then:
Run: `cd /Users/sethford/Projects/h-uman && python3 scripts/eval_replyfirst_stream_e2e.py --adapter-path "$ADAPTER" --port 8799 --output-json docs/superpowers/plans/results/replyfirst-e2e-verdict.json`
Expected: `"verdict": "PASS"` with `all_beneficial: true` and `any_leak: false`.

- [ ] **Step 3 (only if E2E PASS): Wire the adapter for production**

The production daemon reads `personalization.lora_adapter_path` (per MEMORY.md — NOT `mlx_local.adapter_path`). Document the one-line config change needed in the ship verdict commit message. **Do NOT edit `~/.human/config.json` from the harness** (security constraint) — surface the exact key/value for the user to set:
`personalization.lora_adapter_path = "<ADAPTER absolute path>"`

- [ ] **Step 4: Commit final artifacts + run /verify**

```bash
cd /Users/sethford/Projects/h-uman
git add docs/superpowers/plans/results/replyfirst-e2e-verdict.json
git commit -m "test(voice): v5 reply-first E2E streaming SOTA proof"
```
Then spawn the `verifier` agent (`/verify`) to independently re-run the unit-test suite (Tasks 1,3,4,5 + the e2e selftest) and confirm `RESULT_verifier=PASS` before declaring the work done.

---

## Self-Review (run after writing this plan)

- **Spec coverage:** Task 0 ↔ Open Questions/marker spike; Task 1+2 ↔ build_replyfirst_corpus.py; Task 3 ↔ train_replyfirst.py + scale assertion; Task 4 ↔ eval_ordering.py; Task 5 ↔ +22pp floor reuse of eval_fidelity_nightly.py; Task 6+8 ↔ E2E proof; Task 7 ↔ first run; Task 8 ↔ ship gate + error-handling branches. All spec sections mapped.
- **Type consistency:** `SENTINEL`/`sentinel` default `<|channel|>thought` is consistent across Tasks 1,4,5,6. `build_target`/`split_deliberation_reply`/`reorder_to_replyfirst`/`format_sft_example` signatures match between Task 1 impl and its tests. `ship_decision`/`is_reply_first`/`first_reply_token_index`/`build_verdict`/`assert_scale_2`/`streaming_beneficial` names match between impl and tests.
- **No placeholders:** every code step has complete code; every run step has an exact command + expected output. The only intentional template is `<runid>` (timestamp) and `$ADAPTER` (captured at runtime).
