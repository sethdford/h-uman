#!/usr/bin/env bash
# Agent pre-flight check: runs the right validations based on what changed.
# Usage: scripts/agent-preflight.sh [--full]
#   --full  Run all checks regardless of what changed
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

FULL=0
if [ "${1:-}" = "--full" ]; then
    FULL=1
fi

ERRORS=0

pass() { printf "${GREEN}PASS${NC}  %s\n" "$1"; }
fail() { printf "${RED}FAIL${NC}  %s\n" "$1"; ERRORS=$((ERRORS + 1)); }
skip() { printf "${YELLOW}SKIP${NC}  %s\n" "$1"; }
info() { printf "      %s\n" "$1"; }

changed_files() {
    { git diff --name-only HEAD 2>/dev/null; git diff --cached --name-only 2>/dev/null; } | sort -u
}

has_changes_in() {
    local pattern="$1"
    changed_files | grep -qE "$pattern"
}

C_CHANGED=0
UI_CHANGED=0
TOKENS_CHANGED=0
WEBSITE_CHANGED=0

if [ "$FULL" -eq 1 ]; then
    C_CHANGED=1; UI_CHANGED=1; TOKENS_CHANGED=1; WEBSITE_CHANGED=1; APPS_CHANGED=1; DOCS_CHANGED=1
else
    has_changes_in '\.c$|\.h$|CMakeLists' && C_CHANGED=1 || true
    has_changes_in '^ui/' && UI_CHANGED=1 || true
    has_changes_in '^design-tokens/' && TOKENS_CHANGED=1 || true
    has_changes_in '^website/' && WEBSITE_CHANGED=1 || true
    has_changes_in '^apps/' && APPS_CHANGED=1 || true
    has_changes_in '^docs/|^human-skills/|^skill-registry/|\.md$|scripts/(check_markdown|check-docs|check-human-skills|doc-fleet)\.sh' && DOCS_CHANGED=1 || true
fi

printf "\n=== Agent Pre-Flight Check ===\n"
printf "C=%d  UI=%d  Tokens=%d  Website=%d  Apps=%d  Docs=%d  Full=%d\n\n" \
    "$C_CHANGED" "$UI_CHANGED" "$TOKENS_CHANGED" "$WEBSITE_CHANGED" "${APPS_CHANGED:-0}" "${DOCS_CHANGED:-0}" "$FULL"

if [ "$C_CHANGED" -eq 1 ]; then
    printf '%s\n' "--- C Backend ---"

    if cmake --build build -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" 2>&1 | tail -3; then
        pass "C build"
    else
        fail "C build"
    fi

    if [ -f scripts/check-memory-v2-header-collision.sh ]; then
        if bash scripts/check-memory-v2-header-collision.sh; then
            pass "memory-v2 header collision (src/)"
        else
            fail "memory-v2 header collision (src/)"
        fi
    fi

    if [ -f scripts/check-memory-query-variant.sh ]; then
        if bash scripts/check-memory-query-variant.sh; then
            pass "memory query variant scan (src+tests)"
        else
            fail "memory query variant scan (explicit .variant after memset)"
        fi
    fi

    if [ "$FULL" -eq 1 ]; then
        if ./build/human_tests 2>&1 | tail -3; then
            pass "Full test suite"
        else
            fail "Full test suite"
        fi
    else
        SUITES=$(./scripts/what-to-test.sh $(changed_files | grep -E '\.c$|\.h$') 2>/dev/null || true)
        if [ -n "$SUITES" ]; then
            info "Running targeted tests: $SUITES"
            if ./build/human_tests $SUITES 2>&1 | tail -5; then
                pass "Targeted tests"
            else
                fail "Targeted tests"
            fi
        else
            info "No test mapping found — running full suite"
            if ./build/human_tests 2>&1 | tail -3; then
                pass "Full test suite"
            else
                fail "Full test suite"
            fi
        fi
    fi

    C_FILES=$(changed_files | grep -E '\.(c|h)$' || true)
    if [ -n "$C_FILES" ]; then
        FORMAT_ISSUES=$(echo "$C_FILES" | xargs clang-format --dry-run 2>&1 | grep -c "warning:" || true)
        if [ "$FORMAT_ISSUES" -eq 0 ]; then
            pass "clang-format"
        else
            fail "clang-format ($FORMAT_ISSUES files need formatting)"
        fi
    fi
    printf "\n"
fi

if [ "$UI_CHANGED" -eq 1 ]; then
    printf '%s\n' "--- UI Dashboard ---"

    if (cd ui && npm run typecheck 2>&1 | tail -3); then
        pass "TypeScript typecheck"
    else
        fail "TypeScript typecheck"
    fi

    if (cd ui && npm test 2>&1 | tail -3); then
        pass "UI tests"
    else
        fail "UI tests"
    fi

    if (cd ui && npm run lint:tokens 2>&1 | tail -3); then
        pass "Token lint"
    else
        fail "Token lint"
    fi
    printf "\n"
fi

if [ "$TOKENS_CHANGED" -eq 1 ]; then
    printf '%s\n' "--- Design Tokens ---"
    if design-tokens/check-drift.sh 2>&1 | tail -3; then
        pass "Token drift check"
    else
        fail "Token drift check"
    fi
    printf "\n"
fi

if [ "$WEBSITE_CHANGED" -eq 1 ]; then
    printf '%s\n' "--- Website ---"
    if (cd website && npm run build 2>&1 | tail -3); then
        pass "Website build"
    else
        fail "Website build"
    fi
    printf "\n"
fi

if [ "${APPS_CHANGED:-0}" -eq 1 ]; then
    printf '%s\n' "--- Native apps (apps/) ---"
    if (cd apps/shared/HumanKit && swift test 2>&1 | tail -5); then
        pass "HumanKit swift test"
    else
        fail "HumanKit swift test"
    fi
    if (cd apps/ios && swift build 2>&1 | tail -3); then
        pass "iOS swift build"
    else
        fail "iOS swift build"
    fi
    if (cd apps/macos && swift build 2>&1 | tail -3); then
        pass "macOS swift build"
    else
        fail "macOS swift build"
    fi
    if command -v java &>/dev/null; then
        if (cd apps/android && chmod +x gradlew && ./gradlew assembleDebug lint test --no-daemon 2>&1 | tail -8); then
            pass "Android assemble + lint + unit tests"
        else
            fail "Android assemble + lint + unit tests"
        fi
    else
        skip "Android Gradle (no java on PATH)"
    fi
    info "For XCUITest + emulator matrix see .github/workflows/native-apps-fleet.yml"
    info "Local: scripts/run-native-fleet-local.sh quick | full"
    printf "\n"
fi

if [ "${DOCS_CHANGED:-0}" -eq 1 ]; then
    printf '%s\n' "--- Documentation (doc fleet) ---"
    if bash scripts/doc-fleet.sh; then
        pass "Doc fleet"
    else
        fail "Doc fleet"
    fi
    printf "\n"
fi

printf "=== Summary ===\n"
if [ "$ERRORS" -eq 0 ]; then
    printf "${GREEN}All checks passed.${NC}\n"
    exit 0
else
    printf "${RED}%d check(s) failed.${NC}\n" "$ERRORS"
    exit 1
fi
