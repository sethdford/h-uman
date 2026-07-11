#!/bin/bash
#
# Integration test for DPO training quality gate
#
# Tests that:
# 1. Results are recorded to JSONL
# 2. Regression verdict is computed
# 3. Exit code reflects PASS/FAIL status
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEST_TMP=$(mktemp -d)
trap "rm -rf '$TEST_TMP'" EXIT INT TERM

echo "=== DPO Training Integration Test ==="
echo "Test directory: $TEST_TMP"

# Create mock training output
MOCK_TRAIN_LOG="$TEST_TMP/train.log"
cat > "$MOCK_TRAIN_LOG" <<'EOF'
[dpo_mlx_train] starting (model=test-model, iters=100, ...)
Iter 1: loss 0.8234, chosen_r 10.234, rejected_r 10.456, acc 0.500
Iter 2: loss 0.7923, chosen_r 11.234, rejected_r 10.856, acc 0.600
Iter 10: loss 0.5234, chosen_r 15.234, rejected_r 14.856, acc 0.800
Val loss 0.4567
[dpo_mlx_train] DONE — adapter at /tmp/adapter (12345 bytes)
EOF

# Create mock data file
MOCK_DATA="$TEST_TMP/training_pairs.jsonl"
cat > "$MOCK_DATA" <<'EOF'
{"prompt": "test1", "chosen": "response1", "rejected": "response2"}
{"prompt": "test2", "chosen": "response3", "rejected": "response4"}
EOF

# Test 1: Append result via Python CLI
echo ""
echo "Test 1: Appending training result..."
RESULTS_FILE="$TEST_TMP/results.jsonl"
python3 "$REPO_ROOT/scripts/dpo_results.py" \
  --results-file "$RESULTS_FILE" \
  --append "adapter-test-1" '{"imessage": 2}' "0.5234" "0.4567" "null" "2.0" "100" "commit123"

if [ ! -f "$RESULTS_FILE" ]; then
  echo "FAIL: Results file not created"
  exit 1
fi

# Verify the record was written
if ! grep -q "adapter-test-1" "$RESULTS_FILE"; then
  echo "FAIL: Adapter ID not found in results"
  exit 1
fi

echo "PASS: Result recorded to $RESULTS_FILE"

# Test 2: Check regression verdict on first result
echo ""
echo "Test 2: Regression verdict on first result..."
# Use empty results file to test FIRST_RUN detection
EMPTY_RESULTS="$TEST_TMP/empty_results.jsonl"
VERDICT=$(python3 "$REPO_ROOT/scripts/dpo_results.py" \
  --results-file "$EMPTY_RESULTS" \
  --check 0.4567 2>/dev/null || echo "ERROR")

if [ "$VERDICT" = "FIRST_RUN" ]; then
  echo "PASS: First run correctly identified as FIRST_RUN"
else
  echo "FAIL: Expected FIRST_RUN, got $VERDICT"
  exit 1
fi

# Test 3: Add a baseline and check improvement
echo ""
echo "Test 3: Adding baseline and verifying improvement detection..."
python3 "$REPO_ROOT/scripts/dpo_results.py" \
  --results-file "$RESULTS_FILE" \
  --append "adapter-baseline" '{"imessage": 10}' "0.6000" "0.5500" "null" "2.0" "100" "commit456"

# New result better than baseline should pass
NEW_VERDICT=$(python3 "$REPO_ROOT/scripts/dpo_results.py" \
  --results-file "$RESULTS_FILE" \
  --check 0.5400 2>/dev/null || echo "ERROR")

if [ "$NEW_VERDICT" = "PASS" ]; then
  echo "PASS: Improvement correctly detected"
else
  echo "FAIL: Expected PASS for improvement, got $NEW_VERDICT"
  exit 1
fi

# Test 4: Regression detection (worse than baseline)
echo ""
echo "Test 4: Detecting regression (worse than baseline)..."
BAD_VERDICT=$(python3 "$REPO_ROOT/scripts/dpo_results.py" \
  --results-file "$RESULTS_FILE" \
  --check 0.6700 2>/dev/null) || true

if [ "$BAD_VERDICT" = "FAIL" ]; then
  echo "PASS: Regression correctly detected"
else
  echo "FAIL: Expected FAIL for regression, got $BAD_VERDICT"
  exit 1
fi

# Test 5: Degenerate signature detection
echo ""
echo "Test 5: Detecting degenerate signature (random baseline)..."
DEGEN_VERDICT=$(python3 "$REPO_ROOT/scripts/dpo_results.py" \
  --results-file "$RESULTS_FILE" \
  --check 0.6931 2>/dev/null) || true

if [ "$DEGEN_VERDICT" = "FAIL" ]; then
  echo "PASS: Degenerate signature correctly detected"
else
  echo "FAIL: Expected FAIL for degenerate, got $DEGEN_VERDICT"
  exit 1
fi

echo ""
echo "Test 6: Simulating training_loop.py outcome recording..."

# Add a baseline from 7 days ago (passing training)
python3 "$REPO_ROOT/scripts/dpo_results.py" \
  --results-file "$RESULTS_FILE" \
  --append "adapter-baseline-7d" '{"outcomes": 42}' "0.5000" "0.4500" "null" "2.0" "500" "commit-baseline"

# Now simulate training_loop.py recording a worse run
python3 "$REPO_ROOT/scripts/dpo_results.py" \
  --results-file "$RESULTS_FILE" \
  --append "adapter-new-training" '{"outcomes": 48}' "0.5234" "0.5800" "null" "2.0" "500" "commit-new"

# Check verdict — should FAIL because 0.5800 > 0.4500 + 0.1
FAIL_VERDICT=$(python3 "$REPO_ROOT/scripts/dpo_results.py" \
  --results-file "$RESULTS_FILE" \
  --check 0.5800 2>/dev/null) || true

if [ "$FAIL_VERDICT" = "FAIL" ]; then
  echo "PASS: training_loop.py path correctly detects regression"
else
  echo "FAIL: Expected FAIL for regression in training_loop.py path, got $FAIL_VERDICT"
  exit 1
fi

# Verify exit code behavior — C side (lora_training_runner.c:391-396) checks this
set +e  # Temporarily disable exit-on-error to capture exit code
python3 "$REPO_ROOT/scripts/dpo_results.py" \
  --results-file "$RESULTS_FILE" \
  --check 0.5800 >/dev/null 2>&1
EXIT_CODE=$?
set -e  # Re-enable exit-on-error

if [ $EXIT_CODE -ne 0 ]; then
  echo "PASS: dpo_results.py exits non-zero on FAIL (blocks adapter swap)"
else
  echo "FAIL: Expected non-zero exit code on regression"
  exit 1
fi

echo ""
echo "=== All integration tests PASSED ==="
exit 0
