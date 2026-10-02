#!/usr/bin/env python3
"""Nightly learner for the Learned Style Profile (learned-style/v1).

Learns how Seth texts each persona contact from his OWN sent iMessages, per
situation, and writes ~/.human/personas/<persona>.learned-style.json for the C
runtime (HU_LEARNED_STYLE). It replaces hand-written length rules ("5-15
words") with measured ones. Contract: docs/guides/learned-style.md.

What counts as a sample: a reply TURN (consecutive is_from_me bubbles, each
within 90 s of the previous) in a 1:1 chat with a persona contact, whose
immediately preceding message is the contact's, sent within 6 hours. Its
shape is taken from the whole inbound burst (every contact bubble since Seth's
previous send, joined with "\n"), the same text the C runtime classifies. Seth follow-ups with no inbound
in between are not replies and are skipped.

Only Seth's own texts are learned: attribution reuses
eval_conversation_quality.attribute() (the single source of attribution), so
a turn with any h-uman or ambiguous bubble is dropped. Learning from the
twin's own sends would feed its habits back into its style. Owner self-test
handles (persona contact relationship "test") are excluded.

Privacy: message text exists only in memory, inside load_samples(), to
compute features. No text is written, logged or printed; the output file's
leaves are numbers, booleans, null and three fixed metadata strings.

Guardrails: a per-run change cap (30% relative or 10 bytes for lengths;
--no-cap reseeds), history of the last 14 files, one counts-only JSON line
per run in ~/.human/logs/learned-style.jsonl, and refusal (exit 2, nothing
written) when more than 5% of sends are ambiguously attributed (a twin send
from a path with no provenance would otherwise be learned as Seth's), when
global n < 50, when more than half the previous contacts would disappear, or
when chat.db / memory.db cannot be read.

Usage:
  python3 scripts/learned_style_profile.py --persona seth --dry-run
  python3 scripts/learned_style_profile.py --persona seth
"""
import argparse
import datetime as dt
import json
import math
import os
import re
import shutil
import sqlite3
import sys
import tempfile
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import eval_conversation_quality as cq  # noqa: E402

SCHEMA = "learned-style/v1"
WINDOW_DAYS = 180
HALF_LIFE_DAYS = 21
PAIR_WINDOW_S = 6 * 3600
# A contact bubble joins the burst being answered only if it is at most this
# long before the NEXT bubble of the burst (and within PAIR_WINDOW_S of the
# reply). The daemon batches one poll's consecutive messages plus one re-poll
# after the read delay (daemon.c burst accumulation) and defines no time
# constant; 10 minutes is the review ruling for a gap that still reads as one
# thought.
BURST_GAP_S = 10 * 60
BUBBLE_GAP_S = 90
RAPID_S = 120
SHRINK_K = 8
MIN_BUCKET_N = 3
MIN_CONTACT_N = 5
MIN_GLOBAL_N = 50
MAX_VANISH_SHARE = 0.5
# Attribution "ambiguous" = h-uman was active near a send whose text matches
# nothing it logged. Above this share, unattributed twin sends could be
# leaking into Seth's samples through paths that write no provenance.
MAX_AMBIGUOUS_FRAC = 0.05
HISTORY_KEEP = 14
CAP_REL = 0.30
# Absolute floor for the per-run cap. Lengths: 10 bytes (contract). The other
# floors are not in the contract: without one, a rate whose previous value is
# 0.0 could never move again (30% of 0 is 0).
CAP_ABS = {"len_p25": 10, "len_p50": 10, "len_p90": 10, "bubbles_p50": 0.5,
           "lower_start_rate": 0.05, "emoji_rate": 0.05, "end_punct_rate": 0.05,
           "latency_p50_s": 60}
INT_FIELDS = ("len_p25", "len_p50", "len_p90", "latency_p50_s")
VALUE_FIELDS = ("len_p25", "len_p50", "len_p90", "bubbles_p50", "lower_start_rate",
                "emoji_rate", "end_punct_rate", "latency_p50_s")
BUCKETS = ("shape:question", "shape:story", "shape:casual",
           "time:day", "time:evening", "time:late", "pace:rapid")
LOG_NAME = "learned-style.jsonl"
PERSONA_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
C_SPACE = " \t\n\r\v\f"   # C isspace() set: both parts must trim identically


