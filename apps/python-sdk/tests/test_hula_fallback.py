"""Tests for HuLa binary resolution + auto-download fallback.

No pytest — uses stdlib unittest. No real PATH lookup, no real download:
the pure `resolve_binary` resolver takes its `which` and `ensure`
dependencies as injected callables, so every case is deterministic.

Run from the repo root:

    PYTHONPATH=apps/python-sdk python3 apps/python-sdk/tests/test_hula_fallback.py
"""

import os
import sys
import unittest
from unittest.mock import patch

# Make the SDK importable when the test file is invoked directly.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from human import HuLa, resolve_binary


class _Spy:
    """Records calls and returns a fixed value."""

    def __init__(self, return_value):
        self.return_value = return_value
        self.calls = []

    def __call__(self, *args):
        self.calls.append(args)
        return self.return_value


class TestResolveBinary(unittest.TestCase):
    """The pure resolver: explicit > PATH > download."""

    def test_explicit_wins_never_downloads(self):
        """AC#3: an explicit binary is returned as-is; no PATH/download."""
        which = _Spy("/usr/local/bin/human")  # would be found if consulted
        ensure = _Spy("/cache/human-latest")
        result = resolve_binary(
            explicit=True, human_bin="/custom/human", which=which, ensure=ensure
        )
        self.assertEqual(result, "/custom/human")
        # Explicit short-circuits BEFORE any PATH lookup or download.
        self.assertEqual(which.calls, [])
        self.assertEqual(ensure.calls, [])

    def test_explicit_wins_even_when_missing(self):
        """Explicit path is respected even if it does not exist — no download."""
        which = _Spy(None)
        ensure = _Spy("/cache/human-latest")
        result = resolve_binary(
            explicit=True, human_bin="/no/such/human", which=which, ensure=ensure
        )
        self.assertEqual(result, "/no/such/human")
        self.assertEqual(ensure.calls, [])

    def test_path_hit_no_download(self):
        """AC#1: a binary on PATH is used; ensure() is never called."""
        which = _Spy("/usr/local/bin/human")
        ensure = _Spy("/cache/human-latest")
        result = resolve_binary(
            explicit=False, human_bin="human", which=which, ensure=ensure
        )
        self.assertEqual(result, "/usr/local/bin/human")
        self.assertEqual(which.calls, [("human",)])
        self.assertEqual(ensure.calls, [])

    def test_fallback_triggers_download(self):
        """AC#2: nothing explicit, nothing on PATH -> ensure() is called."""
        which = _Spy(None)  # not on PATH
        ensure = _Spy("/cache/human-latest")
        result = resolve_binary(
            explicit=False, human_bin="human", which=which, ensure=ensure
        )
        self.assertEqual(result, "/cache/human-latest")
        self.assertEqual(which.calls, [("human",)])
        self.assertEqual(len(ensure.calls), 1)


class TestHuLaWiring(unittest.TestCase):
    """HuLa._resolve_bin wires the resolver to shutil.which + ensure_binary."""

    @patch.dict(os.environ, {}, clear=False)
    def test_explicit_arg_sets_explicit_flag(self):
        os.environ.pop("HUMAN_BIN", None)
        hula = HuLa("/custom/human")
        self.assertTrue(hula._explicit)
        self.assertEqual(hula.human_bin, "/custom/human")

    @patch.dict(os.environ, {"HUMAN_BIN": "/env/human"})
    def test_env_sets_explicit_flag(self):
        hula = HuLa()
        self.assertTrue(hula._explicit)
        self.assertEqual(hula.human_bin, "/env/human")

    @patch.dict(os.environ, {}, clear=False)
    def test_default_is_not_explicit(self):
        os.environ.pop("HUMAN_BIN", None)
        hula = HuLa()
        self.assertFalse(hula._explicit)
        self.assertEqual(hula.human_bin, "human")

    @patch.dict(os.environ, {}, clear=False)
    @patch("human._binary.ensure_binary")
    @patch("shutil.which")
    def test_explicit_never_calls_ensure_binary(self, mock_which, mock_ensure):
        """AC#3 at the HuLa layer: explicit binary skips PATH + download."""
        os.environ.pop("HUMAN_BIN", None)
        mock_which.return_value = None
        hula = HuLa("/custom/human")
        self.assertEqual(hula._resolve_bin(), "/custom/human")
        mock_which.assert_not_called()
        mock_ensure.assert_not_called()

    @patch.dict(os.environ, {}, clear=False)
    @patch("human._binary.ensure_binary")
    @patch("shutil.which")
    def test_path_hit_never_calls_ensure_binary(self, mock_which, mock_ensure):
        """AC#1 at the HuLa layer: PATH binary skips download."""
        os.environ.pop("HUMAN_BIN", None)
        mock_which.return_value = "/usr/local/bin/human"
        hula = HuLa()
        self.assertEqual(hula._resolve_bin(), "/usr/local/bin/human")
        mock_ensure.assert_not_called()

    @patch.dict(os.environ, {}, clear=False)
    @patch("human._binary.ensure_binary")
    @patch("shutil.which")
    def test_fallback_calls_ensure_binary(self, mock_which, mock_ensure):
        """AC#2 at the HuLa layer: missing binary triggers ensure_binary."""
        os.environ.pop("HUMAN_BIN", None)
        mock_which.return_value = None
        mock_ensure.return_value = "/cache/human-latest"
        hula = HuLa()
        self.assertEqual(hula._resolve_bin(), "/cache/human-latest")
        mock_ensure.assert_called_once()

    @patch.dict(os.environ, {}, clear=False)
    @patch("human._binary.ensure_binary")
    @patch("shutil.which")
    def test_resolution_is_cached(self, mock_which, mock_ensure):
        """Resolution runs once; later calls reuse the cached path."""
        os.environ.pop("HUMAN_BIN", None)
        mock_which.return_value = None
        mock_ensure.return_value = "/cache/human-latest"
        hula = HuLa()
        self.assertEqual(hula._resolve_bin(), "/cache/human-latest")
        self.assertEqual(hula._resolve_bin(), "/cache/human-latest")
        mock_ensure.assert_called_once()  # NOT twice

    @patch("human._binary.ensure_binary")
    def test_construction_does_no_io(self, mock_ensure):
        """Constructing a HuLa must never download (lazy resolution)."""
        HuLa()
        HuLa("/custom/human")
        mock_ensure.assert_not_called()


if __name__ == "__main__":
    unittest.main()
