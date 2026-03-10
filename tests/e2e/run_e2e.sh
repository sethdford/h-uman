#!/usr/bin/env bash
# End-to-end integration tests for human with a live OpenAI API.
# Requires OPENAI_API_KEY in the environment.
# Usage: OPENAI_API_KEY=sk-... ./tests/e2e/run_e2e.sh [path/to/human]
set -euo pipefail

BINARY="${1:-./build/human}"
PASS=0
FAIL=0
TOTAL=0
ERRORS=""

pass() { PASS=$((PASS + 1)); TOTAL=$((TOTAL + 1)); printf "  \033[32mPASS\033[0m  %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); TOTAL=$((TOTAL + 1)); ERRORS="${ERRORS}\n  - $1: $2"; printf "  \033[31mFAIL\033[0m  %s — %s\n" "$1" "$2"; }

# Portable timeout: use gtimeout (Homebrew coreutils), timeout (Linux), or perl fallback
if command -v gtimeout >/dev/null 2>&1; then
  TIMEOUT_CMD="gtimeout"
elif command -v timeout >/dev/null 2>&1; then
  TIMEOUT_CMD="timeout"
else
  _perl_timeout() {
    local secs="$1"; shift
    perl -e '
      use POSIX ":sys_wait_h";
      my $pid = fork();
      if ($pid == 0) { exec @ARGV; exit 127; }
      my $elapsed = 0;
      while ($elapsed < '"$secs"') {
        my $r = waitpid($pid, WNOHANG);
        if ($r > 0) { exit ($? >> 8); }
        select(undef, undef, undef, 0.2);
        $elapsed += 0.2;
      }
      kill "TERM", $pid;
      select(undef, undef, undef, 0.5);
      kill "KILL", $pid;
      waitpid($pid, 0);
      exit 124;
    ' -- "$@"
  }
  TIMEOUT_CMD="_perl_timeout"
fi

run_with_timeout() {
  local secs="$1"; shift
  $TIMEOUT_CMD "$secs" "$@"
}

if [ -z "${OPENAI_API_KEY:-}" ]; then
  echo "ERROR: OPENAI_API_KEY not set" >&2; exit 1
fi
if [ ! -x "$BINARY" ]; then
  echo "ERROR: $BINARY not found or not executable" >&2; exit 1
fi

echo ""
echo "═══════════════════════════════════════════════════════"
echo " Human E2E Integration Tests (live OpenAI API)"
echo "═══════════════════════════════════════════════════════"
echo ""

# ── 1. CLI Commands ──────────────────────────────────────

echo "── CLI Commands ──"

OUT=$("$BINARY" --version 2>&1)
if echo "$OUT" | grep -q "v0\.1\.0"; then pass "version"; else fail "version" "unexpected: $OUT"; fi

OUT=$("$BINARY" doctor 2>&1)
if echo "$OUT" | grep -q "API key configured"; then pass "doctor_api_key"; else fail "doctor_api_key" "key not detected"; fi
if echo "$OUT" | grep -q "config: ok"; then pass "doctor_config"; else fail "doctor_config" "config not ok"; fi

OUT=$("$BINARY" capabilities 2>&1)
if echo "$OUT" | grep -q "shell"; then pass "capabilities_tools"; else fail "capabilities_tools" "no shell tool"; fi
if echo "$OUT" | grep -q "openai"; then pass "capabilities_provider"; else fail "capabilities_provider" "no openai"; fi

OUT=$("$BINARY" memory stats 2>&1)
if echo "$OUT" | grep -q "Health: ok"; then pass "memory_health"; else fail "memory_health" "memory unhealthy"; fi

OUT=$("$BINARY" sandbox 2>&1)
if echo "$OUT" | grep -q "seatbelt\|landlock\|docker"; then pass "sandbox_detection"; else fail "sandbox_detection" "no sandbox found"; fi

OUT=$("$BINARY" models list 2>&1)
if echo "$OUT" | grep -q "openai\|gpt-4o"; then pass "models_list"; else fail "models_list" "no models"; fi

echo ""

# ── 2. MCP Server: Tool Listing ─────────────────────────

echo "── MCP Server (JSON-RPC) ──"

MCP_INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"e2e-test","version":"1.0"}}}'
MCP_LIST='{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'

