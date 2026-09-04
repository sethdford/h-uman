import os, sys, importlib.util
spec = importlib.util.spec_from_file_location("tl", os.path.join(os.path.dirname(os.path.abspath(__file__)), "training_loop.py"))
tl = importlib.util.module_from_spec(spec); spec.loader.exec_module(tl)

def test_success_requires_rc0_and_adapter():
    assert tl.training_outcome_rc(0, True) == 0

def test_refusal_or_failure_never_reports_success():
    assert tl.training_outcome_rc(3, False) == 3     # preflight refusal
    assert tl.training_outcome_rc(0, False) == 3     # 'rc=0 but no adapter' — the 2026-09-02 case
    assert tl.training_outcome_rc(1, True) == 3      # trainer failed but left a file

def test_no_placeholder_string_remains():
    src = open(tl.__file__).read()
    assert "Falling back to empty-tensors" not in src
