#!/usr/bin/env python3
"""Contract C3 measurement — does the daemon's own commitment detector
recall the real commitments hiding in its production replies?

Pulls up to --n daemon replies (production_outcomes.chosen — the daemon's
own SENT text, read-only) from ~/.human/memory.db that contain a
commitment-shaped keyword, runs each through
`human memory agent-facts-dry <reply>` (the pure-extraction CLI hook added
alongside hu_agent_facts_record_reply — no graph/memory writes, nothing
sent anywhere), and scores the detector's `commitment: true/false` output
against a HAND-LABELLED ground truth baked into this script (HAND_LABELS
below, keyed by row id + a category, never by quoting the reply itself —
see "no private text on disk" below).

This is NOT a claim that the candidate pool is unbiased: the SQL selection
uses the same keyword vocabulary the detector itself scans for (i'll, i
will, gonna, promise, let me, we should), so recall on this sample is an
upper bound, not a general-population estimate — every candidate is
*already* something the detector's vocabulary can see. What the sample DOES
expose honestly is precision: how many of the keyword-matched replies are
courtesy filler ("let me know...") vs a genuine first-person deliverable.
Report BOTH numbers; do not headline recall alone.

No private text on disk: production_outcomes.chosen is Seth's real sent
message content. This script reads it, but never WRITES the raw reply (or
the detector's extracted commitment substring) into the results JSON, this
script's source, or stdout beyond a length — only row ids, booleans, and
counts, none of which reconstruct the message. The results file is what
gets committed; the raw text lives only in ~/.human/memory.db, which never
leaves this machine.

Refuses (exit 2, nothing written) when: the human binary is missing, the
production database is missing, fewer than --min-labelled hand-labels exist
for the pulled ids, or the pool has fewer than --n candidates.
"""
import argparse, json, os, sqlite3, subprocess, sys, time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DB = os.path.expanduser("~/.human/memory.db")
DEFAULT_BIN = os.path.join(REPO_ROOT, "build", "human")
OUT_PATH = os.path.join(
    REPO_ROOT, "docs", "plans", "2026-08-02-semantic-retrieval", "agent-facts-results.json"
)

# Keyword vocabulary the C detector (hu_conversation_detect_commitment,
# data/conversation/commitment_keywords.json) scans for. Used only to pull a
# candidate pool from real production replies — see the module docstring for
# why this makes the pool a recall UPPER BOUND, not a general estimate.
CANDIDATE_LIKE = [
    "%i''ll%",
    "%i will%",
    "%gonna%",
    "%promise%",
    "%let me%",
]

# Hand-labelled ground truth, by production_outcomes.id: True if the reply
# is a genuine first-person commitment to a future concrete action (a
# deliverable, or a specific promised state); False if it is a bare
# courtesy invitation ("let me know ...", asking THEM to inform ME) or
# vague filler with no concrete deliverable ("I'll help you with that.
# What would you like...", "that's gonna add up fast" — not a first-person
# future action at all). Labelled 2026-09-02 by reading each reply's plain
# meaning directly in a local shell (never copied into this file or any
# git-tracked artifact — see "no private text on disk" above); the `note`
# field below is a category label, not a quote.
HAND_LABELS = {
    426: (False, "courtesy invitation (let-me-know)"),
    411: (False, "courtesy invitation (let-me-know)"),
    410: (True, "concrete deliverable"),
    391: (True, "concrete promised state"),
    390: (False, "boilerplate refusal tail, no deliverable"),
    378: (False, "generic 'I'll help', no specific action"),
    377: (True, "concrete deliverable"),
    372: (False, "'gonna' used conversationally, not 1st-person future action"),
    370: (False, "generic 'I'll help', no specific action"),
    358: (True, "concrete deliverable"),
    314: (True, "concrete promised state"),
    309: (True, "concrete deliverable"),
    305: (True, "concrete deliverable"),
    304: (True, "self-directed action ('let me' + verb, not let-me-know)"),
    297: (False, "'gonna' used conversationally, not 1st-person future action"),
    292: (True, "concrete deliverable"),
    277: (False, "courtesy invitation (let-me-know)"),
    267: (True, "leads with deliverable marker 'I will', not a courtesy prefix"),
    262: (True, "concrete deliverable"),
    226: (True, "concrete deliverable"),
}


def fetch_candidates(db_path, n):
    if not os.path.exists(db_path):
        sys.exit(f"REFUSING: {db_path} missing — no production replies to measure against")
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    where = " OR ".join(f"chosen LIKE '{pat}'" for pat in CANDIDATE_LIKE)
    rows = con.execute(
        f"SELECT id, chosen FROM production_outcomes WHERE {where} "
        f"ORDER BY send_timestamp DESC LIMIT ?",
        (n,),
    ).fetchall()
    con.close()
    return rows