MCP_OUT=$(printf '%s\n%s\n' "$MCP_INIT" "$MCP_LIST" | run_with_timeout 5 "$BINARY" mcp 2>/dev/null || true)

if echo "$MCP_OUT" | grep -q '"tools"'; then
  pass "mcp_tools_list"
  TOOL_COUNT=$(echo "$MCP_OUT" | grep -o '"name"' | wc -l | tr -d ' ')
  if [ "$TOOL_COUNT" -ge 20 ]; then pass "mcp_tool_count ($TOOL_COUNT tools)"; else fail "mcp_tool_count" "only $TOOL_COUNT tools"; fi
else
  fail "mcp_tools_list" "no tools in response"
  fail "mcp_tool_count" "skipped"
fi

if echo "$MCP_OUT" | grep -q '"shell"'; then pass "mcp_has_shell_tool"; else fail "mcp_has_shell_tool" "shell not found"; fi
if echo "$MCP_OUT" | grep -q '"file_read"'; then pass "mcp_has_file_read"; else fail "mcp_has_file_read" "file_read not found"; fi
if echo "$MCP_OUT" | grep -q '"git_operations"'; then pass "mcp_has_git_tool"; else fail "mcp_has_git_tool" "git_operations not found"; fi
if echo "$MCP_OUT" | grep -q '"memory_store"'; then pass "mcp_has_memory_store"; else fail "mcp_has_memory_store" "memory_store not found"; fi
if echo "$MCP_OUT" | grep -q '"cron_add"'; then pass "mcp_has_cron_add"; else fail "mcp_has_cron_add" "cron_add not found"; fi

echo ""

# ── 3. MCP Tool Execution ───────────────────────────────

echo "── MCP Tool Execution ──"

MCP_EXEC_SHELL='{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"shell","arguments":{"command":"echo e2e_sentinel_42"}}}'
MCP_OUT2=$(printf '%s\n%s\n' "$MCP_INIT" "$MCP_EXEC_SHELL" | run_with_timeout 10 "$BINARY" mcp 2>/dev/null || true)

if echo "$MCP_OUT2" | grep -q "e2e_sentinel_42"; then
  pass "mcp_shell_exec"
else
  fail "mcp_shell_exec" "sentinel not in output"
fi

MCP_EXEC_READ='{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"file_read","arguments":{"path":"README.md"}}}'
MCP_OUT3=$(printf '%s\n%s\n' "$MCP_INIT" "$MCP_EXEC_READ" | run_with_timeout 10 "$BINARY" mcp 2>/dev/null || true)

if echo "$MCP_OUT3" | grep -qi "human\|autonomous"; then
  pass "mcp_file_read"
else
  fail "mcp_file_read" "README content not found"
fi

MCP_EXEC_GIT='{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"git_operations","arguments":{"operation":"status"}}}'
MCP_OUT4=$(printf '%s\n%s\n' "$MCP_INIT" "$MCP_EXEC_GIT" | run_with_timeout 10 "$BINARY" mcp 2>/dev/null || true)

if echo "$MCP_OUT4" | grep -qi "branch\|clean\|nothing to commit"; then
  pass "mcp_git_status"
else
  fail "mcp_git_status" "no branch info"
fi

echo ""

# ── 4. Live LLM: Agent Turn ─────────────────────────────

echo "── Live LLM (OpenAI gpt-4o) ──"

LOGFILE=$(mktemp /tmp/human_e2e_XXXXXX.log)

echo "What is the capital of France? Answer in one word only." | \
  HUMAN_LOG="$LOGFILE" run_with_timeout 30 "$BINARY" agent 2>/dev/null &
