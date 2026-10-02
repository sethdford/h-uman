"""Hermetic tests for scripts/cutover/cutover_report.py: the statistics, the
pre-registered decision rules R1-R5, the PlistBuddy command text, and the
report rendering. Synthetic arm outputs only; no harness, no network, no
~/.human, no real data."""
import csv
import json
import os
import sys
import types

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.join(HERE, "..", "scripts", "cutover")
sys.path.insert(0, KIT)
import cutover_report as cr  # noqa: E402

BOOT = 400  # fast; the rules are tested with deltas far outside the noise


# ── synthetic arms ──────────────────────────────────────────────────────

def turn(action="text", length=10, fragment=False, question=False, deflection=None):
    if action != "text":
        return {"action": action, "len": None, "fragment": None, "question": None,
                "deflection": None}
    return {"action": "text", "len": length, "fragment": fragment, "question": question,
            "deflection": deflection}


def make_arm(n=60, *, detect_rate=0.5, frag_rate=0.0, lengths=None, defl_rate=0.0,
             memory_rate=None):
    turns, judge, memory = {}, {}, {}
    for k in range(n):
        tid = f"t{k:04d}"
        L = lengths[k] if lengths else 10
        turns[tid] = turn(length=L, fragment=k < round(frag_rate * n),
                          deflection=(k < round(defl_rate * 20)) if k < 20 else None)
        judge[tid] = k < round(detect_rate * n)
    if memory_rate is not None:
        for k in range(50):
            memory[f"p{k:03d}"] = {"category": ["single_hop", "temporal"][k % 2],
                                   "hit": k < round(memory_rate * 50)}
    return {"turns": turns, "judge": judge, "memory": memory}


def seth_arm(n=60, lengths=None):
    return {"turns": {f"t{k:04d}": turn(length=(lengths[k] if lengths else 10))
                      for k in range(n)}}


def rows_for(arm_name, n=60, fp=lambda k: f"fp{k}"):
    return [{"id": f"t{k:04d}", "action": "text", "bubbles": [f"r{k}"], "reply_fp": fp(k)}
            for k in range(n)]


SETH_LENS = [5 + (k % 10) for k in range(60)]        # 5..14
FAR_LENS = [80 + (k % 10) for k in range(60)]        # 80..89: far from Seth


def scenario():
    """B with three gates:
      HU_THREAD_CONTEXT  removing it raises detection 0.45 -> 0.90   => PROMOTE (R1 judge)
      HU_LENGTH_POLICY   ablation sends identical requests           => HOLD (R3 inert)
      HU_DIRECTOR_V2     removing it pushes lengths far from Seth's
                         (R1 via KS) but B has 30% fragments vs 0%   => HOLD (R2)
    """
    arms = {
        "A": make_arm(detect_rate=0.9, lengths=FAR_LENS, frag_rate=0.3, memory_rate=0.4),
        "B": make_arm(detect_rate=0.45, lengths=SETH_LENS, frag_rate=0.3, memory_rate=0.4),
        "B-no-thread_context": make_arm(detect_rate=0.9, lengths=SETH_LENS, frag_rate=0.3),
        "B-no-length_policy": make_arm(detect_rate=0.45, lengths=SETH_LENS, frag_rate=0.3),
        "B-no-director_v2": make_arm(detect_rate=0.45, lengths=FAR_LENS, frag_rate=0.0),
    }
    rows = {a: rows_for(a) for a in arms}
    rows["B-no-length_policy"] = rows_for("x")  # same fp/action/bubbles as B
    rows["B-no-thread_context"] = rows_for("x", fp=lambda k: f"other{k}")
    rows["B-no-director_v2"] = rows_for("x", fp=lambda k: f"dir{k}")
    return arms, seth_arm(lengths=SETH_LENS), rows


# ── statistics ──────────────────────────────────────────────────────────

def test_ks_d_identical_is_zero_and_disjoint_is_one():
    assert cr.ks_d([1, 2, 3], [1, 2, 3]) == 0.0
    assert cr.ks_d([1, 2, 3], [10, 11]) == 1.0
    assert cr.ks_d([], [1]) is None


def test_ks_d_matches_hand_computation():
    # F_x = 1/2, 1 at v = 1, 2; F_y = 0, 1/3 there. Max gap at v=2: |1 - 1/3| = 2/3
    assert cr.ks_d([1, 2], [2, 3, 4]) == pytest.approx(2 / 3)


def test_bootstrap_is_deterministic_and_brackets_the_point():
    units = list(range(60))

    def fn(s):
        return sum(1 for u in s if u < 30) / len(s)

    a = cr.bootstrap(units, fn, "t", boot=BOOT)
    b = cr.bootstrap(units, fn, "t", boot=BOOT)
    assert a == b
    assert a["point"] == 0.5 and a["lo"] < 0.5 < a["hi"]
    assert 0.3 < a["lo"] and a["hi"] < 0.7


