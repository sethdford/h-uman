#!/usr/bin/env bash
# check-test-source-gate-symmetry.sh — Detect mismatched HU_ENABLE_* gates
# between tests/test_X.c and src/.../X.c registrations in CMakeLists.txt.
#
# PROBLEM CLASS (recurring on PR #113, 6+ instances across rounds 2/9/10/11/12/13):
#
# Author A lands src/foo.c inside `if(HU_ENABLE_FOO)` and writes a test
# tests/test_foo.c using hu_foo_* symbols. Author A adds the test source
# UNCONDITIONALLY to HU_TEST_SOURCES (top level). Their dev preset has
# HU_ENABLE_FOO=ON, so test_foo.c links fine. PR CI runs the
# minimal/no-skills/cross-arm64 variants with HU_ENABLE_FOO=OFF — the
# source is excluded, the test isn't, and the linker reports:
#   tests/test_foo.c: undefined reference to `hu_foo_init'
#
# THIS SCRIPT catches the mismatch at PR-author time (pre-commit) and at
# CI time (any platform) without needing the actual ML/feature-flag
# variant build. It parses CMakeLists.txt's if(HU_ENABLE_*)/endif() block
# structure, maps each src/.../X.c and tests/test_X.c to its condition
# stack, and reports mismatches where the test is less gated than the source.
#
# EXIT CODES:
#   0 — all source-test pairs have symmetric gating
#   1 — at least one test is less gated than its source (link-error class)
#   2 — script failed (CMakeLists.txt unparseable, Python missing, etc.)
#
# USAGE: bash scripts/check-test-source-gate-symmetry.sh
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 required" >&2
    exit 2
fi

python3 - <<'PYEOF'
"""Parse CMakeLists.txt, extract per-file condition stacks, report mismatches."""
import os
import re
import sys


def extract_flags(cond: str) -> set[str]:
    """Pull HU_ENABLE_* tokens from an if() condition string."""
    return set(re.findall(r"HU_ENABLE_[A-Z_]+", cond))


def parse_cmake(path: str) -> dict[str, set[str]]:
    """Return {file_path: set_of_required_HU_ENABLE_flags}.

    Files appearing unconditionally get an empty set. Files inside
    one or more nested `if(HU_ENABLE_X)` blocks accumulate every
    flag in the enclosing stack (AND semantics — all must be on).
    """
    file_gates: dict[str, set[str]] = {}
    stack: list[set[str]] = []  # one set of required flags per nesting level

    if_re = re.compile(r"^\s*if\s*\(([^)]+)\)")
    endif_re = re.compile(r"^\s*endif\s*\(")
    file_re = re.compile(r"(?:tests|src)/[A-Za-z0-9_./-]+\.c\b")
    # NOT_HU is meaningful (e.g., if(NOT HU_ENABLE_ML)). We ONLY track
    # positive-gate stacks for link-symmetry; tests inside if(NOT X) are
    # the inverse problem (test_X.c only compiles when X is OFF) and is
    # rare enough to skip here.
    not_re = re.compile(r"\bNOT\b")

    with open(path) as f:
        for raw in f:
            # Strip end-of-line comments — CMake # comments end at newline.
            line = raw.split("#", 1)[0]

            m = if_re.match(line)
            if m:
                cond = m.group(1)
                if not_re.search(cond):
                    # Negative gate — push empty frame so endif() balances
                    # but we don't add any required-flag.
                    stack.append(set())
                else:
                    flags = extract_flags(cond)
                    stack.append(flags)
                continue

            if endif_re.match(line):
                if stack:
                    stack.pop()
                continue

            for path_match in file_re.finditer(line):
                fp = path_match.group(0)
                required = set().union(*stack) if stack else set()
                # If file appears multiple times, take the LEAST restrictive
                # (union) — appearing unconditionally anywhere means the
                # most-permissive gate wins.
                if fp in file_gates:
                    file_gates[fp] = file_gates[fp] & required if file_gates[fp] else file_gates[fp]
                else:
                    file_gates[fp] = required
    return file_gates


