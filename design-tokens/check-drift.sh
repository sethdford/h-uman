#!/usr/bin/env bash
# Compares generated token outputs against committed versions to detect manual edits/drift.
# Exit 1 if drift detected, 0 if clean.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Rebuild tokens into a temp directory
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

cd "$SCRIPT_DIR"
npx tsx build.ts --outdir "$TMPDIR" 2>/dev/null || {
  echo "Error: Token build failed"
  exit 1
}

# Format generated CSS to match committed (prettier) version
# Use ui/.prettierrc so output matches postbuild formatting
if [ -f "$TMPDIR/_tokens.css" ] && [ -f "$REPO_ROOT/ui/.prettierrc" ]; then
  npx -y prettier --write --config "$REPO_ROOT/ui/.prettierrc" "$TMPDIR/_tokens.css" 2>/dev/null || true
fi

DRIFT=0

# Check CSS output
if ! diff -q "$TMPDIR/_tokens.css" "$REPO_ROOT/ui/src/styles/_tokens.css" >/dev/null 2>&1; then
  echo "DRIFT: ui/src/styles/_tokens.css differs from generated output"
  diff "$TMPDIR/_tokens.css" "$REPO_ROOT/ui/src/styles/_tokens.css" || true
  DRIFT=1
fi

# Check website CSS output
if ! diff -q "$TMPDIR/_tokens.css" "$REPO_ROOT/website/src/styles/_tokens.css" >/dev/null 2>&1; then
  echo "DRIFT: website/src/styles/_tokens.css differs from generated output"
  DRIFT=1
fi

# Check C header output (compare raw build.ts output — no clang-format, since
# version differences between CI and local create false drift positives)
if ! diff -q "$TMPDIR/design_tokens.h" "$REPO_ROOT/include/human/design_tokens.h" >/dev/null 2>&1; then
  echo "DRIFT: include/human/design_tokens.h differs from generated output"
  DRIFT=1
fi

# Check Swift output
if [ -f "$TMPDIR/DesignTokens.swift" ] && [ -f "$REPO_ROOT/apps/shared/HumanKit/Sources/HumanChatUI/DesignTokens.swift" ]; then
  if ! diff -q "$TMPDIR/DesignTokens.swift" "$REPO_ROOT/apps/shared/HumanKit/Sources/HumanChatUI/DesignTokens.swift" >/dev/null 2>&1; then
    echo "DRIFT: apps/shared/.../DesignTokens.swift differs from generated output"
    DRIFT=1
  fi
fi

# Check Kotlin output
if [ -f "$TMPDIR/DesignTokens.kt" ] && [ -f "$REPO_ROOT/apps/android/app/src/main/java/ai/human/app/ui/DesignTokens.kt" ]; then
  if ! diff -q "$TMPDIR/DesignTokens.kt" "$REPO_ROOT/apps/android/app/src/main/java/ai/human/app/ui/DesignTokens.kt" >/dev/null 2>&1; then
    echo "DRIFT: apps/android/.../DesignTokens.kt differs from generated output"
    DRIFT=1
  fi
fi

# Check runtime JSON output
if [ -f "$TMPDIR/tokens.json" ] && [ -f "$REPO_ROOT/docs/tokens.json" ]; then
  if ! diff -q "$TMPDIR/tokens.json" "$REPO_ROOT/docs/tokens.json" >/dev/null 2>&1; then
    echo "DRIFT: docs/tokens.json differs from generated output"
    DRIFT=1
  fi
fi

# Check TypeScript output
if [ -f "$TMPDIR/tokens.ts" ] && [ -f "$REPO_ROOT/docs/tokens.ts" ]; then
  if ! diff -q "$TMPDIR/tokens.ts" "$REPO_ROOT/docs/tokens.ts" >/dev/null 2>&1; then
    echo "DRIFT: docs/tokens.ts differs from generated output"
    DRIFT=1
  fi
fi

# Check docs reference JSON (ignore "generated" timestamp which changes every build)
if [ -f "$TMPDIR/design-tokens-reference.json" ] && [ -f "$REPO_ROOT/docs/design-tokens-reference.json" ]; then
  if ! diff -I '"generated":' "$TMPDIR/design-tokens-reference.json" "$REPO_ROOT/docs/design-tokens-reference.json" >/dev/null 2>&1; then
    echo "DRIFT: docs/design-tokens-reference.json differs from generated output"
    DRIFT=1
  fi
fi

# Check Tokens Studio export (not emitted by build.ts; has its own generator)
if ! npx tsx sync-tokens-studio.ts --out "$TMPDIR/tokens-studio.json" >/dev/null 2>&1; then
  echo "Error: sync-tokens-studio.ts failed"
  exit 1
fi
if ! diff -q "$TMPDIR/tokens-studio.json" "$REPO_ROOT/docs/tokens-studio.json" >/dev/null 2>&1; then
  echo "DRIFT: docs/tokens-studio.json differs from generated output (run: cd design-tokens && npm run sync:tokens-studio)"
  DRIFT=1
fi

if [ "$DRIFT" -eq 1 ]; then
  echo ""
  echo "Token drift detected! Run 'cd design-tokens && npm run build && npm run sync:tokens-studio' to regenerate."
  exit 1
else
  echo "No token drift detected."
  exit 0
fi
