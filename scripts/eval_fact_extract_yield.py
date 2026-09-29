#!/usr/bin/env python3
"""Measure the LLM fact-extractor's YIELD on real historical messages.

Before building a backfill over 1,789 messages we need to know how many facts
that would actually produce. At the observed live rate -- 1 extraction in 15
hours, returning zero facts -- the graph fills in months, so the question is
whether history is worth replaying at all.

There is precedent for measuring this: fact_extract_llm.c records that the
original predicate set "measured ~0 facts on the real iMessage corpus
(2026-07-11, :8744 base-model measurement)", which is why the event/plan
predicates were added. This re-measures the CURRENT prompt.

Faithfulness: uses the exact system prompt, user template and gating rules the
daemon uses (HU_FACT_LLM_SYS / HU_FACT_LLM_USER_TEMPLATE, min length 16, LLM
path only). Read-only -- reads chat.db, writes nothing but its results file.

REFUSES rather than guessing: if the endpoint is unreachable or returns
unparseable output for most of the sample, it exits non-zero rather than
reporting a yield derived from failures.
"""
import argparse, json, os, re, sqlite3, sys, time, urllib.request, urllib.error

CHATDB = os.path.expanduser("~/Library/Messages/chat.db")
ENDPOINT = "http://127.0.0.1:8741/v1/chat/completions"
MIN_LEN = 16  # HU_LLM_FACT_EXTRACT_MIN_LEN in personal_model.c

# Closed predicate vocabulary from HU_FACT_LLM_USER_TEMPLATE. Anything outside it
# is drift: a predicate space that grows per message cannot be queried, which
# defeats the retrieval this feeds. Measured before filtering: 10 of 18 distinct
# predicates were off-vocabulary (has_condition, talking_to, working_on, ...).
CLOSED_PREDICATES = {
    "likes", "hates", "lives_in", "works_at", "owns", "uses", "prefers", "avoids",
    "knows", "learning", "graduating", "moving_to", "visiting", "attending",
    "planning", "expecting", "celebrating", "asking_about",
}

import re as _re
DAEMON_SHAPE = _re.compile(
    r"^(want me to send|should i (send|text|reply)|"
    r"you'?ve been (kind of )?quiet|i'?ll (look into|get back to) that)", _re.I)

SYS = ("You are a personal-fact extractor for text-message conversations. Read the "
       "message and output ONLY a JSON object listing personal facts you can "
       "extract. Output the JSON immediately - no preamble, no reasoning, no "
       "explanation, no markdown fences.")

USER_TMPL = (
    'Extract personal facts from this message. Output JSON of the shape:\n'
    '{"facts": [\n'
    '  {"subject":"user","predicate":"<verb>","object":"<value>","confidence":<0-1>}\n]}\n'
    'Predicates use short forms - states: likes, hates, lives_in, works_at, owns, uses, '
    'prefers, avoids, knows, learning; events and plans: graduating, moving_to, visiting, '
    'attending, planning, expecting, celebrating, asking_about. Subject "user" means the '
    'message author. Questions the author asks count as asking_about facts. Output '
    '{"facts":[]} only when the message truly carries no personal content (bare '
    'acknowledgments like "ok").\n\nMessage: {msg}')


POOL_SIZE = 0


def sample_messages(db, n, seed, own_only=True):
    """Sample real messages, DECODING attributedBody.

    On this machine 3,830 of 3,911 chat.db rows have text = NULL and carry their
    content in attributedBody. A plain `WHERE text IS NOT NULL` query therefore
    sees under 2% of history -- an earlier run of this script sampled 50 messages
    from a pool of 65 and then projected the result onto 1,789, which is why that
    projection was withdrawn.

    own_only filters to is_from_me=1. The extractor defines subject:"user" as the
    MESSAGE AUTHOR, so extracting from inbound messages files other people's facts
    as Seth's. The plain-text pool was 51% inbound, so this is not hypothetical.
    """
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "blind_ab"))
    from imessage_text import decode_attributed_body  # canonical decoder

    con = sqlite3.connect(db)
    con.text_factory = bytes
    # FILTER 1 (structural): associated_message_type != 0 marks a REACTION. A
    # tapback's text quotes the message it reacts to -- 'Loved "I also understand
    # the struggles of mental illness..."' -- so extracting from it files the OTHER
    # person's facts as the user's. Measured: 145/1423 (10.2%) are reactions and
    # 95 (6.7%) literally quote someone else. The sample produced
    # has_condition=adhd / depression / mother-with-bipolar about the wrong person
    # from exactly one such row. This is the single most important filter here.
    where = "WHERE is_from_me = 1 AND COALESCE(associated_message_type,0) = 0" \
        if own_only else "WHERE COALESCE(associated_message_type,0) = 0"
    rows = con.execute(
        f"SELECT text, attributedBody FROM message {where} ORDER BY date DESC").fetchall()

    msgs, decoded, plain, self_gen = [], 0, 0, 0
    for txt, blob in rows:
        s = None
        if txt:
            try:
                s = txt.decode("utf-8", errors="replace")
                plain += 1
            except Exception:
                s = None
        if not s and blob:
            try:
                s = decode_attributed_body(blob)
                if s:
                    decoded += 1
            except Exception:
                s = None
        if not s or len(s.strip()) < MIN_LEN:
            continue
        t = s.strip()
        # FILTER 2 (partial, and honestly labelled): drop text the DAEMON wrote.
        # Re-ingesting the assistant's own output as the user's facts is a
        # feedback loop that amplifies its own errors -- the sample extracted
        # asking_about=Annie from 'want me to send a quick check-in text to
        # Annie?', which is a daemon proposal, not something Seth said.
        # proactive_sends records only 14 rows and REACTIVE daemon replies are
        # not recorded anywhere, so they are NOT separable from Seth's own typing
        # in chat.db. This catches the proposal shape only; the residual is a
        # known limitation, not a solved problem.
        if DAEMON_SHAPE.search(t):
            self_gen += 1
            continue
        msgs.append(t)

    print(f"[yield] pool: {len(msgs)} eligible (>= {MIN_LEN} chars) "
          f"from {len(rows)} rows -- {plain} plain-text, {decoded} decoded from attributedBody"
          f"{'; is_from_me=1 only' if own_only else ''}; dropped {self_gen} daemon-shaped, reactions excluded")
    global POOL_SIZE
    POOL_SIZE = len(msgs)
    import random
    random.Random(seed).shuffle(msgs)
    return msgs[:n]


