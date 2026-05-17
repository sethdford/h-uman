#!/usr/bin/env bash
# test_update_formula_hashes.sh — Unit tests for scripts/update-formula-hashes.sh.
#
# Covers:
#   - Happy path: real hashes substituted, version updated, all three archs replaced.
#   - Idempotent re-run: running twice yields the same content.
#   - Error: missing sha256sums.txt input.
#   - Error: malformed sha256sums.txt (no usable hash lines).
#   - Error: sha256sums.txt missing an architecture (partial update refused).
#   - Error: formula file missing.
#   - Error: missing --version / --sums args.
#
# Tests-that-pin-bugs rule: positive path asserts BOTH exit 0 AND the resulting
# formula content (digest + version). Error paths assert exit != 0 AND the
# original formula is left intact (no partial mutation).
#
# Run:
#   tests/test_update_formula_hashes.sh
#
# Exit codes:
#   0   all tests passed
#   1   one or more tests failed

set -u
# Intentionally NOT -e: each test case manages its own exit-code expectations.

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SUT="$SCRIPT_DIR/scripts/update-formula-hashes.sh"

if [[ ! -x "$SUT" ]]; then
  echo "FATAL: $SUT is not executable" >&2
  exit 1
fi

TESTS_RUN=0
TESTS_FAILED=0

# ----- fixtures ---------------------------------------------------------------

make_fixture_formula() {
  # Writes a minimal but structurally-faithful Formula/human.rb to $1.
  cat > "$1" <<'EOF'
class Human < Formula
  desc "test"
  version "0.0.0"

  on_macos do
    if Hardware::CPU.arm?
      url "https://github.com/sethdford/h-uman/releases/download/v0.0.0/human-macos-aarch64.bin"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
  end

  on_linux do
    if Hardware::CPU.arm?
      url "https://github.com/sethdford/h-uman/releases/download/v0.0.0/human-linux-aarch64.bin"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
    if Hardware::CPU.intel?
      url "https://github.com/sethdford/h-uman/releases/download/v0.0.0/human-linux-x86_64.bin"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
  end
end
EOF
}

# Three distinct, valid-shaped digests so we can verify each landed in the right slot.
MAC_HASH="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
ARM_HASH="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
X86_HASH="cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"

make_fixture_sums_full() {
  cat > "$1" <<EOF
${X86_HASH}  human-linux-x86_64.bin
${ARM_HASH}  human-linux-aarch64.bin
${MAC_HASH}  human-macos-aarch64.bin
EOF
}

# ----- assertions -------------------------------------------------------------

fail() {
  echo "  FAIL: $*" >&2
  TESTS_FAILED=$((TESTS_FAILED + 1))
}

assert_eq() {
  local actual="$1" expected="$2" msg="${3:-}"
  if [[ "$actual" != "$expected" ]]; then
    fail "${msg} expected='${expected}' actual='${actual}'"
    return 1
  fi
  return 0
}

assert_contains() {
  local haystack="$1" needle="$2" msg="${3:-}"
  if ! grep -qF -- "$needle" <<< "$haystack"; then
    fail "${msg} needle not found: $needle"
    return 1
  fi
  return 0
}

assert_file_contains() {
  local file="$1" needle="$2" msg="${3:-}"
  if ! grep -qF -- "$needle" "$file"; then
    fail "${msg} file=$file needle not found: $needle"
    return 1
  fi
  return 0
}

assert_file_unchanged() {
  local file="$1" snapshot="$2" msg="${3:-}"
  if ! diff -q "$file" "$snapshot" >/dev/null 2>&1; then
    fail "${msg} file mutated when it should have been left intact: $file"
    return 1
  fi
  return 0
}

start_test() {
  TESTS_RUN=$((TESTS_RUN + 1))
  echo "TEST: $1"
}

# ----- test cases -------------------------------------------------------------

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# --- T1: happy path -----------------------------------------------------------
start_test "happy_path_substitutes_version_and_three_digests"
T1="$WORK/t1"
mkdir -p "$T1"
make_fixture_formula "$T1/formula.rb"
make_fixture_sums_full "$T1/sums.txt"

