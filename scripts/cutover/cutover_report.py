#!/usr/bin/env python3
"""Go/no-go report for the human-level cut-over kit (scripts/cutover/run_cutover.sh).

Reads one cut-over run (real held-out turns replayed per arm, the local
cross-family judge's verdicts, the memory probes) and writes a markdown report:
one row per arm with 95% bootstrap CIs, the per-ablation deltas, a PROMOTE /
HOLD verdict per gate, and the PlistBuddy commands that would set the PROMOTE
gates live plus their rollback. The commands are printed, never executed.

Arms: A = every candidate gate off; B = every candidate gate live;
B-no-<gate> = B with that one gate off (cutover_plan.py builds them).

PRE-REGISTERED DECISION RULES (fixed 2026-10-02, before any real run; changing
them after seeing a run's numbers voids that run's verdicts)
---------------------------------------------------------------------------
Statistics. Every CI is a 95% percentile bootstrap, BOOT=2000 resamples, fixed
seed. A delta between two arms is PAIRED: the same resampled turn ids feed both
arms. The unit is the turn (judge: the judged item, keyed by turn; memory: the
probe). Lower is better for judge detection (the judge picking Seth's real
reply in 2AFC; 0.50 = indistinguishable), length KS D vs Seth, fragment rate
and deflection rate; higher is better for memory-probe accuracy.

For gate g, X = arm B-no-g, and:
  dDetect = detect(X) - detect(B)   > 0 means g lowers detection
  dKS     = ksD(X) - ksD(B)         > 0 means g brings lengths closer to Seth's
  dFrag   = frag(B) - frag(X)       > 0 means g adds fragments
  dDefl   = defl(B) - defl(X)       > 0 means g adds deflections

R1 BENEFIT  g helps when dDetect's CI lies entirely above 0 (needs >= MIN_JUDGED
            items judged in BOTH arms) OR dKS's CI lies entirely above 0 (needs
            >= MIN_TEXT text replies in both arms).
R2 HARM     g hurts on a rate when that rate's worsening CI lies entirely above
            0, OR its point estimate is >= HARM_POINT_TOLERANCE (0.10): a large
            worsening is not waved through because n is small. Checked for
            dFrag and dDefl.
R3 INERT    g is inert when X and B made byte-identical reply requests
            (reply_fp) with the same action and bubbles on every turn: the gate
            never reached the reply path, so nothing about it was measured.
R4 STACK    B vs A must not be worse on fragment rate, deflection rate (R2's
            test with A in place of X) or memory-probe accuracy (worsening =
            acc(A) - acc(B), same test). A failed stack guard HOLDs every gate.
            Memory skipped by the operator (--skip-memory) is reported as NOT
            EVALUATED and does not block; memory that was attempted and is
            incomplete blocks.
R5 COMPLETE Every arm must be COMPLETE in the driver's manifest and cover the
            same turns. Otherwise every gate HOLDs: a partial arm is not a
            measurement.

PROMOTE(g)  <=>  R5 and R4 pass, g is not INERT, R1 holds, and neither R2 check
                 fires. Everything else is HOLD, with the reason.
Cut-over is GO when at least one gate is PROMOTE; it covers exactly those gates.
Tapback share, question rate and the median length ratio are reported, not
gated (the replay's director runs on the replay endpoint, not production's).

Privacy: reads the private run dir; writes report.md / report.json there (0600)
and prints only aggregates, gate names and commands. Never writes
~/.human/blind_ab_gate.json and never calls score.py --emit-gate.
"""
import argparse
import csv
import datetime
import importlib
import json
import math
import os
import random
import statistics
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from cutover_plan import CANDIDATE_GATES, gate_for_arm  # noqa: E402

BOOT = 2000
SEED = 20261002
CI_LEVEL = 0.95
MIN_JUDGED = 30
MIN_TEXT = 30
HARM_POINT_TOLERANCE = 0.10
MAX_UNDEFINED_SHARE = 0.10  # resamples where a metric is undefined, before its CI is dropped

PROMOTE, HOLD = "PROMOTE", "HOLD"


# ── statistics core (pure) ─────────────────────────────────────────────

def ks_d(xs, ys):
    """Two-sample Kolmogorov-Smirnov D. None when a side is empty."""
    if not xs or not ys:
        return None
    a, b = sorted(xs), sorted(ys)
    n, m = len(a), len(b)
    i = j = 0
    d = 0.0
    while i < n and j < m:
        v = min(a[i], b[j])
        while i < n and a[i] == v:
            i += 1
        while j < m and b[j] == v:
            j += 1
        d = max(d, abs(i / n - j / m))
    return d


