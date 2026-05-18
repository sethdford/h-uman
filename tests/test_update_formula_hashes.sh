#!/usr/bin/env bash
# test_update_formula_hashes.sh — shell-driven test suite for
# scripts/update-formula-hashes.sh. Covers the 10+ cases enumerated
# in sprints/sprint-43/designs/US-43.1.md.
#
# Each case sets up a tmpdir with a stub formula + a sha256sums.txt
# fixture, invokes the script, and asserts both the exit code AND
# the resulting formula contents (or unchanged-on-failure).
#
# Following ~/.claude/rules/tests-that-pin-bugs.md: every error-path
# test asserts BOTH (a) exit != 0 AND (b) formula left byte-identical
# to the input. Both legs must hold or the test fails.
set -euo pipefail
IFS=$'\n\t'

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
SCRIPT="$REPO/scripts/update-formula-hashes.sh"

if [ ! -x "$SCRIPT" ]; then
    if [ -f "$SCRIPT" ]; then
        chmod +x "$SCRIPT"
    else
        printf 'FATAL: script not found at %s\n' "$SCRIPT" >&2
        exit 99
    fi
fi

PASS=0
FAIL=0
FAILED_CASES=""

# --- helpers ----------------------------------------------------------------

# Returns a path to a fresh tmpdir that will be cleaned up on script exit.
TMPDIRS=()
cleanup() {
    local d
    for d in "${TMPDIRS[@]:-}"; do
        if [ -n "$d" ] && [ -d "$d" ]; then
            rm -rf "$d"
        fi
    done
    return 0
}
trap cleanup EXIT

mktmp() {
    local d
    d="$(mktemp -d "${TMPDIR:-/tmp}/htest.XXXXXX")"
    TMPDIRS+=("$d")
    printf '%s' "$d"
}

# Stub formula. Mirrors the real Formula/human.rb structure for the
# three URL/sha256 pairs we mutate. The leading comments and trailing
# Ruby method bodies are NOT touched by the script and serve as a
# comment-preservation fixture.
write_stub_formula() {
    local path="$1"
    cat > "$path" <<'RUBY'
# typed: false
# frozen_string_literal: true

# IMPORTANT: this comment must be preserved verbatim by the updater.
class Human < Formula
  desc "The smallest fully autonomous AI assistant infrastructure"
  homepage "https://h-uman.ai"
  license "MIT"
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

  # TODO(US-42.4): preserved comment near where bottle do lands.
  head "https://github.com/sethdford/h-uman.git", branch: "main"

  def install
    # body intentionally elided in stub
  end
end
RUBY
}

write_full_sha_file() {
    local path="$1"
    cat > "$path" <<'SHAS'
1111111111111111111111111111111111111111111111111111111111111111  human-macos-aarch64.bin
2222222222222222222222222222222222222222222222222222222222222222  human-linux-aarch64.bin
3333333333333333333333333333333333333333333333333333333333333333  human-linux-x86_64.bin
SHAS
}

assert_contains() {
    local file="$1" needle="$2" msg="$3"
    if ! grep -q -- "$needle" "$file"; then
        printf '  FAIL: %s\n    %s does not contain: %s\n' "$msg" "$file" "$needle" >&2
        return 1
    fi
}

assert_not_contains() {
    local file="$1" needle="$2" msg="$3"
    if grep -q -- "$needle" "$file"; then
        printf '  FAIL: %s\n    %s unexpectedly contains: %s\n' "$msg" "$file" "$needle" >&2
        return 1
    fi
}

assert_files_equal() {
    local a="$1" b="$2" msg="$3"
    if ! cmp -s "$a" "$b"; then
        printf '  FAIL: %s\n    files differ: %s vs %s\n' "$msg" "$a" "$b" >&2
        return 1
    fi
}