# ── shape rule (identical in src/ C runtime; vectors in both test suites) ──

def shape(text):
    """question / story / casual for an INBOUND message, per the contract."""
    t = (text or "").strip(C_SPACE)
    if "?" in t:
        return "question"
    nbytes = len(t.encode("utf-8"))
    if nbytes >= 140:
        return "story"
    if nbytes >= 80 and len(re.findall(r"[.!]+", t)) >= 2:
        return "story"
    return "casual"


def time_band(when, tz=None):
    """tz None means the machine's local zone, DST-correct for each date."""
    h = (when.astimezone(tz) if tz else when.astimezone()).hour
    if 6 <= h < 18:
        return "day"
    if 18 <= h < 23:
        return "evening"
    return "late"


def is_emoji_char(ch):
    """Unicode So (other symbol) plus the emoji blocks. Sk is limited to the
    non-ASCII range so '^' and '`' never count."""
    o = ord(ch)
    if o < 0x80:
        return False
    cat = unicodedata.category(ch)
    return cat == "So" or cat == "Sk" or 0x1F000 <= o <= 0x1FAFF or 0x2600 <= o <= 0x27BF


def lower_start(text):
    for ch in text:
        if ch.isalpha():
            return ch == ch.lower() and ch != ch.upper()
    return False


def end_punct(text):
    t = text.rstrip(C_SPACE)
    return bool(t) and t[-1] in ".!?"


# ── weighting, quantiles, stats ────────────────────────────────────────────

def recency_weight(age_days):
    return 0.5 ** (max(0.0, age_days) / HALF_LIFE_DAYS)


def weighted_quantile(values, weights, q):
    """Smallest value whose cumulative weight reaches q of the total."""
    pairs = sorted(zip(values, weights))
    total = sum(w for _, w in pairs)
    target = q * total
    acc = 0.0
    for v, w in pairs:
        acc += w
        if acc >= target - 1e-12:
            return v
    return pairs[-1][0]


def _rate(samples, weights, key):
    total = sum(weights)
    return sum(w for s, w in zip(samples, weights) if s[key]) / total


def compute_stats(samples):
    """Raw (unshrunk) stats for a non-empty list of samples."""
    w = [recency_weight(s["age_days"]) for s in samples]
    lens = [s["len"] for s in samples]
    lat = [(s["latency_s"], wi) for s, wi in zip(samples, w) if s["latency_s"] is not None]
    return {
        "n": len(samples),
        "n_eff": round(sum(w), 4),
        "len_p25": int(weighted_quantile(lens, w, 0.25)),
        "len_p50": int(weighted_quantile(lens, w, 0.50)),
        "len_p90": int(weighted_quantile(lens, w, 0.90)),
        "bubbles_p50": float(weighted_quantile([s["bubbles"] for s in samples], w, 0.5)),
        "lower_start_rate": round(_rate(samples, w, "lower"), 4),
        "emoji_rate": round(_rate(samples, w, "emoji"), 4),
        "end_punct_rate": round(_rate(samples, w, "end_punct"), 4),
        "latency_p50_s": (int(weighted_quantile([v for v, _ in lat], [x for _, x in lat], 0.5))
                          if lat else None),
        "shrunk": False,
    }


def _finish(st):
    """Round to the schema's types and keep the quantiles ordered."""
    for f in VALUE_FIELDS:
        v = st[f]
        if v is None:
            continue
        st[f] = int(round(v)) if f in INT_FIELDS else round(float(v), 4)
    # Lower the higher quantile, never raise one: raising p50 / p90 could
    # push them past their per-run cap. With an ordered previous file the
    # clamped values are already ordered (the cap interval is monotone in
    # the previous value), so this only fires on a malformed one. Ruling:
    # for an UNORDERED previous file, ordering wins over p25's (or p50's)
    # downward cap; there is no assignment that satisfies both.
    st["len_p50"] = min(st["len_p50"], st["len_p90"])
    st["len_p25"] = min(st["len_p25"], st["len_p50"])
    return st


