#!/usr/bin/env python3
"""Hermetic tests for scripts/retrain/prune_adapters.py.

Everything happens inside a tempdir; nothing here reads or writes
$HOME/.human. unittest.TestCase so this runs stdlib-only via
`python3 scripts/retrain/test_prune_adapters.py` AND is pytest-discoverable.
"""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prune_adapters as P


def _touch(path, size=0, mtime=None):
    path = Path(path)
    path.mkdir(parents=True, exist_ok=True)
    f = path / "adapters.safetensors"
    f.write_bytes(b"x" * size)
    if mtime is not None:
        os.utime(path, (mtime, mtime))
        os.utime(f, (mtime, mtime))


class ClassifyKindTests(unittest.TestCase):
    def test_mlxtune_kind_strips_both_stamps(self):
        self.assertEqual(
            P.classify_kind("seth-glm-air-mlxtune-simpo-20260911-0313-20260911-031300"),
            "seth-glm-air-mlxtune-simpo",
        )

    def test_mlxtune_different_mode_is_a_different_kind(self):
        self.assertEqual(
            P.classify_kind("seth-glm-air-mlxtune-orpo-20260905-0856-20260905-085655"),
            "seth-glm-air-mlxtune-orpo",
        )

    def test_m3_outcomes_with_and_without_glm_are_separate_kinds(self):
        self.assertEqual(P.classify_kind("seth-m3-outcomes-20260904-212919-glm"),
                         "seth-m3-outcomes-glm")
        self.assertEqual(P.classify_kind("seth-m3-outcomes-20260904-212919"), "seth-m3-outcomes")
        self.assertNotEqual(P.classify_kind("seth-m3-outcomes-20260904-212919-glm"),
                            P.classify_kind("seth-m3-outcomes-20260904-212919"))

    def test_unknown_name_is_not_classified(self):
        self.assertIsNone(P.classify_kind("seth-lora-v6"))
        self.assertIsNone(P.classify_kind("dpo-20260802-040017"))
        self.assertIsNone(P.classify_kind("seth-m3-outcomes-20260904-212919.rejected-1234"))


class PlanTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.adapters = Path(self.tmp) / "adapters"
        self.adapters.mkdir()
        self.config = Path(self.tmp) / "config.json"
        self.registry = self.adapters / "registry.json"
        self.plist_dir = Path(self.tmp) / "LaunchAgents"
        self.plist_dir.mkdir()

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _make_m3_family(self, n, served_idx=None, kind_suffix=""):
        """n sequential seth-m3-outcomes dirs, oldest=1, newest=n."""
        for i in range(1, n + 1):
            name = f"seth-m3-outcomes-202609{i:02d}-030000{kind_suffix}"
            _touch(self.adapters / name, size=1024, mtime=1726000000 + i * 86400)

    def test_refuses_when_config_missing(self):
        with self.assertRaises(P.RefusedError):
            P.plan(self.adapters, 2, str(self.config), str(self.registry), str(self.plist_dir))

    def test_refuses_when_served_adapter_does_not_exist_on_disk(self):
        self.config.write_text(json.dumps({
            "personalization": {"lora_adapter_path": str(self.adapters / "nope")}
        }))
        with self.assertRaises(P.RefusedError):
            P.plan(self.adapters, 2, str(self.config), str(self.registry), str(self.plist_dir))

    def test_keeps_newest_n_per_kind_and_the_served_and_registered_ones(self):
        self._make_m3_family(5)
        served = self.adapters / "seth-m3-outcomes-20260901-030000"
        self.config.write_text(json.dumps({
            "personalization": {"lora_adapter_path": str(served)}
        }))
        self.registry.write_text(json.dumps({
            "adapters": {"seth-m3-outcomes-20260903-030000": {}}
        }))
        kept, to_delete, kinds = P.plan(self.adapters, 2, str(self.config), str(self.registry),
                                        str(self.plist_dir))
        # newest 2 (04, 05) + served (01) + registered (03) = 4 kept; 02 deleted.
        self.assertEqual(kinds, ["seth-m3-outcomes"])
        self.assertEqual(kept, {
            "seth-m3-outcomes-20260901-030000",
            "seth-m3-outcomes-20260903-030000",
            "seth-m3-outcomes-20260904-030000",
            "seth-m3-outcomes-20260905-030000",
        })
        self.assertEqual([n for n, _, _ in to_delete], ["seth-m3-outcomes-20260902-030000"])

    def test_glm_and_non_glm_are_independent_kinds_with_independent_keep_windows(self):
        self._make_m3_family(3)                       # non-glm: 01,02,03
        self._make_m3_family(3, kind_suffix="-glm")    # glm:     01,02,03
        served = self.adapters / "seth-m3-outcomes-20260901-030000"
        self.config.write_text(json.dumps({
            "personalization": {"lora_adapter_path": str(served)}
        }))
        kept, to_delete, kinds = P.plan(self.adapters, 2, str(self.config), str(self.registry),
                                        str(self.plist_dir))
        self.assertEqual(sorted(kinds), ["seth-m3-outcomes", "seth-m3-outcomes-glm"])
        # non-glm kind keeps newest 2 (02,03) + served (01) = 3, nothing drops. glm kind is a
        # SEPARATE kind the served (non-glm) adapter does not protect: it keeps only its own
        # newest 2 (02-glm,03-glm) and drops the oldest (01-glm) — proves the kinds don't share
        # a keep-window or a protection set.
        deleted_names = {n for n, _, _ in to_delete}
        self.assertEqual(deleted_names, {"seth-m3-outcomes-20260901-030000-glm"})
        # Tighten: add a 4th non-glm entry that is neither served nor in the newest-2 window.
        _touch(self.adapters / "seth-m3-outcomes-20260801-030000", size=1024, mtime=1700000000)
        kept2, to_delete2, _ = P.plan(self.adapters, 2, str(self.config), str(self.registry),
                                      str(self.plist_dir))
        self.assertIn("seth-m3-outcomes-20260801-030000", {n for n, _, _ in to_delete2})
        self.assertNotIn("seth-m3-outcomes-20260801-030000", kept2)

    def test_unknown_pattern_dirs_are_never_touched(self):
        _touch(self.adapters / "seth-lora-v6", size=1024)
        _touch(self.adapters / "dpo-20260802-040017", size=1024)
        served = self.adapters / "seth-lora-v6"
        self.config.write_text(json.dumps({
            "personalization": {"lora_adapter_path": str(served)}
        }))
        kept, to_delete, kinds = P.plan(self.adapters, 2, str(self.config), str(self.registry),
                                        str(self.plist_dir))
        self.assertEqual(kinds, [])
        self.assertEqual(to_delete, [])

    def test_plist_referenced_adapter_is_protected(self):
        self._make_m3_family(4)
        served = self.adapters / "seth-m3-outcomes-20260904-030000"
        self.config.write_text(json.dumps({
            "personalization": {"lora_adapter_path": str(served)}
        }))
        (self.plist_dir / "ai.human.something.plist").write_text(
            f"<plist><string>{self.adapters}/seth-m3-outcomes-20260901-030000</string></plist>"
        )
        kept, to_delete, _ = P.plan(self.adapters, 1, str(self.config), str(self.registry),
                                    str(self.plist_dir))
        self.assertIn("seth-m3-outcomes-20260901-030000", kept)

    def test_symlink_escape_is_never_classified_or_deleted(self):
        self._make_m3_family(1)
        served = self.adapters / "seth-m3-outcomes-20260901-030000"
        self.config.write_text(json.dumps({
            "personalization": {"lora_adapter_path": str(served)}
        }))
        outside = Path(self.tmp) / "outside-target"
        _touch(outside, size=999999)
        link = self.adapters / "seth-m3-outcomes-20260905-030000"
        os.symlink(outside, link)
        kept, to_delete, kinds = P.plan(self.adapters, 1, str(self.config), str(self.registry),
                                        str(self.plist_dir))
        self.assertNotIn("seth-m3-outcomes-20260905-030000", kept)
        self.assertEqual([n for n, _, _ in to_delete], [])
        self.assertTrue(outside.exists())  # never touched


class MainModeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.adapters = Path(self.tmp) / "adapters"
        self.adapters.mkdir()
        for i in range(1, 6):
            _touch(self.adapters / f"seth-m3-outcomes-202609{i:02d}-030000", size=4096,
                  mtime=1726000000 + i * 86400)
        self.served = self.adapters / "seth-m3-outcomes-20260901-030000"
        self.config = Path(self.tmp) / "config.json"
        self.config.write_text(json.dumps({
            "personalization": {"lora_adapter_path": str(self.served)}
        }))

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _argv(self, mode):
        return ["--adapters-dir", str(self.adapters), "--mode", mode, "--keep", "2",
               "--config", str(self.config), "--registry", str(self.adapters / "registry.json"),
               "--plist-dir", str(Path(self.tmp) / "nope")]

    def test_off_mode_touches_nothing_even_a_missing_config(self):
        before = sorted(p.name for p in self.adapters.iterdir())
        rc = P.main(["--mode", "off", "--config", str(Path(self.tmp) / "nonexistent.json")])
        self.assertEqual(rc, 0)
        self.assertEqual(sorted(p.name for p in self.adapters.iterdir()), before)

    def test_shadow_mode_deletes_nothing(self):
        before = sorted(p.name for p in self.adapters.iterdir())
        rc = P.main(self._argv("shadow"))
        self.assertEqual(rc, 0)
        self.assertEqual(sorted(p.name for p in self.adapters.iterdir()), before)

    def test_live_mode_deletes_exactly_what_shadow_would_have(self):
        kept, to_delete, _ = P.plan(self.adapters, 2, str(self.config),
                                    str(self.adapters / "registry.json"),
                                    str(Path(self.tmp) / "nope"))
        planned = {n for n, _, _ in to_delete}
        rc = P.main(self._argv("live"))
        self.assertEqual(rc, 0)
        remaining = {p.name for p in self.adapters.iterdir()}
        self.assertEqual(remaining, kept)
        for name in planned:
            self.assertNotIn(name, remaining)

    def test_refusal_exits_nonzero_and_deletes_nothing(self):
        before = sorted(p.name for p in self.adapters.iterdir())
        rc = P.main(["--adapters-dir", str(self.adapters), "--mode", "live",
                    "--config", str(Path(self.tmp) / "nonexistent.json")])
        self.assertEqual(rc, 2)
        self.assertEqual(sorted(p.name for p in self.adapters.iterdir()), before)


if __name__ == "__main__":
    unittest.main()