def _mean(vals):
    vals = list(vals)
    return (sum(1 for v in vals if v) / len(vals)) if vals else None


def m_tapback(arm, ids):
    return _mean(arm["turns"][i]["action"] == "tapback" for i in ids)


def _text_flag(key):
    def m(arm, ids):
        return _mean(arm["turns"][i][key] for i in ids
                     if arm["turns"][i]["len"] is not None and arm["turns"][i][key] is not None)
    return m


m_fragment = _text_flag("fragment")
m_question = _text_flag("question")
m_deflection = _text_flag("deflection")


def _lens(arm, ids):
    return [arm["turns"][i]["len"] for i in ids if arm["turns"][i]["len"] is not None]


def m_ks(seth):
    def m(arm, ids):
        return ks_d(_lens(arm, ids), _lens(seth, ids))
    return m


def m_median_ratio(seth):
    def m(arm, ids):
        xs, ys = _lens(arm, ids), _lens(seth, ids)
        if not xs or not ys or statistics.median(ys) == 0:
            return None
        return statistics.median(xs) / statistics.median(ys)
    return m


def m_detect(arm, ids):
    return _mean(arm["judge"][i] for i in ids if i in arm["judge"])


def m_memory(arm, ids):
    return _mean(arm["memory"][i]["hit"] for i in ids if i in arm["memory"])


def _rng(tag):
    return random.Random(SEED ^ zlib.crc32(tag.encode()))


def _percentiles(vals):
    vals.sort()
    k = len(vals)
    lo_q = (1.0 - CI_LEVEL) / 2.0
    return vals[int(math.floor(lo_q * k))], vals[min(k - 1, int(math.ceil((1.0 - lo_q) * k)) - 1)]


def bootstrap(units, fn, tag, boot=BOOT):
    """{point, lo, hi, n}: fn on the full units, and its percentile CI over
    `boot` resamples of the units with replacement. lo/hi are None when the
    metric is undefined in more than MAX_UNDEFINED_SHARE of resamples."""
    units = list(units)
    point = fn(units) if units else None
    out = {"point": point, "lo": None, "hi": None, "n": len(units)}
    if point is None:
        return out
    rng = _rng(tag)
    vals = []
    for _ in range(boot):
        v = fn([units[rng.randrange(len(units))] for _ in units])
        if v is not None:
            vals.append(v)
    if len(vals) >= boot * (1.0 - MAX_UNDEFINED_SHARE):
        out["lo"], out["hi"] = _percentiles(vals)
    return out


def paired_delta(units, fn_x, fn_y, tag, boot=BOOT):
    """fn_x - fn_y on the same resampled units (paired bootstrap)."""
    def diff(ids):
        x, y = fn_x(ids), fn_y(ids)
        return None if x is None or y is None else x - y
    return bootstrap(units, diff, tag, boot)


# ── decision rules (pure) ──────────────────────────────────────────────

def ci_above_zero(d):
    return d is not None and d.get("lo") is not None and d["lo"] > 0


def is_harm(d):
    """R2 on a worsening delta (positive = worse)."""
    if d is None or d.get("point") is None:
        return False
    return ci_above_zero(d) or d["point"] >= HARM_POINT_TOLERANCE - 1e-12


def gate_verdict(g, *, complete, stack_failures, inert, d_detect, d_ks, d_frag, d_defl,
                 judged_n, text_n, min_judged=MIN_JUDGED, min_text=MIN_TEXT):
    """(verdict, [reasons]) for one gate, by R1-R5."""
    if not complete:
        return HOLD, ["R5: incomplete measurement"]
    if stack_failures:
        return HOLD, [f"R4: stack guard failed ({', '.join(stack_failures)})"]
    if inert is None:
        return HOLD, ["no ablation arm for this gate"]
    if inert:
        return HOLD, ["R3: INERT - identical reply requests on every turn"]
    reasons = []
    judge_ok = judged_n >= min_judged and ci_above_zero(d_detect)
    ks_ok = text_n >= min_text and ci_above_zero(d_ks)
    if judge_ok:
        reasons.append("R1: lowers judge detection")
    if ks_ok:
        reasons.append("R1: lengths closer to Seth (KS)")
    harms = [name for name, d in (("fragment", d_frag), ("deflection", d_defl)) if is_harm(d)]
    for h in harms:
        reasons.append(f"R2: worse on {h}")
    if not (judge_ok or ks_ok):
        why = []
        if judged_n < min_judged:
            why.append(f"judge n={judged_n} < {min_judged}")
        if text_n < min_text:
            why.append(f"text n={text_n} < {min_text}")
        reasons.append("R1: no benefit outside the noise" + (f" ({'; '.join(why)})" if why else ""))
    return (PROMOTE if (judge_ok or ks_ok) and not harms else HOLD), reasons