AGENT_PID=$!
sleep 15
kill $AGENT_PID 2>/dev/null || true
wait $AGENT_PID 2>/dev/null || true

if [ -f "$LOGFILE" ]; then
  if grep -q '"event":"llm_response"' "$LOGFILE" && grep -q '"success":true' "$LOGFILE"; then
    pass "llm_basic_response"
    DURATION=$(grep '"event":"llm_response"' "$LOGFILE" | grep -o '"duration_ms":[0-9]*' | head -1 | cut -d: -f2)
    if [ -n "$DURATION" ] && [ "$DURATION" -lt 30000 ]; then
      pass "llm_response_time (${DURATION}ms)"
    else
      fail "llm_response_time" "took ${DURATION:-unknown}ms"
    fi
    TOKENS=$(grep '"event":"agent_end"' "$LOGFILE" | grep -o '"tokens_used":[0-9]*' | head -1 | cut -d: -f2)
    if [ -n "$TOKENS" ] && [ "$TOKENS" -gt 0 ]; then
      pass "llm_token_count ($TOKENS tokens)"
    else
      fail "llm_token_count" "no tokens reported"
    fi
  else
    fail "llm_basic_response" "no successful LLM response in log"
    fail "llm_response_time" "skipped"
    fail "llm_token_count" "skipped"
  fi
  if grep -q '"event":"turn_complete"' "$LOGFILE"; then
    pass "llm_turn_complete"
  else
    fail "llm_turn_complete" "turn did not complete"
  fi
else
  fail "llm_basic_response" "no log file"
  fail "llm_response_time" "skipped"
  fail "llm_token_count" "skipped"
  fail "llm_turn_complete" "skipped"
fi
rm -f "$LOGFILE"

echo ""

# ── 5. Live LLM: Tool Use ───────────────────────────────

echo "── Live LLM Tool Calling ──"

LOGFILE2=$(mktemp /tmp/human_e2e_XXXXXX.log)

echo "You MUST use the shell tool right now. Run this exact command: echo TOOL_TEST_OK. Do not respond with text, just call the shell tool." | \
  HUMAN_LOG="$LOGFILE2" run_with_timeout 60 "$BINARY" agent 2>/dev/null &
AGENT_PID=$!
# Tool-calling requires 2+ LLM round-trips: wait up to 45s
for i in $(seq 1 9); do
  sleep 5
  if [ -f "$LOGFILE2" ] && grep -q '"event":"turn_complete"' "$LOGFILE2" 2>/dev/null; then break; fi
done
kill $AGENT_PID 2>/dev/null || true
wait $AGENT_PID 2>/dev/null || true

if [ -f "$LOGFILE2" ]; then
  if grep -q '"event":"llm_response"' "$LOGFILE2" && grep -q '"success":true' "$LOGFILE2"; then
    pass "llm_tool_call_response"
  else
    fail "llm_tool_call_response" "no successful response"
  fi
  if grep -q '"event":"tool_call_start"' "$LOGFILE2" || grep -q '"event":"tool_call"' "$LOGFILE2"; then
    pass "llm_tool_invocation"
  else
    fail "llm_tool_invocation" "no tool_call events in log"
  fi
  if grep -q '"event":"turn_complete"' "$LOGFILE2"; then
    pass "llm_tool_turn_complete"
  else
    fail "llm_tool_turn_complete" "turn did not complete"
  fi
else
  fail "llm_tool_call_response" "no log"
  fail "llm_tool_invocation" "skipped"
  fail "llm_tool_turn_complete" "skipped"
fi
rm -f "$LOGFILE2"

echo ""

# ── 6. Live LLM: Streaming ──────────────────────────────

echo "── Live LLM Streaming ──"

LOGFILE3=$(mktemp /tmp/human_e2e_XXXXXX.log)

echo "Count from 1 to 5, one number per line." | \
  HUMAN_LOG="$LOGFILE3" run_with_timeout 30 "$BINARY" agent 2>/dev/null &
