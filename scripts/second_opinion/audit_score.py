"""Score Seth's one-time check (spec §4.1). The curator's wrong-accept rate is
estimated as d*PPV + (1-d)*FOR: d = Gemma's disagreement rate on kept notes,
PPV = share of Gemma-'unsupported' notes Seth also calls unsupported, FOR =
share of Gemma-'supported' notes Seth calls unsupported. Bonferroni: each
component's Wilson interval is computed at 97.5% so the combined interval has
joint coverage >=95% (linearly combining two 95% intervals only guarantees
>=90% joint coverage).

The interval treats d as KNOWN: d's own sampling uncertainty is not
propagated, so the reported upper bound is slightly optimistic. The output
says so ("d_treated_as_known": true).

d is taken from ONE backend's verdicts on ONE source population (default:
local Gemma on curator_wide notes, the population the HU_INSIGHT_WIDE
shadow->live gate is about); the key file written by audit_sheet records the
same provenance and a mismatch refuses."""
import argparse
import csv
import json
import sys

from . import audit, backend as be, stats, store

# Bonferroni correction for the 2-way union bound: each component interval at
# 1 - 0.05/2 = 97.5% confidence so the combined (worst-case additive) interval
# has joint coverage >= 95%.
BONFERRONI_Z = 2.2414


def _key_rows(key):
    return {k: v for k, v in key.items() if not k.startswith("_")}


def score_check(sheet_csv, key_json, disagreement_rate):
    with open(key_json) as f:
        key = _key_rows(json.load(f))
    with open(sheet_csv, newline="") as f:
        rows = list(csv.DictReader(f))
    labels = {r["row"]: store.parse_yes_no(r.get("supported")) for r in rows}
    if not rows or any(labels.get(k) is None for k in key):
        return stats.NOT_MEASURED
    u = [labels[k] for k, v in key.items() if v["gemma"] == "unsupported"]
    s = [labels[k] for k, v in key.items() if v["gemma"] == "supported"]
    if not u or not s:
        return stats.NOT_MEASURED
    ppv_k, for_k = u.count(False), s.count(False)
    ppv, fo = ppv_k / len(u), for_k / len(s)
    d = disagreement_rate
    p_lo, p_hi = stats.wilson(ppv_k, len(u), z=BONFERRONI_Z)
    f_lo, f_hi = stats.wilson(for_k, len(s), z=BONFERRONI_Z)
    return {"ppv": round(ppv, 4), "false_omission": round(fo, 4),
            "estimated_wrong_accept_rate": d * ppv + (1 - d) * fo,
            "ci95": [round(d * p_lo + (1 - d) * f_lo, 4), round(d * p_hi + (1 - d) * f_hi, 4)],
            "n_labeled": len(u) + len(s), "disagreement_rate": d,
            "d_treated_as_known": True}


def prompt_versions(con, backend, source):
    where, args = audit.audit_filter(backend, source)
    return sorted(r[0] for r in con.execute("SELECT DISTINCT prompt_version FROM audits" + where,
                                            args))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sheet")
    ap.add_argument("--key", required=True)
    ap.add_argument("--store", default=store.DEFAULT_PATH)
    ap.add_argument("--backend", default=be.GemmaBackend().name,
                    help="whose verdicts give d (default: the local Gemma backend)")
    ap.add_argument("--source", choices=["wide", "persona", "all"], default="wide",
                    help="note population for d (default wide: the HU_INSIGHT_WIDE gate)")
    a = ap.parse_args(argv)
    with open(a.key) as f:
        meta = json.load(f).get("_meta") or {}
    for field in ("backend", "source"):
        if meta.get(field) is not None and meta[field] != getattr(a, field):
            print(f"refusing: the key was drawn with {field}={meta[field]!r}, "
                  f"not {getattr(a, field)!r}", file=sys.stderr)
            return 2
    con = store.open_store(a.store)
    dis = audit.audit_report(con, backend=a.backend)[a.source]["disagreement"]
    if dis == stats.NOT_MEASURED:
        print(f"refusing: no {a.source} audits yet for {a.backend}", file=sys.stderr)
        return 2
    r = score_check(a.sheet, a.key, dis["rate"])
    if r != stats.NOT_MEASURED:
        r.update({"backend": a.backend, "source": a.source,
                  "prompt_versions": prompt_versions(con, a.backend, a.source),
                  "note": "interval treats the disagreement share d as known"})
    print(json.dumps(r, indent=1))
    return 0 if r != stats.NOT_MEASURED else 2


if __name__ == "__main__":
    sys.exit(main())