def stack_guards(d_frag, d_defl, d_mem, memory_status):
    """R4: names of the failed B-vs-A guards."""
    failed = [n for n, d in (("fragment", d_frag), ("deflection", d_defl)) if is_harm(d)]
    if memory_status == "measured" and is_harm(d_mem):
        failed.append("memory accuracy")
    if memory_status == "incomplete":
        failed.append("memory probes incomplete")
    return failed


def is_inert(rows_x, rows_b):
    """R3: every common turn sent the same reply request, action and bubbles."""
    bx = {r["id"]: r for r in rows_x}
    common = [r for r in rows_b if r["id"] in bx]
    if not common:
        return None

    def sig(r):
        return (r.get("reply_fp"), r.get("action"), tuple(r.get("bubbles") or ()))
    return all(sig(r) == sig(bx[r["id"]]) for r in common)


def evaluate(arms, seth, rows, *, complete, memory_status, boot=BOOT,
             min_judged=MIN_JUDGED, min_text=MIN_TEXT):
    """The whole decision. arms: {name: {"turns", "judge", "memory"}}; seth:
    {"turns"}; rows: {name: raw driver rows} (for R3)."""
    ids = sorted(set.intersection(*(set(a["turns"]) for a in arms.values()))) if arms else []
    res = {"turn_ids": len(ids), "arms": {}, "seth": {}, "gates": {}, "stack": {},
           "complete": complete, "memory_status": memory_status,
           "thresholds": {"boot": boot, "min_judged": min_judged, "min_text": min_text,
                          "harm_point_tolerance": HARM_POINT_TOLERANCE, "ci": CI_LEVEL,
                          "preregistered": (boot, min_judged, min_text) == (BOOT, MIN_JUDGED,
                                                                            MIN_TEXT)}}
    rate_metrics = (("tapback", m_tapback), ("question", m_question), ("fragment", m_fragment),
                    ("deflection", m_deflection))
    for name, fn in rate_metrics:
        res["seth"][name] = fn(seth, ids)
    seth_lens = _lens(seth, ids)
    res["seth"]["len_median"] = statistics.median(seth_lens) if seth_lens else None
    res["seth"]["text_n"] = len(seth_lens)
    for an, a in arms.items():
        def on(fn, a=a):
            return lambda s: fn(a, s)
        r = {"text_n": len(_lens(a, ids))}
        r["ks"] = bootstrap(ids, on(m_ks(seth)), f"ks:{an}", boot)
        r["median_ratio"] = bootstrap(ids, on(m_median_ratio(seth)), f"mr:{an}", boot)
        for name, fn in rate_metrics:
            r[name] = bootstrap(ids, on(fn), f"{name}:{an}", boot)
        r["deflection_n"] = sum(1 for i in ids if a["turns"][i]["deflection"] is not None)
        jid = sorted(i for i in ids if i in a["judge"])
        r["detect"] = bootstrap(jid, on(m_detect), f"detect:{an}", boot)
        mid = sorted(a["memory"])
        r["memory"] = bootstrap(mid, on(m_memory), f"mem:{an}", boot) if mid else None
        cats = {}
        for pid in mid:
            cats.setdefault(a["memory"][pid]["category"], []).append(a["memory"][pid]["hit"])
        r["memory_by_category"] = {c: {"n": len(v), "acc": sum(v) / len(v)}
                                   for c, v in sorted(cats.items())}
        res["arms"][an] = r

    def pd(x, y, fn, tag, judge=False, memory=False):
        if x not in arms or y not in arms:
            return None
        ax, ay = arms[x], arms[y]
        if judge:
            units = sorted(i for i in ids if i in ax["judge"] and i in ay["judge"])
        elif memory:
            units = sorted(set(ax["memory"]) & set(ay["memory"]))
        else:
            units = ids
        d = paired_delta(units, lambda s: fn(ax, s), lambda s: fn(ay, s), f"{tag}:{x}-{y}", boot)
        d["units"] = len(units)
        return d

    st = res["stack"]
    st["d_detect"] = pd("A", "B", m_detect, "detect", judge=True)
    st["d_ks"] = pd("A", "B", m_ks(seth), "ks")
    st["d_frag"] = pd("B", "A", m_fragment, "frag")
    st["d_defl"] = pd("B", "A", m_deflection, "defl")
    st["d_mem"] = (pd("A", "B", m_memory, "mem", memory=True)
                   if memory_status == "measured" else None)
    st["failed"] = stack_guards(st["d_frag"], st["d_defl"], st["d_mem"], memory_status)

    for g in gates_in(arms):
        x = arm_for_gate(g)
        if x not in arms:
            inert = None
            dd = dk = df = dl = None
        else:
            inert = is_inert(rows.get(x, []), rows.get("B", []))
            dd = pd(x, "B", m_detect, "detect", judge=True)
            dk = pd(x, "B", m_ks(seth), "ks")
            df = pd("B", x, m_fragment, "frag")
            dl = pd("B", x, m_deflection, "defl")
        text_n = min(res["arms"].get(x, {}).get("text_n", 0), res["arms"]["B"]["text_n"]) \
            if "B" in res["arms"] else 0
        verdict, reasons = gate_verdict(
            g, complete=complete, stack_failures=st["failed"], inert=inert, d_detect=dd,
            d_ks=dk, d_frag=df, d_defl=dl, judged_n=(dd or {}).get("units", 0), text_n=text_n,
            min_judged=min_judged, min_text=min_text)
        res["gates"][g] = {"arm": x, "inert": inert, "d_detect": dd, "d_ks": dk, "d_frag": df,
                           "d_defl": dl, "verdict": verdict, "reasons": reasons}
    res["promote"] = [g for g, v in res["gates"].items() if v["verdict"] == PROMOTE]
    res["go"] = bool(res["promote"])
    return res