def extract(msg, timeout):
    body = json.dumps({
        "model": "GLM-4.5-Air-4bit",
        "messages": [{"role": "system", "content": SYS},
                     {"role": "user", "content": USER_TMPL.replace("{msg}", msg)}],
        "max_tokens": 220, "temperature": 0.0}).encode()
    req = urllib.request.Request(ENDPOINT, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        txt = json.loads(r.read())["choices"][0]["message"]["content"]
    m = re.search(r'\{[^{}]*"facts"\s*:\s*\[.*?\]\s*\}', txt, re.S)
    if not m:
        return None, txt
    try:
        return json.loads(m.group(0)).get("facts", []), txt
    except json.JSONDecodeError:
        return None, txt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=60)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--include-inbound", action="store_true",
                    help="also sample inbound; facts get attributed to the WRONG person")
    ap.add_argument("--out", default="docs/plans/2026-08-02-semantic-retrieval/fact-yield.json")
    a = ap.parse_args()

    msgs = sample_messages(CHATDB, a.n, a.seed, own_only=not a.include_inbound)
    if len(msgs) < a.n // 2:
        sys.exit(f"FATAL: only {len(msgs)} eligible messages; sample too thin to measure.")
    print(f"[yield] {len(msgs)} real messages (>= {MIN_LEN} chars), endpoint {ENDPOINT}")

    rows, unparseable, errors = [], 0, 0
    t0 = time.time()
    for i, m in enumerate(msgs, 1):
        try:
            facts, raw = extract(m, a.timeout)
        except (urllib.error.URLError, OSError, KeyError, ValueError) as e:
            errors += 1
            rows.append({"msg": m[:90], "n_facts": None, "error": type(e).__name__})
            print(f"  [{i}/{len(msgs)}] ERROR {type(e).__name__}")
            continue
        if facts is None:
            unparseable += 1
            rows.append({"msg": m[:90], "n_facts": None, "unparseable": raw[:80]})
        else:
            preds = [str(f.get("predicate", "")) for f in facts]
            in_vocab = [p for p in preds if p in CLOSED_PREDICATES]
            rows.append({"msg": m[:90], "n_facts": len(facts),
                         "n_facts_in_vocab": len(in_vocab),
                         "off_vocab": sorted(set(p for p in preds if p not in CLOSED_PREDICATES)),
                         "facts": [f"{f.get('predicate')}={f.get('object')}" for f in facts[:4]]})
        if i % 10 == 0:
            print(f"  [{i}/{len(msgs)}] {time.time()-t0:.0f}s elapsed")

    ok = [r for r in rows if r.get("n_facts") is not None]
    if len(ok) < len(msgs) * 0.6:
        sys.exit(f"FATAL: only {len(ok)}/{len(msgs)} produced parseable output "
                 f"({unparseable} unparseable, {errors} errors). Refusing to report a "
                 f"yield derived mostly from failures.")

    total = sum(r["n_facts"] for r in ok)
    in_vocab = sum(r.get("n_facts_in_vocab", 0) for r in ok)
    off = sorted({p for r in ok for p in r.get("off_vocab", [])})
    with_fact = sum(1 for r in ok if r["n_facts"] > 0)
    res = {
        "n_sampled": len(msgs), "n_parseable": len(ok),
        "unparseable": unparseable, "errors": errors,
        "facts_total": total,
        "facts_per_message": round(total / len(ok), 3),
        "pct_messages_with_a_fact": round(100 * with_fact / len(ok), 1),
        "facts_in_vocab": in_vocab,
        "pct_facts_in_closed_vocab": round(100 * in_vocab / max(total, 1), 1),
        "off_vocabulary_predicates": off,
        "filters": ["reactions excluded (associated_message_type != 0)",
                    "is_from_me=1 only", "daemon-shaped proposals dropped (partial)"],
        "known_limitation": "reactive daemon replies are not recorded anywhere and are "
                            "NOT separable from Seth's own typing in chat.db",
        "pool_size": POOL_SIZE,
        "projected_facts_over_pool": round(total / len(ok) * POOL_SIZE),
        "endpoint": ENDPOINT, "min_len": MIN_LEN,
        "measures": "LLM fact-extractor yield on real historical messages; the daemon "
                    "only invokes this path when the regex pass found nothing",
        "rows": rows,
    }
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    json.dump(res, open(a.out, "w"), indent=2)

    print(f"\n[yield] parseable {len(ok)}/{len(msgs)}  ({unparseable} unparseable, {errors} err)")
    print(f"  facts/message          : {res['facts_per_message']}")
    print(f"  messages with >=1 fact : {res['pct_messages_with_a_fact']}%")
    print(f"  projected over pool {POOL_SIZE:<5}: ~{res['projected_facts_over_pool']} facts")
    print(f"  facts in closed vocab  : {res['pct_facts_in_closed_vocab']}% "
          f"({in_vocab}/{total})")
    print(f"  off-vocabulary preds   : {off[:6]}")
    print(f"  wrote {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
