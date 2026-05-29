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
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from typing import Callable, Optional

# Mirrored from include/human/hula_sdk.h. Bumped in lock-step with the C
# macros so Python consumers can detect API breaks.
#
# 0.2.0 (2026-05-26) — Phase 1.5: added expand/compile/replay/schema
# 0.1.0 (2026-05-26) — Phase 1: subprocess wrapper, validate + run
HULA_SDK_VERSION_MAJOR = 0
HULA_SDK_VERSION_MINOR = 2
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


def resolve_binary(
    explicit: bool,
    human_bin: str,
    which: Callable[[str], Optional[str]],
    ensure: Callable[[], str],
) -> str:
    """Pure resolver for which `human` binary to invoke.

    Decision order (no I/O of its own — `which` and `ensure` are
    injected so this is unit-testable without PATH lookups or a real
    download):

      1. An explicitly-chosen binary (constructor arg or HUMAN_BIN) is
         returned as-is — NEVER triggers a download, even if the path
         turns out not to exist. The user asked for it; respect it.
      2. Otherwise, if `which(human_bin)` finds a copy on PATH, use it.
      3. Otherwise (nothing explicit, nothing on PATH) fall back to
         `ensure()` — the platform-matched auto-download.

    Args:
        explicit: True if the caller passed `human_bin` or set HUMAN_BIN.
        human_bin: The configured binary name/path (default "human").
        which: PATH-lookup function (e.g. shutil.which).
        ensure: Last-resort downloader returning an absolute path
            (e.g. ensure_binary).

    Returns:
        The resolved binary path to invoke.
    """
    if explicit:
        return human_bin
    found = which(human_bin)
    if found:
        return found
    return ensure()


class HuLa:
    """Subprocess-based binding to the `human hula` CLI.

    Construct once and reuse; each call to `validate` / `run` /
    `compile` spawns a fresh `human` process. Thread-safe (no shared
    state across calls).

    Binary resolution is LAZY: construction never touches the network.
    The binary is resolved on first use (`validate`/`run`/etc.) via
    `resolve_binary`, and the result is cached for subsequent calls.

    Args:
        human_bin: Path to the `human` binary. Defaults to the
            HUMAN_BIN env var if set, otherwise the first `human` on
            PATH. If neither is set and nothing is on PATH, a
            platform-matched binary is auto-downloaded on first use
            (see human._binary.ensure_binary). An explicitly-passed
            `human_bin` or HUMAN_BIN always wins and never downloads.
    """

    def __init__(self, human_bin: Optional[str] = None):
        # Whether the caller explicitly chose a binary. Captured at
        # construction so a later os.environ mutation can't retroactively
        # flip an implicit default into an "explicit" choice mid-session.
        self._explicit = human_bin is not None or "HUMAN_BIN" in os.environ
        self.human_bin = human_bin or os.environ.get("HUMAN_BIN", "human")
        # Lazily resolved on first use; cached thereafter.
        self._resolved_bin: Optional[str] = None

    def _resolve_bin(self) -> str:
        """Resolve (and cache) the binary path, downloading as a last resort.

        Cheap and side-effect-free on the common path (binary on PATH or
        explicitly configured). Only the genuinely-missing case performs
        network I/O, and only once.
        """
        if self._resolved_bin is not None:
            return self._resolved_bin
        # Import lazily so merely constructing a HuLa never imports the
        # download machinery, and offline/air-gapped users who supply a
        # binary explicitly never pay for it.
        from ._binary import ensure_binary
        self._resolved_bin = resolve_binary(
            self._explicit,
            self.human_bin,
            shutil.which,
            lambda: str(ensure_binary()),
        )
        return self._resolved_bin

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
                [self._resolve_bin(), "hula", verb, tmp_path],
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

    def schema(self) -> HulaResult:
        """Return the canonical HuLa JSON Schema.

        `result.stdout` contains the schema's path on the first line
        followed by the schema body. Useful for validating dicts
        against the published schema before calling `validate`.
        """
        proc = subprocess.run(
            [self._resolve_bin(), "hula", "schema"],
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

    def expand(self, template: str, vars: dict) -> HulaResult:
        """Expand `{{key}}` placeholders in `template` using `vars`.

        Wraps `human hula expand <tmpl> <vars.json>`. Both the
        template body and the vars dict are written to temp files;
        the expanded text comes back in `result.stdout`.
        """
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".tmpl.txt", delete=False
        ) as tt:
            tt.write(template)
            tmpl_path = tt.name
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".vars.json", delete=False
        ) as tv:
            json.dump(vars, tv)
            vars_path = tv.name
        try:
            proc = subprocess.run(
                [self._resolve_bin(), "hula", "expand", tmpl_path, vars_path],
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
            for p in (tmpl_path, vars_path):
                try:
                    os.unlink(p)
                except OSError:
                    pass

    def compile(self, source: str, lite: bool = False) -> HulaResult:
        """Compile a HuLa program source to canonical JSON.

        With `lite=True`, `source` is treated as lite-syntax HuLa.
        With `lite=False` (default), `source` is expected to be a
        canonical HuLa JSON program — the CLI normalizes it.

        Note: despite the name, this is NOT LLM-driven synthesis. It
        is a syntactic transform from lite-syntax (or JSON) to
        canonical JSON. LLM-driven program synthesis is on the M5
        Phase 3 roadmap.
        """
        suffix = ".hula" if lite else ".hula.json"
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=suffix, delete=False
        ) as tf:
            tf.write(source)
            tmp_path = tf.name
        try:
            argv = [self._resolve_bin(), "hula", "compile"]
            if lite:
                argv.append("--lite")
            argv.append(tmp_path)
            proc = subprocess.run(
                argv,
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

    def replay(self, trace: dict, config_path: Optional[str] = None) -> HulaResult:
        """Re-run an embedded HuLa program from a captured trace.

        `trace` is a HuLa trace JSON object — typically captured via
        `HU_HULA_TRACE_DIR=/path` on a prior `run` call. The trace
        carries the original program and inputs; `replay` re-executes
        them deterministically.

        Pass `config_path` to use a `human` config.json (so the same
        tools are wired) instead of the CLI's demo tools.
        """
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".trace.json", delete=False
        ) as tf:
            json.dump(trace, tf)
            tmp_path = tf.name
        try:
            argv = [self._resolve_bin(), "hula", "replay"]
            if config_path:
                argv.extend(["--config", config_path])
            argv.append(tmp_path)
            proc = subprocess.run(
                argv,
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


__all__ = [
    "HuLa",
    "HulaResult",
    "resolve_binary",
    "HULA_SDK_VERSION_STRING",
    "__version__",
]
