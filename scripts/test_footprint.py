#!/usr/bin/env python3
"""Tests for scripts/footprint.py: every guard is shown to FAIL on the input it
exists to catch, not just to pass on a clean tree. Run: python3 scripts/test_footprint.py"""
import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import footprint as fp  # noqa: E402

BUILD = {
    "name": "release-size", "binary_bytes": 2_827_152, "text_section_bytes": 1_961_564,
    "startup_ms_samples": [3.64, 4.0, 4.21, 4.5, 5.3], "startup_ms_median": 4.21,
    "version_peak_rss_bytes": 6_930_432, "idle_rss_bytes": 8_585_216,
    "git_rev": "aaaaaaaaa", "code_rev": "bbbbbbbbb", "measured_at": "2026-09-29T00:10:09+00:00",
    "platform": "macOS arm64 26.6.2", "host": "local",
}
BUDGET = {
    "primary_build": "release-size", "full_build": "release-preset",
    "budgets": {
        "binary_bytes": {"build": "release-size", "max": 2_867_200, "why": "w"},
        "version_peak_rss_bytes": {"build": "release-size", "max": 8_000_000, "why": "w"},
        "startup_ms_median": {"build": "release-size", "max": 100, "why": "w"},
    },
    "heal": {"approx_tolerance": 0.05},
}


def M(k):
    return f"<!-- fp:{k} --><!-- /fp -->"


_TMPDIRS = []


def tearDownModule():
    for d in _TMPDIRS:
        d.cleanup()


