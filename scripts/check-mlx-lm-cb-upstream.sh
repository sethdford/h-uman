#!/usr/bin/env bash
# Phase 7 unblock detector — Gemma throughput program.
#
# Checks whether mlx-lm has shipped continuous batching (the only
# missing dependency for Phase 7 RadixAttention cross-user prefix
# sharing per docs/guides/radix-attention-upstream-watch.md).
#
# Exits 0 with the unblock signal when EITHER:
#   (a) latest release notes mention continuous-batching / paged-attention
#   (b) mlx-lm/main contains a paged_kv_cache.py / continuous_batching.py
#       / paged_attention.py / batch_scheduler.py module
#
# Exits 1 with a "still waiting" status line otherwise.
#
# Suitable for /loop 24h or a daily cron. When it fires, the next step
# is the §implementation section of docs/guides/radix-attention-upstream-watch.md.
#
# No GitHub auth required — uses the public REST API. Subject to GitHub's
# unauthenticated rate limit (60 req/hr); each invocation uses 2 calls.

set -euo pipefail

REPO="ml-explore/mlx-lm"
API="https://api.github.com/repos/${REPO}"

if ! command -v curl >/dev/null 2>&1; then
    echo "check-mlx-lm-cb-upstream: curl required" >&2
    exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "check-mlx-lm-cb-upstream: python3 required for JSON parse" >&2
    exit 2
fi

# Check 1 — latest release notes contain the trigger phrase.
latest_notes="$(curl -s "${API}/releases/latest" \
                  | python3 -c 'import json,sys; r=json.load(sys.stdin); print(r.get("body","") or "")' \
                  2>/dev/null || echo "")"

if echo "${latest_notes}" \
        | grep -iE 'continuous.{0,4}batching|paged.{0,4}attention' >/dev/null 2>&1; then
    echo "✅ Phase 7 UNBLOCKED — mlx-lm release notes mention CB / paged attn:"
    echo "${latest_notes}" \
        | grep -iE 'continuous.{0,4}batching|paged.{0,4}attention' \
        | head -3 \
        | sed 's/^/    /'
    echo
    echo "Next step: see §implementation in"
    echo "  docs/guides/radix-attention-upstream-watch.md"
    exit 0
fi

# Check 2 — module presence on the main branch.
tree_json="$(curl -s "${API}/contents/mlx_lm?ref=main" 2>/dev/null || echo "[]")"
for path in paged_kv_cache.py continuous_batching.py paged_attention.py batch_scheduler.py; do
    if echo "${tree_json}" | grep -q "\"name\": \"${path}\""; then
        echo "✅ Phase 7 UNBLOCKED — mlx-lm/main contains mlx_lm/${path}"
        echo
        echo "Next step: see §implementation in"
        echo "  docs/guides/radix-attention-upstream-watch.md"
        exit 0
    fi
done

# Still gated.
tag="$(curl -s "${API}/releases/latest" \
         | python3 -c 'import json,sys; r=json.load(sys.stdin); print(r.get("tag_name","?"))' \
         2>/dev/null || echo "?")"

cat <<EOM
Phase 7 still gated on mlx-lm continuous batching.
  Last checked:  $(date -u +%Y-%m-%dT%H:%M:%SZ)
  Latest mlx-lm release: ${tag}
  Background: docs/guides/radix-attention-upstream-watch.md
EOM
exit 1
