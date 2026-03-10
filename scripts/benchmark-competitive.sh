#!/usr/bin/env bash
# Human competitive benchmarking via Google PageSpeed Insights API v5.
# Usage: ./scripts/benchmark-competitive.sh [--human-only] [--markdown] [--help]
# Benchmarks Human and 15 competitor sites; outputs table, JSON, and optionally markdown report.

set -euo pipefail

RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

die() { printf "${RED}error:${NC} %s\n" "$1" >&2; exit 1; }
warn() { printf "${YELLOW}warning:${NC} %s\n" "$1" >&2; }

# --- Check required tools ---
for cmd in curl jq; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    die "Required tool '$cmd' not found. Install it (e.g. brew install $cmd) and try again."
  fi
done

# --- URL array ---
URLS=(
  "Human|https://human.dev"
  "Linear|https://linear.app"
  "Vercel|https://vercel.com"
  "Raycast|https://raycast.com"
  "Warp|https://warp.dev"
  "Cursor|https://cursor.com"
  "Stripe|https://stripe.com"
  "Notion|https://notion.so"
  "Figma|https://figma.com"
  "Superhuman|https://superhuman.com"
  "Apple Dev|https://developer.apple.com"
  "Spotify|https://spotify.com"
  "Lando Norris|https://landonorris.com"
  "Scout Motors|https://scoutmotors.com"
  "Immersive Garden|https://immersive-g.com"
  "Malvah|https://malvah.com"
)

# --- Parse flags ---
HUMAN_ONLY=0
MARKDOWN=0

while [ $# -gt 0 ]; do
  case "$1" in
    --human-only)
      HUMAN_ONLY=1
      shift
      ;;
    --markdown)
      MARKDOWN=1
      shift
      ;;
    --help|-h)
      printf "Usage: %s [--human-only] [--markdown] [--help]\n" "$0"
      printf "\n"
      printf "Benchmarks Human and competitor sites via Google PageSpeed Insights API v5.\n"
      printf "\n"
      printf "Options:\n"
      printf "  --human-only   Only benchmark Human (https://human.dev)\n"
      printf "  --markdown      Write markdown report to docs/competitive-benchmarks-latest.md\n"
      printf "  --help          Show this usage info\n"
      printf "\n"
      printf "Environment:\n"
      printf "  PAGESPEED_API_KEY  Optional. Set for higher quota (unauthenticated has lower limits).\n"
      printf "\n"
      printf "Output:\n"
      printf "  stdout                    Formatted table\n"
      printf "  benchmark-competitive.json Full JSON array of results\n"
      printf "  docs/competitive-benchmarks-latest.md  (if --markdown)\n"
      exit 0
      ;;
    *)
      die "Unknown option: $1. Use --help for usage."
      ;;
  esac
done

# --- Filter URLs if --human-only ---
if [ "$HUMAN_ONLY" -eq 1 ]; then
  URLS=("Human|https://human.dev")
fi

# --- API key warning ---
if [ -z "${PAGESPEED_API_KEY:-}" ]; then
  warn "PAGESPEED_API_KEY not set. Using unauthenticated API with lower quota."
fi

# --- Paths ---
SCRIPTS_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPTS_DIR/.." && pwd)
JSON_FILE="${ROOT_DIR}/benchmark-competitive.json"
MD_FILE="${ROOT_DIR}/docs/competitive-benchmarks-latest.md"

# --- API base (pagespeedonline.googleapis.com is the official v5 endpoint) ---
API_BASE="https://pagespeedonline.googleapis.com/pagespeedonline/v5/runPagespeed"