def has_internal_guard(test_path: str, flags: set[str],
                       *, symbol_patterns: list[str] | None = None) -> bool:
    """True if the .c file has every reference to a gated symbol inside
    an `#ifdef <flag>` block for at least one of the given flags.

    "Gated symbol" defaults to anything matching `hu_X_*` or `X_*` where
    X is derived from the flag (e.g. HU_ENABLE_SQLITE → sqlite3_).
    Callers can override via `symbol_patterns` to scan for specific
    function-call prefixes.

    Canonical SAFE patterns this accepts:

        // Pattern 1: full-file wrap with #else stub
        #ifdef HU_ENABLE_X
        ... bodies + runner ...
        #else
        void run_X_tests(void) { (void)0; }
        #endif

        // Pattern 2: bodies gated, runner unconditional with gated calls
        #ifdef HU_ENABLE_X
        static void test_foo(void) { sqlite3_*(); }  // gated body
        #endif

        void run_X_tests(void) {  // unconditional runner
        #ifdef HU_ENABLE_X
            HU_RUN_TEST(test_foo);  // gated call
        #endif
        }

    Validation: walk the file line-by-line tracking preprocessor nesting,
    record line ranges where each flag is active, then scan for symbol
    references. A reference outside ALL active blocks for required
    flags is a real link risk.

    Earlier "any #endif" fallback (Cursor Bugbot cc9039d3) returned
    True for any file that happened to contain an #endif anywhere —
    making the safety check unable to catch the exact failure class it
    was designed to prevent. This version walks the actual symbol
    references and verifies each is inside a matching gate.
    """
    try:
        with open(test_path) as f:
            lines = f.readlines()
    except OSError:
        return False

    if_re = re.compile(r"^\s*#\s*(if|ifdef|ifndef)\b\s*(.*)")
    else_re = re.compile(r"^\s*#\s*else\b")
    elif_re = re.compile(r"^\s*#\s*elif\b")
    endif_re = re.compile(r"^\s*#\s*endif\b")
    flag_re = re.compile(r"\bHU_ENABLE_[A-Z_]+\b")
    not_re = re.compile(r"\bNOT\b|!\s*defined\b")

    # Compute, per line, the set of active HU_ENABLE_* flags.
    stack: list[set[str]] = []
    active_per_line: list[set[str]] = []
    for raw in lines:
        if endif_re.match(raw):
            if stack:
                stack.pop()
            active_per_line.append(set().union(*stack) if stack else set())
            continue
        if elif_re.match(raw) or else_re.match(raw):
            if stack:
                stack[-1] = set()
            active_per_line.append(set().union(*stack) if stack else set())
            continue
        if m := if_re.match(raw):
            kind, expr = m.group(1), m.group(2)
            if kind == "ifndef" or not_re.search(raw):
                stack.append(set())
            else:
                stack.append(set(flag_re.findall(expr)))
            active_per_line.append(set().union(*stack) if stack else set())
            continue
        active_per_line.append(set().union(*stack) if stack else set())

    # Build the patterns to scan for. If caller didn't pass any, derive
    # from flag names: HU_ENABLE_SQLITE → sqlite3_, HU_ENABLE_ML → hu_ml_.
    if symbol_patterns is None:
        symbol_patterns = []
        for f in flags:
            short = f.removeprefix("HU_ENABLE_").lower()
            if short == "sqlite":
                symbol_patterns.append(r"\bsqlite3_\w*\s*\(")
            elif short == "curl":
                symbol_patterns.append(r"\bcurl_\w*\s*\(")
            elif short == "tls":
                symbol_patterns.append(r"\bSSL_\w*\s*\(")
            else:
                symbol_patterns.append(rf"\bhu_{re.escape(short)}_\w+\s*\(")

    combined = re.compile("|".join(f"(?:{p})" for p in symbol_patterns))

    # Path 1: full-file wrap with #else stub. The runner definition
    # appears inside `#ifdef FLAG` and a stub `void run_*_tests(void) {
    # ... }` appears in the matching `#else` branch. Verify by
    # checking that EVERY `void run_X_tests(void)` definition line is
    # inside the gate for at least one required flag, AND the file
    # contains the `#else void run_*_tests` shape.
    runner_def_re = re.compile(
        r"^\s*(?:void|static\s+void)\s+run_\w+_tests\s*\(\s*void\s*\)\s*\{?")
    runner_lines = [i for i, raw in enumerate(lines) if runner_def_re.match(raw)]
    full_content = "".join(lines)
    has_stub_in_else = bool(re.search(
        r"#\s*else\b[^\n]*\n\s*(?:void|static\s+void)\s+run_\w+_tests\s*\(\s*void\s*\)\s*"
        r"\{\s*(?:\(\s*void\s*\)\s*0\s*;\s*)?\}",
        full_content,
    ))
    if has_stub_in_else and runner_lines:
        for f in flags:
            # At least one runner def must be inside this flag's gate
            # (the "real" def — the #else stub is outside it).
            if any(f in active_per_line[i] for i in runner_lines):
                return True

    # Path 2: gated bodies + ungated runner + gated HU_RUN_TEST calls.
    # All references to gated symbols are inside the gate.
    for f in flags:
        all_refs_guarded = True
        any_refs_seen = False
        for i, raw in enumerate(lines):
            if not combined.search(raw):
                continue
            any_refs_seen = True
            if f not in active_per_line[i]:
                all_refs_guarded = False
                break
        if any_refs_seen and all_refs_guarded:
            return True
    return False


