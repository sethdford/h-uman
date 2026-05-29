"""
Version-pinned binary auto-download helper for the human CLI.

This module provides ensure_binary(version="0.1.0") to automatically download
the correct precompiled human binary for the current platform, pinned to the
SDK version. Downloads are cached under a platform-specific user cache dir
and verified to be executable before use.

Supported platforms:
  - Linux x86_64
  - Linux aarch64
  - macOS aarch64

Raises:
  RuntimeError: if the platform is not supported or download fails.
  IOError: if the cache directory cannot be created or written.
"""

import os
import platform
import stat
import urllib.request
import urllib.error
from pathlib import Path


def _get_cache_dir():
    """Return the platform-specific cache directory for the human binary.

    Uses the XDG_CACHE_HOME env var on Linux, ~/Library/Caches on macOS,
    or a platform-neutral fallback.
    """
    if "XDG_CACHE_HOME" in os.environ:
        cache_base = os.environ["XDG_CACHE_HOME"]
    elif platform.system() == "Darwin":
        cache_base = os.path.expanduser("~/Library/Caches")
    else:
        cache_base = os.path.expanduser("~/.cache")

    cache_dir = Path(cache_base) / "human-sdk"
    cache_dir.mkdir(parents=True, exist_ok=True)
    return cache_dir


def _release_ref():
    """Resolve which binary release to download from.

    The SDK's own semver (e.g. 0.1.0) is DECOUPLED from the h-uman binary
    releases, which are date-tagged (e.g. v2026.3.3). Building a tag from the
    SDK version (v0.1.0) points at a release that does not carry the
    human-*.bin assets and 404s. So we default to the "latest" published
    release, which always carries the current human-*.bin assets. Operators
    who need a reproducible pin can set HUMAN_BINARY_RELEASE to an explicit
    tag (e.g. "v2026.3.3").
    """
    return os.environ.get("HUMAN_BINARY_RELEASE", "latest").strip() or "latest"


def _platform_to_asset_name(version):
    """Map the current platform to the release asset name and download URL.

    The h-uman release workflow produces binaries named:
      human-linux-x86_64.bin
      human-linux-aarch64.bin
      human-macos-aarch64.bin

    Args:
        version: The SDK version (accepted for API compatibility; the binary
                 release is selected by _release_ref(), not this value).

    Returns:
        A tuple of (asset_name, asset_url, release_ref). When the release ref
        is "latest", asset_url uses GitHub's /releases/latest/download/ alias,
        which redirects to the current release's asset. When pinned, it points
        at /releases/download/{ref}/.

    Raises:
        RuntimeError: if the platform is not supported.
    """
    system = platform.system()
    machine = platform.machine()

    # Normalize machine names
    if machine in ("x86_64", "AMD64"):
        machine = "x86_64"
    elif machine in ("aarch64", "arm64", "ARM64"):
        machine = "aarch64"

    if system == "Linux" and machine == "x86_64":
        asset_name = "human-linux-x86_64.bin"
    elif system == "Linux" and machine == "aarch64":
        asset_name = "human-linux-aarch64.bin"
    elif system == "Darwin" and machine == "aarch64":
        asset_name = "human-macos-aarch64.bin"
    else:
        raise RuntimeError(
            f"Unsupported platform: {system} {machine}. "
            "Supported: Linux x86_64, Linux aarch64, macOS aarch64. "
            "Build the human binary from source: "
            "https://github.com/sethdford/h-uman"
        )

    # Release assets are published to GitHub Releases. The SDK version is
    # DECOUPLED from the binary release tag (see _release_ref docstring), so we
    # resolve the ref independently rather than building a tag from `version`.
    #   latest : https://github.com/{owner}/{repo}/releases/latest/download/{asset}
    #   pinned : https://github.com/{owner}/{repo}/releases/download/{ref}/{asset}
    repo_base = "https://github.com/sethdford/h-uman/releases"
    ref = _release_ref()
    if ref == "latest":
        asset_url = f"{repo_base}/latest/download/{asset_name}"
    else:
        asset_url = f"{repo_base}/download/{ref}/{asset_name}"

    return asset_name, asset_url, ref


def ensure_binary(version="0.1.0"):
    """Download and cache the human binary for the current platform.

    Checks the cache for an existing binary; if not present or invalid,
    downloads from the release URL (HTTPS only). The binary is verified
    to be executable before return. Subsequent calls for the same version
    use the cached copy (no re-download).

    Args:
        version: The SDK version (default: "0.1.0"). Accepted for API
                 compatibility; the binary release is selected by the
                 HUMAN_BINARY_RELEASE env var (default "latest"), NOT this
                 value. The SDK semver is decoupled from binary release tags.

    Returns:
        The absolute Path to the cached binary, ready to execute.

    Raises:
        RuntimeError: if the platform is unsupported or the download fails.
        IOError: if the cache directory cannot be created or the binary
                 cannot be written to disk.
    """
    asset_name, asset_url, ref = _platform_to_asset_name(version)

    # HTTPS-only: hard project rule for all outbound. Refuse any non-HTTPS URL
    # (defends against a misconfigured base_url or an http:// redirect target).
    if not asset_url.lower().startswith("https://"):
        raise RuntimeError(
            f"Refusing to download human binary over non-HTTPS URL: {asset_url}"
        )

    cache_dir = _get_cache_dir()
    # Cache is keyed on the release ref (not the SDK version) because the binary
    # is selected by the ref. A "latest" cache entry is refreshed whenever the
    # cached copy is missing/invalid; operators who need reproducibility pin
    # HUMAN_BINARY_RELEASE so each pinned ref gets its own immutable cache slot.
    cache_path = cache_dir / f"human-{ref}"

    # If the cached binary exists and is executable, return it.
    if cache_path.exists() and os.access(cache_path, os.X_OK):
        return cache_path

    # Download the binary from the release URL (HTTPS only).
    try:
        with urllib.request.urlopen(asset_url) as response:
            binary_data = response.read()
    except urllib.error.URLError as e:
        raise RuntimeError(
            f"Failed to download human binary from {asset_url}: {e}. "
            "Ensure the release exists and your network allows HTTPS."
        ) from e
    except Exception as e:
        raise RuntimeError(
            f"Unexpected error downloading {asset_url}: {e}"
        ) from e

    # Write the binary to the cache and make it executable.
    try:
        cache_path.write_bytes(binary_data)
        # Make executable: user + group can read/execute, others can read
        cache_path.chmod(cache_path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP)
    except OSError as e:
        raise IOError(
            f"Failed to write cached binary to {cache_path}: {e}"
        ) from e

    # Verify it's executable before returning.
    if not os.access(cache_path, os.X_OK):
        raise RuntimeError(
            f"Downloaded binary {cache_path} is not executable. "
            "This should not happen; check file permissions."
        )

    return cache_path


__all__ = ["ensure_binary"]
