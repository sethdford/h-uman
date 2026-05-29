#!/usr/bin/env python3
"""
Wave 7 / W7-4 — combined voice-fidelity scorecard (one command).

Runs the three Wave 7 instruments and synthesizes one report that answers
"where are we, really?":
  - data viability        (voice_data_audit)   — is the corpus even trainable?
  - per-axis fidelity      (fidelity_axes)      — which voice axes are off?
  - LoRA vs RAG            (lora_vs_rag_ab)     — is the inherited bet right?

Each section runs only if its inputs are provided, so a partial run still gives
a partial scorecard. The synthesized headline + prioritized recommendations
turn three JSON blobs into a decision.

Usage (any subset of inputs):
  scripts/fidelity_scorecard.py \\
      --db ~/.human/memory.db \\
      --reference real_msgs.jsonl \\
      --responses model_out.jsonl \\
      --prompts heldout.jsonl --dry-run \\
      --output-json scorecard.json
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import fidelity_axes as fx
import lora_vs_rag_ab as ab
import voice_data_audit as vda


def synthesize_scorecard(audit_report, axes_report, ab_report):
    """Pure: fold the (optional) sub-reports into a headline + ranked actions."""
    bits = []
    recs = []

    if audit_report:
        v = audit_report["verdict"]
        n = audit_report["stats"]["assistant_messages"]
        bits.append(f"DATA={v} ({n} msgs)")
        if v == "starved":
            recs.append(
                "P0 DATA: corpus is starved — collect more real messages before trusting any "
                "voice metric; in the meantime prefer RAG-over-own-messages (needs no training)."
            )
        elif v == "thin":
            recs.append(
                "P1 DATA: corpus is thin — expect a low ceiling from a LoRA adapter; favor "
                "RAG or a lightly-fine-tuned + RAG hybrid until volume/diversity grows."
            )

    if axes_report:
        agg = axes_report["aggregate"]
        weak = ", ".join(w["axis"] for w in axes_report["weakest_axes"])
        bits.append(f"FIDELITY={agg:.2f} (weak: {weak})")
        recs.append(f"P1 AXES: target the weakest voice axes first — {weak}.")

    if ab_report:
        w = ab_report["overall_winner"]
        bits.append(f"APPROACH={w}")
        if w == "rag":
            recs.append(
                "P0 APPROACH: RAG beats the LoRA adapter on this data — make RAG-over-own-"
                "messages the primary voice path (or hybrid), not the fine-tune."
            )
        elif w == "lora":
            recs.append("P2 APPROACH: LoRA wins on this data — keep the adapter as primary.")
        else:
            recs.append("P2 APPROACH: LoRA/RAG tie — prefer RAG (cheaper, no training run).")

    # Ground truth is always the missing piece until humans rate.
    recs.append(
        "P0 TRUTH: run the blind human eval (scripts/blind_eval_run.sh) — automated metrics "
        "correlate only ~40-60% with human judgment; that's the real number."
    )

    headline = " | ".join(bits) if bits else "no sections ran (provide --db / --reference+--responses / --prompts)"
    # Stable priority sort (P0 before P1 before P2) without reordering within a tier.
    recs.sort(key=lambda r: r.split(":")[0])
    return {"headline": headline, "recommendations": recs}


def main():
    ap = argparse.ArgumentParser(description="Combined voice-fidelity scorecard (W7-4)")
    ap.add_argument("--db", type=Path, default=None, help="conversation DB for the data audit")
    ap.add_argument("--reference", type=Path, default=None, help="real target messages")
    ap.add_argument("--responses", type=Path, default=None, help="model responses for per-axis scoring")
    ap.add_argument("--prompts", type=Path, default=None, help="held-out prompts for LoRA-vs-RAG")
    ap.add_argument("--dry-run", action="store_true", help="A/B without a live model")
    ap.add_argument("--k", type=int, default=3)
    ap.add_argument("--output-json", type=Path, default=None)
    args = ap.parse_args()

    report = {}

    # 1. Data audit
    if args.db and args.db.exists():
        rows = vda.read_messages(str(args.db))
        if rows is not None:
            stats = vda.audit_rows(rows)
            verdict, reasons = vda.data_viability_verdict(stats)
            report["audit"] = {"verdict": verdict, "reasons": reasons, "stats": stats}

    # 2. Per-axis fidelity (needs reference + model responses)
    reference = fx.load_messages(args.reference) if (args.reference and args.reference.exists()) else None
    if reference and args.responses and args.responses.exists():
        report["axes"] = fx.decompose(reference, fx.load_messages(args.responses))

    # 3. LoRA vs RAG (needs reference + prompts)
    if reference and args.prompts and args.prompts.exists():
        try:
            report["lora_vs_rag"] = ab.run_ab(
                reference, fx.load_messages(args.prompts), args.dry_run, "http://127.0.0.1:8741/v1", args.k
            )
        except SystemExit as e:
            report["lora_vs_rag_skipped"] = str(e)

    report["scorecard"] = synthesize_scorecard(
        report.get("audit"), report.get("axes"), report.get("lora_vs_rag")
    )

    print(json.dumps(report, indent=2))
    print(f"\n=== SCORECARD: {report['scorecard']['headline']} ===", file=sys.stderr)
    for r in report["scorecard"]["recommendations"]:
        print(f"  - {r}", file=sys.stderr)
    if args.output_json:
        args.output_json.write_text(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