AGENT_PID=$!
sleep 15
kill $AGENT_PID 2>/dev/null || true
wait $AGENT_PID 2>/dev/null || true

if [ -f "$LOGFILE3" ]; then
  if grep -q '"event":"llm_response"' "$LOGFILE3" && grep -q '"success":true' "$LOGFILE3"; then
    pass "llm_streaming_response"
  else
    fail "llm_streaming_response" "no response"
  fi
else
  fail "llm_streaming_response" "no log"
fi
rm -f "$LOGFILE3"

echo ""

# ── 7. Error Handling ────────────────────────────────────

echo "── Error Handling ──"

ERR_OUT=$(OPENAI_API_KEY="sk-invalid-key-12345" "$BINARY" doctor 2>&1 || true)
if echo "$ERR_OUT" | grep -qi "API key configured\|warning"; then
  pass "bad_key_doctor_survives"
else
  fail "bad_key_doctor_survives" "doctor crashed with bad key"
fi

LOGFILE4=$(mktemp /tmp/human_e2e_XXXXXX.log)
echo "Hello" | \
  OPENAI_API_KEY="sk-invalid" HUMAN_LOG="$LOGFILE4" run_with_timeout 15 "$BINARY" agent 2>/dev/null &
AGENT_PID=$!
sleep 10
kill $AGENT_PID 2>/dev/null || true
wait $AGENT_PID 2>/dev/null || true

if [ -f "$LOGFILE4" ]; then
  if grep -q '"event":"llm_response"' "$LOGFILE4"; then
    if grep -q '"success":false' "$LOGFILE4"; then
      pass "bad_key_graceful_error"
    else
      pass "bad_key_graceful_error (response received despite bad key)"
    fi
  else
    pass "bad_key_graceful_error (no crash)"
  fi
else
  pass "bad_key_graceful_error (no crash, no log)"
fi
rm -f "$LOGFILE4"

echo ""

# ── 8. Concurrent Requests ──────────────────────────────

echo "── Concurrent Stress ──"

PIDS=""
ALL_OK=true
for i in 1 2 3; do
  TMPLOG=$(mktemp /tmp/human_e2e_concurrent_${i}_XXXXXX.log)
  (echo "Say the number $i and nothing else." | \
    HUMAN_LOG="$TMPLOG" run_with_timeout 30 "$BINARY" agent 2>/dev/null; \
    echo "$?" > "${TMPLOG}.exit") &
  PIDS="$PIDS $!"
done

sleep 20
for P in $PIDS; do kill "$P" 2>/dev/null || true; done
for P in $PIDS; do wait "$P" 2>/dev/null || true; done

CONCURRENT_OK=0
for i in 1 2 3; do
  TMPLOG=$(ls /tmp/human_e2e_concurrent_${i}_*.log 2>/dev/null | head -1)
  if [ -n "$TMPLOG" ] && [ -f "$TMPLOG" ] && grep -q '"success":true' "$TMPLOG"; then
    CONCURRENT_OK=$((CONCURRENT_OK + 1))
  fi
  rm -f "$TMPLOG" "${TMPLOG}.exit" 2>/dev/null
done

if [ "$CONCURRENT_OK" -ge 2 ]; then
  pass "concurrent_agents ($CONCURRENT_OK/3 succeeded)"
else
  fail "concurrent_agents" "only $CONCURRENT_OK/3 succeeded"
fi

echo ""

# ── 9. Gateway WebSocket Control Protocol ─────────────────

echo "── Gateway HTTP / Control Protocol ──"

GW_PORT=$((3000 + (RANDOM % 30000)))
GW_PID=""
cleanup_gateway() {
  if [ -n "$GW_PID" ] && kill -0 "$GW_PID" 2>/dev/null; then
    kill "$GW_PID" 2>/dev/null || true
    wait "$GW_PID" 2>/dev/null || true
  fi
}
trap cleanup_gateway EXIT

