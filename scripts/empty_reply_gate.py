#!/usr/bin/env python3
# scripts/empty_reply_gate.py
#
# The empty-reply promotion gate for nightly persona-adapter candidates, and
# the per-candidate promotion manifest it writes.
#
# WHY (2026-10-02): the served GLM adapter ends ~13-18% of classifier-style
# replies immediately (no visible text), which sends that traffic to cloud
# failover. A nightly candidate must not make that worse. nightly-retrain.sh
# measures both adapters with scripts/eval_empty_reply_rate.py on a spare
# server (MLX_EMPTY_RETRY=0, same flags, same night, prod down) and calls this
# script, which writes <candidate_dir>/promotion_manifest.json:
#   empty_reply.verdict  PASS iff rate(candidate) <= rate(serving) on the
#                        overall ("all") rate, both arms comparable;
#                        INCONCLUSIVE when either arm was not measured.
#   empty_reply.enforce  true only in HU_RETRAIN_EMPTY_EVAL=live.
#   promotion_gate       live: authorship verdict AND empty_reply must PASS.
#                        shadow: the authorship verdict, unchanged.
# scripts/m3_promote.py reads the manifest and refuses a swap when
# empty_reply.enforce is true and the verdict is not PASS, and -- when
# HU_RETRAIN_EMPTY_EVAL=live at promote time -- also when the manifest is
# missing or unreadable (required_empty_reply_verdict: fail closed). Otherwise
# no manifest, or a shadow manifest, changes nothing.
#
# Pure arithmetic on numbers eval_empty_reply_rate.py measured. Never loads a
# model or touches a server. Only rates, counts and paths are recorded; reply
# text stays in the eval script's own result files.
"""Empty-reply promotion gate + promotion manifest for nightly candidates."""

import argparse
import json
import os
import sys
import time
from pathlib import Path

MANIFEST_NAME = "promotion_manifest.json"
# Arms are only comparable when measured the same way.
_COMPARABLE_KEYS = ("prompts_file", "samples", "max_tokens", "temperature")


def _rate(report, cat):
    try:
        v = report["summary"][cat]["rate"]
        n = report["summary"][cat]["n"]
    except (KeyError, TypeError):
        return None, None
    if v is None or not isinstance(n, int) or n <= 0:
        return None, None
    return float(v), n


def decide_empty_reply(candidate, serving):
    """Pure. {"verdict": PASS|BLOCK|INCONCLUSIVE, "reason", rates...}."""
    if not isinstance(candidate, dict) or not isinstance(serving, dict):
        return {"verdict": "INCONCLUSIVE", "reason": "an arm was not measured"}
    for k in _COMPARABLE_KEYS:
        if candidate.get(k) != serving.get(k):
            return {"verdict": "INCONCLUSIVE",
                    "reason": f"arms not comparable: {k} {candidate.get(k)!r} != {serving.get(k)!r}"}
    c, cn = _rate(candidate, "all")
    s, sn = _rate(serving, "all")
    if c is None or s is None:
        return {"verdict": "INCONCLUSIVE", "reason": "missing or empty summary.all"}
    out = {"candidate_rate": round(c, 4), "serving_rate": round(s, 4), "n_per_arm": cn,
           "candidate_classifier_rate": _rate(candidate, "classifier")[0],
           "serving_classifier_rate": _rate(serving, "classifier")[0]}
    if c <= s:
        return {**out, "verdict": "PASS", "reason": "candidate_empty_rate_not_above_serving"}
    return {**out, "verdict": "BLOCK", "reason": "candidate_empty_rate_above_serving"}


def combine(authorship_verdict, empty_reply, mode):
    """The candidate's overall promotion verdict. shadow never changes it."""
    a = authorship_verdict or "UNKNOWN"
    if mode != "live" or a != "PASS":
        return a
    return "PASS" if empty_reply.get("verdict") == "PASS" else empty_reply.get("verdict", "INCONCLUSIVE")