def shrink(child, parent, k=SHRINK_K):
    """(n_eff * v + K * parent_v) / (n_eff + K) for every value field;
    quantiles element-wise. n and n_eff are counts and never shrink."""
    out = dict(child)
    ne = child["n_eff"]
    for f in VALUE_FIELDS:
        v, p = child[f], parent[f]
        if v is None or p is None:
            out[f] = v if p is None else p
            continue
        out[f] = (ne * v + k * p) / (ne + k)
    out["shrunk"] = True
    return _finish(out)


def in_bucket(sample, bucket):
    kind, val = bucket.split(":", 1)
    if kind == "shape":
        return sample["shape"] == val
    if kind == "time":
        return sample["band"] == val
    return sample["rapid"]


def build_profile(samples_by_contact, persona, now):
    """The learned-style/v1 document from per-contact samples. Contacts with
    n < 5 and buckets with n < 3 are omitted; global covers every sample."""
    every = [s for ss in samples_by_contact.values() for s in ss]
    glob = compute_stats(every) if every else None
    contacts = {}
    for c in sorted(samples_by_contact):
        ss = samples_by_contact[c]
        if len(ss) < MIN_CONTACT_N:
            continue
        overall = shrink(compute_stats(ss), glob)
        buckets = {}
        for b in BUCKETS:
            bs = [s for s in ss if in_bucket(s, b)]
            if len(bs) >= MIN_BUCKET_N:
                buckets[b] = shrink(compute_stats(bs), overall)
        contacts[c] = {"overall": overall, "buckets": buckets}
    return {"schema": SCHEMA, "persona": persona,
            "generated_at": now.astimezone(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "window_days": WINDOW_DAYS, "half_life_days": HALF_LIFE_DAYS,
            "global": glob, "contacts": contacts}


# ── samples from chat.db (the only place message text exists) ──────────────

def learnable_contacts(contacts):
    """Persona contact handles minus the owner's own self-test handles."""
    return [h for h, c in contacts.items()
            if not (isinstance(c, dict) and c.get("relationship") == "test")]


def burst_text(burst, reply_t):
    """The inbound text a reply answers: walking back from the contact's last
    bubble, keep each bubble that is <= BURST_GAP_S before the next kept one
    and <= PAIR_WINDOW_S before the reply; stop at the first that is not.
    Joined in time order with "\n". burst: [(time, text)] in time order."""
    kept = []
    for t, text in reversed(burst):
        if (reply_t - t).total_seconds() > PAIR_WINDOW_S:
            break
        if kept and (kept[-1][0] - t).total_seconds() > BURST_GAP_S:
            break
        kept.append((t, text))
    return "\n".join(text for _, text in reversed(kept))


def samples_from_timeline(timeline, labels, now, tz):
    """Feature dicts for every learnable reply turn in one contact's 1:1
    timeline (sorted by time, reactions removed). Text is read here and
    reduced to numbers; nothing textual leaves this function."""
    out = []
    last_me_t = None        # Seth's (any from-me) latest send so far
    inbound_gap = None      # inbound arrival minus the from-me send before it
    burst = []              # contact's bubbles since that send, in time order
    i, n = 0, len(timeline)
    while i < n:
        m = timeline[i]
        if not m["from_me"]:
            inbound_gap = (m["t"] - last_me_t).total_seconds() if last_me_t else None
            burst.append((m["t"], m["text"] or ""))
            i += 1
            continue
        j = i + 1
        while (j < n and timeline[j]["from_me"]
               and (timeline[j]["t"] - timeline[j - 1]["t"]).total_seconds() <= BUBBLE_GAP_S):
            j += 1
        turn = timeline[i:j]
        prev = timeline[i - 1] if i > 0 else None
        answered = burst_text(burst, turn[0]["t"])
        last_shape = shape(burst[-1][1]) if burst else "casual"
        burst = []
        last_me_t = turn[-1]["t"]
        i = j
        if prev is None or prev["from_me"]:
            continue
        latency = (turn[0]["t"] - prev["t"]).total_seconds()
        if latency > PAIR_WINDOW_S:
            continue
        if any(labels.get(b["guid"]) != "seth" for b in turn):
            continue
        texts = [b["text"] for b in turn if b["text"]]
        if not texts:
            continue
        age = (now - turn[0]["t"]).total_seconds() / 86400
        if age > WINDOW_DAYS:
            continue
        out.append({
            "age_days": age,
            "len": sum(len(t.encode("utf-8")) for t in texts),
            "bubbles": len(texts),
            "lower": lower_start(texts[0]),
            "emoji": any(is_emoji_char(ch) for t in texts for ch in t),
            "end_punct": end_punct(texts[-1]),
            "latency_s": int(latency),
            # The whole burst, joined with "\n": the C runtime classifies
            # the same text, so both sides bucket a reply identically.
            "shape": shape(answered),
            "burst_changed_shape": shape(answered) != last_shape,
            "band": time_band(turn[0]["t"], tz),
            "rapid": inbound_gap is not None and inbound_gap < RAPID_S and latency <= RAPID_S,
        })
    return out


def load_samples(chat_path, mem_path, contacts, now, tz):
    """({contact: [sample]}, attribution counts) for the given handles.
    The counts cover every from-me message to those handles in the window:
    sent_n, ambiguous_n, huuman_n, plus exact_unmatched (outbound_sends
    records that never resolved to a delivered message). Raises
    sqlite3.Error / OSError when either database cannot be read."""
    for p in (chat_path, mem_path):
        if not os.path.isfile(p):
            raise OSError(f"cannot read {os.path.basename(p)}")
    since = now - dt.timedelta(days=WINDOW_DAYS)
    att = cq.attribute(chat_path, mem_path, since)
    out = {}
    counts = {"sent_n": 0, "ambiguous_n": 0, "huuman_n": 0,
              "exact_unmatched": int(att["exact_unmatched"])}
    for c in contacts:
        tl = att["timelines"].get(c)
        out[c] = samples_from_timeline(tl, att["labels"], now, tz) if tl else []
        for _, label in att["labeled"].get(c, []):
            counts["sent_n"] += 1
            counts["ambiguous_n"] += label == "ambiguous"
            counts["huuman_n"] += label == "huuman"
    return out, counts


# ── per-run cap ────────────────────────────────────────────────────────────

def cap_stats(new, prev):
    """Clamp each value field of `new` to within the allowed move from
    `prev`. Returns (capped copy, [clamped field names], max relative change
    before clamping, or None when nothing was comparable)."""
    out = dict(new)
    clamped, max_rel = [], None
    for f in VALUE_FIELDS:
        v, p = new.get(f), prev.get(f) if isinstance(prev, dict) else None
        if v is None or not isinstance(p, (int, float)) or isinstance(p, bool):
            continue
        if p != 0:
            rel = abs(v - p) / abs(p)
            max_rel = rel if max_rel is None else max(max_rel, rel)
        allowed = max(CAP_REL * abs(p), CAP_ABS[f])
        if abs(v - p) > allowed + 1e-9:
            if f in INT_FIELDS:
                allowed = math.floor(allowed)
            out[f] = p + allowed if v > p else p - allowed
            clamped.append(f)
    if clamped:
        _finish(out)
    return out, clamped, max_rel


def _stat_nodes(doc):
    if not isinstance(doc, dict):
        return
    if isinstance(doc.get("global"), dict):
        yield ("global",), doc["global"]
    for c, entry in (doc.get("contacts") or {}).items():
        if not isinstance(entry, dict):
            continue
        if isinstance(entry.get("overall"), dict):
            yield ("contacts", c, "overall"), entry["overall"]
        for b, st in (entry.get("buckets") or {}).items():
            if isinstance(st, dict):
                yield ("contacts", c, "buckets", b), st


def apply_cap(new_doc, prev_doc):
    """Cap every stats node of new_doc against the same node in prev_doc.
    Mutates new_doc. Returns (clamped_n, {field: count}, max_rel_change)."""
    prev_nodes = dict(_stat_nodes(prev_doc))
    clamped_n, fields, max_rel = 0, {}, None
    for path, st in list(_stat_nodes(new_doc)):
        if path not in prev_nodes:
            continue
        capped, clamped, rel = cap_stats(st, prev_nodes[path])
        st.update(capped)
        clamped_n += len(clamped)
        for f in clamped:
            fields[f] = fields.get(f, 0) + 1
        if rel is not None:
            max_rel = rel if max_rel is None else max(max_rel, rel)
    return clamped_n, fields, max_rel


# ── files ──────────────────────────────────────────────────────────────────

def write_atomic(path, obj):
    d = os.path.dirname(path) or "."
    os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".learned-style.", suffix=".tmp", dir=d)
    try:
        with os.fdopen(fd, "w") as f:
            json.dump(obj, f, indent=1, sort_keys=True)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o600)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def archive_previous(path, hist_dir, persona, now):
    """Copy the current file into the history dir (0600) and keep the last
    HISTORY_KEEP copies for this persona."""
    os.makedirs(hist_dir, mode=0o700, exist_ok=True)
    stamp = now.astimezone(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    dest = os.path.join(hist_dir, f"{persona}.{stamp}.json")
    k = 1
    while os.path.exists(dest):
        dest = os.path.join(hist_dir, f"{persona}.{stamp}-{k}.json")
        k += 1
    shutil.copyfile(path, dest)
    os.chmod(dest, 0o600)
    mine = sorted(f for f in os.listdir(hist_dir)
                  if f.startswith(persona + ".") and f.endswith(".json"))
    for old in mine[:-HISTORY_KEEP]:
        os.unlink(os.path.join(hist_dir, old))


def append_log(log_dir, record):
    os.makedirs(log_dir, exist_ok=True)
    path = os.path.join(log_dir, LOG_NAME)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    with os.fdopen(fd, "a") as f:
        f.write(json.dumps(record, sort_keys=True) + "\n")


def load_previous(path):
    """(doc or None, status) where status is absent / ok / unreadable."""
    if not os.path.exists(path):
        return None, "absent"
    try:
        with open(path) as f:
            doc = json.load(f)
    except (OSError, ValueError):
        return None, "unreadable"
    if not isinstance(doc, dict) or doc.get("schema") != SCHEMA:
        return None, "unreadable"
    return doc, "ok"


# ── CLI ────────────────────────────────────────────────────────────────────

def _state_dir():
    """$HU_STATE_DIR when set and non-empty, else ~/.human (hu_paths_state)."""
    return os.environ.get("HU_STATE_DIR") or os.path.expanduser("~/.human")


def _default_persona_dir():
    """$HU_PERSONA_DIR, else <state>/personas (hu_persona_base_dir)."""
    return os.environ.get("HU_PERSONA_DIR") or os.path.join(_state_dir(), "personas")


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--persona", default="seth")
    ap.add_argument("--persona-dir", default=None,
                    help="where <persona>.json is read (default $HU_PERSONA_DIR or ~/.human/personas)")
    ap.add_argument("--out-dir", default=None,
                    help="where the learned file and its history go (default: --persona-dir)")
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--memory-db", default=os.path.join(_state_dir(), "memory.db"))
    ap.add_argument("--log-dir", default=os.path.join(_state_dir(), "logs"))
    ap.add_argument("--now", default=None, help="ISO-8601 UTC override (tests)")
    ap.add_argument("--max-ambiguous-frac", type=float, default=MAX_AMBIGUOUS_FRAC,
                    help="refuse when more than this share of sends is ambiguously attributed")
    ap.add_argument("--tz", choices=("local", "utc"), default="local",
                    help="time zone for the time:* bands (tests use utc)")
    ap.add_argument("--dry-run", action="store_true", help="print counts only; write nothing")
    ap.add_argument("--no-cap", action="store_true", help="skip the per-run change cap (reseed)")
    a = ap.parse_args(argv)
    if not PERSONA_RE.match(a.persona):
        ap.error("--persona must be letters, digits, '-' or '_'")
    a.persona_dir = a.persona_dir or _default_persona_dir()
    a.out_dir = a.out_dir or a.persona_dir
    return a


def _now(a):
    if a.now:
        return dt.datetime.fromisoformat(a.now.replace("Z", "+00:00")).astimezone(dt.timezone.utc)
    return dt.datetime.now(dt.timezone.utc)


def _refusal(doc, prev, att, max_ambiguous_frac):
    frac = att["ambiguous_n"] / att["sent_n"] if att["sent_n"] else 0.0
    if frac > max_ambiguous_frac:
        return ("refused_ambiguous",
                f"{att['ambiguous_n']} of {att['sent_n']} sends ambiguously attributed "
                f"({frac:.1%} > {max_ambiguous_frac:.1%}); h-uman sends may be leaking "
                f"into Seth's samples")
    if doc["global"] is None or doc["global"]["n"] < MIN_GLOBAL_N:
        n = doc["global"]["n"] if doc["global"] else 0
        return "refused_global_n", f"global n={n} < {MIN_GLOBAL_N}"
    if prev and prev.get("contacts"):
        before = set(prev["contacts"])
        gone = before - set(doc["contacts"])
        if len(gone) / len(before) > MAX_VANISH_SHARE:
            return ("refused_contacts_vanished",
                    f"{len(gone)} of {len(before)} previous contacts would disappear")
    return None, None


def main(argv=None):
    a = parse_args(argv)
    now = _now(a)
    tz = dt.timezone.utc if a.tz == "utc" else None
    out_path = os.path.join(a.out_dir, f"{a.persona}.learned-style.json")
    record = {"ts": now.strftime("%Y-%m-%dT%H:%M:%SZ"), "persona": a.persona}

    def refuse(status, msg, **extra):
        sys.stderr.write(f"learned_style: refusing to write: {msg}\n")
        if not a.dry_run:
            append_log(a.log_dir, dict(record, status=status, **extra))
        return 2

    try:
        with open(os.path.join(a.persona_dir, f"{a.persona}.json")) as f:
            persona_contacts = json.load(f).get("contacts") or {}
    except (OSError, ValueError):
        return refuse("refused_persona_unreadable", "persona file unreadable")
    handles = learnable_contacts(persona_contacts)
    try:
        samples, att = load_samples(a.chat_db, a.memory_db, handles, now, tz)
    except (OSError, sqlite3.Error):
        # memory.db is required: without it h-uman's own sends would be
        # learned as Seth's.
        return refuse("refused_db_unreadable", "chat.db or memory.db unreadable")

    doc = build_profile(samples, a.persona, now)
    prev, prev_status = load_previous(out_path)
    status, msg = _refusal(doc, prev, att, a.max_ambiguous_frac)
    first_run = prev is None
    clamped_n, clamped_fields, max_rel = 0, {}, None
    if status is None and prev is not None:
        if a.no_cap:
            _, _, max_rel = apply_cap(json.loads(json.dumps(doc)), prev)
        else:
            clamped_n, clamped_fields, max_rel = apply_cap(doc, prev)
    counts = {
        "samples": sum(len(v) for v in samples.values()),
        "contacts": len(doc["contacts"]),
        "contacts_omitted": sum(1 for v in samples.values() if len(v) < MIN_CONTACT_N),
        "buckets": sum(len(c["buckets"]) for c in doc["contacts"].values()),
        "global_n": doc["global"]["n"] if doc["global"] else 0,
        "clamped_n": clamped_n,
        "max_rel_change": round(max_rel, 4) if max_rel is not None else None,
        "first_run": first_run,
        # Replies whose shape differs between the last inbound bubble alone and
        # the whole burst: how much the burst rule actually moves bucketing.
        "shape_changed_by_burst_n": sum(1 for v in samples.values() for x in v
                                        if x["burst_changed_shape"]),
        "sent_n": att["sent_n"],
        "ambiguous_n": att["ambiguous_n"],
        "huuman_n": att["huuman_n"],
        "ambiguous_frac": round(att["ambiguous_n"] / att["sent_n"], 4) if att["sent_n"] else 0.0,
        "exact_unmatched": att["exact_unmatched"],
    }
    if a.dry_run:
        summary = dict(counts, dry_run=True,
                       refuse_global_n=status == "refused_global_n",
                       refuse_contacts_vanished=status == "refused_contacts_vanished",
                       refuse_ambiguous=status == "refused_ambiguous")
        print(json.dumps(summary, sort_keys=True))
        return 2 if status else 0
    if status:
        return refuse(status, msg, **counts)
    if prev_status != "absent":
        archive_previous(out_path, os.path.join(a.out_dir, "learned-style-history"),
                         a.persona, now)
    write_atomic(out_path, doc)
    append_log(a.log_dir, dict(record, status="written", prev=prev_status,
                               no_cap=a.no_cap, clamped_fields=clamped_fields, **counts))
    print(f"learned_style: wrote {counts['contacts']} contacts, {counts['buckets']} buckets, "
          f"global n={counts['global_n']}, clamped {clamped_n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