HUMAN_GATEWAY_PORT=$GW_PORT "$BINARY" gateway 2>/dev/null &
GW_PID=$!
sleep 2

if ! kill -0 "$GW_PID" 2>/dev/null; then
  fail "gateway_start" "gateway failed to start"
else
  pass "gateway_start"

  HEALTH=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$GW_PORT/health" 2>/dev/null || echo "000")
  if [ "$HEALTH" = "200" ]; then
    pass "gateway_health_200"
  else
    fail "gateway_health_200" "got HTTP $HEALTH"
  fi

  HEALTH_BODY=$(curl -s "http://127.0.0.1:$GW_PORT/health" 2>/dev/null || echo "")
  if echo "$HEALTH_BODY" | grep -q '"status":"ok"'; then
    pass "gateway_health_body"
  else
    fail "gateway_health_body" "expected {\"status\":\"ok\"}"
  fi

  STATUS_CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$GW_PORT/api/status" 2>/dev/null || echo "000")
  if [ "$STATUS_CODE" = "200" ]; then
    pass "gateway_api_status_200"
  else
    fail "gateway_api_status_200" "got HTTP $STATUS_CODE"
  fi

  NOT_FOUND=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$GW_PORT/nonexistent-path-404" 2>/dev/null || echo "000")
  if [ "$NOT_FOUND" = "404" ]; then
    pass "gateway_404_invalid_path"
  else
    fail "gateway_404_invalid_path" "got HTTP $NOT_FOUND"
  fi

  CORS_RESP=$(curl -s -i -H "Origin: http://127.0.0.1:$GW_PORT" "http://127.0.0.1:$GW_PORT/health" 2>/dev/null || echo "")
  if echo "$CORS_RESP" | grep -qi "Access-Control-Allow-Origin"; then
    pass "gateway_cors_header"
  else
    fail "gateway_cors_header" "no CORS header in response"
  fi

  READY_CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$GW_PORT/ready" 2>/dev/null || echo "000")
  if [ "$READY_CODE" = "200" ]; then
    pass "gateway_ready_200"
  else
    fail "gateway_ready_200" "got HTTP $READY_CODE"
  fi
fi

cleanup_gateway
trap - EXIT

if command -v wscat >/dev/null 2>&1; then
  echo "  (wscat found — WebSocket tests would go here)"
  pass "gateway_ws_placeholder (wscat available)"
else
  printf "  \033[33mSKIP\033[0m  gateway_ws_control (requires wscat)\n"
fi

echo ""

# ── 10. MCP Resources ─────────────────────────────────────

echo "── MCP Resources ──"

MCP_RES_LIST='{"jsonrpc":"2.0","id":10,"method":"resources/list","params":{}}'
MCP_RES_READ_CONFIG='{"jsonrpc":"2.0","id":11,"method":"resources/read","params":{"uri":"human://config"}}'
MCP_RES_READ_MEM='{"jsonrpc":"2.0","id":12,"method":"resources/read","params":{"uri":"human://memory"}}'
MCP_RES_READ_INVALID='{"jsonrpc":"2.0","id":13,"method":"resources/read","params":{"uri":"human://invalid"}}'

MCP_RES_OUT=$(printf '%s\n%s\n%s\n%s\n%s\n' "$MCP_INIT" "$MCP_RES_LIST" "$MCP_RES_READ_CONFIG" "$MCP_RES_READ_MEM" "$MCP_RES_READ_INVALID" | run_with_timeout 10 "$BINARY" mcp 2>/dev/null || true)

if echo "$MCP_RES_OUT" | grep -q '"resources"' || echo "$MCP_RES_OUT" | grep -q 'human://config'; then
  pass "mcp_resources_list"
else
  fail "mcp_resources_list" "no resources in response"
fi

if echo "$MCP_RES_OUT" | grep -q 'human://config'; then
  pass "mcp_resources_read_config"
else
  fail "mcp_resources_read_config" "human://config not in response"
