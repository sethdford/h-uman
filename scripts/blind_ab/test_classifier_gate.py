import csv, json, os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import classifier_gate as cg

def mk_cycle(d, n):
    with open(os.path.join(d, "rating_sheet.csv"), "w", newline="") as f:
        w = csv.writer(f); w.writerow(["id", "context", "option_A", "option_B", "choice"])
        for i in range(n): w.writerow([f"t{i}", f"ctx{i}", f"real{i}" if i % 2 == 0 else f"ai{i}", f"ai{i}" if i % 2 == 0 else f"real{i}", ""])
    json.dump({f"t{i}": ("A" if i % 2 == 0 else "B") for i in range(n)}, open(os.path.join(d, "answer_key.json"), "w"))

def test_builder_puts_real_on_the_keyed_side():
    d = tempfile.mkdtemp(); mk_cycle(d, 4)
    t = cg.build_trials(d)
    assert [x["real_seth"] for x in t] == ["real0", "real1", "real2", "real3"]
    assert [x["ai_response"] for x in t] == ["ai0", "ai1", "ai2", "ai3"]

def test_refuses_below_min_n_and_writes_nothing():
    d = tempfile.mkdtemp(); mk_cycle(d, 5)
    r = subprocess.run([sys.executable, os.path.join(HERE, "classifier_gate.py"), "--cycle-dir", d, "--trials-only"], capture_output=True, text=True)
    assert r.returncode != 0 and "REFUSING" in r.stderr
    assert not os.path.exists(os.path.join(d, "classifier_trials.json"))

def test_extract_auc_picks_best_oriented():
    assert cg.extract_auc({"scores": {"a": {"auc_oriented": 0.6}, "b": {"auc_oriented": 0.8}}}) == ("b", 0.8)
    assert cg.extract_auc({}) == (None, None)