def test_bootstrap_on_no_units_is_absent_not_zero():
    d = cr.bootstrap([], lambda s: 0.0, "t", boot=BOOT)
    assert d["point"] is None and d["lo"] is None and d["n"] == 0


def test_paired_delta_cancels_shared_noise():
    units = list(range(60))
    x = {u: u % 3 == 0 for u in units}

    def fx(s):
        return sum(x[u] for u in s) / len(s)

    d = cr.paired_delta(units, fx, fx, "same", boot=BOOT)
    assert d["point"] == 0.0 and d["lo"] == 0.0 and d["hi"] == 0.0


# ── the rules, one at a time ────────────────────────────────────────────

POS = {"point": 0.4, "lo": 0.2, "hi": 0.6}
NULL = {"point": 0.01, "lo": -0.1, "hi": 0.12}
HARM_CI = {"point": 0.05, "lo": 0.01, "hi": 0.09}
HARM_POINT = {"point": 0.12, "lo": -0.02, "hi": 0.3}


def verdict(**kw):
    base = dict(complete=True, stack_failures=[], inert=False, d_detect=NULL, d_ks=NULL,
                d_frag=NULL, d_defl=NULL, judged_n=50, text_n=50)
    base.update(kw)
    return cr.gate_verdict("HU_X", **base)


def test_r1_judge_benefit_promotes():
    assert verdict(d_detect=POS)[0] == cr.PROMOTE


def test_r1_ks_benefit_promotes():
    assert verdict(d_ks=POS)[0] == cr.PROMOTE


def test_r1_no_benefit_holds():
    v, why = verdict()
    assert v == cr.HOLD and any("no benefit" in r for r in why)


def test_r1_judge_benefit_needs_min_judged():
    v, why = verdict(d_detect=POS, judged_n=cr.MIN_JUDGED - 1)
    assert v == cr.HOLD and any("judge n=" in r for r in why)


def test_r1_ks_benefit_needs_min_text():
    assert verdict(d_ks=POS, text_n=cr.MIN_TEXT - 1)[0] == cr.HOLD


def test_r2_fragment_harm_by_ci_holds_despite_benefit():
    v, why = verdict(d_detect=POS, d_frag=HARM_CI)
    assert v == cr.HOLD and "R2: worse on fragment" in why


def test_r2_deflection_harm_by_point_tolerance_holds():
    v, why = verdict(d_detect=POS, d_defl=HARM_POINT)
    assert v == cr.HOLD and "R2: worse on deflection" in why


def test_r2_point_just_under_tolerance_is_noise():
    under = {"point": cr.HARM_POINT_TOLERANCE - 0.01, "lo": -0.05, "hi": 0.2}
    assert verdict(d_detect=POS, d_frag=under)[0] == cr.PROMOTE


def test_r3_inert_holds_even_with_benefit():
    v, why = verdict(d_detect=POS, inert=True)
    assert v == cr.HOLD and why[0].startswith("R3")


def test_r4_stack_failure_holds_every_gate():
    v, why = verdict(d_detect=POS, stack_failures=["memory accuracy"])
    assert v == cr.HOLD and why[0].startswith("R4")


def test_r5_incomplete_holds_every_gate():
    v, why = verdict(d_detect=POS, complete=False)
    assert v == cr.HOLD and why[0].startswith("R5")


def test_missing_ablation_arm_holds():
    assert verdict(d_detect=POS, inert=None)[0] == cr.HOLD


def test_stack_guards():
    assert cr.stack_guards(NULL, NULL, NULL, "measured") == []
    assert cr.stack_guards(NULL, NULL, HARM_CI, "measured") == ["memory accuracy"]
    assert cr.stack_guards(NULL, NULL, None, "skipped") == []
    assert cr.stack_guards(NULL, NULL, None, "incomplete") == ["memory probes incomplete"]
    assert cr.stack_guards(HARM_POINT, NULL, None, "skipped") == ["fragment"]


def test_is_inert_compares_request_action_and_bubbles():
    b = rows_for("b")
    assert cr.is_inert(rows_for("x"), b) is True
    changed = rows_for("x")
    changed[3]["bubbles"] = ["different"]
    assert cr.is_inert(changed, b) is False
    assert cr.is_inert([], b) is None


# ── the whole decision on synthetic arms ────────────────────────────────