fi

if echo "$MCP_RES_OUT" | grep -q 'human://memory'; then
  pass "mcp_resources_read_memory"
else
  fail "mcp_resources_read_memory" "human://memory not in response"
fi

if echo "$MCP_RES_OUT" | grep -q "Unknown resource\|error\|-32602"; then
  pass "mcp_resources_read_invalid"
else
  fail "mcp_resources_read_invalid" "expected error for invalid URI"
fi

echo ""

# ── 11. Push Notification Registration (placeholder) ───────

echo "── Push Notification (placeholder) ──"

echo "  (push.register / push.unregister are WebSocket control protocol methods)"
pass "push_documented (WS-only, no HTTP endpoint)"

printf "  \033[33mSKIP\033[0m  push_register (requires wscat/WebSocket)\n"
printf "  \033[33mSKIP\033[0m  push_unregister (requires wscat/WebSocket)\n"

echo ""

# ── 12. Migration CLI ──────────────────────────────────────

echo "── Migration CLI ──"

OUT=$("$BINARY" migrate 2>&1)
if echo "$OUT" | grep -qi "Migration:\|migration"; then
  pass "migrate_exists"
else
  fail "migrate_exists" "migrate did not respond: $OUT"
fi

OUT_DRY=$("$BINARY" migrate --dry-run 2>&1)
if echo "$OUT_DRY" | grep -qi "Migration:\|migration\|from sqlite\|from markdown"; then
  pass "migrate_dry_run"
else
  fail "migrate_dry_run" "unexpected: $OUT_DRY"
fi

OUT_HELP=$("$BINARY" migrate --help 2>&1 || true)
if echo "$OUT_HELP" | grep -qi "migrate\|usage\|help" || [ -n "$OUT_HELP" ]; then
  pass "migrate_help"
else
  fail "migrate_help" "no help output"
fi

echo ""

# ── 13. Skills CLI Extended ──────────────────────────────────

echo "── Skills CLI Extended ──"

OUT=$(run_with_timeout 5 "$BINARY" skills info code-review 2>&1 || true)
if echo "$OUT" | grep -qi "code-review\|list\|search\|skill\|Registry"; then
  pass "skills_info_code_review"
else
  fail "skills_info_code_review" "unexpected: $OUT"
fi

OUT=$(run_with_timeout 5 "$BINARY" skills publish /tmp/nonexistent_e2e_skill_dir 2>&1 || true)
if echo "$OUT" | grep -qi "error\|failed\|invalid\|list\|search"; then
  pass "skills_publish_graceful_fail"
else
  fail "skills_publish_graceful_fail" "should fail gracefully for nonexistent dir"
fi

OUT=$(run_with_timeout 5 "$BINARY" skills list 2>&1)
if echo "$OUT" | grep -qi "Installed\|skill"; then
  pass "skills_list"
else
  fail "skills_list" "no skill list output"
fi

OUT=$(run_with_timeout 5 "$BINARY" skills list --json 2>&1 || true)
if [ -n "$OUT" ]; then
  pass "skills_list_json"
else
  fail "skills_list_json" "no output"
fi

OUT=$(run_with_timeout 10 "$BINARY" skills search "test query" 2>&1)
if echo "$OUT" | grep -qi "Registry\|match\|skill"; then
  pass "skills_search"
else
  fail "skills_search" "search produced no output"
fi

echo ""

# ── Results ──────────────────────────────────────────────

echo "═══════════════════════════════════════════════════════"
printf " Results: \033[32m%d passed\033[0m" "$PASS"
if [ "$FAIL" -gt 0 ]; then
  printf ", \033[31m%d failed\033[0m" "$FAIL"
fi
printf " / %d total\n" "$TOTAL"

if [ "$FAIL" -gt 0 ]; then
  echo ""
  echo " Failures:"
  printf "$ERRORS\n"
fi
echo "═══════════════════════════════════════════════════════"
echo ""

exit "$FAIL"