# --- Fetch and parse a single URL ---
fetch_and_parse() {
  local name="$1"
  local url="$2"
  local api_url="${API_BASE}?url=$(printf '%s' "$url" | jq -sRr @uri)&strategy=mobile&category=PERFORMANCE&category=ACCESSIBILITY&category=BEST_PRACTICES&category=SEO"
  if [ -n "${PAGESPEED_API_KEY:-}" ]; then
    api_url="${api_url}&key=${PAGESPEED_API_KEY}"
  fi

  local resp
  resp=$(curl -sf --max-time 120 "$api_url" 2>/dev/null) || true

  if [ -z "$resp" ]; then
    echo "ERROR"
    return 1
  fi

  # Check for API error
  if echo "$resp" | jq -e '.error' >/dev/null 2>&1; then
    echo "ERROR"
    return 1
  fi

  # Check for lighthouseResult
  if ! echo "$resp" | jq -e '.lighthouseResult' >/dev/null 2>&1; then
    echo "ERROR"
    return 1
  fi

  # Extract metrics (scores 0-1, multiply by 100; numericValue in ms where applicable)
  local perf a11y bp seo lcp cls tbt ttfb
  perf=$(echo "$resp" | jq -r '(.lighthouseResult.categories.performance.score // 0) * 100 | floor')
  a11y=$(echo "$resp" | jq -r '(.lighthouseResult.categories.accessibility.score // 0) * 100 | floor')
  bp=$(echo "$resp" | jq -r '(.lighthouseResult.categories["best-practices"].score // 0) * 100 | floor')
  seo=$(echo "$resp" | jq -r '(.lighthouseResult.categories.seo.score // 0) * 100 | floor')
  lcp=$(echo "$resp" | jq -r '(.lighthouseResult.audits["largest-contentful-paint"].numericValue // 0) | tonumber')
  cls=$(echo "$resp" | jq -r '(.lighthouseResult.audits["cumulative-layout-shift"].numericValue // 0) | tonumber')
  tbt=$(echo "$resp" | jq -r '(.lighthouseResult.audits["total-blocking-time"].numericValue // 0) | tonumber')
  ttfb=$(echo "$resp" | jq -r '(.lighthouseResult.audits["server-response-time"].numericValue // 0) | tonumber')

  # Round for display (Lighthouse numericValue: LCP/TBT/TTFB in ms; CLS unitless)
  lcp_ms=$(printf '%.0f' "$lcp" 2>/dev/null || echo "0")
  tbt_ms=$(printf '%.0f' "$tbt" 2>/dev/null || echo "0")
  ttfb_ms=$(printf '%.0f' "$ttfb" 2>/dev/null || echo "0")
  cls=$(printf '%.2f' "$cls" 2>/dev/null || echo "0")

  printf '%s|%s|%s|%s|%s|%s|%s|%s' "$perf" "$a11y" "$bp" "$seo" "$lcp_ms" "$cls" "$tbt_ms" "$ttfb_ms"
}

# --- Build JSON object for a result ---
json_object() {
  local name="$1"
  local url="$2"
  local perf="$3"
  local a11y="$4"
  local bp="$5"
  local seo="$6"
  local lcp_ms="$7"
  local cls="$8"
  local tbt_ms="$9"
  local ttfb_ms="${10}"
  local err="${11:-}"
  local ts
  ts=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

  if [ "$err" = "true" ]; then
    jq -n \
      --arg name "$name" \
      --arg url "$url" \
      --arg ts "$ts" \
      '{name: $name, url: $url, timestamp: $ts, error: "fetch failed"}'
  else
    jq -n \
      --arg name "$name" \
      --arg url "$url" \
      --arg ts "$ts" \
      --argjson perf "$perf" \
      --argjson a11y "$a11y" \
      --argjson bp "$bp" \
      --argjson seo "$seo" \
      --argjson lcp_ms "$lcp_ms" \
      --arg cls "$cls" \
      --argjson tbt_ms "$tbt_ms" \
      --argjson ttfb_ms "$ttfb_ms" \
      '{
        name: $name,
        url: $url,
        timestamp: $ts,
        performance: $perf,
        accessibility: $a11y,
        best_practices: $bp,
        seo: $seo,
        lcp_ms: $lcp_ms,
        cls: ($cls | tonumber),
        tbt_ms: $tbt_ms,
        ttfb_ms: $ttfb_ms
      }'
  fi
}

# --- Print table header ---
printf "\n%-18s %4s %4s %4s %4s %6s %6s %6s %6s\n" "Site" "Perf" "A11y" "BP" "SEO" "LCP" "CLS" "TBT" "TTFB"
printf "%s\n" "--------------------------------------------------------------------------------"

# --- Collect results ---
RESULTS_JSON="[]"
COUNT=0
TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

