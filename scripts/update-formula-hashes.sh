#!/usr/bin/env bash
# update-formula-hashes.sh — Rewrite Formula/human.rb with real SHA256 digests
# from the release artifact's sha256sums.txt file.
#
# Inputs:
#   --version <semver>  Release version (e.g. 0.5.0 — leading 'v' is stripped if present)
#   --sums <path>       Path to sha256sums.txt produced by the release workflow.
#                       Expected line format: "<64-hex>  <artifact-filename>"
#   --formula <path>    Optional. Path to Formula/human.rb. Defaults to <repo>/Formula/human.rb.
#
# Behavior:
#   - Replaces the "version" line with the supplied version.
#   - Replaces each "sha256" line whose preceding "url" line references one of the
#     three known artifact filenames (human-macos-aarch64.bin, human-linux-aarch64.bin,
#     human-linux-x86_64.bin) with the matching hash from sha256sums.txt.
#   - Matches placeholders by URL CONTEXT, not by all-zero pattern — this is intentional
#     and prevents accidentally rewriting unrelated hashes if the formula ever ships
#     with non-placeholder values during re-runs.
#   - Idempotent: re-running with the same inputs is a no-op (digest already correct).
#   - Exits non-zero if any of the three architectures is unmatched, the sums file is
#     malformed, or any input is missing.
#
# Exit codes:
#   0   success
#   2   missing or unreadable input
#   3   malformed sha256sums.txt (no usable lines, or no 64-hex digest)
#   4   architecture not found in sha256sums.txt (partial update refused)
#   5   formula file did not contain the expected URL context for a target artifact

set -euo pipefail

usage() {
  cat <<'EOF'
usage: update-formula-hashes.sh --version <semver> --sums <sha256sums.txt> [--formula <path>]

Rewrites Formula/human.rb with real SHA256 digests from a release sha256sums.txt.

Required:
  --version  Release version (with or without leading 'v')
  --sums     Path to sha256sums.txt published by the release workflow

Optional:
  --formula  Path to Formula/human.rb (defaults to <repo-root>/Formula/human.rb)
  -h, --help Show this help and exit
EOF
}

VERSION=""
SUMS_FILE=""
FORMULA=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION="${2:-}"; shift 2 ;;
    --sums)    SUMS_FILE="${2:-}"; shift 2 ;;
    --formula) FORMULA="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "error: unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$VERSION" || -z "$SUMS_FILE" ]]; then
  echo "error: --version and --sums are required" >&2
  usage >&2
  exit 2
fi

# Strip leading 'v' if present.
VERSION="${VERSION#v}"

# Validate version is a semver-shaped string. Reject empty / shell-metachar input.
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-+][A-Za-z0-9.+-]+)?$ ]]; then
  echo "error: --version must be semver (got: $VERSION)" >&2
  exit 2
fi

if [[ ! -r "$SUMS_FILE" ]]; then
  echo "error: --sums file not readable: $SUMS_FILE" >&2
  exit 2
fi

if [[ -z "$FORMULA" ]]; then
  REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
  FORMULA="$REPO_ROOT/Formula/human.rb"
fi

if [[ ! -f "$FORMULA" ]]; then
  echo "error: formula file not found: $FORMULA" >&2
  exit 2
fi
if [[ ! -w "$FORMULA" ]]; then
  echo "error: formula file not writable: $FORMULA" >&2
  exit 2
fi