class Repo:
    """A throwaway git repo shaped like h-uman, with footprint files in it."""

    def __init__(self, build=BUILD, files=None):
        self.tmp = tempfile.TemporaryDirectory()
        _TMPDIRS.append(self.tmp)
        self.root = Path(self.tmp.name)
        self.write("docs/perf/footprint.json", json.dumps({"builds": {build["name"]: build}}))
        self.write("docs/perf/footprint-budget.json", json.dumps(BUDGET))
        for rel, text in (files or {}).items():
            self.write(rel, text)
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.add()

    def write(self, rel, text):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)

    def read(self, rel):
        return (self.root / rel).read_text()

    def add(self):
        subprocess.run(["git", "-C", str(self.root), "add", "-A"], check=True)

    def run(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            rc = fp.main(["--root", str(self.root), *args])
        return rc, out.getvalue()

    def fresh(self, **over):
        p = self.root / "fresh.json"
        p.write_text(json.dumps({**BUILD, **over}))
        return str(p)


class Derive(unittest.TestCase):
    def claims(self, **over):
        return fp.derive({"builds": {"release-size": {**BUILD, **over}}}, BUDGET)

    def test_strings_follow_the_rounding_rules(self):
        c = self.claims()
        self.assertEqual(c["binary_kb"].text, "~2760 KB")          # floor(bytes/1024)
        self.assertEqual(c["binary_mb"].text, "~3 MB")             # SI, rounded
        self.assertEqual(c["idle_rss_mb"].text, "8.6 MB")
        self.assertEqual(c["idle_rss_bound"].text, "<9 MB")        # floor(MB)+1: always true
        self.assertEqual(c["startup_range"].text, "3–6 ms")        # floor(min)..ceil(max)
        self.assertEqual(c["startup_bound"].text, "<6 ms")
        self.assertEqual(c["measured_rev"].text, "bbbbbbbbb")      # code_rev survives a squash
        self.assertEqual(c["measured_platform"].text, "macOS arm64")
        self.assertEqual(c["budget_binary_kb"].text, "2800 KB")

    def test_a_bound_is_strict_even_on_a_whole_number(self):
        self.assertEqual(self.claims(idle_rss_bytes=9_000_000)["idle_rss_bound"].text, "<10 MB")

    def test_missing_primary_build_is_input_error_not_a_default(self):
        with self.assertRaises(fp.InputError):
            fp.derive({"builds": {}}, BUDGET)


class SyncAndCheck(unittest.TestCase):
    def test_sync_fills_markers_and_templates_then_check_passes(self):
        r = Repo(files={
            "README.md": f"Tiny: {M('binary_kb')} binary, {M('idle_rss_bound')} idle.\n",
            "src/a.c": '    // fp-template: "({{binary_mb}} binary)"\n    "(old)"\n',
        })
        self.assertEqual(r.run("check")[0], 1)                    # empty markers are stale
        self.assertEqual(r.run("sync")[0], 0)
        self.assertIn("<!-- fp:binary_kb -->~2760 KB<!-- /fp -->", r.read("README.md"))
        self.assertIn('\n    "(~3 MB binary)"\n', r.read("src/a.c"))  # indent of the rendered line kept
        rc, out = r.run("check")
        self.assertEqual(rc, 0, out)

    def test_hand_edited_marker_is_caught(self):
        r = Repo(files={"README.md": "<!-- fp:binary_kb -->~9999 KB<!-- /fp -->\n"})
        rc, out = r.run("check")
        self.assertEqual(rc, 1)
        self.assertIn("README.md: stale footprint claims", out)

    def test_hand_edited_template_line_is_caught(self):
        r = Repo(files={"src/a.c": '    // fp-template: "({{binary_mb}})"\n    "(~1 MB)"\n'})
        self.assertEqual(r.run("check")[0], 1)

    def test_unknown_key_is_refused_not_rendered(self):
        r = Repo(files={"README.md": "<!-- fp:no_such_key --><!-- /fp -->\n"})
        rc, out = r.run("sync")
        self.assertEqual(rc, 2)
        self.assertIn("unknown footprint key 'no_such_key'", out)

    def test_website_data_generated_only_where_the_site_exists(self):
        r = Repo()
        r.run("sync")
        self.assertFalse((r.root / fp.WEBSITE_DATA_REL).exists())
        r.write("website/src/data/.keep", "")
        r.run("sync")
        data = json.loads(r.read(fp.WEBSITE_DATA_REL))
        self.assertEqual(data["text"]["idle_rss_bound"], "<9 MB")
        self.assertEqual(data["measured"]["binary_mb"], 2.83)

    def test_committed_measurement_over_budget_fails_check(self):
        r = Repo(build={**BUILD, "binary_bytes": 3_000_000})
        rc, out = r.run("check")
        self.assertEqual(rc, 1)
        self.assertIn("exceeds the budget", out)


class Lint(unittest.TestCase):
    def check(self, files):
        r = Repo(files=files)
        r.run("sync")
        return r.run("check")

    def test_unmanaged_claim_in_a_live_doc_fails(self):
        rc, out = self.check({"docs/guide.md": "It is a ~1696 KB binary.\n"})
        self.assertEqual(rc, 1)
        self.assertIn("docs/guide.md:1", out)

    def test_bare_tilde_kb_needs_no_keyword(self):
        self.assertEqual(self.check({"docs/guide.md": "| Size | ~1696 KB |\n"})[0], 1)

    def test_rss_and_startup_claims_are_caught(self):
        self.assertEqual(self.check({"a.md": "Uses < 6 MB RAM.\n"})[0], 1)
        self.assertEqual(self.check({"a.md": "Cold start 4-27 ms.\n"})[0], 1)
        self.assertEqual(self.check({"a.md": "Boots in &lt;30 ms.\n"})[0], 1)

    def test_history_may_keep_old_numbers(self):
        self.assertEqual(self.check({"docs/plans/old.md": "It was a ~1696 KB binary.\n",
                                     "CHANGELOG.md": "398 KB binary, 5.1 MB peak RSS\n"})[0], 0)

    def test_fp_ignore_and_managed_lines_pass(self):
        rc, out = self.check({
            "a.md": "Budget: binary delta < 50 KB per change <!-- fp:ignore: policy -->\n"
                    f"| RAM | < 10 MB (competitor) | {M('idle_rss_bound')} |\n",
        })
        self.assertEqual(rc, 0, out)

    def test_code_only_checks_string_literals_about_the_binary(self):
        self.assertEqual(self.check({"src/b.c": '"Flash: 512 KB, RAM: 128 KB."\n'})[0], 0)
        self.assertEqual(self.check({"src/b.c": "/* the binary is ~3 MB */\n"})[0], 0)
        self.assertEqual(self.check({"src/b.c": '"runtime (~3 MB binary)"\n'})[0], 1)


class Evaluate(unittest.TestCase):
    def evaluate(self, **over):
        r = Repo()
        out_file = r.root / "gh_out"
        rc, out = r.run("evaluate", r.fresh(**over), "--github-output", str(out_file))
        return rc, out, (out_file.read_text() if out_file.exists() else "")

    def test_same_measurement_needs_nothing(self):
        rc, _, gh = self.evaluate()
        self.assertEqual((rc, gh), (0, "heal=false\nbreach=false\n"))

    def test_rounded_claim_further_than_tolerance_but_same_text_does_not_heal(self):
        # "~3 MB" is 6% from 2.83 MB; re-rendering it from the same data gives "~3 MB"
        # again, so it is not stale. This once healed on every run.
        rc, out, gh = self.evaluate(binary_bytes=BUILD["binary_bytes"] + 1)
        self.assertEqual(gh, "heal=false\nbreach=false\n", out)

    def test_small_drift_inside_tolerance_does_not_heal(self):
        rc, _, gh = self.evaluate(binary_bytes=2_860_000)       # +1.2%, still under budget
        self.assertEqual((rc, gh), (0, "heal=false\nbreach=false\n"))

    def test_drift_beyond_tolerance_heals_and_says_which_claim(self):
        rc, out, gh = self.evaluate(idle_rss_bytes=9_400_000)   # "<9 MB" is now false
        self.assertEqual(rc, 0)
        self.assertIn("heal=true", gh)
        self.assertIn("idle_rss_bound", out)

    def test_startup_outside_published_range_heals(self):
        rc, out, gh = self.evaluate(startup_ms_samples=[9.0, 9.5, 10.0, 10.5, 11.0])
        self.assertIn("heal=true", gh)
        self.assertIn("startup_range", out)

    def test_budget_breach_fails(self):
        rc, out, gh = self.evaluate(binary_bytes=2_900_000)
        self.assertEqual(rc, 1)
        self.assertIn("breach=true", gh)
        self.assertIn("binary_bytes", out)

    def test_incomplete_measurement_is_refused_without_a_verdict(self):
        rc, out, gh = self.evaluate(idle_rss_bytes=0)
        self.assertEqual(rc, 2)
        self.assertEqual(gh, "")                                 # no heal/breach written at all


class Adopt(unittest.TestCase):
    def test_adopt_records_the_build_keeps_others_and_regenerates(self):
        r = Repo(files={"README.md": f"{M('binary_kb')}\n"})
        measured = json.loads(r.read(fp.MEASURED_REL))
        measured["builds"]["release-preset"] = {**BUILD, "name": "release-preset", "binary_bytes": 3_327_168}
        r.write(fp.MEASURED_REL, json.dumps(measured))
        self.assertEqual(r.run("adopt", r.fresh(binary_bytes=2_700_000))[0], 0)
        after = json.loads(r.read(fp.MEASURED_REL))["builds"]
        self.assertEqual(after["release-size"]["binary_bytes"], 2_700_000)
        self.assertEqual(after["release-preset"]["binary_bytes"], 3_327_168)
        self.assertIn("~2636 KB", r.read("README.md"))

    def test_adopt_refuses_an_incomplete_measurement_and_writes_nothing(self):
        # load_fresh is the only guard in front of the write: anything that fails
        # later (derive, sync) fails after footprint.json was already overwritten.
        for bad in ({"startup_ms_samples": []}, {"idle_rss_bytes": 0}, {"git_rev": ""}):
            r = Repo()
            before = r.read(fp.MEASURED_REL)
            self.assertEqual(r.run("adopt", r.fresh(**bad))[0], 2, bad)
            self.assertEqual(r.read(fp.MEASURED_REL), before, bad)


if __name__ == "__main__":
    unittest.main(verbosity=1)
