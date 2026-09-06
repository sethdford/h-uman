#!/usr/bin/env python3
"""Build a blind 2AFC rating sheet from triples.json.

Each triple {id, context, seth_reply, huuman_reply} becomes one row with two
unlabeled options (A/B) in randomized order. Emits:
  - rating_sheet.csv : columns id, context, option_A, option_B, choice, confidence
                       (raters fill `choice` = A|B and `confidence` = 1..5)
  - answer_key.json  : { id: "A"|"B" }  -- meaning depends on --mode (below).
                       Keep this private; raters must never see it.

Two modes (--mode, default "detection" -- existing behavior, unchanged):

  detection (default): "Which is more like the real Seth?" answer_key.json's
    value is which side holds Seth's REAL reply. Scored by score.py.
    0.5 detection == indistinguishable (the goal).

  preference (US-6): "Which reply would you rather receive?" answer_key.json's
    value is which side holds the MODEL (huuman) reply, and a top-level
    "_mode": "preference" marker is added so score_preference.py (and nothing
    else) will ever score this key -- a detection-framed key and a
    preference-framed key must never be conflated (see
    sprints/sprint-better-than-human-2026-09-05/designs/US-6.md). Scored by
    score_preference.py, a SEPARATE, non-promotion-gating measurement.

In BOTH modes, `context`/`seth_reply`/`huuman_reply` are redacted for
phone-number-shaped and contact-name-shaped substrings (AC-6.4) before any
row is built -- redaction was previously entirely absent, a real privacy gap
even for the existing detection sheets.

In preference mode ONLY, a triple whose seth_reply and huuman_reply are
byte-identical after redaction is skipped and counted (a real-vs-real pair
has no "model side" to prefer) -- see designs/US-6.md refusal condition #4.
If that empties the sheet, the build refuses (exit non-zero, nothing written).

Usage:
    python3 make_rating_sheet.py triples.json [--seed 42] [--out-dir .]
    python3 make_rating_sheet.py triples.json --mode preference --out-dir ~/blind_ab_run
"""
import argparse, csv, json, os, random, re, sys

FIELDNAMES = ["id", "context", "option_A", "option_B", "choice", "confidence",
              "axis_opinion", "axis_memory", "axis_reasoning",
              "axis_lexical", "axis_tone", "axis_syntax"]

# Phone-shaped substrings, four alternatives in increasing grouping generality:
#   1. \+\d{7,15}                          international, digits only, no separators
#                                           (e.g. "+14155551234")
#   2. \(?\d{3}\)?[-.\s]?\d{3}[-.\s]?\d{4}  US 3-3-4, optional parens/separators
#                                           (e.g. "555-123-4567", "(415) 555-0199",
#                                           "415.555.0199", or a bare 10-digit run)
#   3. \+\d{1,3}(?:[-.\s]\d{2,4}){2,4}      international, GROUPED with separators
#                                           (e.g. "+44 20 7946 0958", "+44.20.7946.0958")
#   4. \d{2,4}(?:[-.\s]\d{2,4}){2,3}        national, GROUPED with separators, 3-4
#                                           groups of 2-4 digits (e.g. "020 7946 0958")
# Alternatives 3 and 4 REQUIRE a separator between every group (no bare-digit-run
# fallback) and require >=3 total groups of 2-4 digits each -- specifically so this
# does NOT redact ordinary non-phone numbers that merely contain digits and a
# separator: a bare 4-digit year ("2026", no separator at all), an HH:MM time
# (":" is not in the separator class), a 2-decimal price ("$19.99" is only 2
# groups, below the 3-group minimum), or a bare 5-digit zip (single group, no
# separator; a zip+4 like "12345-6789" is a 5-digit group, above the 2-4 range).
# Reuses the normalization intent of scripts/mine_all_data.py's normalize_phone()
# (digit-run based), but this is a REDACTION regex (find-and-mask), not a
# normalize-and-compare function.
_PHONE_RE = re.compile(
    r'\+\d{7,15}'
    r'|\(?\d{3}\)?[-.\s]?\d{3}[-.\s]?\d{4}'
    r'|\+\d{1,3}(?:[-.\s]\d{2,4}){2,4}'
    r'|\d{2,4}(?:[-.\s]\d{2,4}){2,3}'
)

# Single alphabetic token, with an optional trailing possessive/contraction
# suffix ("Sarah's", "O'Brien's") captured separately so the possessive "'s"
# survives redaction while the name itself does not. Deliberately NOT a
# "capitalized word" heuristic -- that false-positives on ordinary
# sentence-initial/proper words ("Friday", "God") per
# .claude/rules/substring-classifier-pitfalls.md. This matches only tokens
# present in an explicit, real name list.
_NAME_TOKEN_RE = re.compile(r"[A-Za-z]+(?:'[A-Za-z]+)?")


def build_name_tokens(names):
    """Split an iterable of full contact names ("Sarah Jones") into a set of
    lowercase individual tokens ("sarah", "jones") for exact-token redaction
    matching. Also accepts already-tokenized single names unchanged."""
    tokens = set()
    for full in names or ():
        for tok in re.split(r"\s+", (full or "").strip()):
            tok = tok.strip("'\".,")
            if tok:
                tokens.add(tok.lower())
    return tokens


def redact(text, name_tokens=None):
    """Redact phone-number-shaped and contact-name-shaped substrings.

    name_tokens: iterable of name strings/tokens, matched case-insensitively
    as WHOLE tokens only (never a substring match within a longer word).
    Returns `text` unchanged if it is falsy.
    """
    if not text:
        return text
    out = _PHONE_RE.sub("[phone]", text)
    if name_tokens:
        name_set = {str(t).lower() for t in name_tokens}

        def _repl(m):
            tok = m.group(0)
            base, sep, rest = tok.partition("'")
            if base.lower() in name_set:
                return "[name]" + (sep + rest if sep else "")
            return tok

        out = _NAME_TOKEN_RE.sub(_repl, out)
    return out


