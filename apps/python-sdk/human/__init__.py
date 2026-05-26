"""
human — Python SDK for HuLa (Human Language) programs.

M5 Phase 1 (2026-05-26): subprocess-based wrapper around the `human hula`
CLI. Apps using this SDK do NOT need to link the C library or build a
shared dylib — they just need the `human` binary on PATH (or pointed at
via the `HUMAN_BIN` env var, or passed explicitly).

Quick start:

    from human import HuLa

    prog = {
        "version": 1,
        "nodes": [
            {"id": "n1", "op": "emit", "emit_key": "greeting",
             "emit_value": "hello from python"}
        ]
    }
    hula = HuLa()  # uses `human` from PATH
    result = hula.validate(prog)
    if result.ok:
        run_result = hula.run(prog)
        print(run_result.stdout)

Status: Phase 1 ships the subprocess boundary. Phase 2 will add a
ctypes-based in-process binding once the project builds a shared
libhuman.dylib. Both phases expose the same Python API so existing
SDK consumers won't need to change anything when Phase 2 lands.

See the project's `include/human/hula_sdk.h` for the C-level SDK
surface this wraps.
"""

import json
import os
import subprocess
import tempfile
from dataclasses import dataclass
from typing import Optional

# Mirrored from include/human/hula_sdk.h. Bumped in lock-step with the C
# macros so Python consumers can detect API breaks.
HULA_SDK_VERSION_MAJOR = 0
HULA_SDK_VERSION_MINOR = 1
HULA_SDK_VERSION_PATCH = 0
HULA_SDK_VERSION_STRING = f"{HULA_SDK_VERSION_MAJOR}.{HULA_SDK_VERSION_MINOR}.{HULA_SDK_VERSION_PATCH}"

__version__ = HULA_SDK_VERSION_STRING


@dataclass
class HulaResult:
    """Result of a `human hula <verb>` subprocess invocation.

    Attributes:
        ok: True if the subprocess exited with status 0.
        returncode: Raw exit status (0 = success, non-zero = error).
        stdout: Captured stdout (the program output / parsed JSON / etc.).
        stderr: Captured stderr (error messages from the C-side validator).
    """
    ok: bool
    returncode: int
    stdout: str
    stderr: str


class HuLa:
    """Subprocess-based binding to the `human hula` CLI.

    Construct once and reuse; each call to `validate` / `run` /
    `compile` spawns a fresh `human` process. Thread-safe (no shared
    state across calls).

    Args:
        human_bin: Path to the `human` binary. Defaults to the
            HUMAN_BIN env var if set, otherwise the first `human`
            on PATH.
    """

    def __init__(self, human_bin: Optional[str] = None):
        self.human_bin = human_bin or os.environ.get("HUMAN_BIN", "human")

    def _run_with_program(self, verb: str, program: dict) -> HulaResult:
        """Spawn `human hula <verb> <tmpfile>` with `program` as JSON."""
        # `human hula validate/run` accept a file path or a literal JSON
        # string as the last arg. We use a temp file so the program can
        # be larger than the shell's argv limit and so we don't have to
        # worry about JSON shell-escaping.
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".hula.json", delete=False
        ) as tf:
            json.dump(program, tf)
            tmp_path = tf.name
        try:
            proc = subprocess.run(
                [self.human_bin, "hula", verb, tmp_path],
                capture_output=True,
                text=True,
                timeout=30,
            )
            return HulaResult(
                ok=(proc.returncode == 0),
                returncode=proc.returncode,
                stdout=proc.stdout,
                stderr=proc.stderr,
            )
        finally:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

    def validate(self, program: dict) -> HulaResult:
        """Validate a HuLa program (structure, refs, depth, cycles).

        Returns a HulaResult; check `.ok` to see if the program is
        valid. `.stderr` holds the specific reason on failure.
        """
        return self._run_with_program("validate", program)

    def run(self, program: dict) -> HulaResult:
        """Execute a HuLa program with the CLI's built-in demo tools.

        The `human hula run` CLI uses stub demo tools (echo, search,
        write, analyze) — fine for development and testing but not a
        full agent runtime. For production execution with real tools,
        embed the C SDK directly (see include/human/hula_sdk.h).
        """
        return self._run_with_program("run", program)


__all__ = [
    "HuLa",
    "HulaResult",
    "HULA_SDK_VERSION_STRING",
    "__version__",
]
