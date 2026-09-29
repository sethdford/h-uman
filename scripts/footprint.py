#!/usr/bin/env python3
"""One measured source for every footprint number h-uman publishes.

docs/perf/footprint.json          what was measured (scripts/measure-build-footprint.sh)
docs/perf/footprint-budget.json   policy: which build the claims describe, the hard
                                  budgets, and when a fresh measurement refreshes docs

Every published footprint string is derived from those two files, never typed:

  Markdown       <!-- fp:binary_kb -->~2694 KB<!-- /fp -->     (inline marker)
  Code comments  // fp-template: "... {{binary_mb}} binary ..."  (renders the NEXT line)
  Website        website/src/data/footprint.json                (generated, imported)

Commands
  sync            rewrite every marker, template line and the website data file
  check           fail if anything is stale, if an unmanaged footprint number appears
                  in a live doc, or if the committed measurement breaks a budget
  evaluate FRESH  compare a fresh measurement with the budgets (exit 1 on a breach)
                  and with the published claims; report which claims it falsifies
  adopt FRESH     record a fresh measurement in footprint.json, then sync

Exit codes: 0 ok · 1 stale / breach · 2 unusable input (never a verdict; see
.claude/rules/no-number-without-a-measurement.md).
"""
from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

MEASURED_REL = "docs/perf/footprint.json"
BUDGET_REL = "docs/perf/footprint-budget.json"
WEBSITE_DATA_REL = "website/src/data/footprint.json"

MARKER_RE = re.compile(r"<!-- fp:([a-z0-9_]+) -->(.*?)<!-- /fp -->")
TEMPLATE_RE = re.compile(r"^\s*(?://|#|<!--|\{/\*)\s*fp-template:\s?(.*?)\s*(?:-->|\*/\})?\s*$")
PLACEHOLDER_RE = re.compile(r"\{\{([a-z0-9_]+)\}\}")

# Files that carry markers/templates, and that the unmanaged-claim lint reads.
SCAN_SUFFIXES = {".md", ".mdc", ".mdx", ".astro", ".html", ".swift", ".kt", ".c", ".h"}
# History is allowed to say what the numbers used to be.
EXEMPT_PREFIXES = (
    "docs/plans/", "docs/research/", "docs/superpowers/", "docs/perf/", "docs/evaluation/",
    "sprints/", "specs/", "archive/", "vendor/", "third_party/", "tests/", "fuzz/",
    ".claude/", "website/src/data/", "node_modules/", "docs/investigations/",
    "website/vision-",  # standalone design-concept mockups, not the built site
)
EXEMPT_FILES = {"CHANGELOG.md"}

KEYWORD_RE = re.compile(r"binary|\bRSS\b|\bRAM\b|footprint|startup|cold[- ]start|\bboots?\b", re.I)
# In code, only string literals that talk about the binary: "RAM: 128 KB" in a
# board description (src/tools/hardware_info.c) is not a claim about h-uman.
CODE_KEYWORD_RE = re.compile(r"binary|\bRSS\b|startup", re.I)
# "~2694 KB" needs no keyword: in a live doc a bare approximate KB size is the binary.
TILDE_KB_RE = re.compile(r"~\s?\d{4,}\s?KB\b")
SIZE_RE = re.compile(r"(?:~|≈|<|&lt;|≤)?\s?\d[\d,]*(?:\.\d+)?\s?(?:KB|MB|KiB|MiB)\b")
MS_RE = re.compile(r"(?:(?:~|<|&lt;)\s?\d+(?:\.\d+)?|\d+(?:\.\d+)?\s?[–-]\s?\d+(?:\.\d+)?)\s?ms\b")


class InputError(Exception):
    """Unusable input: missing file, malformed measurement. Exit 2."""


# ── Claims ───────────────────────────────────────────────────────────────────

@dataclass
class Claim:
    key: str
    text: str
    kind: str                 # approx | bound | range | exact
    build: str | None = None  # the measured build this claim describes
    metric: str | None = None
    shown: float | None = None      # approx: the value the text displays
    limit: float | None = None      # bound: claim is "< limit"
    lo: float | None = None         # range: claim is "lo–hi"
    hi: float | None = None


