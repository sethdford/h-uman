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


def build_prompt(lines, other_names=()):
    """other_names: the contact's own name(s). Named in the user message so the model
    knows who "the person he is texting" is (local model only; verify_names drops
    them again whatever the model answers)."""
    head = ""
    other = next((n.strip() for n in other_names or () if n and n.strip()), "")
    if other:
        head = f"The other person in this conversation is {other}; do not list them.\n"
    return SYSTEM, head + "texts (oldest first):\n" + "\n".join(lines)


def parse_answer(text):
    """Model output -> the answer array, or None when there is none (a parse failure,
    not "no names"). Takes the first '[' at which a JSON array holding an object
    decodes and ignores whatever follows it, so trailing prose (even prose with its own
    brackets) does not hide the answer. `[]` counts only as the FIRST bracket in the
    text: later on it is an inner "evidence": [] surfacing from a truncated answer, as
    a list with no object in it (["t0"]) always is."""
    text = text or ""
    dec = json.JSONDecoder()
    i = text.find("[")
    first = True
    while i != -1:
        try:
            arr, _ = dec.raw_decode(text, i)
        except ValueError:
            arr = None
        if isinstance(arr, list) and ((not arr and first)
                                      or any(isinstance(o, dict) for o in arr)):
            return arr
        first = False
        i = text.find("[", i + 1)
    return None


def parse_names(text):
    """Model output -> [{"name", "type", "evidence_tokens"}]; malformed items dropped.
    [] for no answer too -- callers that must tell the two apart use parse_answer."""
    return names_from_answer(parse_answer(text) or [])


def names_from_answer(arr):
    out = []
    for o in arr:
        if not isinstance(o, dict):
            continue
        name = str(o.get("name") or "").strip()
        ev = o.get("evidence") if isinstance(o.get("evidence"), list) else []
        if name:
            out.append({"name": name, "type": str(o.get("type") or "").strip().lower(),
                        "evidence_tokens": [str(e) for e in ev]})
    return out


def drop_names(display_names):
    """Lowercased names never written for this contact: Seth's and the contact's own
    (each full name and its first word). display_names: one name, several, or None."""
    if isinstance(display_names, str):
        display_names = [display_names]
    out = set(SELF_NAMES)
    for dn in display_names or ():
        dn = (dn or "").strip().lower()
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


MAX_TOPIC_LEN = 40
MAX_TOPIC_WORDS = 4


def _length_ok(name, ntype):
    """Topics are short phrases ("the lake house"): a longer "topic" is a sentence the
    contact typed, and would persist into their topic line (e.g. an instruction)."""
    if ntype == "topic":
        return 2 <= len(name) <= MAX_TOPIC_LEN and len(name.split()) <= MAX_TOPIC_WORDS
    return 2 <= len(name) <= MAX_NAME_LEN


def verify_names(proposed, cite, drop):
    """-> (kept [{"name", "type"}], rejected count). Kept only when the type is known,
    the length fits the type (topics <= 4 words / 40 chars, others 2-60 chars), the
    name is not dropped (compared case-insensitively as written AND canonicalized), it
    cites >= 1 [tN] row and no [dN] row, and it is said in a cited row. One entry per
    canonical name (first wins)."""
    kept, seen, rejected = [], set(), 0
    for p in proposed:
        name, ntype = p["name"], p["type"]
        canon = canonical_name(name, ntype)
        if canon.lower() in seen:
            continue
        t_idx, daemon = ce.parse_evidence(p["evidence_tokens"])
        rows = [cite[i] for i in t_idx if i in cite]
        dropped = name.lower() in drop or canon.lower() in drop
        ok = (ntype in TYPES and _length_ok(name, ntype) and not dropped
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


def write_text_private(path, text):
    """0600 from creation, atomic: the same O_EXCL mkstemp (0600, never follows a
    planted symlink, never inherits an existing file's wider mode) + rename
    pattern as write_jsonl_private, generalized to arbitrary text -- CSV,
    Markdown, or a single JSON document -- so a caller that needs more than
    JSONL does not clone the O_EXCL dance per format. `newline=""` on the open
    so a caller that pre-formats CSV rows (csv.writer into a string) does not
    get its line endings translated a second time."""
    d = os.path.dirname(path) or "."
    os.makedirs(d, mode=0o700, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".priv-", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", newline="") as f:
            f.write(text)
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
