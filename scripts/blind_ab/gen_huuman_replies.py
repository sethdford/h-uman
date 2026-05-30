#!/usr/bin/env python3
"""Generate h-uman's reply for each context, producing triples.json for the blind A/B.

Reuses the eval runner's generation path (which loads persona 'seth' for channel
'imessage' and applies the full persona system prompt) — the SAME path the product
uses — rather than a bespoke call. Steps:

  1. Read contexts.json: [{id, context, seth_reply}, ...]
  2. Emit a temp eval suite whose task `prompt` IS each context (non-judge match_mode,
     so no judge cost — we only want the generated reply).
  3. Run `build/human eval run <tmp_suite>` (test/CI mode disabled for real generation).
  4. Read each task's actual_output back from ~/.human/memory.db:eval_results.
  5. Write triples.json: [{id, context, seth_reply, huuman_reply}, ...].

Then feed triples.json to make_rating_sheet.py.

Usage:
    python3 gen_huuman_replies.py contexts.json [--out triples.json] \
        [--human ../../build/human] [--db ~/.human/memory.db]

Requires a configured provider (same as `human eval`). Costs ~$0.02-0.05 per context.
"""
import argparse, json, os, subprocess, sys, sqlite3, tempfile


def build_suite(contexts):
    tasks = []
    for c in contexts:
        for f in ("id", "context", "seth_reply"):
            if f not in c:
                raise ValueError(f"context missing '{f}': {c!r}")
        tasks.append({
            "id": c["id"],
            "category": "blind_ab",
            "difficulty": 1,
            "prompt": c["context"],
            "expected": "_",          # non-empty so the validator accepts the task
            "timeout_ms": 90000,      # generous; the fix keeps slow output anyway
        })
    return {
        "name": "blind-ab-generation",
        "description": "Generate h-uman replies for blind A/B; scoring is irrelevant.",
        "match_mode": "contains",     # local check, NO llm judge call -> no judge cost
        "tasks": tasks,
    }


def read_outputs(db_path, ids):
    """Latest actual_output per task_id from eval_results."""
    con = sqlite3.connect(db_path)
    out = {}
    for tid in ids:
        row = con.execute(
            "SELECT actual_output FROM eval_results WHERE task_id=? ORDER BY id DESC LIMIT 1",
            (tid,)).fetchone()
        out[tid] = (row[0] if row else None) or ""
    con.close()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("contexts")
    ap.add_argument("--out", default="triples.json")
    ap.add_argument("--human", default=os.path.join(os.path.dirname(__file__), "..", "..", "build", "human"))
    ap.add_argument("--db", default=os.path.expanduser("~/.human/memory.db"))
    a = ap.parse_args()

    with open(a.contexts) as f:
        contexts = json.load(f)
    if not isinstance(contexts, list) or not contexts:
        print("contexts.json must be a non-empty list", file=sys.stderr); sys.exit(2)

    suite = build_suite(contexts)
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as tf:
        json.dump(suite, tf)
        suite_path = tf.name

    env = dict(os.environ)
    env.pop("CI", None)
    env.pop("HU_IS_TEST", None)
    print(f"generating {len(contexts)} replies via {a.human} eval run ...", file=sys.stderr)
    proc = subprocess.run([a.human, "eval", "run", suite_path],
                          env=env, capture_output=True, text=True)
    os.unlink(suite_path)
    if proc.returncode != 0:
        print(f"eval run failed (exit {proc.returncode}):\n{proc.stderr[-500:]}", file=sys.stderr)
        sys.exit(1)
    if "INVALID RUN" in proc.stderr:
        print(f"WARNING: runner flagged INVALID (empty-dominated). Generation may have "
              f"failed:\n{proc.stderr[-300:]}", file=sys.stderr)

    outputs = read_outputs(a.db, [c["id"] for c in contexts])
    triples, missing = [], []
    for c in contexts:
        reply = outputs.get(c["id"], "")
        if not reply:
            missing.append(c["id"])
        triples.append({"id": c["id"], "context": c["context"],
                        "seth_reply": c["seth_reply"], "huuman_reply": reply})
    with open(a.out, "w") as f:
        json.dump(triples, f, indent=2)
    if missing:
        print(f"WARNING: {len(missing)} contexts got EMPTY replies: {missing}", file=sys.stderr)
    print(f"wrote {a.out} ({len(triples)} triples, {len(triples)-len(missing)} with replies)")


if __name__ == "__main__":
    main()