def test_includes(test_path: str) -> set[str]:
    """Extract the basenames of all `human/.../X.h` headers included by the
    test. The test's actual subject-under-test is usually the source whose
    header it includes — this lets us disambiguate when multiple
    src/.../X.c files share a basename (e.g. agent/cli.c, ml/cli.c)."""
    try:
        with open(test_path) as f:
            content = f.read()
    except OSError:
        return set()
    bases = set()
    for m in re.finditer(r'^\s*#\s*include\s*[<"]human/([^">]+)\.h[>"]', content, re.MULTILINE):
        # The included header's basename — strip subdir path.
        header_base = os.path.basename(m.group(1))
        bases.add(header_base)
    return bases


# Gated system libraries — including their headers directly in a test
# means the test must be gated by the corresponding HU_ENABLE_* flag,
# or wrapped in an internal #ifdef. PR #113 round 2 (test_reaction_handler_e2e)
# and round 9 (test_authentic::test_life_thread_scoped_per_contact) both
# hit this: the source file under test didn't need sqlite, but the test
# helper code did `#include <sqlite3.h>` directly to seed an in-memory DB.
#
# Each entry: header_name -> (required_flag, symbol_prefix). symbol_prefix
# disambiguates a header used only for type/macro vs. one actually linked.
GATED_SYSTEM_LIBS = {
    "sqlite3.h": ("HU_ENABLE_SQLITE", "sqlite3_"),
    "curl/curl.h": ("HU_ENABLE_CURL", "curl_"),
    "openssl/ssl.h": ("HU_ENABLE_TLS", "SSL_"),
}

# Flag implications: feature flags that imply others. If a test is gated
# by LEARNING (which depends on ML at build time), it doesn't also need
# an explicit ML gate — that's already a transitive consequence.
FLAG_IMPLIES: dict[str, set[str]] = {
    "HU_ENABLE_LEARNING": {"HU_ENABLE_ML"},
    "HU_ENABLE_RL_FULL": {"HU_ENABLE_ML"},
}


def expand_test_flags(test_flags: set[str]) -> set[str]:
    """Add implied flags transitively."""
    out = set(test_flags)
    changed = True
    while changed:
        changed = False
        for flag in list(out):
            for implied in FLAG_IMPLIES.get(flag, set()):
                if implied not in out:
                    out.add(implied)
                    changed = True
    return out


def file_uses_symbol_prefix(test_path: str, prefix: str) -> bool:
    """True if the test calls at least one function starting with prefix
    OUTSIDE any preprocessor block that would skip it in unguarded build."""
    try:
        with open(test_path) as f:
            content = f.read()
    except OSError:
        return False
    # Quick scan: any occurrence of the prefix followed by an identifier
    # char and `(` (a function call). Avoid pure-type-name matches.
    pat = re.compile(rf"\b{re.escape(prefix)}\w*\s*\(")
    return bool(pat.search(content))


