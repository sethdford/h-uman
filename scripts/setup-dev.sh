#!/usr/bin/env bash
# setup-dev.sh — one-time dev environment setup for human
set -euo pipefail

echo "Setting up human development environment..."

# 1. Activate git hooks
echo "  Activating git hooks (.githooks/)..."
git config core.hooksPath .githooks
echo "  Done: pre-commit, pre-push, commit-msg hooks active."

# 2. Check required tools
MISSING=()

if ! command -v cmake &>/dev/null; then
  MISSING+=("cmake")
fi

if ! command -v clang-format &>/dev/null; then
  # Check Homebrew paths on macOS
  if ! [ -x /opt/homebrew/opt/llvm/bin/clang-format ] && ! [ -x /usr/local/opt/llvm/bin/clang-format ]; then
    MISSING+=("clang-format")
  fi
fi

if ! command -v node &>/dev/null; then
  MISSING+=("node (v22+)")
fi

if [ ${#MISSING[@]} -gt 0 ]; then
  echo ""
  echo "  Warning: missing tools: ${MISSING[*]}"
  echo "  Install them before building."
else
  echo "  All required tools found."
fi

# 3. Install UI deps if package-lock exists
if [ -f "ui/package-lock.json" ]; then
  echo "  Installing UI dependencies..."
  (cd ui && npm ci --silent 2>/dev/null) || echo "  Warning: npm ci failed for ui/"
fi

# 4. Install design-token deps
if [ -f "design-tokens/package-lock.json" ]; then
  echo "  Installing design-token dependencies..."
  (cd design-tokens && npm ci --silent 2>/dev/null) || echo "  Warning: npm ci failed for design-tokens/"
fi

echo ""
echo "Setup complete. Build with:"
echo "  mkdir -p build && cd build"
echo "  cmake .. -DSC_ENABLE_ALL_CHANNELS=ON"
echo "  cmake --build . -j\$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
echo "  ./human_tests"
