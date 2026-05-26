"""Smoke tests for the Python SDK. No pytest dependency — runnable with
stdlib unittest so this works in any Python 3.7+ environment without
extra installs.

Run from the repo root after building the `human` binary:

    cmake --build build --target human
    PYTHONPATH=apps/python-sdk \
      HUMAN_BIN=./build/human \
      python3 -m unittest apps/python-sdk/tests/test_smoke.py
"""

import os
import sys
import unittest

# Make the SDK importable when the test file is invoked directly.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from human import HULA_SDK_VERSION_STRING, HuLa, HulaResult


def _human_bin_available() -> bool:
    """The smoke tests need a `human` binary. CI without a built binary
    skips them rather than failing — matches the existing project
    pattern for tests that depend on artifacts.

    Tries three resolution paths in order:
    1. HUMAN_BIN as an absolute path
    2. HUMAN_BIN as a path relative to CWD (e.g. ./build/human)
    3. HUMAN_BIN basename via PATH lookup
    """
    bin_path = os.environ.get("HUMAN_BIN", "human")
    if os.path.isabs(bin_path):
        return os.access(bin_path, os.X_OK)
    if os.sep in bin_path or "/" in bin_path:
        # Relative path with a directory component (e.g. ./build/human)
        return os.access(bin_path, os.X_OK)
    # Bare basename — PATH lookup
    for p in os.environ.get("PATH", "").split(os.pathsep):
        full = os.path.join(p, bin_path)
        if os.access(full, os.X_OK):
            return True
    return False


@unittest.skipUnless(_human_bin_available(),
                     "human binary not on PATH or HUMAN_BIN; build with "
                     "`cmake --build build --target human` and set HUMAN_BIN")
class TestPythonSdkSmoke(unittest.TestCase):

    def test_version_string_is_set(self):
        """The SDK exposes a semver string mirroring the C-side macro."""
        self.assertIsInstance(HULA_SDK_VERSION_STRING, str)
        parts = HULA_SDK_VERSION_STRING.split(".")
        self.assertEqual(len(parts), 3,
                         f"expected MAJOR.MINOR.PATCH, got {HULA_SDK_VERSION_STRING!r}")
        for p in parts:
            self.assertTrue(p.isdigit(), f"non-numeric version part: {p!r}")

    def test_validate_minimal_program(self):
        """A 1-node EMIT program validates cleanly."""
        program = {
            "name": "smoke",
            "version": 1,
            "root": {
                "id": "n1",
                "op": "emit",
                "emit_key": "k",
                "emit_value": "v",
            },
        }
        hula = HuLa()
        result = hula.validate(program)
        self.assertIsInstance(result, HulaResult)
        self.assertTrue(result.ok,
                        f"validate failed: rc={result.returncode}, "
                        f"stderr={result.stderr!r}")

    def test_validate_returns_failure_on_malformed_program(self):
        """A program with no `root` field should fail validation."""
        program = {"name": "broken", "version": 1}  # missing root
        hula = HuLa()
        result = hula.validate(program)
        self.assertFalse(result.ok,
                         f"expected validate to fail on malformed program; "
                         f"got rc={result.returncode}")

    def test_hulaResult_is_dataclass(self):
        """HulaResult exposes the four documented fields."""
        r = HulaResult(ok=True, returncode=0, stdout="x", stderr="")
        self.assertTrue(r.ok)
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stdout, "x")
        self.assertEqual(r.stderr, "")


if __name__ == "__main__":
    unittest.main()