def test_evaluate_scenario_promotes_exactly_the_helping_gate():
    arms, seth, rows = scenario()
    res = cr.evaluate(arms, seth, rows, complete=True, memory_status="measured", boot=BOOT)
    g = res["gates"]
    assert g["HU_THREAD_CONTEXT"]["verdict"] == cr.PROMOTE
    assert g["HU_LENGTH_POLICY"]["verdict"] == cr.HOLD and g["HU_LENGTH_POLICY"]["inert"]
    assert g["HU_DIRECTOR_V2"]["verdict"] == cr.HOLD
    assert "R1: lengths closer to Seth (KS)" in g["HU_DIRECTOR_V2"]["reasons"]
    assert res["promote"] == ["HU_THREAD_CONTEXT"] and res["go"] is True
    # the per-arm numbers are the synthetic truth
    assert res["arms"]["B"]["detect"]["point"] == pytest.approx(27 / 60)
    assert res["arms"]["B-no-thread_context"]["detect"]["point"] == pytest.approx(54 / 60)
    assert res["arms"]["B"]["ks"]["point"] == 0.0
    assert res["arms"]["A"]["ks"]["point"] == 1.0
    assert res["arms"]["A"]["memory"]["point"] == pytest.approx(0.4)


def test_evaluate_director_gate_promotes_once_its_fragments_are_removed():
    """Mutation check on R2: the same scenario with B's fragments removed
    promotes HU_DIRECTOR_V2, so its HOLD above is caused by the fragment rule."""
    arms, seth, rows = scenario()
    arms["B"] = make_arm(detect_rate=0.45, lengths=SETH_LENS, frag_rate=0.0, memory_rate=0.4)
    res = cr.evaluate(arms, seth, rows, complete=True, memory_status="measured", boot=BOOT)
    assert res["gates"]["HU_DIRECTOR_V2"]["verdict"] == cr.PROMOTE
    assert res["promote"] == ["HU_THREAD_CONTEXT", "HU_DIRECTOR_V2"]


def test_evaluate_memory_drop_blocks_the_whole_stack():
    arms, seth, rows = scenario()
    arms["B"]["memory"] = make_arm(memory_rate=0.1)["memory"]  # A has 0.4
    res = cr.evaluate(arms, seth, rows, complete=True, memory_status="measured", boot=BOOT)
    assert res["stack"]["failed"] == ["memory accuracy"]
    assert res["promote"] == [] and res["go"] is False


def test_evaluate_incomplete_run_promotes_nothing():
    arms, seth, rows = scenario()
    res = cr.evaluate(arms, seth, rows, complete=False, memory_status="measured", boot=BOOT)
    assert res["promote"] == [] and all(v["verdict"] == cr.HOLD for v in res["gates"].values())


def test_thresholds_are_flagged_when_overridden():
    arms, seth, rows = scenario()
    res = cr.evaluate(arms, seth, rows, complete=True, memory_status="skipped", boot=BOOT)
    assert res["thresholds"]["preregistered"] is False
    res = cr.evaluate(arms, seth, rows, complete=True, memory_status="skipped",
                      boot=cr.BOOT, min_judged=cr.MIN_JUDGED, min_text=cr.MIN_TEXT)
    assert res["thresholds"]["preregistered"] is True


# ── commands and report ─────────────────────────────────────────────────

PLIST = "/Users/x/Library/LaunchAgents/ai.human.service-loop.plist"


def test_plist_commands_promote_and_restore_prior_values():
    promote, rollback = cr.plist_commands(
        PLIST, ["HU_THREAD_CONTEXT", "HU_LENGTH_POLICY"],
        {"HU_THREAD_CONTEXT": None, "HU_LENGTH_POLICY": "shadow"}, run_name="cutover-x")
    text = "\n".join(promote)
    assert f'cp "{PLIST}" "{PLIST}.pre-cutover-x"' in text
    assert ('Set :EnvironmentVariables:HU_THREAD_CONTEXT live" "' + PLIST) in text
    assert "Add :EnvironmentVariables:HU_THREAD_CONTEXT string live" in text
    assert "launchctl bootout gui/$(id -u)/ai.human.service-loop" in text
    rb = "\n".join(rollback)
    assert "Delete :EnvironmentVariables:HU_THREAD_CONTEXT" in rb  # absent before
    assert "Set :EnvironmentVariables:HU_LENGTH_POLICY shadow" in rb  # restored
    assert f'launchctl bootstrap gui/$(id -u) "{PLIST}"' in rb


def test_plist_commands_empty_without_promotions():
    assert cr.plist_commands(PLIST, [], {}) == ([], [])