def _metric(build: dict, metric: str) -> float:
    if metric == "startup_ms_median":
        samples = build.get("startup_ms_samples")
        if samples:
            return statistics.median(samples)
        return build["startup_ms_median"]
    return build[metric]


def _startup_span(build: dict) -> tuple[float, float]:
    samples = build.get("startup_ms_samples") or []
    if not samples:
        raise InputError(f"build {build.get('name')!r} has no startup_ms_samples")
    return min(samples), max(samples)


def _platform(build: dict) -> str:
    # "macOS arm64 26.6.2" -> "macOS arm64": the OS version is provenance, not a claim.
    return " ".join(build.get("platform", "").split()[:2])


def derive(measured: dict, budget: dict) -> dict[str, Claim]:
    builds = measured.get("builds") or {}
    pname, fname = budget["primary_build"], budget.get("full_build")
    if pname not in builds:
        raise InputError(f"{MEASURED_REL} has no measurement for the primary build {pname!r}")
    P = builds[pname]
    for field in ("binary_bytes", "idle_rss_bytes", "version_peak_rss_bytes", "measured_at", "git_rev"):
        if not P.get(field):
            raise InputError(f"primary build {pname!r} is missing {field}")
    c: dict[str, Claim] = {}

    def approx(key, text, build, metric, shown):
        c[key] = Claim(key, text, "approx", build, metric, shown=shown)

    def bound(key, value, unit, scale, build, metric):
        n = math.floor(value / scale) + 1
        c[key] = Claim(key, f"<{n} {unit}", "bound", build, metric, limit=n * scale)

    b = P["binary_bytes"]
    approx("binary_kb", f"~{b // 1024} KB", pname, "binary_bytes", (b // 1024) * 1024)
    approx("binary_mb", f"~{round(b / 1e6)} MB", pname, "binary_bytes", round(b / 1e6) * 1e6)
    if P.get("text_section_bytes"):
        t = P["text_section_bytes"]
        approx("text_kb", f"{t // 1024} KB", pname, "text_section_bytes", (t // 1024) * 1024)
    for key, metric in (("idle_rss", "idle_rss_bytes"), ("version_rss", "version_peak_rss_bytes")):
        v = P[metric]
        approx(f"{key}_mb", f"{v / 1e6:.1f} MB", pname, metric, round(v / 1e6, 1) * 1e6)
        bound(f"{key}_bound", v, "MB", 1e6, pname, metric)
    lo, hi = _startup_span(P)
    a, z = math.floor(lo), math.ceil(hi)
    if z == a:
        z = a + 1
    c["startup_range"] = Claim("startup_range", f"{a}–{z} ms", "range", pname, "startup_ms_median", lo=a, hi=z)
    bound("startup_bound", hi, "ms", 1, pname, "startup_ms_median")
    c["measured_date"] = Claim("measured_date", P["measured_at"][:10], "exact")
    c["measured_rev"] = Claim("measured_rev", P.get("code_rev") or P["git_rev"], "exact")
    c["measured_platform"] = Claim("measured_platform", _platform(P), "exact")

    if fname and fname in builds:
        F = builds[fname]
        approx("full_binary_mb", f"{F['binary_bytes'] / 1e6:.1f} MB", fname, "binary_bytes",
               round(F["binary_bytes"] / 1e6, 1) * 1e6)
        c["full_binary_bytes"] = Claim("full_binary_bytes", f"{F['binary_bytes']:,}", "exact")
        approx("full_idle_rss_mb", f"{F['idle_rss_bytes'] / 1e6:.1f} MB", fname, "idle_rss_bytes",
               round(F["idle_rss_bytes"] / 1e6, 1) * 1e6)
        c["full_measured_date"] = Claim("full_measured_date", F["measured_at"][:10], "exact")

    B = budget["budgets"]
    c["budget_binary_kb"] = Claim("budget_binary_kb", f"{B['binary_bytes']['max'] // 1024} KB", "exact")
    c["budget_version_rss_mb"] = Claim("budget_version_rss_mb", f"{B['version_peak_rss_bytes']['max'] / 1e6:g} MB", "exact")
    c["budget_startup_ms"] = Claim("budget_startup_ms", f"{B['startup_ms_median']['max']:g} ms", "exact")
    return c


def claim_holds(claim: Claim, fresh: dict, tolerance: float, fresh_text: str | None) -> tuple[bool, str]:
    """Is this published claim still true of the fresh measurement?

    fresh_text is what the same key renders to from the fresh measurement. A rounded
    claim ("~3 MB") can sit further than the tolerance from its own measurement, so an
    approximate claim holds when it is within tolerance OR renders identically."""
    value = _metric(fresh, claim.metric)
    if claim.kind == "approx":
        err = abs(value - claim.shown) / value if value else 1.0
        ok = err <= tolerance or fresh_text == claim.text
        return ok, f"shows {claim.text}, measured {value:,.0f} ({err:.1%} off, tolerance {tolerance:.0%})"
    if claim.kind == "bound":
        return value < claim.limit, f"claims {claim.text}, measured {value:,.2f}"
    if claim.kind == "range":
        return claim.lo <= value <= claim.hi, f"claims {claim.text}, measured median {value:.2f} ms"
    return True, ""


# ── Rendering ────────────────────────────────────────────────────────────────

class Repo:
    def __init__(self, root: Path):
        self.root = root

    def load(self, rel: str) -> dict:
        p = self.root / rel
        if not p.exists():
            raise InputError(f"{rel} not found")
        try:
            return json.loads(p.read_text())
        except json.JSONDecodeError as e:
            raise InputError(f"{rel}: {e}") from e

    def claims(self) -> dict[str, Claim]:
        return derive(self.load(MEASURED_REL), self.load(BUDGET_REL))

    def files(self) -> list[Path]:
        try:
            out = subprocess.run(["git", "-C", str(self.root), "ls-files", "-z"], check=True,
                                 capture_output=True, text=True).stdout
            rels = [r for r in out.split("\0") if r]
        except (subprocess.CalledProcessError, FileNotFoundError):
            rels = [str(p.relative_to(self.root)) for p in self.root.rglob("*") if p.is_file()]
        return [self.root / r for r in sorted(rels) if Path(r).suffix in SCAN_SUFFIXES]

    @staticmethod
    def exempt(rel: str) -> bool:
        return rel in EXEMPT_FILES or rel.startswith(EXEMPT_PREFIXES)


def render_text(text: str, claims: dict[str, Claim], where: str, errors: list[str]) -> str:
    def text_of(key: str) -> str | None:
        if key not in claims:
            errors.append(f"{where}: unknown footprint key {key!r} (known: {', '.join(sorted(claims))})")
            return None
        return claims[key].text

    def sub_marker(m: re.Match) -> str:
        t = text_of(m.group(1))
        return m.group(0) if t is None else f"<!-- fp:{m.group(1)} -->{t}<!-- /fp -->"

    lines = text.split("\n")
    out = []
    i = 0
    while i < len(lines):
        line = MARKER_RE.sub(sub_marker, lines[i])
        out.append(line)
        tm = TEMPLATE_RE.match(line)
        if tm and "fp-template:" in line:
            if i + 1 >= len(lines):
                errors.append(f"{where}:{i + 1}: fp-template has no following line to render")
            else:
                nxt = lines[i + 1]
                indent = nxt[: len(nxt) - len(nxt.lstrip())]
                rendered = PLACEHOLDER_RE.sub(lambda m: text_of(m.group(1)) or m.group(0), tm.group(1))
                out.append(indent + rendered)
                i += 1
        i += 1
    return "\n".join(out)


def website_data(claims: dict[str, Claim], measured: dict, budget: dict) -> str:
    """Strings for prose, raw numbers (SI MB, ms) for charts. The site imports this."""
    P = measured["builds"][budget["primary_build"]]
    doc = {
        "_generated_by": "scripts/footprint.py sync; do not edit. Source: docs/perf/footprint.json",
        "text": {k: c.text for k, c in sorted(claims.items())},
        "measured": {
            "binary_mb": round(P["binary_bytes"] / 1e6, 2),
            "idle_rss_mb": round(P["idle_rss_bytes"] / 1e6, 2),
            "version_rss_mb": round(P["version_peak_rss_bytes"] / 1e6, 2),
            "startup_ms_median": round(_metric(P, "startup_ms_median"), 2),
        },
    }
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def plan_sync(repo: Repo) -> tuple[dict[Path, str], list[str]]:
    """Every file whose content would change, mapped to its new content."""
    measured, budget = repo.load(MEASURED_REL), repo.load(BUDGET_REL)
    claims = derive(measured, budget)
    errors: list[str] = []
    changes: dict[Path, str] = {}
    for path in repo.files():
        try:
            old = path.read_text()
        except (UnicodeDecodeError, FileNotFoundError):
            continue
        if "fp:" not in old and "fp-template:" not in old:
            continue
        new = render_text(old, claims, str(path.relative_to(repo.root)), errors)
        if new != old:
            changes[path] = new
    web = repo.root / WEBSITE_DATA_REL
    if web.parent.is_dir():
        data = website_data(claims, measured, budget)
        if not web.exists() or web.read_text() != data:
            changes[web] = data
    return changes, errors


def lint(repo: Repo) -> list[str]:
    """Footprint-shaped numbers in live files that nothing keeps in sync."""
    found = []
    for path in repo.files():
        rel = str(path.relative_to(repo.root))
        if repo.exempt(rel):
            continue
        try:
            lines = path.read_text().split("\n")
        except (UnicodeDecodeError, FileNotFoundError):
            continue
        code = path.suffix in {".c", ".h", ".swift", ".kt"}
        skip_next = False
        for n, line in enumerate(lines, 1):
            if skip_next:
                skip_next = False
                continue
            if "fp-template:" in line:
                skip_next = True
                continue
            if "fp:" in line or "fp.text." in line or "fp.measured." in line:
                continue             # a managed line; competitor numbers may share it
            if code:
                if '"' in line and CODE_KEYWORD_RE.search(line) and (SIZE_RE.search(line) or MS_RE.search(line)):
                    found.append(f"{rel}:{n}: {line.strip()[:140]}")
                continue
            if TILDE_KB_RE.search(line) or (KEYWORD_RE.search(line) and (SIZE_RE.search(line) or MS_RE.search(line))):
                found.append(f"{rel}:{n}: {line.strip()[:140]}")
    return found


def budget_breaches(budget: dict, build: dict) -> list[str]:
    out = []
    for metric, rule in budget["budgets"].items():
        if rule["build"] != build["name"]:
            continue
        value = _metric(build, metric)
        if value > rule["max"]:
            out.append(f"{metric} = {value:,.2f} exceeds the budget {rule['max']:,} "
                       f"({BUDGET_REL}: {rule['why']})")
    return out


# ── Commands ─────────────────────────────────────────────────────────────────

REQUIRED = ("name", "binary_bytes", "idle_rss_bytes", "version_peak_rss_bytes",
            "startup_ms_samples", "measured_at", "git_rev", "platform")


def load_fresh(path: str) -> dict:
    try:
        fresh = json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError) as e:
        raise InputError(f"cannot read fresh measurement {path}: {e}") from e
    missing = [f for f in REQUIRED if not fresh.get(f)]
    if missing:
        raise InputError(f"fresh measurement {path} lacks {', '.join(missing)}; refusing to use it")
    if len(fresh["startup_ms_samples"]) < 5:
        raise InputError(f"fresh measurement {path} has only {len(fresh['startup_ms_samples'])} startup samples")
    return fresh


def cmd_sync(repo: Repo, args) -> int:
    changes, errors = plan_sync(repo)
    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    if errors:
        return 2
    for path, content in changes.items():
        path.write_text(content)
        print(f"synced {path.relative_to(repo.root)}")
    if not changes:
        print("footprint claims already in sync")
    return 0


def cmd_check(repo: Repo, args) -> int:
    changes, errors = plan_sync(repo)
    problems = list(errors)
    for path in changes:
        problems.append(f"{path.relative_to(repo.root)}: stale footprint claims "
                        "(run: python3 scripts/footprint.py sync)")
    for hit in lint(repo):
        problems.append(f"unmanaged footprint claim — wrap it in <!-- fp:KEY -->…<!-- /fp -->, "
                        f"an fp-template line, or mark the line fp:ignore: {hit}")
    measured, budget = repo.load(MEASURED_REL), repo.load(BUDGET_REL)
    for name, build in (measured.get("builds") or {}).items():
        for breach in budget_breaches(budget, {**build, "name": name}):
            problems.append(f"committed measurement of {name}: {breach}")
    for p in problems:
        print(p)
    if problems:
        return 1
    print(f"footprint: all claims match {MEASURED_REL}; no unmanaged claims; budgets hold")
    return 0


def cmd_evaluate(repo: Repo, args) -> int:
    fresh = load_fresh(args.fresh)
    measured, budget = repo.load(MEASURED_REL), repo.load(BUDGET_REL)
    claims = derive(measured, budget)
    fresh_claims = derive({**measured, "builds": {**measured.get("builds", {}), fresh["name"]: fresh}}, budget)
    tol = budget["heal"]["approx_tolerance"]
    breaches = budget_breaches(budget, fresh)
    stale = []
    for claim in claims.values():
        if claim.build != fresh["name"] or claim.kind == "exact":
            continue
        fresh_claim = fresh_claims.get(claim.key)
        ok, why = claim_holds(claim, fresh, tol, fresh_claim.text if fresh_claim else None)
        if not ok:
            stale.append(f"{claim.key}: {why}")
    heal = bool(stale)
    lines = [f"## Footprint of `{fresh['name']}` at {fresh['git_rev']} ({fresh.get('host', '?')}, "
             f"{fresh['platform']}, load {fresh.get('load_avg_1m', '?')})", ""]
    lines += ["### Budget breaches", *(f"- {b}" for b in breaches), ""] if breaches else ["Budgets: all hold.", ""]
    if heal:
        lines += ["### Published claims this measurement falsifies", *(f"- {s}" for s in stale), "",
                  "The docs need a refresh: `python3 scripts/footprint.py adopt <fresh.json>`."]
    else:
        lines += ["Published claims: all still true within tolerance; no refresh needed."]
    report = "\n".join(lines) + "\n"
    print(report)
    if args.report:
        Path(args.report).write_text(report)
    if args.github_output:
        with open(args.github_output, "a") as fh:
            fh.write(f"heal={'true' if heal else 'false'}\nbreach={'true' if breaches else 'false'}\n")
    return 1 if breaches else 0


def cmd_adopt(repo: Repo, args) -> int:
    fresh = load_fresh(args.fresh)
    measured = repo.load(MEASURED_REL)
    measured.setdefault("builds", {})[fresh["name"]] = fresh
    (repo.root / MEASURED_REL).write_text(json.dumps(measured, indent=2, ensure_ascii=False) + "\n")
    print(f"recorded {fresh['name']} ({fresh['git_rev']}, {fresh['measured_at']}) in {MEASURED_REL}")
    return cmd_sync(repo, args)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("sync")
    sub.add_parser("check")
    ev = sub.add_parser("evaluate")
    ev.add_argument("fresh")
    ev.add_argument("--report")
    ev.add_argument("--github-output")
    ad = sub.add_parser("adopt")
    ad.add_argument("fresh")
    args = ap.parse_args(argv)
    repo = Repo(Path(args.root).resolve())
    try:
        return {"sync": cmd_sync, "check": cmd_check, "evaluate": cmd_evaluate, "adopt": cmd_adopt}[args.cmd](repo, args)
    except InputError as e:
        print(f"footprint: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