run_case() {
    local name="$1"; shift
    local rc=0
    if "$@"; then
        PASS=$((PASS + 1))
        printf 'PASS  %s\n' "$name"
    else
        rc=$?
        FAIL=$((FAIL + 1))
        FAILED_CASES="$FAILED_CASES $name"
        printf 'FAIL  %s (rc=%d)\n' "$name" "$rc"
    fi
}

# --- cases ------------------------------------------------------------------

case_happy_path_all_three_archs() {
    local d; d="$(mktmp)"
    local formula="$d/human.rb"
    local sha_file="$d/sha256sums.txt"
    write_stub_formula "$formula"
    write_full_sha_file "$sha_file"

    "$SCRIPT" --formula "$formula" --version "1.2.3" --sha256sums "$sha_file" >/dev/null

    assert_contains "$formula" 'version "1.2.3"' "version line updated" || return 1
    assert_contains "$formula" '"1111111111111111111111111111111111111111111111111111111111111111"' "macos-aarch64 sha applied" || return 1
    assert_contains "$formula" '"2222222222222222222222222222222222222222222222222222222222222222"' "linux-aarch64 sha applied" || return 1
    assert_contains "$formula" '"3333333333333333333333333333333333333333333333333333333333333333"' "linux-x86_64 sha applied" || return 1
    assert_not_contains "$formula" '"0000000000000000000000000000000000000000000000000000000000000000"' "no placeholder zero-sha remains" || return 1
    assert_contains "$formula" "/v1.2.3/human-macos-aarch64.bin" "url path version rewritten" || return 1
}

case_idempotent_rerun_byte_identical() {
    local d; d="$(mktmp)"
    local formula="$d/human.rb"
    local sha_file="$d/sha256sums.txt"
    write_stub_formula "$formula"
    write_full_sha_file "$sha_file"

    "$SCRIPT" --formula "$formula" --version "1.2.3" --sha256sums "$sha_file" >/dev/null
    cp "$formula" "$d/after-first.rb"
    "$SCRIPT" --formula "$formula" --version "1.2.3" --sha256sums "$sha_file" >/dev/null

    assert_files_equal "$formula" "$d/after-first.rb" "re-run produces byte-identical output" || return 1
}

case_partial_sha256sums_missing_one_arch() {
    local d; d="$(mktmp)"
    local formula="$d/human.rb"
    local sha_file="$d/sha256sums.txt"
    write_stub_formula "$formula"
    # Only two of three archs:
    cat > "$sha_file" <<'SHAS'
1111111111111111111111111111111111111111111111111111111111111111  human-macos-aarch64.bin
2222222222222222222222222222222222222222222222222222222222222222  human-linux-aarch64.bin
SHAS
    cp "$formula" "$d/before.rb"

    if "$SCRIPT" --formula "$formula" --version "1.2.3" --sha256sums "$sha_file" >/dev/null 2>&1; then
        printf '  FAIL: script should have exited non-zero on partial sha256sums\n' >&2
        return 1
    fi
    assert_files_equal "$formula" "$d/before.rb" "formula unchanged on partial sha256sums" || return 1
}

case_malformed_semver_rejected() {
    local d; d="$(mktmp)"
    local formula="$d/human.rb"
    local sha_file="$d/sha256sums.txt"
    write_stub_formula "$formula"
    write_full_sha_file "$sha_file"
    cp "$formula" "$d/before.rb"

    # Each of these must be rejected:
    local bad
    for bad in "1.2" "1.2.3.4" "v1.2.3" "1.2.3-" "abc.def.ghi" "1..2.3" ""; do
        if "$SCRIPT" --formula "$formula" --version "$bad" --sha256sums "$sha_file" >/dev/null 2>&1; then
            printf '  FAIL: bad semver accepted: %q\n' "$bad" >&2
            return 1
        fi
    done
    assert_files_equal "$formula" "$d/before.rb" "formula unchanged for all malformed semvers" || return 1

    # Prerelease forms must be accepted:
    "$SCRIPT" --formula "$formula" --version "1.2.3-rc.1" --sha256sums "$sha_file" >/dev/null
    assert_contains "$formula" 'version "1.2.3-rc.1"' "prerelease semver accepted" || return 1
}