def test_render_prints_commands_only_for_promoted_gates():
    arms, seth, rows = scenario()
    res = cr.evaluate(arms, seth, rows, complete=True, memory_status="measured", boot=BOOT)
    md = cr.render(res, run_name="cutover-x", gates_present=list(res["gates"]),
                   gates_absent=["HU_IMMERSIVE_CONTEXT"], plist=PLIST,
                   prior={}, judge_model="gemma4-26b-mmap", probes_n=50)
    assert "## Verdict: GO" in md
    assert "| HU_THREAD_CONTEXT | **PROMOTE** |" in md
    assert "HU_THREAD_CONTEXT string live" in md
    assert "HU_DIRECTOR_V2 string live" not in md
    assert "NOT pre-registered" in md  # BOOT override
    assert "HU_IMMERSIVE_CONTEXT" in md


# ── loading ─────────────────────────────────────────────────────────────

class FakeHeuristics(types.SimpleNamespace):
    """Stand-in for replay_feed's heuristics: 'frag' marks a fragment, a
    reply 'idk' to a question is a deflection."""

    @staticmethod
    def joined(b):
        return "\n".join(x for x in (b or []) if x)

    @staticmethod
    def is_fragment(b):
        return b.endswith("frag")

    @staticmethod
    def has_question(r):
        return "?" in r

    @staticmethod
    def asked_question(i):
        return "?" in (i or "")

    @staticmethod
    def is_deflection(i, r):
        return r == "idk"


def test_feature_uses_the_heuristics_only_for_text_replies():
    rf = FakeHeuristics()
    f = cr.feature(rf, "text", ["hey", "so frag"], ["you up?"])
    assert f == {"action": "text", "len": len("hey\nso frag"), "fragment": True,
                 "question": False, "deflection": False}
    assert cr.feature(rf, "text", ["idk"], ["you up?"])["deflection"] is True
    assert cr.feature(rf, "text", ["idk"], ["ok"])["deflection"] is None  # not asked
    t = cr.feature(rf, "tapback", [], ["you up?"])
    assert t["len"] is None and t["fragment"] is None


def test_load_judge_counts_seth_picks_and_skips_parse_failures(tmp_path):
    (tmp_path / "answer_key.json").write_text(json.dumps({"t1": "A", "t2": "B", "t3": "A"}))
    with open(tmp_path / "judged.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["id", "choice"])
        w.writeheader()
        w.writerows([{"id": "t1", "choice": "A"}, {"id": "t2", "choice": "A"},
                     {"id": "t3", "choice": ""}])
    assert cr.load_judge(str(tmp_path)) == {"t1": True, "t2": False}
    assert cr.load_judge(str(tmp_path / "missing")) == {}


def test_manifest_complete(tmp_path):
    assert cr.manifest_complete(str(tmp_path), ["A"])[0] is False
    (tmp_path / "manifest.json").write_text(json.dumps(
        {"arms": [{"arm": "A", "complete": True}, {"arm": "B", "complete": False}]}))
    assert cr.manifest_complete(str(tmp_path), ["A"])[0] is True
    assert cr.manifest_complete(str(tmp_path), ["A", "B"])[0] is False


# ── safety: the kit never writes the gate file or runs the commands ─────

def _code_strings(path):
    """String constants in a Python file's code, docstrings excluded."""
    import ast
    tree = ast.parse(open(path).read())
    docs = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.FunctionDef, ast.ClassDef)) and node.body:
            first = node.body[0]
            if isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant):
                docs.add(id(first.value))
    return [n.value for n in ast.walk(tree)
            if isinstance(n, ast.Constant) and isinstance(n.value, str) and id(n) not in docs]


def _shell_code(path):
    with open(path) as f:
        return [ln for ln in f if not ln.lstrip().startswith("#")]


def test_kit_never_emits_a_gate_or_scores_with_a_rater():
    for fn in sorted(os.listdir(KIT)):
        p = os.path.join(KIT, fn)
        if fn.endswith(".py"):
            code = _code_strings(p)
        elif fn.endswith(".sh"):
            code = _shell_code(p)
        else:
            continue
        for s in code:
            assert "--emit-gate" not in s and "--rater" not in s, (fn, s)
            assert "score.py" not in s.replace("memory_probe_score", ""), (fn, s)
            assert "blind_ab_gate.json" not in s or fn == "run_cutover.sh", (fn, s)


def test_run_script_never_executes_plistbuddy_or_launchctl():
    import re
    invoke = re.compile(r"(^|[;&|(`]|\$\(|\bthen|\bdo)\s*(sudo\s+)?"
                        r"(/usr/libexec/PlistBuddy|PlistBuddy|launchctl|/bin/launchctl)\b")
    for ln in _shell_code(os.path.join(KIT, "run_cutover.sh")):
        assert not invoke.search(ln.strip()), ln
    assert invoke.search("/usr/libexec/PlistBuddy -c x p")  # the pattern bites
    assert invoke.search('x=1; launchctl bootout gui/1/y')
