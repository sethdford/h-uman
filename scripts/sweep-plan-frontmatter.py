#!/usr/bin/env python3
"""Sweep plan frontmatter based on audit verdicts."""
import os
import re
import sys
from pathlib import Path

AUDIT_DIR = Path("/Users/sethford/Projects/h-uman/.claude/worktrees/beautiful-mcclintock-4701a5/docs/research/2026-05-17-plan-validation/by-plan")
PLANS_DIR = Path("docs/plans")

def parse_frontmatter(text):
    """Return (frontmatter_dict, frontmatter_raw_lines, body) or (None, None, text)."""
    if not text.startswith("---\n"):
        return None, None, text
    end = text.find("\n---\n", 4)
    if end < 0:
        return None, None, text
    raw_fm = text[4:end]
    body = text[end+5:]
    fm = {}
    for line in raw_fm.split("\n"):
        if ":" in line and not line.strip().startswith("#"):
            k, _, v = line.partition(":")
            fm[k.strip()] = v.strip()
    return fm, raw_fm, body

def parse_audit(audit_file):
    """Return dict from audit file frontmatter."""
    text = audit_file.read_text()
    fm, _, _ = parse_frontmatter(text)
    return fm or {}

def update_plan_frontmatter(plan_path, new_status, last_audit, superseded_by=None):
    """Update status: line and append last_audit: line. Returns (old_status, new_status)."""
    text = plan_path.read_text()
    fm, raw_fm, body = parse_frontmatter(text)
    if fm is None:
        # No frontmatter — create one
        new_fm = f"status: {new_status}\nlast_audit: {last_audit}\n"
        if superseded_by:
            new_fm = f"status: {new_status}\nsuperseded_by: {superseded_by}\nlast_audit: {last_audit}\n"
        new_text = f"---\n{new_fm}---\n\n" + text
        plan_path.write_text(new_text)
        return ("(none)", new_status)

    old_status = fm.get("status", "(none)")
    lines = raw_fm.split("\n")
    out_lines = []
    has_status = False
    has_last_audit = False
    has_superseded_by = False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("status:"):
            out_lines.append(f"status: {new_status}")
            has_status = True
        elif stripped.startswith("last_audit:"):
            out_lines.append(f"last_audit: {last_audit}")
            has_last_audit = True
        elif stripped.startswith("superseded_by:") and superseded_by:
            out_lines.append(f"superseded_by: {superseded_by}")
            has_superseded_by = True
        else:
            out_lines.append(line)
    if not has_status:
        out_lines.append(f"status: {new_status}")
    if superseded_by and not has_superseded_by:
        out_lines.append(f"superseded_by: {superseded_by}")
    if not has_last_audit:
        out_lines.append(f"last_audit: {last_audit}")
    # Strip trailing empty lines
    while out_lines and out_lines[-1] == "":
        out_lines.pop()
    new_fm = "\n".join(out_lines)
    new_text = f"---\n{new_fm}\n---\n{body}"
    plan_path.write_text(new_text)
    return (old_status, new_status)


def main():
    if not PLANS_DIR.exists():
        print(f"PLANS_DIR not found: {PLANS_DIR.resolve()}")
        sys.exit(1)
    if not AUDIT_DIR.exists():
        print(f"AUDIT_DIR not found: {AUDIT_DIR}")
        sys.exit(1)

    audit_files = sorted(AUDIT_DIR.glob("*.md"))
    modifications = []
    skipped = {"verdict": 0, "confidence": 0, "missing_plan": 0}

    for af in audit_files:
        audit_fm = parse_audit(af)
        verdict = audit_fm.get("verdict", "").strip()
        confidence = audit_fm.get("confidence", "").strip()
        plan_rel = audit_fm.get("plan", "").strip()

        # Determine target status
        target_status = None
        superseded_by = None

        if verdict == "SHIPPED" and confidence == "HIGH":
            target_status = "complete"
        elif verdict == "SUPERSEDED":
            target_status = "superseded"
            # Try to extract superseded_by from audit body if present
            body_text = af.read_text()
            m = re.search(r"superseded[_ ]by[:\s]+([^\n]+)", body_text, re.IGNORECASE)
            if m:
                sb = m.group(1).strip().strip(".`")
                # Only keep if it looks like a plan path
                if "docs/plans/" in sb or sb.endswith(".md"):
                    superseded_by = sb
        elif verdict == "OBSOLETE":
            target_status = "obsolete"
        else:
            if verdict in ("PARTIAL", "NOT_STARTED", "SHIPPED_UNWIRED"):
                skipped["verdict"] += 1
            elif confidence != "HIGH" and verdict == "SHIPPED":
                skipped["confidence"] += 1
            continue

        # For SHIPPED, require HIGH confidence
        if verdict == "SHIPPED" and confidence != "HIGH":
            skipped["confidence"] += 1
            continue

        # Resolve plan file path
        plan_name = af.name  # same name as audit file
        plan_path = PLANS_DIR / plan_name
        if not plan_path.exists():
            skipped["missing_plan"] += 1
            print(f"WARN: plan file missing: {plan_path}", file=sys.stderr)
            continue

        old, new = update_plan_frontmatter(plan_path, target_status, "2026-05-17", superseded_by=superseded_by)
        modifications.append((plan_name, verdict, confidence, old, new, superseded_by))

    print(f"\n=== Modifications: {len(modifications)} ===")
    by_transition = {}
    for name, verdict, conf, old, new, sb in modifications:
        key = f"{old} -> {new}"
        by_transition.setdefault(key, []).append(name)
    for key, files in sorted(by_transition.items()):
        print(f"  {key}: {len(files)}")

    print(f"\n=== Skipped ===")
    for k, v in skipped.items():
        print(f"  {k}: {v}")

    print(f"\n=== Sample (first 5) ===")
    for m in modifications[:5]:
        print(f"  {m[0]}: verdict={m[1]} conf={m[2]} status: {m[3]} -> {m[4]}" + (f" superseded_by={m[5]}" if m[5] else ""))

if __name__ == "__main__":
    main()
