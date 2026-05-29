"""Tests for the binary download helper (_binary.py).

No pytest — uses stdlib unittest. Mocks all network requests.

Run from the repo root:

    PYTHONPATH=apps/python-sdk python3 apps/python-sdk/tests/test_binary.py
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch, MagicMock

# Make the SDK importable when the test file is invoked directly.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from human._binary import (
    _platform_to_asset_name,
    _get_cache_dir,
    ensure_binary,
)


class TestPlatformToAssetName(unittest.TestCase):
    """Tests for the platform -> asset-name mapping."""

    @patch("platform.system", return_value="Linux")
    @patch("platform.machine", return_value="x86_64")
    def test_linux_x86_64(self, mock_machine, mock_system):
        """Linux x86_64 maps to human-linux-x86_64.bin."""
        asset_name, url = _platform_to_asset_name("0.1.0")
        self.assertEqual(asset_name, "human-linux-x86_64.bin")
        self.assertIn("v0.1.0", url)
        self.assertTrue(url.startswith("https://"))

    @patch("platform.system", return_value="Linux")
    @patch("platform.machine", return_value="aarch64")
    def test_linux_aarch64(self, mock_machine, mock_system):
        """Linux aarch64 maps to human-linux-aarch64.bin."""
        asset_name, url = _platform_to_asset_name("0.1.0")
        self.assertEqual(asset_name, "human-linux-aarch64.bin")
        self.assertIn("v0.1.0", url)
        self.assertTrue(url.startswith("https://"))

    @patch("platform.system", return_value="Darwin")
    @patch("platform.machine", return_value="aarch64")
    def test_macos_aarch64(self, mock_machine, mock_system):
        """macOS aarch64 maps to human-macos-aarch64.bin."""
        asset_name, url = _platform_to_asset_name("0.1.0")
        self.assertEqual(asset_name, "human-macos-aarch64.bin")
        self.assertIn("v0.1.0", url)
        self.assertTrue(url.startswith("https://"))

    @patch("platform.system", return_value="Windows")
    @patch("platform.machine", return_value="x86_64")
    def test_unsupported_platform_windows(self, mock_machine, mock_system):
        """Windows is not supported; raises RuntimeError."""
        with self.assertRaises(RuntimeError) as ctx:
            _platform_to_asset_name("0.1.0")
        self.assertIn("Unsupported platform", str(ctx.exception))
        self.assertIn("Windows", str(ctx.exception))

    @patch("platform.system", return_value="Linux")
    @patch("platform.machine", return_value="i686")
    def test_unsupported_arch_i686(self, mock_machine, mock_system):
        """Linux i686 (32-bit) is not supported; raises RuntimeError."""
        with self.assertRaises(RuntimeError) as ctx:
            _platform_to_asset_name("0.1.0")
        self.assertIn("Unsupported platform", str(ctx.exception))

    @patch("platform.system", return_value="Darwin")
    @patch("platform.machine", return_value="x86_64")
    def test_unsupported_arch_macos_intel(self, mock_machine, mock_system):
        """macOS Intel (x86_64) is not supported; raises RuntimeError."""
        with self.assertRaises(RuntimeError) as ctx:
            _platform_to_asset_name("0.1.0")
        self.assertIn("Unsupported platform", str(ctx.exception))

    @patch("platform.system", return_value="Linux")
    @patch("platform.machine", return_value="x86_64")
    def test_url_uses_https(self, mock_machine, mock_system):
        """The returned URL always uses HTTPS, never HTTP."""
        _, url = _platform_to_asset_name("0.1.0")
        self.assertTrue(url.startswith("https://"),
                        f"URL must use HTTPS: {url}")
        self.assertNotIn("http://", url,
                         f"URL must not contain unencrypted http: {url}")


class TestEnsureBinary(unittest.TestCase):
    """Tests for ensure_binary() function."""

    def setUp(self):
        """Create a temporary cache directory for each test."""
        self.temp_cache = tempfile.TemporaryDirectory()
        self.cache_dir = Path(self.temp_cache.name)

    def tearDown(self):
        """Clean up the temporary cache directory."""
        self.temp_cache.cleanup()

    @patch("platform.system", return_value="Linux")
    @patch("platform.machine", return_value="x86_64")
    @patch("human._binary._get_cache_dir")
    @patch("urllib.request.urlopen")
    def test_download_and_cache(self, mock_urlopen, mock_cache_dir,
                                mock_machine, mock_system):
        """ensure_binary downloads and caches the binary."""
        mock_cache_dir.return_value = self.cache_dir
        mock_response = MagicMock()
        mock_response.read.return_value = b"\x7fELF"  # Fake ELF header
        mock_response.__enter__.return_value = mock_response
        mock_urlopen.return_value = mock_response

        path = ensure_binary("0.1.0")

        self.assertEqual(path, self.cache_dir / "human-0.1.0")
        self.assertTrue(path.exists())
        self.assertTrue(os.access(path, os.X_OK))
        # Verify urlopen was called with an HTTPS URL
        called_url = mock_urlopen.call_args[0][0]
        self.assertTrue(called_url.startswith("https://"))

    @patch("platform.system", return_value="Linux")
    @patch("platform.machine", return_value="x86_64")
    @patch("human._binary._get_cache_dir")
    @patch("urllib.request.urlopen")
    def test_cache_hit_avoids_download(self, mock_urlopen, mock_cache_dir,
                                       mock_machine, mock_system):
        """If cached binary exists, ensure_binary returns it without download."""
        mock_cache_dir.return_value = self.cache_dir
        cache_path = self.cache_dir / "human-0.1.0"
        cache_path.write_bytes(b"\x7fELF")
        cache_path.chmod(0o755)

        path = ensure_binary("0.1.0")

        self.assertEqual(path, cache_path)
        mock_urlopen.assert_not_called()

    @patch("platform.system", return_value="Linux")
    @patch("platform.machine", return_value="x86_64")
    @patch("human._binary._get_cache_dir")
    @patch("urllib.request.urlopen")
    def test_download_failure_raises(self, mock_urlopen, mock_cache_dir,
                                    mock_machine, mock_system):
        """If download fails, ensure_binary raises RuntimeError."""
        mock_cache_dir.return_value = self.cache_dir
        mock_urlopen.side_effect = Exception("Connection refused")

        with self.assertRaises(RuntimeError) as ctx:
            ensure_binary("0.1.0")
        # The error message may be "Failed to download" or "Unexpected error downloading"
        self.assertIn("download", str(ctx.exception).lower())

    @patch("platform.system", return_value="Windows")
    @patch("platform.machine", return_value="x86_64")
    @patch("human._binary._get_cache_dir")
    def test_unsupported_platform_raises(self, mock_cache_dir,
                                        mock_machine, mock_system):
        """ensure_binary raises RuntimeError on unsupported platform."""
        mock_cache_dir.return_value = self.cache_dir

        with self.assertRaises(RuntimeError) as ctx:
            ensure_binary("0.1.0")
        self.assertIn("Unsupported platform", str(ctx.exception))

    @patch("platform.system", return_value="Linux")
    @patch("platform.machine", return_value="x86_64")
    @patch("human._binary._get_cache_dir")
    @patch("urllib.request.urlopen")
    def test_binary_is_executable(self, mock_urlopen, mock_cache_dir,
                                  mock_machine, mock_system):
        """Downloaded binary is marked executable."""
        mock_cache_dir.return_value = self.cache_dir
        mock_response = MagicMock()
        mock_response.read.return_value = b"\x7fELF"
        mock_response.__enter__.return_value = mock_response
        mock_urlopen.return_value = mock_response

        path = ensure_binary("0.1.0")

        self.assertTrue(os.access(path, os.X_OK),
                        f"Binary {path} is not executable")
        stat_info = path.stat()
        self.assertTrue(stat_info.st_mode & 0o100,
                        "Binary missing user execute bit")


if __name__ == "__main__":
    unittest.main()
