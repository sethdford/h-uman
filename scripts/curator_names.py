"""Nightly typed-name pass helpers (spec 2026-09-29-named-entity-extraction §4.4).

Pure over curator_evidence rows, so every decision is testable without chat.db,
a model or the human binary. Only [tN] rows (the contact's texts and Seth's own)
are evidence; a name is kept only when it is said, word-bounded and
case-insensitively, in a row it cites. Python never writes graph.db: kept names
become entity lines for `human memory import-facts`.
"""
import contextlib
import json
import os
import re
import subprocess
import tempfile

import curator_evidence as ce

TYPES = ("person", "place", "org", "event", "topic")
SOURCE = "names:nightly"
CONFIDENCE = 0.8
MAX_NAME_LEN = 60
SELF_NAMES = frozenset({"seth", "seth ford"})

SYSTEM = (
    "You are reading Seth Ford's recent texts with one person. List the specific names "
    "the texts mention: people, places, organizations, named events (a trip, a wedding, a "
    "game), and recurring topics (a short lowercase phrase, e.g. \"the lake house\"). Only "
    "names written in a [tN] text; never a [dN] text. Never Seth himself and never the "
    "person he is texting.\n\n"
    "Output ONLY a JSON array of objects: {\"name\": the name exactly as written, "
    "\"type\": \"person\"|\"place\"|\"org\"|\"event\"|\"topic\", \"evidence\": [the N of "
    "each [tN] text that contains the name]}. No prose before or after."
)


def build_prompt(lines):
    return SYSTEM, "texts (oldest first):\n" + "\n".join(lines)


def parse_names(text):
    """Model output -> [{"name", "type", "evidence_tokens"}]; malformed items dropped."""
    m = re.search(r"\[[\s\S]*\]", text or "")
    if not m:
        return []
    try:
        arr = json.loads(m.group(0))
    except ValueError:
        return []
    out = []
    for o in arr if isinstance(arr, list) else []:
        if not isinstance(o, dict):
            continue
        name = str(o.get("name") or "").strip()
        ev = o.get("evidence") if isinstance(o.get("evidence"), list) else []
        if name:
            out.append({"name": name, "type": str(o.get("type") or "").strip().lower(),
                        "evidence_tokens": [str(e) for e in ev]})
    return out


def drop_names(display_name):
    """Lowercased names never written for this contact: Seth's and the contact's own."""
    out = set(SELF_NAMES)
    dn = (display_name or "").strip().lower()
    if dn:
        out.add(dn)
        out.add(dn.split()[0])
    return out


def canonical_name(name, ntype):
    """Name types are stored with capitalized words ("priya" -> "Priya", "st pete" ->
    "St Pete"), so lowercase texting lands on the same row as the per-turn catcher's
    Capitalized names. Topics keep the model's spelling."""
    if ntype == "topic":
        return name
    return " ".join(w[:1].upper() + w[1:] if w[:1].islower() else w for w in name.split())


def verify_names(proposed, cite, drop):
    """-> (kept [{"name", "type"}], rejected count). Kept only when the type is known,
    the name is 2-60 chars and not dropped, it cites >= 1 [tN] row and no [dN] row, and
    it is said in a cited row. One entry per canonical name (first wins)."""
    kept, seen, rejected = [], set(), 0
    for p in proposed:
        name, ntype = p["name"], p["type"]
        canon = canonical_name(name, ntype)
        if canon.lower() in seen:
            continue
        t_idx, daemon = ce.parse_evidence(p["evidence_tokens"])
        rows = [cite[i] for i in t_idx if i in cite]
        ok = (ntype in TYPES and 2 <= len(name) <= MAX_NAME_LEN and name.lower() not in drop
              and not daemon and bool(rows) and ce.name_said(name, [r[3] for r in rows]))
        if not ok:
            rejected += 1
            continue
        seen.add(canon.lower())
        kept.append({"name": canon, "type": ntype})
    return kept, rejected


def entity_lines(handle, kept, source=SOURCE, confidence=CONFIDENCE, retype_only=False):
    lines = []
    for k in kept:
        line = {"kind": "entity", "contact": handle, "name": k["name"], "type": k["type"],
                "source": source, "confidence": confidence}
        if retype_only:
            line["retype_only"] = True
        lines.append(line)
    return lines


def write_jsonl_private(path, lines):
    """0600 from creation: the file holds contact handles and names. Written to an
    O_EXCL temp sibling (mkstemp: 0600, never follows a planted symlink, never
    inherits an existing file's wider mode) and renamed over `path`."""
    d = os.path.dirname(path) or "."
    os.makedirs(d, mode=0o700, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".names-", suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as f:
            for line in lines:
                f.write(json.dumps(line, sort_keys=True) + "\n")
        os.replace(tmp, path)
    except BaseException:
        with contextlib.suppress(OSError):
            os.unlink(tmp)
        raise
    return path


def parse_import_output(stdout):
    """`human memory import-facts` prints one JSON object -> its "entities", else None."""
    for line in reversed((stdout or "").splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return int(json.loads(line).get("entities", 0))
            except (ValueError, TypeError, AttributeError):
                return None
    return None


def run_import(human_bin, graph_db, path, timeout=600):
    """-> (entities imported or None, returncode). Every graph write goes through the
    C importer; HU_GRAPH_DB pins which graph it writes."""
    env = {**os.environ, "HU_GRAPH_DB": graph_db}
    try:
        r = subprocess.run([human_bin, "memory", "import-facts", path], env=env,
                           capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None, -1
    return parse_import_output(r.stdout), r.returncode