def resolve_contact_name_tokens():
    """Resolve the live contact-name list for redaction, from the real macOS
    AddressBook -- CLI invocation time ONLY, never from a test (that read is
    live machine state, not hermetic fixture data). Set
    HU_BLIND_AB_SKIP_ADDRESSBOOK=1 (used by this module's hermetic tests) to
    force an empty result without touching the AddressBook at all. Any
    failure (non-macOS, missing AddressBook, sqlite error) degrades to
    phone-only redaction rather than crashing the build.
    """
    if os.environ.get("HU_BLIND_AB_SKIP_ADDRESSBOOK"):
        print("warning: HU_BLIND_AB_SKIP_ADDRESSBOOK is set; "
              "proceeding with phone-only redaction (no contact-name "
              "redaction this run)", file=sys.stderr)
        return set()
    try:
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
        import mine_all_data as _mine  # local import: only needed for the real CLI path
        names = _mine.resolve_contacts_from_addressbook().values()
        return build_name_tokens(names)
    except Exception as e:  # pragma: no cover -- environment-dependent
        print(f"warning: contact-name redaction unavailable ({e}); "
              f"proceeding with phone-only redaction", file=sys.stderr)
        return set()


def build(triples, seed, mode="detection", name_tokens=None):
    """Build (rows, key, skipped) from triples.

    mode="detection" (default): key[id] is which side holds the REAL Seth
      reply -- identical semantics to the pre-US-6 behavior.
    mode="preference": key[id] is which side holds the MODEL (huuman) reply;
      a real==model duplicate pair (post-redaction) is excluded and counted
      in `skipped` rather than being scored either way.
    """
    if mode not in ("detection", "preference"):
        raise ValueError(f"unknown --mode: {mode!r}")
    rng = random.Random(seed)
    rows, key = [], {}
    skipped = 0
    for t in triples:
        for f in ("id", "context", "seth_reply", "huuman_reply"):
            if f not in t:
                raise ValueError(f"triple missing field '{f}': {t!r}")
        context = redact(t["context"], name_tokens)
        seth_reply = redact(t["seth_reply"], name_tokens)
        huuman_reply = redact(t["huuman_reply"], name_tokens)
        if mode == "preference" and seth_reply.strip() == huuman_reply.strip():
            # Real-vs-real pair: no model side to prefer. Not scored as a win
            # or a loss -- see designs/US-6.md refusal condition #4.
            skipped += 1
            continue
        first_is_seth = rng.random() < 0.5
        opt_a = seth_reply if first_is_seth else huuman_reply
        opt_b = huuman_reply if first_is_seth else seth_reply
        if mode == "preference":
            key[t["id"]] = "B" if first_is_seth else "A"   # which option is the MODEL reply
        else:
            key[t["id"]] = "A" if first_is_seth else "B"   # which option is the REAL Seth
        rows.append({"id": t["id"], "context": context,
                     "option_A": opt_a, "option_B": opt_b,
                     "choice": "", "confidence": "",
                     "axis_opinion": "", "axis_memory": "", "axis_reasoning": "",
                     "axis_lexical": "", "axis_tone": "", "axis_syntax": ""})
    rng.shuffle(rows)
    return rows, key, skipped


def write_outputs(rows, key, mode, out_dir):
    """Write rating_sheet.csv + answer_key.json to out_dir. Returns the two
    paths written. Factored out of main() so tests can exercise the exact
    on-disk shape without going through argv/subprocess."""
    os.makedirs(out_dir, exist_ok=True)
    sheet = os.path.join(out_dir, "rating_sheet.csv")
    keyf = os.path.join(out_dir, "answer_key.json")
    with open(sheet, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDNAMES)
        w.writeheader(); w.writerows(rows)
    key_out = dict(key)
    if mode == "preference":
        key_out["_mode"] = "preference"
    with open(keyf, "w") as f:
        json.dump(key_out, f, indent=2)
    return sheet, keyf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("triples")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--out-dir", default=".")
    ap.add_argument("--mode", choices=("detection", "preference"), default="detection",
                     help="detection (default, unchanged): 'which is real Seth?'. "
                          "preference (US-6): 'which reply would you rather receive?' "
                          "-- a separate, non-promotion-gating measurement scored by "
                          "score_preference.py.")
    a = ap.parse_args()
    with open(a.triples) as f:
        triples = json.load(f)
    if not isinstance(triples, list) or not triples:
        print("triples.json must be a non-empty list", file=sys.stderr); sys.exit(2)

    name_tokens = resolve_contact_name_tokens()
    rows, key, skipped = build(triples, a.seed, mode=a.mode, name_tokens=name_tokens)

    if skipped:
        print(f"skipped {skipped}/{len(triples)} triples: seth_reply == huuman_reply "
              f"(no model side to compare)", file=sys.stderr)

    if not rows:
        print("REFUSING: 0 usable rows"
              + (" after excluding real==model duplicate pairs (refusal "
                 "condition #4, designs/US-6.md)" if a.mode == "preference" else "")
              + "; nothing written", file=sys.stderr)
        sys.exit(2)

    sheet, keyf = write_outputs(rows, key, a.mode, a.out_dir)
    print(f"wrote {sheet} ({len(rows)} items, mode={a.mode}) and {keyf} (KEEP PRIVATE)")


if __name__ == "__main__":
    main()
