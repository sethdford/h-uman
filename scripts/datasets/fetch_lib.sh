#!/usr/bin/env bash
# Shared by scripts/datasets/fetch_<name>.sh — sourced, not executed.
#
# hu_fetch_pinned NAME URL SHA256 FILE REVISION LICENSE
#   Downloads URL into $HU_DATASETS_DIR/NAME/FILE (default ~/.human/datasets)
#   and accepts it only if its sha256 equals SHA256. Idempotent: a present
#   file with the right checksum is never re-downloaded; a corrupted one is
#   replaced. On mismatch nothing is written and the caller exits 2.
#   Writes NAME/PROVENANCE (url, revision, sha256, license) on download.
#
# Test-only override (hermetic tests, file:// URLs): HU_FETCH_TEST_URL and
# HU_FETCH_TEST_SHA256 must be set TOGETHER; one without the other is refused,
# so an override can never silently drop the checksum.
#
# Exit codes: 0 ok · 1 usage/environment · 2 download failed or checksum mismatch.

hu_sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

hu_write_provenance() {
  local name="$1" url="$2" sha="$3" revision="$4" license="$5" when="$6"
  {
    echo "dataset: $name"
    echo "url: $url"
    echo "revision: $revision"
    echo "sha256: $sha"
    echo "license: $license"
    echo "$when"
    echo "use: internal memory-mechanics evaluation only; never persona training; never commit"
  } >"$HU_DATASET_DIR/PROVENANCE"
}

hu_fetch_pinned() {
  local name="$1" url="$2" sha="$3" file="$4" revision="$5" license="$6"
  if [[ -n "${HU_FETCH_TEST_URL:-}" || -n "${HU_FETCH_TEST_SHA256:-}" ]]; then
    if [[ -z "${HU_FETCH_TEST_URL:-}" || -z "${HU_FETCH_TEST_SHA256:-}" ]]; then
      echo "[$name] HU_FETCH_TEST_URL and HU_FETCH_TEST_SHA256 must be set together" >&2
      exit 1
    fi
    url="$HU_FETCH_TEST_URL"
    sha="$HU_FETCH_TEST_SHA256"
  fi
  command -v curl >/dev/null 2>&1 || { echo "[$name] curl is required" >&2; exit 1; }

  HU_DATASET_DIR="${HU_DATASETS_DIR:-$HOME/.human/datasets}/$name"
  HU_DATASET_FILE="$HU_DATASET_DIR/$file"
  mkdir -p "$HU_DATASET_DIR"

  if [[ -f "$HU_DATASET_FILE" ]]; then
    if [[ "$(hu_sha256 "$HU_DATASET_FILE")" == "$sha" ]]; then
      echo "[$name] already present, sha256 verified: $HU_DATASET_FILE"
      [[ -f "$HU_DATASET_DIR/PROVENANCE" ]] || hu_write_provenance "$name" "$url" "$sha" \
        "$revision" "$license" "verified_at: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
      return 0
    fi
    echo "[$name] local copy fails the checksum; re-downloading" >&2
  fi

  local tmp
  tmp="$(mktemp "$HU_DATASET_DIR/.download.XXXXXX")"
  echo "[$name] fetching $url"
  if ! curl -fsSL --retry 2 --max-time 900 -o "$tmp" "$url"; then
    rm -f "$tmp"
    echo "[$name] download failed: $url" >&2
    exit 2
  fi
  local got
  got="$(hu_sha256 "$tmp")"
  if [[ "$got" != "$sha" ]]; then
    rm -f "$tmp"
    echo "[$name] checksum mismatch: expected $sha got $got — nothing written" >&2
    exit 2
  fi
  mv -f "$tmp" "$HU_DATASET_FILE"
  hu_write_provenance "$name" "$url" "$sha" "$revision" "$license" \
    "fetched_at: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "[$name] ok: $HU_DATASET_FILE ($sha)"
}