def run_dry(binp, reply):
    proc = subprocess.run(
        [binp, "memory", "agent-facts-dry", reply], capture_output=True, text=True, timeout=30
    )
    lines = [l for l in proc.stdout.splitlines() if l.strip().startswith("{")]
    if not lines:
        return None
    try:
        return json.loads(lines[-1])
    except json.JSONDecodeError:
        return None


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--db", default=DEFAULT_DB)
    ap.add_argument("--bin", default=DEFAULT_BIN)
    ap.add_argument("--n", type=int, default=20)
    ap.add_argument(
        "--min-labelled", type=int, default=20, help="refuse if fewer ids have a hand label"
    )
    ap.add_argument("--out", default=OUT_PATH)
    args = ap.parse_args()

    if not os.path.exists(args.bin):
        sys.exit(f"REFUSING: {args.bin} missing — build the human target first")

    rows = fetch_candidates(args.db, args.n)
    if len(rows) < args.n:
        sys.exit(f"REFUSING: only {len(rows)} candidate replies found, need {args.n}")

    unlabelled = [rid for rid, _ in rows if rid not in HAND_LABELS]
    if unlabelled:
        sys.exit(
            f"REFUSING: {len(unlabelled)} pulled ids have no hand label in HAND_LABELS "
            f"(ids: {unlabelled}) — label them honestly (by id + category, never by quoting "
            f"the reply) in this script before scoring"
        )
    if len(HAND_LABELS) < args.min_labelled:
        sys.exit(f"REFUSING: only {len(HAND_LABELS)} hand labels, need >= {args.min_labelled}")

    # per_item never carries the raw reply or the detector's extracted
    # commitment substring — both are Seth's real message content. Only
    # row id, reply length (a shape signal, not content), and booleans.
    results = []
    for rid, reply in rows:
        out = run_dry(args.bin, reply)
        if out is None:
            sys.exit(f"REFUSING: agent-facts-dry produced no parseable JSON for id={rid}")
        detected = bool(out.get("commitment"))
        truth, note = HAND_LABELS[rid]
        results.append(
            {
                "id": rid,
                "reply_len": len(reply),
                "hand_label_is_commitment": truth,
                "hand_label_note": note,
                "detector_says_commitment": detected,
                "fact_count": len(out.get("facts", [])),
            }
        )

    n_true = sum(1 for r in results if r["hand_label_is_commitment"])
    n_detected_true = sum(1 for r in results if r["detector_says_commitment"])
    tp = sum(1 for r in results if r["hand_label_is_commitment"] and r["detector_says_commitment"])
    fn = sum(
        1 for r in results if r["hand_label_is_commitment"] and not r["detector_says_commitment"]
    )
    fp = sum(
        1 for r in results if not r["hand_label_is_commitment"] and r["detector_says_commitment"]
    )

    if n_true == 0:
        sys.exit(
            "REFUSING: hand-labelled set has zero genuine commitments — recall is undefined on n=0"
        )

    recall = tp / n_true
    precision = tp / n_detected_true if n_detected_true else float("nan")

    report = {
        "measured_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source": "production_outcomes.chosen (~/.human/memory.db), read-only, "
        "raw text never written to this file",
        "n_candidates": len(results),
        "n_genuine_commitments_hand_labelled": n_true,
        "n_detector_says_commitment": n_detected_true,
        "true_positives": tp,
        "false_negatives": fn,
        "false_positives": fp,
        "recall": round(recall, 4),
        "precision": round(precision, 4) if n_detected_true else None,
        "caveat": (
            "Candidate pool was selected via the SAME keyword vocabulary the detector "
            "scans for, so recall here is an upper bound on a keyword-matched sample, "
            "not a general-population estimate. Precision is the more informative number "
            "from this sample. As of 2026-09-02, hu_agent_facts_record_reply (and this "
            "dry-run hook) reuse hu_promise_keeper_is_courtesy_invitation, so bare "
            "'let me know...' invitations with no deadline and no first-person "
            "deliverable are excluded before scoring; the remaining false positives are "
            "generic filler ('I'll help you with that...') and conversational 'gonna' "
            "that the courtesy filter's narrower prefix list does not cover — a further "
            "precision gain would need broadening that predicate, not duplicating it here."
        ),
        "per_item": results,
    }

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(report, f, indent=2)

    print(
        f"recall={recall:.3f} ({tp}/{n_true})  precision={report['precision']}  "
        f"n={len(results)}  -> {args.out}"
    )


if __name__ == "__main__":
    main()