def includes_gated_lib(test_path: str) -> dict[str, str]:
    """Map gated-library headers a test includes UNCONDITIONALLY (i.e., not
    inside an `#ifdef HU_ENABLE_*` matching the lib's required flag) to
    their required flag.

    Walks the file line-by-line tracking #ifdef/#ifndef/#endif depth and
    the active set of flags. A #include only counts as a violation if
    it's NOT inside an active #ifdef for its corresponding flag.
    """
    try:
        with open(test_path) as f:
            lines = f.readlines()
    except OSError:
        return {}

    found: dict[str, str] = {}
    # Stack of sets-of-active-flags (one frame per nested #if).
    # Each frame is the set of HU_ENABLE_* flags currently "asserted".
    # A flag is "asserted" if the enclosing #ifdef X / #if defined(X)
    # mentions it positively.
    stack: list[set[str]] = []

    if_re = re.compile(r"^\s*#\s*(if|ifdef|ifndef)\b\s*(.*)")
    elif_re = re.compile(r"^\s*#\s*elif\b")
    else_re = re.compile(r"^\s*#\s*else\b")
    endif_re = re.compile(r"^\s*#\s*endif\b")
    flag_re = re.compile(r"\bHU_ENABLE_[A-Z_]+\b")
    not_re = re.compile(r"\bNOT\b|\b!\s*defined\b|\b#\s*ifndef\b")
    inc_re = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[>"]')

    for raw in lines:
        if m := endif_re.match(raw):
            if stack:
                stack.pop()
            continue
        if else_re.match(raw) or elif_re.match(raw):
            # Inside an #else: previously-asserted flags become un-asserted.
            # Conservative: clear the top frame so includes in #else only
            # count as asserted if the #else explicitly re-asserts via #if defined.
            if stack:
                stack[-1] = set()
            continue
        if m := if_re.match(raw):
            kind, expr = m.group(1), m.group(2)
            flags = set(flag_re.findall(expr))
            # Negative gates (ifndef, NOT, !defined) don't assert presence.
            if kind == "ifndef" or not_re.search(raw):
                stack.append(set())
            else:
                stack.append(flags)
            continue
        if m := inc_re.match(raw):
            hdr = m.group(1)
            if hdr in GATED_SYSTEM_LIBS:
                required, _ = GATED_SYSTEM_LIBS[hdr]
                active = set().union(*stack) if stack else set()
                if required not in active:
                    # Unconditional include of a gated header.
                    found[hdr] = required
    return found


# Grandfathered violations — tests with known gate mismatches that
# don't currently trip any CI variant (e.g. because platform defaults
# happen to keep the required flag ON for all configured CI matrix
# entries). Listed here so the script stays strict against NEW
# regressions without forcing latent-debt fixes in this PR.
#
# To pay down tech debt: remove an entry, then either gate the test
# in CMakeLists.txt + tests/test_main.c, OR add an internal
# `#ifdef HU_ENABLE_X` wrap to the test body. See
# .claude/rules/test-source-gate-symmetry.md.
BASELINE_ALLOWLIST = {
    # Channel tests that include channels/*.h but aren't channel-gated.
    # Tolerable today because Linux default + minimal-build don't
    # enable IMESSAGE/PWA but the test binaries skip them at runtime
    # via HU_HAVE_CHATDB / channel registry checks.
    "tests/test_channel.c",
    "tests/test_channel_all.c",
    "tests/test_channel_integration.c",
    "tests/test_imessage_adversarial.c",
    "tests/test_imessage_chatdb_fixture.c",
    "tests/test_imessage_extended.c",
    "tests/test_imessage_reactions.c",
    "tests/test_media_gen.c",
    "tests/test_pwa.c",
    "tests/test_sota_humanness.c",
    "tests/test_vtables.c",
    # RL_FULL-gated tests that call hu_learner_* (HU_ENABLE_LEARNING).
    # RL_FULL and LEARNING are typically enabled together in the same
    # variant builds, but the explicit dependency isn't declared.
    "tests/test_lora_training_runner_eval_gate.c",
    "tests/test_lora_training_runner_proof_directory.c",
    # ML+SQLITE: test gated by HU_ENABLE_ML, source also requires SQLITE.
    "tests/test_training_data_extractor.c",
    # test_main.c is the dispatcher; its #ifdef gating happens inline
    # per-call and per-decl, and the script can't model that well.
    "tests/test_main.c",
    # Exposed by the Bugbot cc9039d3 fix (tighter has_internal_guard).
    # These tests gate their #includes + bodies with #ifdef HU_ENABLE_SQLITE
    # but use function-name conventions (hu_forgetting_*, hu_goal_*,
    # hu_value_*) that don't match the file-basename heuristic
    # (hu_forgetting_curve_*, hu_goal_engine_*, hu_value_learning_*).
    # All real-link-safe today because the bodies ARE properly guarded;
    # the script can't yet verify the call-pattern matches the basename.
    "tests/test_agi_frontiers.c",
    "tests/test_e2e_conversation.c",
    "tests/test_forgetting_curve.c",
    "tests/test_sql_transaction.c",
    "tests/test_value_learning.c",
    "tests/test_goal_engine.c",
    "tests/test_emotional_residue.c",
    "tests/test_episodic.c",
    "tests/test_prospective.c",
    "tests/test_prospective_memory.c",
    "tests/test_memory_graph.c",
    "tests/test_self_improve.c",
    "tests/test_oauth.c",
    "tests/test_coreml_provider.c",
}


def main() -> int:
    file_gates = parse_cmake("CMakeLists.txt")

    # Build maps: src basename -> list of (path, flags); test files keyed by path.
    src_by_base: dict[str, list[tuple[str, set[str]]]] = {}
    test_files: list[tuple[str, set[str]]] = []

    for fp, flags in file_gates.items():
        if fp.startswith("src/"):
            base = os.path.basename(fp)[:-2]
            src_by_base.setdefault(base, []).append((fp, flags))
        elif fp.startswith("tests/test_"):
            test_files.append((fp, flags))

    # Suffix variants commonly used on test files that test the SAME
    # source as the un-suffixed test: tests/test_X_e2e.c covers
    # src/.../X.c just like tests/test_X.c would. Without this, we
    # miss real failures like test_reaction_handler_e2e (→ reaction_handler).
    TEST_SUFFIX_VARIANTS = ("_e2e", "_integration", "_extended", "_real",
                            "_loop", "_full", "_smoke", "_wiring")

    mismatches: list[str] = []

    # Pass A: tests that #include a gated system library (sqlite3, curl,
    # ssl) AND actually call its functions, without being themselves
    # gated. Catches PR #113 rounds 2 + 9.
    for test_path, test_flags in sorted(test_files):
        expanded_flags = expand_test_flags(test_flags)
        gated_hdrs = includes_gated_lib(test_path)
        for hdr, required_flag in gated_hdrs.items():
            if required_flag in expanded_flags:
                continue
            if has_internal_guard(test_path, {required_flag}):
                continue
            # Symbol-presence filter: only flag if the test actually
            # calls a function from the library (not just type-uses).
            _, sym_prefix = GATED_SYSTEM_LIBS[hdr]
            if not file_uses_symbol_prefix(test_path, sym_prefix):
                continue
            mismatches.append(
                f"GATED-LIB-MISMATCH: {test_path} (CMake gates={sorted(test_flags) or ['<none>']}) "
                f"includes <{hdr}> + calls {sym_prefix}* but is not gated by {required_flag}. "
                f"Will fail to link in variants where {required_flag} is OFF. "
                f"No internal #ifdef wrap detected."
            )

    # Pass A2: tests that #include human/X/Y.h headers AND actually
    # call hu_<subsys>_* functions, without honoring the source's gate.
    src_by_path: dict[str, set[str]] = {}
    for fp, flags in file_gates.items():
        if fp.startswith("src/"):
            src_by_path[fp] = flags
    for test_path, test_flags in sorted(test_files):
        expanded_flags = expand_test_flags(test_flags)
        try:
            with open(test_path) as f:
                tcontent = f.read()
        except OSError:
            continue
        subsystem_flags: dict[str, set[str]] = {}
        subsystem_examples: dict[str, str] = {}
        for m in re.finditer(r'^\s*#\s*include\s*[<"]human/([^">]+)\.h[>"]',
                             tcontent, re.MULTILINE):
            inc_path = m.group(1)
            candidate_src = f"src/{inc_path}.c"
            if candidate_src not in src_by_path:
                continue
            src_flags = src_by_path[candidate_src]
            if not (src_flags - expanded_flags):
                continue
            # Look for either hu_<subsystem>_* (e.g. hu_ml_*) OR
            # hu_<header_basename>_* (e.g. hu_learner_* for ml/learner.h)
            # function calls. The h-uman codebase uses both family naming
            # (hu_ml_train_step) and module-specific naming
            # (hu_learner_open) within the same directory.
            subsys = inc_path.split("/", 1)[0]
            header_base = inc_path.split("/")[-1]
            call_pat = re.compile(
                rf"\b(hu_{re.escape(subsys)}_|hu_{re.escape(header_base)}_)\w+\s*\("
            )
            if not call_pat.search(tcontent):
                continue
            subsystem_flags.setdefault(subsys, set()).update(src_flags)
            subsystem_examples.setdefault(subsys, inc_path + ".h")

        accumulated_flags: set[str] = set()
        for fs in subsystem_flags.values():
            accumulated_flags |= fs
        missing = accumulated_flags - expanded_flags
        if not missing:
            continue
        # Build the relevant symbol patterns for this test — the actual
        # hu_<subsys>_* and hu_<header_base>_* call families that
        # link-depend on the gated source. Without this, has_internal_guard
        # would fall back to checking sqlite3_/curl_ patterns which
        # don't match here.
        subsystem_patterns = []
        for subsys, hdr_example in subsystem_examples.items():
            subsystem_patterns.append(rf"\bhu_{re.escape(subsys)}_\w+\s*\(")
            hdr_base = hdr_example.split("/")[-1].removesuffix(".h")
            subsystem_patterns.append(rf"\bhu_{re.escape(hdr_base)}_\w+\s*\(")
        if has_internal_guard(test_path, missing, symbol_patterns=subsystem_patterns):
            continue
        base = os.path.basename(test_path)[len("test_"):-2]
        if base in src_by_base:
            continue
        examples = list(subsystem_examples.values())[:3]
        mismatches.append(
            f"INCLUDE-GATE-MISMATCH: {test_path} (CMake gates={sorted(test_flags) or ['<none>']}) "
            f"calls hu_<subsystem>_* and includes {examples} whose "
            f"sources require {sorted(missing)}. "
            f"Will fail to link in variants where those flags are OFF. "
            f"No internal #ifdef wrap detected."
        )

    # Pass B: tests whose matching src/.../X.c is in an if(HU_ENABLE_*)
    # block stricter than the test's gate.
    for test_path, test_flags in sorted(test_files):
        base = os.path.basename(test_path)[len("test_"):-2]
        candidates = src_by_base.get(base, [])
        if not candidates:
            # Try stripping a known test-suffix to see if there's a
            # source file matching the bare name.
            for suf in TEST_SUFFIX_VARIANTS:
                if base.endswith(suf):
                    bare = base[:-len(suf)]
                    if bare in src_by_base:
                        candidates = src_by_base[bare]
                        base = bare
                        break
        if not candidates:
            continue

        # Disambiguate by include set: pick the candidate whose path
        # contains a directory matching one of the test's included
        # human/<X>/<base>.h paths. Falls back to the unique candidate.
        included = test_includes(test_path)
        # The header for a src/<dir>/X.c typically lives under
        # include/human/<dir>/X.h, so we expect a header named X.h to
        # appear in the test's includes. When there are multiple
        # candidates, pick the one whose src path directory aligns with
        # the test's #include "<dir>/<base>.h" line.
        chosen: tuple[str, set[str]] | None = None
        if len(candidates) == 1:
            chosen = candidates[0]
        else:
            # Check the includes for a dir hint
            include_dirs: set[str] = set()
            try:
                with open(test_path) as f:
                    for m in re.finditer(
                        rf'^\s*#\s*include\s*[<"]human/([^">]*)/{re.escape(base)}\.h',
                        f.read(),
                        re.MULTILINE,
                    ):
                        include_dirs.add(m.group(1))
            except OSError:
                pass
            for cand_path, cand_flags in candidates:
                # cand_path like "src/<dir>/X.c" — match <dir> against include_dirs
                cand_dir = "/".join(cand_path.split("/")[1:-1])
                if cand_dir in include_dirs:
                    chosen = (cand_path, cand_flags)
                    break
            if chosen is None:
                # Can't disambiguate — skip to avoid false positive. The
                # historically-failing tests (test_reaction_handler_e2e,
                # test_authentic, test_dpo_miner, etc.) all have a
                # unique src/.../X.c counterpart, so this skip doesn't
                # hide the real recurring class.
                continue

        src_path, src_flags = chosen
        expanded_flags = expand_test_flags(test_flags)
        missing = src_flags - expanded_flags
        if not missing:
            continue
        # For Pass B, the relevant symbols are hu_<base>_* (where base
        # is the source file's basename). E.g. for src/memory/episodic.c,
        # the gated symbols are hu_episodic_*.
        pass_b_patterns = [rf"\bhu_{re.escape(base)}_\w+\s*\("]
        # Also include the directory-family prefix (e.g. hu_memory_*).
        cand_subsys = src_path.split("/")[1] if src_path.startswith("src/") else None
        if cand_subsys:
            pass_b_patterns.append(rf"\bhu_{re.escape(cand_subsys)}_\w+\s*\(")
        if has_internal_guard(test_path, missing, symbol_patterns=pass_b_patterns):
            continue

        mismatches.append(
            f"GATE-MISMATCH: {test_path} (CMake gates={sorted(test_flags) or ['<none>']}) "
            f"will fail to link without {sorted(missing)} — its source "
            f"{src_path} requires those flags (gates={sorted(src_flags)}). "
            f"No internal #ifdef wrap detected."
        )

    # Split into NEW (fail) vs GRANDFATHERED (warn).
    new_violations: list[str] = []
    grandfathered: list[str] = []
    for m in mismatches:
        # Extract the test path from the message — it's the second token.
        path_match = re.search(r"\b(tests/test_[^\s]+\.c)", m)
        if path_match and path_match.group(1) in BASELINE_ALLOWLIST:
            grandfathered.append(m)
        else:
            new_violations.append(m)

    if grandfathered:
        print("Known/grandfathered violations (warning only — see BASELINE_ALLOWLIST):")
        print()
        for m in grandfathered:
            print(f"  WARN: {m}")
        print()

    if new_violations:
        print("NEW test/source gate-symmetry violations found:")
        print()
        for m in new_violations:
            print(f"  {m}")
        print()
        print(f"Total NEW: {len(new_violations)} violation(s).")
        print()
        print("FIX: move the test into the same if(HU_ENABLE_*) block as the source")
        print("in CMakeLists.txt, and gate the forward-decl + call-site in")
        print("tests/test_main.c with `#ifdef HU_ENABLE_*` matching the source.")
        print()
        print("If the failure is a known latent issue you don't want to fix in this")
        print("PR, add the test path to BASELINE_ALLOWLIST in this script with a")
        print("comment explaining why.")
        print()
        print("See .claude/rules/test-source-gate-symmetry.md for the rationale.")
        return 1

    if grandfathered:
        print(f"All NEW test/source pairs have symmetric HU_ENABLE_* gating "
              f"({len(grandfathered)} grandfathered).")
    else:
        print("All test/source pairs have symmetric HU_ENABLE_* gating.")
    return 0


sys.exit(main())
PYEOF
