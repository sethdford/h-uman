"""Shared helpers for the memory-benchmark converters (docs/guides/memory-benchmarks.md).

Every converter emits the replay-harness turn JSONL that `human replay`
(sibling branch feat/replay-harness, hu_cli_replay_parse_turn) reads, in the
same shape as its real-turn exporter scripts/blind_ab/replay_export_turns.py:

    {"id", "contact_id", "ts", "inbound_bubbles": [...],
     "history": [{"from_me", "text", "ts"}, ...],
     "seth_action": "text", "seth_reply_bubbles": [...]}

`history` entries fill hu_channel_history_entry_t (include/human/channel.h);
`ts` is "YYYY-MM-DD HH:MM:SS", the shape the iMessage channel serves from
chat.db. `from_me` is the "Seth" speaker. Extra keys (probe, ...) are
benchmark metadata the harness ignores.

Stdlib only, deterministic: the same input always produces byte-identical
output (no clocks, no random, no hash()).
"""
import hashlib
import json
import re
from datetime import datetime, timedelta
from pathlib import Path

TS_FMT = "%Y-%m-%d %H:%M:%S"
TURN_KEYS = ("id", "contact_id", "inbound_bubbles", "ts", "history")
# The daemon serves the last 25 history entries to the reply path
# (load_conversation_history(..., 25, ...) in src/daemon/daemon_reactive_context.c).
DAEMON_HISTORY_WINDOW = 25
HISTORY_TEXT_MAX = 511  # hu_channel_history_entry_t.text is char[512]

CATEGORIES = ("single_hop", "multi_hop", "temporal", "open_domain", "adversarial")


def fmt_ts(dt):
    return dt.strftime(TS_FMT)


def stable_int(key):
    """Deterministic non-negative int from a string (hash() is salted per process)."""
    return int(hashlib.sha256(key.encode("utf-8")).hexdigest()[:12], 16)