def arm_for_gate(g):
    return "B-no-" + g[len("HU_"):].lower()


def gates_in(arms):
    """Gates under test, in CANDIDATE_GATES order: those with an ablation arm."""
    present = {gate_for_arm(a) for a in arms} - {None}
    return [g for g in CANDIDATE_GATES if g in present]


# ── loading (the run dir; heuristics come from the harness) ─────────────

def _jsonl(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def _import_from(dirpath, module):
    if dirpath not in sys.path:
        sys.path.insert(0, dirpath)
    return importlib.import_module(module)


def feature(rf, action, bubbles, inbound_bubbles):
    """One turn's features, by the replay feed's own heuristics (the same
    definitions replay_feed.summarize() uses)."""
    bubbles = [b for b in (bubbles or []) if b]
    reply = rf.joined(bubbles)
    inbound = rf.joined(inbound_bubbles)
    text = action == "text" and bool(reply)
    return {"action": action,
            "len": len(reply) if text else None,
            "fragment": any(rf.is_fragment(b) for b in bubbles) if text else None,
            "question": rf.has_question(reply) if text else None,
            "deflection": (rf.is_deflection(inbound, reply)
                           if text and rf.asked_question(inbound) else None)}


def manifest_complete(run_dir, arm_names):
    """True when the driver's manifest marks every arm complete and lists the
    same turn ids for all of them."""
    try:
        with open(os.path.join(run_dir, "manifest.json")) as f:
            man = json.load(f)
    except (OSError, ValueError):
        return False, {}
    flags = {x["arm"]: x.get("complete") is True for x in man.get("arms", [])}
    return all(flags.get(a) for a in arm_names), man


def load_judge(arm_dir):
    """{turn id: judge picked Seth's real reply}; parse failures are skipped."""
    sheet = os.path.join(arm_dir, "judged.csv")
    keyf = os.path.join(arm_dir, "answer_key.json")
    if not (os.path.isfile(sheet) and os.path.isfile(keyf)):
        return {}
    with open(keyf) as f:
        key = json.load(f)
    out = {}
    with open(sheet, newline="") as f:
        for r in csv.DictReader(f):
            if r.get("choice") in ("A", "B") and r.get("id") in key:
                out[r["id"]] = r["choice"] == key[r["id"]]
    return out


def load_memory(mem_run_dir, probes_path, memory_dir, arm_names):
    """({arm: {probe id: {category, hit}}}, status). status: measured |
    incomplete | skipped."""
    if not mem_run_dir:
        return {a: {} for a in arm_names}, "skipped"
    mps = _import_from(memory_dir, "memory_probe_score")
    probes = _jsonl(probes_path)
    mem_arms = [a for a in arm_names if os.path.isfile(os.path.join(mem_run_dir, "out",
                                                                    f"{a}.jsonl"))]
    complete, _ = manifest_complete(mem_run_dir, mem_arms)
    if not mem_arms or not complete or "A" not in mem_arms or "B" not in mem_arms:
        return {a: {} for a in arm_names}, "incomplete"
    out = {a: {} for a in arm_names}
    for a in mem_arms:
        by_id = {r["id"]: r for r in _jsonl(os.path.join(mem_run_dir, "out", f"{a}.jsonl"))}
        for p in probes:
            res = mps.score_probe(p, mps._reply_of(by_id.get(p["id"])))
            out[a][p["id"]] = {"category": p["probe"]["category"], "hit": bool(res["hit"])}
    return out, "measured"


def load_all(run_dir, harness_dir, mem_run_dir=None, probes=None, memory_dir=None):
    rf = _import_from(harness_dir, "replay_feed")
    turns = _jsonl(os.path.join(run_dir, "turns.jsonl"))
    by_id = {t["id"]: t for t in turns}
    out_dir = os.path.join(run_dir, "out")
    names = sorted(f[:-6] for f in os.listdir(out_dir) if f.endswith(".jsonl"))
    rows = {a: _jsonl(os.path.join(out_dir, f"{a}.jsonl")) for a in names}
    complete, man = manifest_complete(run_dir, names)
    order = [x["arm"] for x in man.get("arms", []) if x["arm"] in rows]
    names = order + [a for a in names if a not in order]
    sets = {frozenset(r["id"] for r in rows[a]) for a in names}
    bad = any(r["action"] == "error" or r["id"] not in by_id for a in names for r in rows[a])
    complete = complete and len(sets) == 1 and not bad and "A" in rows and "B" in rows
    memory, mstatus = load_memory(mem_run_dir, probes, memory_dir, names)
    arms = {}
    for a in names:
        arms[a] = {"turns": {r["id"]: feature(rf, r["action"], r.get("bubbles"),
                                              by_id[r["id"]]["inbound_bubbles"])
                             for r in rows[a] if r["id"] in by_id},
                   "judge": load_judge(os.path.join(run_dir, "feed", a)),
                   "memory": memory.get(a, {})}
    seth = {"turns": {t["id"]: feature(rf, t.get("seth_action", "text"),
                                       t.get("seth_reply_bubbles"), t["inbound_bubbles"])
                      for t in turns}}
    return arms, seth, rows, complete, mstatus, man


# ── rendering ───────────────────────────────────────────────────────────

def fmt_ci(d, digits=3):
    if not d or d.get("point") is None:
        return "-"
    p = f"{d['point']:.{digits}f}"
    if d.get("lo") is None:
        return p + " [n/a]"
    return f"{p} [{d['lo']:.{digits}f}, {d['hi']:.{digits}f}]"


def fmt_pt(v, digits=3):
    return "-" if v is None else f"{v:.{digits}f}"


def plist_commands(plist, gates, prior, label=None, run_name="cutover"):
    """(promote, rollback) shell lines for `gates`. prior: {gate: value or None}
    as the plist had them when the run started. Nothing here runs anything."""
    if not gates:
        return [], []
    label = label or os.path.basename(plist).removesuffix(".plist")
    pb = "/usr/libexec/PlistBuddy"
    q = f'"{plist}"'
    reload = [f"launchctl bootout gui/$(id -u)/{label}",
              f"launchctl bootstrap gui/$(id -u) {q}"]
    promote = [f'cp {q} "{plist}.pre-{run_name}"']
    for g in gates:
        promote.append(f'{pb} -c "Set :EnvironmentVariables:{g} live" {q} 2>/dev/null || '
                       f'{pb} -c "Add :EnvironmentVariables:{g} string live" {q}')
    rollback = []
    for g in gates:
        if prior.get(g) is None:
            rollback.append(f'{pb} -c "Delete :EnvironmentVariables:{g}" {q}')
        else:
            rollback.append(f'{pb} -c "Set :EnvironmentVariables:{g} {prior[g]}" {q}')
    return promote + reload, rollback + reload


def render(res, *, run_name, gates_present, gates_absent, plist, prior, judge_model,
           probes_n, label=None):
    L = []
    th = res["thresholds"]
    L.append(f"# Cut-over report: {run_name}")
    L.append("")
    L.append(f"Generated {datetime.datetime.now().strftime('%Y-%m-%d %H:%M')}. "
             f"{res['turn_ids']} held-out turns, {probes_n} memory probes, "
             f"{len(res['arms'])} arms. Judge: `{judge_model}` (local, cross-family).")
    L.append("")
    L.append(f"- Gates under test (present in the binary): {', '.join(gates_present) or 'none'}")
    if gates_absent:
        L.append(f"- Not in the binary, skipped: {', '.join(gates_absent)}")
    L.append(f"- Complete measurement (R5): {'yes' if res['complete'] else '**NO**'}")
    L.append(f"- Memory probes: {res['memory_status']}")
    if not th["preregistered"]:
        L.append(f"- **THRESHOLDS OVERRIDDEN** (boot={th['boot']}, min_judged={th['min_judged']}, "
                 f"min_text={th['min_text']}): these are NOT pre-registered verdicts.")
    L.append("")
    verdict = "GO" if res["go"] else "NO-GO"
    L.append(f"## Verdict: {verdict}")
    L.append("")
    if res["go"]:
        L.append(f"Promote: {', '.join(res['promote'])}.")
    if res["stack"]["failed"]:
        L.append(f"Stack guard failed (R4): {', '.join(res['stack']['failed'])}.")
    L.append("")
    L.append("| gate | verdict | reasons |")
    L.append("|---|---|---|")
    for g, v in res["gates"].items():
        L.append(f"| {g} | **{v['verdict']}** | {'; '.join(v['reasons'])} |")
    L.append("")
    L.append("## Per arm (95% bootstrap CI)")
    L.append("")
    L.append("Detection: judge picks Seth's real reply (0.50 = indistinguishable, lower is "
             "better). KS D: reply lengths vs Seth's (lower is better).")
    L.append("")
    L.append("| arm | judge detection (n) | length KS D | median len ratio | fragment rate | "
             "deflection rate (n) | tapback share | question rate | memory acc |")
    L.append("|---|---|---|---|---|---|---|---|---|")
    s = res["seth"]
    L.append(f"| seth | - | - | 1 (median {fmt_pt(s['len_median'], 0)} chars) | "
             f"{fmt_pt(s['fragment'])} | {fmt_pt(s['deflection'])} | {fmt_pt(s['tapback'])} | "
             f"{fmt_pt(s['question'])} | - |")
    for an, r in res["arms"].items():
        L.append(f"| {an} | {fmt_ci(r['detect'])} ({r['detect']['n']}) | {fmt_ci(r['ks'])} | "
                 f"{fmt_ci(r['median_ratio'], 2)} | {fmt_ci(r['fragment'])} | "
                 f"{fmt_ci(r['deflection'])} ({r['deflection_n']}) | {fmt_ci(r['tapback'])} | "
                 f"{fmt_ci(r['question'])} | {fmt_ci(r['memory']) if r['memory'] else '-'} |")
    L.append("")
    cats = sorted({c for r in res["arms"].values() for c in r["memory_by_category"]})
    if cats:
        L.append("### Memory-probe accuracy by category")
        L.append("")
        L.append("| arm | " + " | ".join(cats) + " |")
        L.append("|---|" + "---|" * len(cats))
        for an, r in res["arms"].items():
            if r["memory_by_category"]:
                cells = [f"{r['memory_by_category'][c]['acc']:.2f} "
                         f"(n={r['memory_by_category'][c]['n']})"
                         if c in r["memory_by_category"] else "-" for c in cats]
                L.append(f"| {an} | " + " | ".join(cells) + " |")
        L.append("")
    L.append("## Ablation deltas (paired, 95% CI)")
    L.append("")
    L.append("Positive dDetect / dKS = the gate helps. Positive dFrag / dDefl = the gate hurts.")
    L.append("")
    L.append("| gate | ablation arm | inert | dDetect (units) | dKS | dFrag | dDefl |")
    L.append("|---|---|---|---|---|---|---|")
    st = res["stack"]
    L.append(f"| whole stack (A vs B) | A | - | {fmt_ci(st['d_detect'])} "
             f"({(st['d_detect'] or {}).get('units', 0)}) | {fmt_ci(st['d_ks'])} | "
             f"{fmt_ci(st['d_frag'])} | {fmt_ci(st['d_defl'])} |")
    for g, v in res["gates"].items():
        inert = "-" if v["inert"] is None else ("**yes**" if v["inert"] else "no")
        L.append(f"| {g} | {v['arm']} | {inert} | {fmt_ci(v['d_detect'])} "
                 f"({(v['d_detect'] or {}).get('units', 0)}) | {fmt_ci(v['d_ks'])} | "
                 f"{fmt_ci(v['d_frag'])} | {fmt_ci(v['d_defl'])} |")
    if st.get("d_mem"):
        L.append("")
        L.append(f"Memory accuracy, A minus B (positive = the stack loses recall): "
                 f"{fmt_ci(st['d_mem'])}.")
    L.append("")
    L.append("## Commands (printed, NOT executed)")
    L.append("")
    promote, rollback = plist_commands(plist, res["promote"], prior, label, run_name)
    if not promote:
        L.append("No gate is PROMOTE, so there is nothing to set live.")
    else:
        L.append("Set the PROMOTE gates live (the installer regenerates this plist on "
                 "`human service install`; re-apply after a reinstall):")
        L.append("")
        L.append("```bash")
        L.extend(promote)
        L.append("```")
        L.append("")
        L.append("Rollback (restores each gate to its value when this run started):")
        L.append("")
        L.append("```bash")
        L.extend(rollback)
        L.append("```")
    L.append("")
    L.append("Rules: the pre-registered R1-R5 in `scripts/cutover/cutover_report.py`'s header.")
    return "\n".join(L) + "\n"


def _write_private(path, text):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(text)


def _strip(obj):
    """report.json: aggregates only."""
    return json.loads(json.dumps(obj, default=str))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--run-dir", required=True)
    ap.add_argument("--harness-dir", required=True, help="dir holding replay_feed.py")
    ap.add_argument("--mem-run-dir", default=None)
    ap.add_argument("--probes", default=None, help="the memory run's turns.jsonl (probe records)")
    ap.add_argument("--memory-dir", default=None, help="dir holding memory_probe_score.py")
    ap.add_argument("--plist", default="~/Library/LaunchAgents/ai.human.service-loop.plist")
    ap.add_argument("--gates-absent", default="")
    ap.add_argument("--judge-model", default="gemma4-26b-mmap")
    ap.add_argument("--out", default=None, help="default <run-dir>/report.md")
    # Overrides exist for the dry run's tiny fixture; a report using them says so.
    ap.add_argument("--boot", type=int, default=BOOT)
    ap.add_argument("--min-judged", type=int, default=MIN_JUDGED)
    ap.add_argument("--min-text", type=int, default=MIN_TEXT)
    a = ap.parse_args(argv)
    run_dir = os.path.expanduser(a.run_dir)
    if a.mem_run_dir and not (a.probes and a.memory_dir):
        print("refusing: --mem-run-dir needs --probes and --memory-dir", file=sys.stderr)
        return 2
    try:
        arms, seth, rows, complete, mstatus, man = load_all(
            run_dir, a.harness_dir, a.mem_run_dir and os.path.expanduser(a.mem_run_dir),
            a.probes, a.memory_dir)
    except (OSError, ValueError, KeyError, ImportError) as e:
        print(f"refusing: cannot load the run ({type(e).__name__}: {e})", file=sys.stderr)
        return 2
    res = evaluate(arms, seth, rows, complete=complete, memory_status=mstatus, boot=a.boot,
                   min_judged=a.min_judged, min_text=a.min_text)
    base_env = man.get("base_env") or {}
    gates_present = gates_in(arms)
    prior = {g: base_env.get(g) for g in gates_present}
    probes_n = len(_jsonl(a.probes)) if a.probes and os.path.isfile(a.probes) else 0
    plist = os.path.expanduser(a.plist)
    text = render(res, run_name=os.path.basename(run_dir.rstrip("/")),
                  gates_present=gates_present,
                  gates_absent=[g for g in a.gates_absent.split(",") if g],
                  plist=plist, prior=prior, judge_model=a.judge_model, probes_n=probes_n)
    out = a.out or os.path.join(run_dir, "report.md")
    _write_private(out, text)
    _write_private(os.path.join(os.path.dirname(out), "report.json"),
                   json.dumps(_strip(res), indent=2))
    print(text)
    print(f"report: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
