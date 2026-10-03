#!/usr/bin/env python3
"""prune_adapters.py — stop the nightly retrain from filling the disk.

2026-10-03: ~/.human/training-data/adapters/ accumulates ~2.1 GB/night of
`seth-glm-air-mlxtune-<mode>-<stamp>-<stamp>` candidates (scripts/
nightly-retrain.sh's mlx-tune candidate stage) and ~0.5 GB/night of
`seth-m3-outcomes-<stamp>[-glm]` staged adapters (the M3 outcome trainer).
Nothing pruned either family and the disk hit 100%.

Owner-approved policy: keep the newest 2 candidate adapters of each kind.
Everything else in a known nightly-candidate family is a deletion candidate
UNLESS it is protected: the adapter currently served in config.json, an
adapter referenced by any ai.human*.plist, or an adapter present in
registry.json.

Modes (HU_ADAPTER_PRUNE, --mode overrides the env):
  off     - do nothing at all (no classification, no disk access, exit 0)
  shadow  - classify and log exactly what WOULD be deleted (name + GB);
            deletes nothing. DEFAULT.
  live    - classify and actually delete the dirs shadow would have named.

Refuses to run (exit 2, deletes nothing) if config.json cannot be read or
does not name a prod adapter that exists on disk — this script never prunes
blind. `off` mode never even reaches that check: it is a true no-op.

stdlib only. Usage:
  python3 scripts/retrain/prune_adapters.py [--adapters-dir DIR]
      [--mode off|shadow|live] [--keep N] [--config PATH]
      [--registry PATH] [--plist-dir DIR]
"""
import argparse
import json
import os
import re
import shutil
import sys
from pathlib import Path

GIB = 1024.0 ** 3

# Only these two families are nightly-retrain output; anything else under the
# adapters dir (hand-placed adapters, "seth-lora-v6", "dpo-20260802-040017",
# a one-off experiment) is never classified and never touched.
#
# The two trailing groups in each pattern are date-time stamps
# (train-glm-adapter.sh appends its own "-<TAG>-<STAMP>" on top of the
# nightly script's own tag+stamp, see scripts/nightly-retrain.sh). "kind" is
# derived by stripping those stamp groups, so a mode change (simpo/orpo/sft)
# or the presence/absence of a "-glm" base-model suffix each produce their
# own kind automatically — no kind list to keep in sync by hand.
_MLXTUNE_RE = re.compile(r"^seth-glm-air-mlxtune-[a-z0-9]+-\d{8}-\d{4}-\d{8}-\d{6}$")
_M3_OUTCOMES_RE = re.compile(r"^seth-m3-outcomes-\d{8}-\d{6}(-glm)?$")
_STAMP_RE = re.compile(r"-\d{8}-\d{4,6}")


def classify_kind(name):
    """Return this candidate dir's kind, or None if `name` does not match a
    known nightly-retrain pattern (never prune the unrecognized case)."""
    if not (_MLXTUNE_RE.match(name) or _M3_OUTCOMES_RE.match(name)):
        return None
    return _STAMP_RE.sub("", name) or name


def _mtime_or(path, default):
    try:
        return path.stat().st_mtime
    except OSError:
        return default


def sort_key(entry):
    """Newest first: the stamp embedded in the name sorts correctly as a
    string (YYYYMMDD-HHMM... is lexicographically == chronologically
    ordered); fall back to mtime for anything that embeds no stamp we could
    extract, which cannot happen for a name classify_kind() accepted, but the
    fallback keeps this function honest standing alone."""
    name, path = entry
    m = re.search(r"\d{8}-\d{4,6}", name)
    return (m.group(0) if m else "", _mtime_or(path, 0))


def _basenames_from_config(config_path):
    """The live-served adapter's basename, or raise on anything that means
    "cannot establish a protected adapter" — caller turns that into refusal."""
    data = json.loads(Path(config_path).read_text())
    raw = (data.get("personalization", {}) or {}).get("lora_adapter_path") or (
        data.get("mlx_local", {}) or {}
    ).get("adapter_path")
    if not raw:
        raise ValueError(f"{config_path} names no personalization.lora_adapter_path "
                          f"(nor the legacy mlx_local.adapter_path)")
    served_dir = Path(raw)
    if not served_dir.is_dir():
        raise ValueError(f"{config_path} names {raw!r} but it does not exist on disk")
    return served_dir.name


def _basenames_from_plists(plist_dir):
    out = set()
    d = Path(plist_dir)
    if not d.is_dir():
        return out
    for p in sorted(d.glob("ai.human*.plist")):
        try:
            text = p.read_text(errors="ignore")
        except OSError:
            continue
        for m in re.finditer(r"/adapters/([A-Za-z0-9._-]+)", text):
            out.add(m.group(1))
    return out