def typing_gap_seconds(text):
    """Seconds between consecutive messages: a plausible read+type delay that
    grows with length, clamped to [20, 180]."""
    return max(20, min(180, 20 + len(text) // 3))


def bench_contact_id(dataset, conv_id):
    """A contact handle under the reserved .invalid TLD (RFC 2606), so a
    benchmark turn can never collide with a real person's handle."""
    safe = re.sub(r"[^A-Za-z0-9._-]+", "-", str(conv_id)).strip("-") or "x"
    return f"{dataset}.{safe}@bench.invalid"


def history_entry(from_me, text, dt):
    return {"from_me": bool(from_me), "text": text, "ts": fmt_ts(dt)}


# ── text normalization (shared with memory_probe_score.py) ────────────────

_NUM_WORDS = {
    "zero": "0", "one": "1", "two": "2", "three": "3", "four": "4", "five": "5",
    "six": "6", "seven": "7", "eight": "8", "nine": "9", "ten": "10", "eleven": "11",
    "twelve": "12", "thirteen": "13", "fourteen": "14", "fifteen": "15", "sixteen": "16",
    "seventeen": "17", "eighteen": "18", "nineteen": "19", "twenty": "20",
}
_ARTICLES = {"a", "an", "the", "and"}


def normalize(text):
    """LoCoMo's official normalize_answer (lowercase, drop punctuation, drop
    a/an/the/and, collapse spaces) plus number words -> digits, so "two cats"
    and "2 cats" compare equal. One deliberate deviation: punctuation becomes a
    space instead of vanishing, so "PC,Playstation" and "self-care" split into
    words rather than fusing into "pcplaystation" / "selfcare"."""
    import unicodedata

    s = unicodedata.normalize("NFKD", str(text)).encode("ascii", "ignore").decode("ascii")
    s = s.lower()
    s = re.sub(r"[^\w\s]", " ", s).replace("_", " ")
    words = [_NUM_WORDS.get(w, w) for w in s.split() if w not in _ARTICLES]
    return " ".join(words)


def contains_phrase(haystack_norm, needle_norm):
    """Whole-word containment of an already-normalized phrase."""
    if not needle_norm:
        return False
    return f" {needle_norm} " in f" {haystack_norm} "


# ── casual question framing ───────────────────────────────────────────────

_FRAMES = (
    "wait {q}",
    "random q but {q}",
    "hey remind me, {q}",
    "ok dumb question but {q}",
    "{q}",
    "lol wait {q}",
    "omg i forgot, {q}",
)
_AUX = r"(did|does|do|is|was|has|have|would|will|can|could|might|should)"
_AUX_I = {"does": "do", "is": "am", "has": "have"}
_AUX_YOU = {"does": "do", "is": "are", "was": "were", "has": "have"}
_AUX_WE = {"does": "do", "is": "are", "was": "were", "has": "have"}


def _sub_aux(text, name, pronoun, table):
    pat = re.compile(r"\b" + _AUX + r"\s+" + re.escape(name) + r"\b", re.IGNORECASE)

    def rep(m):
        aux = m.group(1)
        mapped = table.get(aux.lower(), aux.lower())
        if aux[0].isupper():
            mapped = mapped[0].upper() + mapped[1:]
        return f"{mapped} {pronoun}"

    return pat.sub(rep, text)


def casual_question(question, contact_name, seth_name, key):
    """Rephrase a third-person benchmark question as the contact texting Seth.

    The contact's own name becomes I/me/my, Seth's becomes you/your, both
    together become "we". Deterministic: the texting-style prefix is chosen
    from `key`. Grammar is best-effort; the original stays in the probe.
    """
    q = question.strip()
    names = [n for n in (contact_name, seth_name) if n]
    if len(names) == 2:
        for a, b in ((contact_name, seth_name), (seth_name, contact_name)):
            both = re.escape(a) + r"\s+and\s+" + re.escape(b)
            q = re.sub(r"\b" + _AUX + r"\s+" + both + r"\b",
                       lambda m: f"{_AUX_WE.get(m.group(1).lower(), m.group(1).lower())} we", q,
                       flags=re.IGNORECASE)
            q = re.sub(r"\b" + both + r"\b", "we", q)
    if contact_name:
        q = re.sub(r"\b" + re.escape(contact_name) + r"'s\b", "my", q)
        q = _sub_aux(q, contact_name, "I", _AUX_I)
        q = re.sub(r"\b" + re.escape(contact_name) + r"\b", "me", q)
    if seth_name:
        q = re.sub(r"\b" + re.escape(seth_name) + r"'s\b", "your", q)
        q = _sub_aux(q, seth_name, "you", _AUX_YOU)
        q = re.sub(r"\b" + re.escape(seth_name) + r"\b", "you", q)
    if q and not q.startswith("I "):
        q = q[0].lower() + q[1:]
    frame = _FRAMES[stable_int(key) % len(_FRAMES)]
    return frame.format(q=q)


# ── turns from a flat message list ────────────────────────────────────────


def build_turns(messages, contact_id, turn_prefix, history_limit=0, extra=None):
    """Group a chronological list of {from_me, text, dt} into replay turns.

    One turn = a maximal run of consecutive contact messages that Seth then
    answered in the same session; history = every message before the run
    (oldest first); seth_reply_bubbles = Seth's following run (what he actually
    said). Runs never cross a session boundary (`session` key, optional): a
    reply twelve days later is a new conversation, not this turn's answer.
    """
    turns = []
    i, n = 0, len(messages)

    def same(a, b):
        return messages[a].get("session") == messages[b].get("session")

    while i < n:
        if messages[i]["from_me"]:
            i += 1
            continue
        j = i
        while j < n and not messages[j]["from_me"] and same(i, j):
            j += 1
        if j >= n or not messages[j]["from_me"] or not same(i, j):
            i = j  # the contact had the last word in this session: no reference
            continue
        k = j
        while k < n and messages[k]["from_me"] and same(j, k):
            k += 1
        hist = [history_entry(m["from_me"], m["text"], m["dt"]) for m in messages[:i]]
        if history_limit:
            hist = hist[-history_limit:]
        turn = {
            "id": f"{turn_prefix}:turn:{len(turns):04d}",
            "contact_id": contact_id,
            "inbound_bubbles": [m["text"] for m in messages[i:j]],
            "ts": fmt_ts(messages[j - 1]["dt"]),
            "history": hist,
            "seth_action": "text",
            "seth_reply_bubbles": [m["text"] for m in messages[j:k]],
        }
        if extra:
            turn.update(extra(i, j, k))
        turns.append(turn)
        i = k
    return turns


def window_flags(idxs, history_len, window):
    """(in_history, in_window) for evidence indices into a history of
    `history_len` entries; the reply path sees the last `window` of them."""
    if not idxs:
        return False, False
    in_hist = all(0 <= x < history_len for x in idxs)
    in_win = in_hist and all(x >= history_len - window for x in idxs)
    return in_hist, in_win


def sample_per_category(probes, n):
    """At most n probes per category, chosen by a stable hash of the id (not
    by position, so a category is not biased toward the first conversations);
    output keeps the input order."""
    keep = set()
    by_cat = {}
    for p in probes:
        by_cat.setdefault(p["probe"]["category"], []).append(p["id"])
    for ids in by_cat.values():
        keep.update(sorted(ids, key=stable_int)[:n])
    return [p for p in probes if p["id"] in keep]


def write_outputs(out_dir, result):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for name in ("turns", "probes"):
        with open(out / f"{name}.jsonl", "w", encoding="utf-8") as f:
            for rec in result[name]:
                f.write(json.dumps(rec, ensure_ascii=False, sort_keys=True) + "\n")
    stats = json.dumps(result["stats"], indent=2, sort_keys=True)
    (out / "stats.json").write_text(stats + "\n", encoding="utf-8")
    return stats


def common_stats(turns, probes, conversations):
    from collections import Counter

    hist_lens = [len(p["history"]) for p in probes] or [len(t["history"]) for t in turns]
    over = sum(1 for rec in list(turns) + list(probes) for h in rec["history"]
               if len(h["text"].encode("utf-8")) > HISTORY_TEXT_MAX)
    by_cat = Counter(p["probe"]["category"] for p in probes)
    return {
        "conversations": conversations,
        "turns": len(turns),
        "probes": len(probes),
        "probes_by_category": dict(sorted(by_cat.items())),
        "probes_evidence_in_window": sum(1 for p in probes if p["probe"].get("evidence_in_window")),
        "max_history_entries": max(hist_lens) if hist_lens else 0,
        "history_entries_over_511_bytes": over,
    }


def parse_dt(text, fmt):
    return datetime.strptime(text, fmt)


__all__ = [
    "CATEGORIES", "DAEMON_HISTORY_WINDOW", "TURN_KEYS", "bench_contact_id", "build_turns",
    "casual_question", "common_stats", "contains_phrase", "fmt_ts", "history_entry",
    "normalize", "parse_dt", "sample_per_category", "stable_int",
    "timedelta", "typing_gap_seconds", "window_flags", "write_outputs",
]