OUT=$("$SUT" --version 0.5.0 --sums "$T1/sums.txt" --formula "$T1/formula.rb" 2>&1)
RC=$?
assert_eq "$RC" "0" "exit code:"
if [[ $RC -eq 0 ]]; then
  assert_file_contains "$T1/formula.rb" "version \"0.5.0\"" "version line:"
  assert_file_contains "$T1/formula.rb" "sha256 \"${MAC_HASH}\"" "mac digest:"
  assert_file_contains "$T1/formula.rb" "sha256 \"${ARM_HASH}\"" "linux-arm digest:"
  assert_file_contains "$T1/formula.rb" "sha256 \"${X86_HASH}\"" "linux-x86 digest:"
  # URL versions should track the new version too.
  assert_file_contains "$T1/formula.rb" "/v0.5.0/human-macos-aarch64.bin" "url version (mac):"
  assert_file_contains "$T1/formula.rb" "/v0.5.0/human-linux-aarch64.bin" "url version (linux-arm):"
  assert_file_contains "$T1/formula.rb" "/v0.5.0/human-linux-x86_64.bin" "url version (linux-x86):"
  # No placeholder zeros should remain.
  if grep -qE 'sha256[[:space:]]+"0{64}"' "$T1/formula.rb"; then
    fail "placeholder zeros still present in formula"
  fi
fi

# --- T2: idempotent re-run ----------------------------------------------------
start_test "idempotent_rerun_yields_identical_content"
T2="$WORK/t2"
mkdir -p "$T2"
make_fixture_formula "$T2/formula.rb"
make_fixture_sums_full "$T2/sums.txt"

"$SUT" --version 0.5.0 --sums "$T2/sums.txt" --formula "$T2/formula.rb" >/dev/null 2>&1
RC=$?
if [[ $RC -ne 0 ]]; then
  fail "first run exited $RC"
else
  cp "$T2/formula.rb" "$T2/formula.rb.after-first"
  "$SUT" --version 0.5.0 --sums "$T2/sums.txt" --formula "$T2/formula.rb" >/dev/null 2>&1
  RC2=$?
  assert_eq "$RC2" "0" "second run exit code:"
  if ! diff -q "$T2/formula.rb" "$T2/formula.rb.after-first" >/dev/null 2>&1; then
    fail "second run produced different content (not idempotent)"
  fi
fi

# --- T3: missing sums file ----------------------------------------------------
start_test "missing_sums_file_exits_nonzero_and_leaves_formula_intact"
T3="$WORK/t3"
mkdir -p "$T3"
make_fixture_formula "$T3/formula.rb"
cp "$T3/formula.rb" "$T3/formula.rb.snapshot"

OUT=$("$SUT" --version 0.5.0 --sums "$T3/does-not-exist.txt" --formula "$T3/formula.rb" 2>&1)
RC=$?
if [[ $RC -eq 0 ]]; then
  fail "expected non-zero exit; got 0"
fi
assert_contains "$OUT" "not readable" "error message:"
assert_file_unchanged "$T3/formula.rb" "$T3/formula.rb.snapshot" "formula on missing sums:"

# --- T4: malformed sums file --------------------------------------------------
start_test "malformed_sums_file_exits_nonzero_and_leaves_formula_intact"
T4="$WORK/t4"
mkdir -p "$T4"
make_fixture_formula "$T4/formula.rb"
cp "$T4/formula.rb" "$T4/formula.rb.snapshot"
cat > "$T4/sums.txt" <<'EOF'
# just a comment, no actual hashes
not-a-hash  some-file.bin
EOF

OUT=$("$SUT" --version 0.5.0 --sums "$T4/sums.txt" --formula "$T4/formula.rb" 2>&1)
RC=$?
if [[ $RC -eq 0 ]]; then
  fail "expected non-zero exit; got 0"
fi
assert_contains "$OUT" "no usable lines" "error message:"
assert_file_unchanged "$T4/formula.rb" "$T4/formula.rb.snapshot" "formula on malformed sums:"