def _basenames_from_registry(registry_path):
    out = set()
    p = Path(registry_path)
    if not p.is_file():
        return out
    try:
        data = json.loads(p.read_text())
    except (OSError, ValueError):
        return out
    adapters = data.get("adapters", data) if isinstance(data, dict) else data
    if isinstance(adapters, dict):
        out.update(str(k) for k in adapters.keys())
    elif isinstance(adapters, list):
        for entry in adapters:
            if not isinstance(entry, dict):
                continue
            for key in ("name", "id", "adapter_id", "path", "adapter", "adapter_path", "dir"):
                v = entry.get(key)
                if isinstance(v, str) and v:
                    out.add(v.rstrip("/").split("/")[-1])
    return out


def dir_size_bytes(path):
    total = 0
    for root, dirs, files in os.walk(path, followlinks=False):
        dirs[:] = [d for d in dirs if not os.path.islink(os.path.join(root, d))]
        for f in files:
            fp = os.path.join(root, f)
            if os.path.islink(fp):
                continue
            try:
                total += os.path.getsize(fp)
            except OSError:
                pass
    return total


class RefusedError(RuntimeError):
    """Raised when pruning must not proceed — never prune blind."""


def plan(adapters_dir, keep, config_path, registry_path, plist_dir):
    """Classify every entry of adapters_dir. Returns (kept_names, to_delete)
    where to_delete is a list of (name, path, size_bytes), newest-first
    ordering not guaranteed. Raises RefusedError if the served adapter
    cannot be established — the one hard precondition for ANY classification,
    because "protected" is meaningless without it."""
    adapters_dir = Path(adapters_dir)
    if not adapters_dir.is_dir():
        raise RefusedError(f"adapters dir does not exist: {adapters_dir}")

    try:
        served = _basenames_from_config(config_path)
    except (OSError, ValueError) as exc:
        raise RefusedError(str(exc)) from exc

    protected = {served}
    protected |= _basenames_from_plists(plist_dir)
    protected |= _basenames_from_registry(registry_path)

    # realpath containment: a candidate name that resolves outside
    # adapters_dir (symlink escape) is never classified, matching the
    # "unknown pattern" treatment — never touched, never counted as kept.
    root_real = os.path.realpath(str(adapters_dir))

    by_kind = {}
    for child in sorted(adapters_dir.iterdir()):
        if not child.is_dir():
            continue
        real = os.path.realpath(str(child))
        if os.path.commonpath([root_real, real]) != root_real or real == root_real:
            continue
        kind = classify_kind(child.name)
        if kind is None:
            continue
        by_kind.setdefault(kind, []).append((child.name, child))

    kept = set()
    to_delete = []
    for kind, entries in by_kind.items():
        entries.sort(key=sort_key, reverse=True)
        for idx, (name, path) in enumerate(entries):
            if name in protected or idx < keep:
                kept.add(name)
            else:
                to_delete.append((name, path, dir_size_bytes(path)))

    return kept, to_delete, sorted(by_kind.keys())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--adapters-dir",
                    default=str(Path.home() / ".human" / "training-data" / "adapters"))
    ap.add_argument("--mode", choices=["off", "shadow", "live"],
                    default=os.environ.get("HU_ADAPTER_PRUNE", "shadow"))
    ap.add_argument("--keep", type=int, default=2)
    ap.add_argument("--config", default=str(Path.home() / ".human" / "config.json"))
    ap.add_argument("--registry", default=None,
                    help="default: <adapters-dir>/registry.json")
    ap.add_argument("--plist-dir", default=str(Path.home() / "Library" / "LaunchAgents"))
    args = ap.parse_args(argv)

    if args.mode == "off":
        print("[adapter_prune] mode=off kinds=- kept=0 deleted=0 freed_gb=0.00 (disabled)")
        return 0

    registry_path = args.registry or str(Path(args.adapters_dir) / "registry.json")

    try:
        kept, to_delete, kinds = plan(args.adapters_dir, args.keep, args.config, registry_path,
                                      args.plist_dir)
    except RefusedError as exc:
        print(f"[adapter_prune] REFUSED: {exc} — never pruning blind", file=sys.stderr)
        return 2

    freed = 0
    deleted_count = 0
    for name, path, size in to_delete:
        gb = size / GIB
        if args.mode == "shadow":
            print(f"[adapter_prune shadow] would delete {name} ({gb:.2f} GB)")
            freed += size
            deleted_count += 1
        else:
            try:
                shutil.rmtree(path)
                print(f"[adapter_prune live] deleted {name} ({gb:.2f} GB)")
                freed += size
                deleted_count += 1
            except OSError as exc:
                print(f"[adapter_prune live] FAILED to delete {name}: {exc}", file=sys.stderr)

    print(f"[adapter_prune] mode={args.mode} kinds={','.join(kinds) or '-'} "
          f"kept={len(kept)} deleted={deleted_count} freed_gb={freed / GIB:.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
