#!/usr/bin/env python3
"""Record the seth-glm-air-v6 training run in the adapter registry.

Metrics are PARSED FROM THE RUN LOG, never hand-typed. A registry row asserting
a val loss nobody measured is the failure class in
.claude/rules/no-number-without-a-measurement.md -- so this refuses to write a
row when the evidence it would cite is absent.

Usage: register_v6_adapter.py --adapter <dir> --log <train log> [--smoke <json>]
"""
import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from adapter_registry import record_training  # noqa: E402

ANSI = re.compile(r"\x1b\[[0-9;]*m")
# mlx_lm_lora (preference modes) reports accuracy and margin; mlx_lm (SFT) reports
# neither. Match both shapes rather than silently finding nothing and registering
# an adapter with no training evidence at all.
VAL = re.compile(
    r"Iter\s+(\d+):\s*Val loss\s+([0-9.-]+).*?Val accuracy\s+([0-9.-]+).*?"
    r"Val margin\s+([0-9.-]+)", re.I)
VAL_SFT = re.compile(r"Iter\s+(\d+):\s*Val loss\s+([0-9.-]+)", re.I)
TRAIN = re.compile(
    r"Iter\s+(\d+):\s*loss\s+([0-9.-]+),.*?acc\s+([0-9.-]+),\s*margin\s+([0-9.-]+)", re.I)
TRAIN_SFT = re.compile(r"Iter\s+(\d+):\s*Train loss\s+([0-9.-]+)", re.I)