case_zero_byte_sha_file_rejected() {
    local d; d="$(mktmp)"
    local formula="$d/human.rb"
    local sha_file="$d/sha256sums.txt"
    write_stub_formula "$formula"
    : > "$sha_file"
    cp "$formula" "$d/before.rb"

    if "$SCRIPT" --formula "$formula" --version "1.2.3" --sha256sums "$sha_file" >/dev/null 2>&1; then
        printf '  FAIL: empty sha file should have been rejected\n' >&2
        return 1
    fi
    assert_files_equal "$formula" "$d/before.rb" "formula unchanged on zero-byte sha file" || return 1
}

case_missing_required_args_exits_nonzero() {
    local d; d="$(mktmp)"
    local formula="$d/human.rb"
    local sha_file="$d/sha256sums.txt"
    write_stub_formula "$formula"
    write_full_sha_file "$sha_file"

    if "$SCRIPT" --formula "$formula" --version "1.2.3" >/dev/null 2>&1; then
        printf '  FAIL: missing --sha256sums should be rejected\n' >&2
        return 1
    fi
    if "$SCRIPT" --formula "$formula" --sha256sums "$sha_file" >/dev/null 2>&1; then
        printf '  FAIL: missing --version should be rejected\n' >&2
        return 1
    fi
    if "$SCRIPT" --version "1.2.3" --sha256sums "$sha_file" >/dev/null 2>&1; then
        printf '  FAIL: missing --formula should be rejected\n' >&2
        return 1
    fi
    if "$SCRIPT" >/dev/null 2>&1; then
        printf '  FAIL: no args should be rejected\n' >&2
        return 1
    fi
}

case_comment_and_method_preservation() {
    local d; d="$(mktmp)"
    local formula="$d/human.rb"
    local sha_file="$d/sha256sums.txt"
    write_stub_formula "$formula"
    write_full_sha_file "$sha_file"

    "$SCRIPT" --formula "$formula" --version "1.2.3" --sha256sums "$sha_file" >/dev/null

    assert_contains "$formula" "IMPORTANT: this comment must be preserved verbatim by the updater." "leading comment preserved" || return 1
    assert_contains "$formula" "TODO(US-42.4): preserved comment near where bottle do lands." "TODO comment preserved" || return 1
    assert_contains "$formula" "def install" "def install body preserved" || return 1
    assert_contains "$formula" "body intentionally elided in stub" "comment inside method preserved" || return 1
    assert_contains "$formula" "# frozen_string_literal: true" "frozen literal directive preserved" || return 1
}

case_file_unchanged_on_malformed_sha_entry() {
    local d; d="$(mktmp)"
    local formula="$d/human.rb"
    local sha_file="$d/sha256sums.txt"
    write_stub_formula "$formula"
    # Malformed sha (too short) for one arch:
    cat > "$sha_file" <<'SHAS'
deadbeef  human-macos-aarch64.bin
2222222222222222222222222222222222222222222222222222222222222222  human-linux-aarch64.bin
3333333333333333333333333333333333333333333333333333333333333333  human-linux-x86_64.bin
SHAS
    cp "$formula" "$d/before.rb"

    if "$SCRIPT" --formula "$formula" --version "1.2.3" --sha256sums "$sha_file" >/dev/null 2>&1; then
        printf '  FAIL: malformed sha entry should be rejected\n' >&2
        return 1
    fi
    assert_files_equal "$formula" "$d/before.rb" "formula unchanged on malformed sha entry" || return 1
}

