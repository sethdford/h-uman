"""Score Seth's one-time check (spec §4.1). The curator's wrong-accept rate is
estimated as d*PPV + (1-d)*FOR: d = Gemma's disagreement rate on kept notes,
PPV = share of Gemma-'unsupported' notes Seth also calls unsupported, FOR =
share of Gemma-'supported' notes Seth calls unsupported. Bonferroni: each
component's Wilson interval is computed at 97.5% so the combined interval has
joint coverage >=95% (linearly combining two 95% intervals only guarantees
>=90% joint coverage)."""
import argparse
import csv
import json
import sys

from . import audit, stats, store

# Bonferroni correction for the 2-way union bound: each component interval at
# 1 - 0.05/2 = 97.5% confidence so the combined (worst-case additive) interval
# has joint coverage >= 95%.
BONFERRONI_Z = 2.2414


def _label(raw):
    """'y'/'yes'/'n'/'no', trimmed and case-insensitive, else None (unlabeled) —
    a bare prefix match (e.g. "not sure" -> "n") is never accepted."""
    s = (raw or "").strip().lower()
    if s in ("y", "yes"):
        return "y"
    if s in ("n", "no"):
        return "n"
    return None


def score_check(sheet_csv, key_json, disagreement_rate):
    key = json.load(open(key_json))
    rows = list(csv.DictReader(open(sheet_csv)))
    labels = {r["row"]: _label(r.get("supported")) for r in rows}
    if not rows or any(labels.get(k) is None for k in key):
        return stats.NOT_MEASURED
    u = [labels[k] for k, v in key.items() if v["gemma"] == "unsupported"]
    s = [labels[k] for k, v in key.items() if v["gemma"] == "supported"]
    if not u or not s:
        return stats.NOT_MEASURED
    ppv_k, for_k = u.count("n"), s.count("n")
    ppv, fo = ppv_k / len(u), for_k / len(s)
    d = disagreement_rate
    p_lo, p_hi = stats.wilson(ppv_k, len(u), z=BONFERRONI_Z)
    f_lo, f_hi = stats.wilson(for_k, len(s), z=BONFERRONI_Z)
    return {"ppv": round(ppv, 4), "false_omission": round(fo, 4),
            "estimated_wrong_accept_rate": d * ppv + (1 - d) * fo,
            "ci95": [round(d * p_lo + (1 - d) * f_lo, 4), round(d * p_hi + (1 - d) * f_hi, 4)],
            "n_labeled": len(u) + len(s), "disagreement_rate": d}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("sheet")
    ap.add_argument("--key", required=True)
    ap.add_argument("--store", default=store.DEFAULT_PATH)
    a = ap.parse_args(argv)
    dis = audit.audit_report(store.open_store(a.store))["all"]["disagreement"]
    if dis == stats.NOT_MEASURED:
        print("refusing: no audits yet", file=sys.stderr)
        return 2
    r = score_check(a.sheet, a.key, dis["rate"])
    print(json.dumps(r, indent=1))
    return 0 if r != stats.NOT_MEASURED else 2


if __name__ == "__main__":
    sys.exit(main())