def val_is_inert(vals):
    """True when every validation reading is bit-identical.

    A metric that never moves while the model demonstrably learns is not a weak
    effect -- it is the treatment never being applied. Recording such a number as
    'val_loss' would put a non-measurement into the registry where a promotion
    gate could later read it as evidence. See
    .claude/rules/no-number-without-a-measurement.md.
    """
    if len(vals) < 2:
        return False
    return len({v[1] for v in vals}) == 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--adapter", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--smoke")
    ap.add_argument("--corpus-manifest",
                    default=str(Path.home() / ".human/training-data/glm-v6-pref/manifest.json"))
    a = ap.parse_args()

    adapter = Path(a.adapter)
    cfg_path = adapter / "adapter_config.json"
    if not (adapter / "adapters.safetensors").exists() or not cfg_path.exists():
        sys.exit(f"FATAL: incomplete adapter at {adapter}")

    cfg = json.load(open(cfg_path))
    scale = cfg.get("lora_parameters", {}).get("scale")
    if scale != 2.0:
        sys.exit(f"FATAL: adapter scale is {scale}, expected 2.0 -- not registering an unsafe adapter")

    text = ANSI.sub("", Path(a.log).read_text(errors="replace"))

    # Determine the objective FROM THE LOG. Never assume it -- an adapter recorded
    # under the wrong objective is a provenance lie a later gate would act on.
    m = re.search(r"Training [Mm]ode:\s*(\w+)", text)
    if m:
        objective = m.group(1).lower()          # mlx_lm_lora path (orpo/dpo/cpo/...)
    elif re.search(r"Iter\s+\d+:\s*Train loss", text):
        objective = "sft"                        # mlx_lm.lora path
    else:
        sys.exit("FATAL: cannot determine the training objective from the log -- "
                 "refusing to register an adapter with unknown provenance")

    # The no-op check, again, at registration. The trainer guard already runs it,
    # but a registry row is what a promotion gate reads, so it must not depend on
    # someone having run the right script. v6 and v6.1 were both registered before
    # anyone checked the weights, and both were no-ops.
    import mlx.core as mx
    w = mx.load(str(adapter / "adapters.safetensors"))
    bkeys = [k for k in w if k.endswith("lora_b")]
    b_nonzero = sum(1 for k in bkeys if float(mx.abs(w[k]).max()) > 0)
    b_max = max((float(mx.abs(w[k]).max()) for k in bkeys), default=0.0)
    if b_nonzero == 0:
        sys.exit(f"FATAL: all {len(bkeys)} lora_b are 0.0 -- this adapter is a no-op "
                 "(identical to base). Refusing to register it as trained.")

    vals = [(int(i), float(l), float(ac), float(m)) for i, l, ac, m in VAL.findall(text)]
    trains = [(int(i), float(l), float(ac), float(m)) for i, l, ac, m in TRAIN.findall(text)]
    # SFT logs carry loss only -- pad accuracy/margin with None-equivalents so the
    # downstream shape is uniform, but never invent values.
    if not vals:
        vals = [(int(i), float(l), float("nan"), float("nan")) for i, l in VAL_SFT.findall(text)]
    if not trains:
        trains = [(int(i), float(l), float("nan"), float("nan")) for i, l in TRAIN_SFT.findall(text)]
    manifest = json.load(open(a.corpus_manifest))

    # mlx_lm_lora writes a MINIMAL adapter_config.json (lora_parameters only), so
    # the recipe is read back from the log, not from the config.
    def grab(pat, cast=str):
        m = re.search(pat, text, re.I)
        return cast(m.group(1)) if m else None

    inert = val_is_inert(vals)

    metrics = {
        "objective": objective,
        "beta": 0.05 if objective in ("orpo", "dpo", "cpo") else None,
        "weights": {"lora_b_nonzero": f"{b_nonzero}/{len(bkeys)}",
                    "max_abs_lora_b": b_max},
        "base_model": grab(r"Model:\s*(\S+)"),
        "iters": len(trains) and max(t[0] for t in trains) or None,
        "learning_rate": grab(r"Learning Rate:\s*(\S+)"),
        "num_layers": cfg.get("num_layers"),
        "lora_scale": scale,
        "n_pairs": manifest["counts"]["total"],
        "n_pairs_by_source": manifest["by_source"],
        "targets": manifest["targets"],
        # Train-side is the only signal that actually moved.
        "train_loss_first": trains[0][1] if trains else None,
        "train_loss_last": trains[-1][1] if trains else None,
        "train_acc_first": trains[0][2] if trains else None,
        "train_acc_last": trains[-1][2] if trains else None,
        "train_margin_first": trains[0][3] if trains else None,
        "train_margin_last": trains[-1][3] if trains else None,
        "train_series": trains or None,
        "corpus_manifest": a.corpus_manifest,
        "train_log": a.log,
        # Provenance for the promotion gate: this adapter is NOT certified.
        "human_gate": "PENDING -- cycle-5 sheet not generated/rated",
        "promoted": False,
    }

    if inert:
        # Do NOT store a val_loss key at all. A consumer that reads
        # metrics["val_loss"] must not find a number that measured nothing.
        metrics["validation"] = {
            "status": "NOT_MEASURED",
            "reason": "every Val reading was bit-identical across all "
                      f"{len(vals)} checkpoints while train metrics moved -- "
                      "mlx_lm_lora's evaluate_orpo did not see the LoRA updates",
            "identical_reading": {"loss": vals[0][1], "accuracy": vals[0][2],
                                  "margin": vals[0][3]} if vals else None,
        }
        print("WARNING: validation was INERT (identical at every checkpoint); "
              "recording status=NOT_MEASURED instead of a val_loss", file=sys.stderr)
    elif vals:
        metrics["validation"] = {
            "status": "measured",
            "val_loss_first": vals[0][1], "val_loss_last": vals[-1][1],
            "val_acc_last": vals[-1][2], "val_margin_last": vals[-1][3],
            "series": vals,
        }
    else:
        metrics["validation"] = {"status": "ABSENT", "reason": "no Val lines in log"}
        print("WARNING: no 'Val loss' lines found -- status=ABSENT, no number invented",
              file=sys.stderr)

    if a.smoke and Path(a.smoke).exists():
        smoke = json.load(open(a.smoke))
        metrics["smoke"] = smoke.get("summary", smoke)
        metrics["smoke_path"] = a.smoke
    else:
        metrics["smoke"] = {"status": "NOT_RUN"}

    adapter_id = adapter.name
    record_training(adapter_id=adapter_id, metrics=metrics)
    print(f"[registry] recorded training for {adapter_id}")
    print(f"  n_pairs      : {metrics['n_pairs']} {metrics['n_pairs_by_source']}")
    print(f"  train loss   : {metrics['train_loss_first']} -> {metrics['train_loss_last']}")
    print(f"  train acc    : {metrics['train_acc_first']} -> {metrics['train_acc_last']}")
    print(f"  train margin : {metrics['train_margin_first']} -> {metrics['train_margin_last']}"
          f"  {'(still NEGATIVE: rejected still scores above chosen)' if (metrics['train_margin_last'] or 0) < 0 else ''}")
    print(f"  validation   : {metrics['validation']['status']}")
    print(f"  smoke        : {metrics['smoke'].get('status', 'recorded')}")
    print(f"  objective    : {objective}")
    print(f"  lora_b       : {b_nonzero}/{len(bkeys)} non-zero, max|B|={b_max:.3e}")
    print(f"  lora scale   : {scale}")
    print(f"  human gate   : {metrics['human_gate']}")


if __name__ == "__main__":
    main()