for entry in "${URLS[@]}"; do
  name="${entry%%|*}"
  url="${entry#*|}"
  COUNT=$((COUNT + 1))

  out=$(fetch_and_parse "$name" "$url") || true

  if [ "$out" = "ERROR" ]; then
    printf "%-18s %s\n" "$name" "ERROR"
    obj=$(json_object "$name" "$url" "0" "0" "0" "0" "0" "0" "0" "0" "true")
  else
    IFS='|' read -r perf a11y bp seo lcp_ms cls tbt_ms ttfb_ms <<< "$out"
    printf "%-18s %4s %4s %4s %4s %6s %6s %6s %6s\n" \
      "$name" "$perf" "$a11y" "$bp" "$seo" "$lcp_ms" "$cls" "$tbt_ms" "$ttfb_ms"
    obj=$(json_object "$name" "$url" "$perf" "$a11y" "$bp" "$seo" "$lcp_ms" "$cls" "$tbt_ms" "$ttfb_ms" "false")
  fi

  RESULTS_JSON=$(echo "$RESULTS_JSON" | jq --argjson obj "$obj" '. + [$obj]')
  [ $COUNT -lt ${#URLS[@]} ] && sleep 1
done

# --- Write JSON ---
echo "$RESULTS_JSON" > "$JSON_FILE"

# --- Write markdown if requested ---
if [ "$MARKDOWN" -eq 1 ]; then
  mkdir -p "$(dirname "$MD_FILE")"
  {
    echo "---"
    echo "title: Human Competitive Benchmarks (Latest Run)"
    echo "generated: $TIMESTAMP"
    echo "---"
    echo ""
    echo "# Human Competitive Benchmarks"
    echo ""
    echo "**Generated:** $TIMESTAMP"
    echo ""
    echo "## Automated Metrics (PageSpeed Insights API v5, Mobile)"
    echo ""
    echo "| Site | Perf | A11y | BP | SEO | LCP (ms) | CLS | TBT (ms) | TTFB (ms) |"
    echo "| ---- | ---- | ---- | -- | --- | -------- | --- | -------- | --------- |"

    echo "$RESULTS_JSON" | jq -r '.[] | "| \(.name) | \(.performance // "—") | \(.accessibility // "—") | \(.best_practices // "—") | \(.seo // "—") | \(.lcp_ms // "—") | \(.cls // "—") | \(.tbt_ms // "—") | \(.ttfb_ms // "—") |"'

    echo ""
    echo "## Human vs Category-Defining Targets"
    echo ""
    echo "| Metric | Industry Best | Human Target | Human Actual |"
    echo "| ------ | ------------- | -------------- | -------------- |"

    human=$(echo "$RESULTS_JSON" | jq '.[] | select(.name == "Human")')
    if [ -n "$human" ] && [ "$human" != "null" ] && ! echo "$human" | jq -e '.error' >/dev/null 2>&1; then
      perf_actual=$(echo "$human" | jq -r '.performance // "—"')
      a11y_actual=$(echo "$human" | jq -r '.accessibility // "—"')
      lcp_actual=$(echo "$human" | jq -r 'if .lcp_ms then "\(.lcp_ms)ms" else "—" end')
      cls_actual=$(echo "$human" | jq -r '.cls // "—"')
      echo "| Lighthouse Performance | 95–97 (Vercel) | 99+ | $perf_actual |"
      echo "| LCP | 0.8s (Linear) | < 0.5s | $lcp_actual |"
      echo "| CLS | ~0.02 (Stripe) | 0.00 | $cls_actual |"
      echo "| Accessibility | 98 (Vercel) | 100 | $a11y_actual |"
    else
      echo "| Lighthouse Performance | 95–97 (Vercel) | 99+ | — |"
      echo "| LCP | 0.8s (Linear) | < 0.5s | — |"
      echo "| CLS | ~0.02 (Stripe) | 0.00 | — |"
      echo "| Accessibility | 98 (Vercel) | 100 | — |"
    fi

    echo ""
  } > "$MD_FILE"
  echo "Markdown report written to $MD_FILE"
fi

printf "\nBenchmarked %d sites. Results saved to %s\n" "$COUNT" "$JSON_FILE"