case_parse_failure_rejection() {
    # Simulate a broken formula (unbalanced quote). The updater must
    # refuse to write the result if `ruby -c` would fail.
    if ! command -v ruby >/dev/null 2>&1; then
        printf '  SKIP: ruby not available; parse-failure rejection unverified\n'
        return 0
    fi

    local d; d="$(mktmp)"
    local formula="$d/human.rb"
    local sha_file="$d/sha256sums.txt"
    # Formula with a deliberate Ruby syntax error in a section the
    # updater does NOT touch (so the broken syntax survives into the
    # tempfile and triggers `ruby -c` failure).
    cat > "$formula" <<'RUBY'
# typed: false
class Human < Formula
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
  def install
    "unterminated string
  end
end
RUBY
    write_full_sha_file "$sha_file"
    cp "$formula" "$d/before.rb"

    if "$SCRIPT" --formula "$formula" --version "1.2.3" --sha256sums "$sha_file" >/dev/null 2>&1; then
        printf '  FAIL: ruby-parse-failing formula should be rejected\n' >&2
        return 1
    fi
    assert_files_equal "$formula" "$d/before.rb" "formula unchanged on ruby parse failure" || return 1
}

case_version_only_no_sha_rejected() {
    # If the sha256sums file is missing ALL three required entries
    # (e.g. only contains unrelated artifacts), reject without
    # touching the formula even if the version is valid.
    local d; d="$(mktmp)"
    local formula="$d/human.rb"
    local sha_file="$d/sha256sums.txt"
    write_stub_formula "$formula"
    cat > "$sha_file" <<'SHAS'
aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  some-other-artifact.tar.gz
SHAS
    cp "$formula" "$d/before.rb"

    if "$SCRIPT" --formula "$formula" --version "1.2.3" --sha256sums "$sha_file" >/dev/null 2>&1; then
        printf '  FAIL: sha file with no required entries should be rejected\n' >&2
        return 1
    fi
    assert_files_equal "$formula" "$d/before.rb" "formula unchanged when no required entries present" || return 1
}

case_nonexistent_formula_path_rejected() {
    local d; d="$(mktmp)"
    local sha_file="$d/sha256sums.txt"
    write_full_sha_file "$sha_file"

    if "$SCRIPT" --formula "$d/does-not-exist.rb" --version "1.2.3" --sha256sums "$sha_file" >/dev/null 2>&1; then
        printf '  FAIL: nonexistent formula path should be rejected\n' >&2
        return 1
    fi
}

case_nonexistent_sha_file_rejected() {
    local d; d="$(mktmp)"
    local formula="$d/human.rb"
    write_stub_formula "$formula"
    cp "$formula" "$d/before.rb"

    if "$SCRIPT" --formula "$formula" --version "1.2.3" --sha256sums "$d/missing.txt" >/dev/null 2>&1; then
        printf '  FAIL: nonexistent sha file path should be rejected\n' >&2
        return 1
    fi
    assert_files_equal "$formula" "$d/before.rb" "formula unchanged on missing sha file" || return 1
}

# --- run --------------------------------------------------------------------

printf 'running tests against %s\n\n' "$SCRIPT"

run_case "happy_path_all_three_archs"          case_happy_path_all_three_archs
run_case "idempotent_rerun_byte_identical"     case_idempotent_rerun_byte_identical
run_case "partial_sha256sums_missing_one_arch" case_partial_sha256sums_missing_one_arch
run_case "malformed_semver_rejected"           case_malformed_semver_rejected
run_case "zero_byte_sha_file_rejected"         case_zero_byte_sha_file_rejected
run_case "missing_required_args_exits_nonzero" case_missing_required_args_exits_nonzero
run_case "comment_and_method_preservation"     case_comment_and_method_preservation
run_case "file_unchanged_on_malformed_sha"     case_file_unchanged_on_malformed_sha_entry
run_case "parse_failure_rejection"             case_parse_failure_rejection
run_case "version_only_no_sha_rejected"        case_version_only_no_sha_rejected
run_case "nonexistent_formula_path_rejected"   case_nonexistent_formula_path_rejected
run_case "nonexistent_sha_file_rejected"       case_nonexistent_sha_file_rejected

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
if [ "$FAIL" -gt 0 ]; then
    printf 'failed:%s\n' "$FAILED_CASES" >&2
    exit 1
fi
exit 0