# --- T5: missing architecture in sums file (partial-update refusal) ----------
start_test "missing_arch_in_sums_refuses_partial_update"
T5="$WORK/t5"
mkdir -p "$T5"
make_fixture_formula "$T5/formula.rb"
cp "$T5/formula.rb" "$T5/formula.rb.snapshot"
# Only two of three architectures provided.
cat > "$T5/sums.txt" <<EOF
${X86_HASH}  human-linux-x86_64.bin
${MAC_HASH}  human-macos-aarch64.bin
EOF

OUT=$("$SUT" --version 0.5.0 --sums "$T5/sums.txt" --formula "$T5/formula.rb" 2>&1)
RC=$?
if [[ $RC -eq 0 ]]; then
  fail "expected non-zero exit; got 0"
fi
assert_contains "$OUT" "human-linux-aarch64.bin not found" "error names missing arch:"
assert_file_unchanged "$T5/formula.rb" "$T5/formula.rb.snapshot" "formula on partial sums:"

# --- T6: missing formula file -------------------------------------------------
start_test "missing_formula_file_exits_nonzero"
T6="$WORK/t6"
mkdir -p "$T6"
make_fixture_sums_full "$T6/sums.txt"

OUT=$("$SUT" --version 0.5.0 --sums "$T6/sums.txt" --formula "$T6/missing.rb" 2>&1)
RC=$?
if [[ $RC -eq 0 ]]; then
  fail "expected non-zero exit; got 0"
fi
assert_contains "$OUT" "formula file not found" "error message:"

# --- T7: missing required args ------------------------------------------------
start_test "missing_version_arg_exits_nonzero"
OUT=$("$SUT" --sums /tmp/whatever 2>&1)
RC=$?
if [[ $RC -eq 0 ]]; then
  fail "expected non-zero exit; got 0"
fi
assert_contains "$OUT" "required" "error message:"

start_test "missing_sums_arg_exits_nonzero"
OUT=$("$SUT" --version 0.5.0 2>&1)
RC=$?
if [[ $RC -eq 0 ]]; then
  fail "expected non-zero exit; got 0"
fi
assert_contains "$OUT" "required" "error message:"

# --- T8: invalid semver -------------------------------------------------------
start_test "invalid_semver_exits_nonzero_and_leaves_formula_intact"
T8="$WORK/t8"
mkdir -p "$T8"
make_fixture_formula "$T8/formula.rb"
cp "$T8/formula.rb" "$T8/formula.rb.snapshot"
make_fixture_sums_full "$T8/sums.txt"

OUT=$("$SUT" --version "not-a-version" --sums "$T8/sums.txt" --formula "$T8/formula.rb" 2>&1)
RC=$?
if [[ $RC -eq 0 ]]; then
  fail "expected non-zero exit; got 0"
fi
assert_contains "$OUT" "semver" "error mentions semver:"
assert_file_unchanged "$T8/formula.rb" "$T8/formula.rb.snapshot" "formula on bad version:"

# --- T9: leading 'v' in version is stripped -----------------------------------
start_test "leading_v_in_version_is_stripped"
T9="$WORK/t9"
mkdir -p "$T9"
make_fixture_formula "$T9/formula.rb"
make_fixture_sums_full "$T9/sums.txt"

"$SUT" --version "v1.2.3" --sums "$T9/sums.txt" --formula "$T9/formula.rb" >/dev/null 2>&1
RC=$?
assert_eq "$RC" "0" "exit code:"
if [[ $RC -eq 0 ]]; then
  assert_file_contains "$T9/formula.rb" "version \"1.2.3\"" "version line (stripped v):"
  if grep -qF 'version "v1.2.3"' "$T9/formula.rb"; then
    fail "formula contains literal 'v1.2.3' — leading 'v' should have been stripped"
  fi
fi

# ----- summary ----------------------------------------------------------------

echo
echo "----------------------------------------------------------------"
echo "tests run:    $TESTS_RUN"
echo "tests failed: $TESTS_FAILED"
if [[ $TESTS_FAILED -gt 0 ]]; then
  echo "RESULT: FAIL"
  exit 1
fi
echo "RESULT: PASS"
exit 0