# Parse sha256sums.txt. Each usable line: "<64-hex>  <filename>".
# Accept one or two spaces (GNU sha256sum uses two; BSD uses one).
declare -A SUMS
HAVE_ANY=0
while IFS= read -r line || [[ -n "$line" ]]; do
  # Skip empty and comment lines.
  [[ -z "${line// }" ]] && continue
  [[ "$line" =~ ^[[:space:]]*# ]] && continue

  if [[ "$line" =~ ^([0-9a-fA-F]{64})[[:space:]]+\*?([^[:space:]].*)$ ]]; then
    digest="${BASH_REMATCH[1]}"
    name="${BASH_REMATCH[2]}"
    # Strip any leading directory components — only the basename matters.
    name="${name##*/}"
    SUMS["$name"]="$digest"
    HAVE_ANY=1
  fi
done < "$SUMS_FILE"

if [[ $HAVE_ANY -eq 0 ]]; then
  echo "error: no usable lines in $SUMS_FILE (expected '<64-hex>  <filename>')" >&2
  exit 3
fi

# The three artifacts the formula references.
TARGETS=(
  "human-macos-aarch64.bin"
  "human-linux-aarch64.bin"
  "human-linux-x86_64.bin"
)

# Verify all three target hashes are present.
for t in "${TARGETS[@]}"; do
  if [[ -z "${SUMS[$t]:-}" ]]; then
    echo "error: $t not found in $SUMS_FILE (refusing partial update)" >&2
    exit 4
  fi
done

# Rewrite the formula. Strategy: read line-by-line; track the most recent 'url'
# line to choose which 'sha256' placeholder to substitute. This binds each
# digest to its URL context rather than blindly matching all-zeros, so the
# script remains correct even after the first successful run (idempotent).
TMP="$(mktemp "${FORMULA}.tmp.XXXXXX")"
trap 'rm -f "$TMP"' EXIT

current_target=""
version_replaced=0
declare -A REPLACED
for t in "${TARGETS[@]}"; do REPLACED["$t"]=0; done

while IFS= read -r raw || [[ -n "$raw" ]]; do
  line="$raw"

  # Version line — only touch top-level `version "x.y.z"`.
  if [[ $version_replaced -eq 0 && "$line" =~ ^([[:space:]]*)version[[:space:]]+\"[^\"]+\"[[:space:]]*$ ]]; then
    indent="${BASH_REMATCH[1]}"
    printf '%sversion "%s"\n' "$indent" "$VERSION" >> "$TMP"
    version_replaced=1
    continue
  fi

  # Track URL → target binding.
  if [[ "$line" =~ url[[:space:]]+\"([^\"]+)\" ]]; then
    url="${BASH_REMATCH[1]}"
    matched=""
    for t in "${TARGETS[@]}"; do
      if [[ "$url" == *"/$t" ]]; then
        matched="$t"
        break
      fi
    done
    if [[ -n "$matched" ]]; then
      current_target="$matched"
      # Rewrite the URL's version segment too (URL embeds /v<version>/).
      new_url="$(printf '%s' "$url" | sed -E "s#/v[0-9]+\.[0-9]+\.[0-9]+([-+][A-Za-z0-9.+-]+)?/${matched}\$#/v${VERSION}/${matched}#")"
      line="$(printf '%s' "$line" | sed -E "s#\"${url//\//\\/}\"#\"${new_url//\//\\/}\"#")"
    else
      current_target=""
    fi
    printf '%s\n' "$line" >> "$TMP"
    continue
  fi

  # SHA256 line — replace if it follows a URL whose target we recognised.
  if [[ -n "$current_target" && "$line" =~ ^([[:space:]]*)sha256[[:space:]]+\"[0-9a-fA-F]+\"[[:space:]]*$ ]]; then
    indent="${BASH_REMATCH[1]}"
    digest="${SUMS[$current_target]}"
    printf '%ssha256 "%s"\n' "$indent" "$digest" >> "$TMP"
    REPLACED["$current_target"]=1
    current_target=""
    continue
  fi

  printf '%s\n' "$line" >> "$TMP"
done < "$FORMULA"

# Confirm every target was actually replaced. If the formula structure drifted
# (e.g. an architecture block was removed), refuse to ship a partial update.
for t in "${TARGETS[@]}"; do
  if [[ "${REPLACED[$t]}" -ne 1 ]]; then
    echo "error: formula did not contain a sha256 line bound to URL for $t" >&2
    echo "       (formula structure may have changed; refusing partial update)" >&2
    exit 5
  fi
done

if [[ $version_replaced -ne 1 ]]; then
  echo "error: formula did not contain a top-level version \"...\" line" >&2
  exit 5
fi

mv "$TMP" "$FORMULA"
trap - EXIT

echo "ok: rewrote $FORMULA"
echo "    version=$VERSION"
for t in "${TARGETS[@]}"; do
  echo "    $t  ${SUMS[$t]}"
done