def enforced_empty_reply_verdict(adapter_dir):
    """For m3_promote.py: the empty_reply block of <adapter_dir>'s manifest when
    it is ENFORCED (written in live mode), else None (nothing to enforce)."""
    p = Path(str(adapter_dir)) / MANIFEST_NAME
    if not p.is_file():
        return None
    try:
        er = json.loads(p.read_text()).get("empty_reply") or {}
    except (OSError, json.JSONDecodeError, AttributeError):
        return {"verdict": "INCONCLUSIVE", "reason": f"unreadable {p}"}
    return er if er.get("enforce") is True else None


def required_empty_reply_verdict(adapter_dir, mode):
    """For m3_promote.py, given the PROMOTE-time HU_RETRAIN_EMPTY_EVAL mode.

    off/shadow: enforced_empty_reply_verdict() -- the manifest decides.
    live: fail closed. A missing manifest (the gate never ran, or crashed
    before writing), an unreadable one (crashed mid-write), or one with no
    empty_reply verdict is INCONCLUSIVE, which blocks; a recorded verdict is
    enforced whatever mode wrote it. A measurement that never completed is
    never a PASS."""
    if mode != "live":
        return enforced_empty_reply_verdict(adapter_dir)
    p = Path(str(adapter_dir)) / MANIFEST_NAME
    if not p.is_file():
        return {"verdict": "INCONCLUSIVE",
                "reason": f"HU_RETRAIN_EMPTY_EVAL=live but no {MANIFEST_NAME} in {adapter_dir} "
                          f"(the gate never ran or crashed before writing it)"}
    try:
        er = json.loads(p.read_text()).get("empty_reply")
    except (OSError, ValueError, AttributeError):
        return {"verdict": "INCONCLUSIVE", "reason": f"unreadable {p}"}
    if not isinstance(er, dict) or not er.get("verdict"):
        return {"verdict": "INCONCLUSIVE", "reason": f"{p} has no empty_reply verdict"}
    return er


def _load(path):
    if not path or not os.path.isfile(path):
        return None
    try:
        return json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError):
        return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mode", choices=("shadow", "live"), required=True)
    ap.add_argument("--candidate-adapter", required=True)
    ap.add_argument("--serving-adapter", required=True)
    ap.add_argument("--candidate-json", default=None, help="eval_empty_reply_rate.py --out")
    ap.add_argument("--serving-json", default=None, help="eval_empty_reply_rate.py --out")
    ap.add_argument("--authorship-json", default=None,
                    help="score_candidate_offline.py output (promotion_gate.verdict)")
    ap.add_argument("--inconclusive", default=None,
                    help="record INCONCLUSIVE with this reason (an arm could not run)")
    ap.add_argument("--out", required=True, help="the candidate's promotion_manifest.json")
    args = ap.parse_args(argv)

    if args.inconclusive:
        er = {"verdict": "INCONCLUSIVE", "reason": args.inconclusive}
    else:
        er = decide_empty_reply(_load(args.candidate_json), _load(args.serving_json))
    er.update({"mode": args.mode, "enforce": args.mode == "live",
               "candidate_json": args.candidate_json, "serving_json": args.serving_json,
               "rule": "rate(candidate) <= rate(serving), summary.all, same spare server, "
                       "MLX_EMPTY_RETRY=0"})
    auth = _load(args.authorship_json) or {}
    auth_verdict = (auth.get("promotion_gate") or {}).get("verdict") if isinstance(auth, dict) else None
    manifest = {
        "schema": 1,
        "written_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "candidate_adapter": args.candidate_adapter,
        "serving_adapter": args.serving_adapter,
        "empty_reply": er,
        "authorship": {"verdict": auth_verdict or "UNKNOWN", "score_json": args.authorship_json},
        "promotion_gate": {"verdict": combine(auth_verdict, er, args.mode), "mode": args.mode},
    }
    # Atomic: a crash mid-write must never leave a half-written manifest.
    tmp = Path(f"{args.out}.tmp.{os.getpid()}")
    tmp.write_text(json.dumps(manifest, indent=2) + "\n")
    os.replace(tmp, args.out)
    print(f"empty_reply={er['verdict']} ({er['reason']}) candidate_rate={er.get('candidate_rate')} "
          f"serving_rate={er.get('serving_rate')} authorship={manifest['authorship']['verdict']} "
          f"promotion_gate={manifest['promotion_gate']['verdict']} mode={args.mode} -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
