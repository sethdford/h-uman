#!/usr/bin/env python3
"""GraphRAG grounding A/B: measure the MARGINAL effect of per-contact community
summaries on h-uman's replies.

For each real (incoming, real_seth_reply) pair belonging to a contact that HAS
community summaries in ~/.human/graph.db, this generates two h-uman replies to
the same incoming:

    reply_ON  = ./build/human agent --contact <id> -m <incoming>   HU_GRAPH_GROUNDING=on
    reply_OFF = ./build/human agent --contact <id> -m <incoming>   HU_GRAPH_GROUNDING=off

then asks a blinded Gemini judge which of {reply_ON, reply_OFF} is closer to the
REAL Seth reply / more human, in randomized order. The win counts give grounding's
marginal effect — the thing blind_ab_gate.json's fool_rate (AI-vs-human absolute)
cannot measure.

This is a PAIRED design: each pair is its own matched control, so the only variable
between the two candidates is whether grounding injected the contact's relationship
context into the prompt. Byte-identical ON/OFF outputs are recorded as no-effect
ties (grounding changed nothing for that pair) and excluded from the win-rate
denominator.

Pipeline:
  1. export_seth_triples.py --keep-handles  -> real pairs keyed by real contact handle
  2. grounding_ab.py --contact <id>          -> reply_ON/reply_OFF + judge  (THIS SCRIPT)
  3. writes docs/research/...md + blind_ab_gate.json proxy half (via --gate)

Usage:
  python3 scripts/grounding_ab.py --contact "+447914633409" --pairs 30
  python3 scripts/grounding_ab.py --contact "+447914633409" --triples /tmp/t.json --gate

Privacy: runs 100% locally against the on-device model server. The judge prompt
(Gemini via ADC) DOES send the message texts off-device — same trust boundary as
eval_blinded_ab.py. Use only with the user's own data and consent.
"""
import argparse
import json
import math
import os
import random
import re
import subprocess
import sys
import time

_SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_SCRIPTS_DIR, ".."))
sys.path.insert(0, _SCRIPTS_DIR)

# Reuse the Gemini/ADC plumbing + gate writer from the existing harness.
import eval_blinded_ab as eab  # noqa: E402  (call_gemini, _get_adc_token, _git_commit)
import blind_ab_gate as gate   # noqa: E402

HUMAN_BIN = os.path.join(_REPO_ROOT, "build", "human")
GRAPH_DB = os.path.expanduser("~/.human/graph.db")

# Judge schema: which candidate is closer to the real Seth reply. Mirrors the
# blinded A/B schema in eval_blinded_ab.py (bare JSON via responseSchema).
_GROUNDING_JUDGE_SCHEMA = {
    "type": "object",
    "properties": {
        "choice": {"type": "string", "enum": ["A", "B"]},
        "confidence": {"type": "integer", "minimum": 1, "maximum": 10},
        "reasoning": {"type": "string"},
    },
    "required": ["choice", "confidence", "reasoning"],
    "propertyOrdering": ["choice", "confidence", "reasoning"],
}

_ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")

# Signatures of a degenerate reply where the model regurgitated its persona
# system-prompt constraints instead of replying (a response_guard bypass glitch).
# These outputs are NOISE orthogonal to grounding — excluding pairs where EITHER
# condition produced one keeps the A/B measuring grounding's effect on legitimate
# replies. Applied symmetrically to ON and OFF; per-condition counts are reported.
_LEAK_SIGNATURES = (
    "lowercase unless shouting", "zero markdown", "zero em-dash", "em-dashes",
    "no formal transitions", "text like on a phone", "no lists",
    "you are this person", "output only what this person", "contractions always",
)
_NUMBERED_ITEM = re.compile(r"(?:^|\s)\d+\.\s")


def _looks_degenerate(reply):
    """True when a reply is a leaked-prompt / instruction-echo artifact rather
    than a conversational reply."""
    if not reply:
        return True
    low = reply.lower()
    if any(sig in low for sig in _LEAK_SIGNATURES):
        return True
    # 3+ numbered list items is the persona-constraint-list echo shape.
    if len(_NUMBERED_ITEM.findall(reply)) >= 3:
        return True
    return False


def _strip_reply(stdout):
    """Extract the assistant reply text from `human agent -m` stdout."""
    out = _ANSI.sub("", stdout)
    lines = [l.strip() for l in out.strip().split("\n")
             if l.strip() and l.strip() != "Goodbye."]
    return " ".join(lines) if lines else ""


def gen_reply(contact_id, incoming, grounding, timeout):
    """Run the local model once through the real agent turn with grounding on/off."""
    env = dict(os.environ)
    env["HU_GRAPH_GROUNDING"] = grounding  # "on" | "off"
    try:
        res = subprocess.run(
            [HUMAN_BIN, "agent", "--contact", contact_id, "-m", incoming],
            capture_output=True, text=True, timeout=timeout, cwd=_REPO_ROOT, env=env,
        )
        reply = _strip_reply(res.stdout)
        return reply
    except subprocess.TimeoutExpired:
        return ""
    except Exception as e:
        return f"(error: {e})"


def judge(incoming, real_seth, cand_a, cand_b):
    prompt = f"""You are judging which of two AI-generated text replies better matches how a specific real person ("Seth") actually replies, and reads as more naturally human.

A friend texted Seth:
"{incoming}"

Seth's REAL reply was:
"{real_seth}"

Two candidate AI replies were generated. Pick the one that is CLOSER to Seth's real reply in voice, content, brevity, and tone — and that reads as more authentically human (casual, brief, opinionated; not over-helpful or generic).

CANDIDATE A: "{cand_a}"
CANDIDATE B: "{cand_b}"

Return ONLY valid JSON:
{{"choice": "A" or "B", "confidence": 1-10, "reasoning": "brief"}}"""
    raw = eab.call_gemini(prompt, temperature=0.2,
                          response_schema=_GROUNDING_JUDGE_SCHEMA)
    if "```json" in raw:
        raw = raw.split("```json")[1].split("```")[0].strip()
    elif "```" in raw:
        raw = raw.split("```")[1].split("```")[0].strip()
    return json.loads(raw)


def detect_contacts_with_summaries():
    """Return contact_ids that have >=1 row in graph.db community_summaries."""
    try:
        import sqlite3
        con = sqlite3.connect(f"file:{GRAPH_DB}?mode=ro", uri=True)
        rows = con.execute(
            "SELECT contact_id, COUNT(*) FROM community_summaries "
            "WHERE contact_id != '' GROUP BY contact_id ORDER BY 2 DESC").fetchall()
        con.close()
        return [r[0] for r in rows]
    except Exception as e:
        print(f"  (could not read {GRAPH_DB}: {e})")
        return []


def load_pairs(triples_path, contact_ids, per_contact_cap, total_limit, min_len):
    """Load pairs for the given contacts, each tagged with its contact_id.

    Triples are keyed by real handle (--keep-handles) under contact_name. Caps
    per contact so one big thread can't dominate, then round-robins across
    contacts for a balanced pool, then truncates to total_limit."""
    with open(triples_path) as f:
        triples = json.load(f)
    want = set(contact_ids)
    by_contact = {cid: [] for cid in contact_ids}
    for t in triples:
        name = t.get("contact_name", "")
        if name not in want:
            continue
        ctx = (t.get("context") or "").strip()
        rep = (t.get("seth_reply") or "").strip()
        if len(ctx) < 3 or len(rep) < min_len:
            continue
        by_contact[name].append({"contact": name, "incoming": ctx, "seth_reply": rep})
    for cid in by_contact:
        random.shuffle(by_contact[cid])
        if per_contact_cap > 0:
            by_contact[cid] = by_contact[cid][:per_contact_cap]
    # Round-robin interleave across contacts for diversity.
    pooled = []
    depth = 0
    buckets = [v for v in by_contact.values() if v]
    while buckets and any(depth < len(b) for b in buckets):
        for b in buckets:
            if depth < len(b):
                pooled.append(b[depth])
        depth += 1
    return pooled[:total_limit]


def wilson_ci(wins, n, z=1.96):
    """Wilson score interval for a binomial proportion (win rate among decisions)."""
    if n == 0:
        return (0.0, 0.0, 0.0)
    p = wins / n
    denom = 1 + z * z / n
    center = (p + z * z / (2 * n)) / denom
    half = (z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n))) / denom
    return (p, max(0.0, center - half), min(1.0, center + half))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--contact", help="single contact_id (e.g. +447914633409)")
    ap.add_argument("--contacts", help="comma-separated contact_ids; pooled, each pair uses its own contact's grounding")
    ap.add_argument("--triples", default=os.path.join(_SCRIPTS_DIR, "blind_ab", "seth_triples.json"),
                    help="JSON array of {contact_name, context, seth_reply} (use --keep-handles export)")
    ap.add_argument("--pairs", type=int, default=80, help="max total pairs to evaluate (pooled)")
    ap.add_argument("--per-contact", type=int, default=0, help="cap pairs per contact (0 = no cap)")
    ap.add_argument("--min-len", type=int, default=12, help="skip real replies shorter than this")
    ap.add_argument("--timeout", type=int, default=300,
                    help="per-generation timeout (s); cold-start + a response_guard retry can take ~280s")
    ap.add_argument("--out-json", default=os.path.join(_REPO_ROOT, "data", "grounding_ab.json"))
    ap.add_argument("--gate", action="store_true",
                    help="write the proxy half of docs/evaluation/blind_ab_gate.json")
    a = ap.parse_args()

    if not os.path.exists(HUMAN_BIN):
        sys.exit(f"build/human not found at {HUMAN_BIN} — build the human target first")
    if not os.path.exists(a.triples):
        sys.exit(f"triples not found: {a.triples}\n  run: python3 scripts/blind_ab/export_seth_triples.py "
                 f"--keep-handles --out {a.triples}")
    if not (os.environ.get("GEMINI_API_KEY") or
            os.path.exists(os.path.expanduser("~/.config/gcloud/application_default_credentials.json"))):
        sys.exit("ERROR: set GEMINI_API_KEY or configure gcloud ADC for the judge")

    # Resolve the contact set: explicit --contact / --contacts, else auto-detect
    # every contact that has community summaries in graph.db.
    if a.contacts:
        contact_ids = [c.strip() for c in a.contacts.split(",") if c.strip()]
    elif a.contact:
        contact_ids = [a.contact]
    else:
        contact_ids = detect_contacts_with_summaries()
        if not contact_ids:
            sys.exit("no contacts with community_summaries found; pass --contact or --contacts")
        print(f"  auto-detected {len(contact_ids)} contacts with summaries: {', '.join(contact_ids)}")

    pairs = load_pairs(a.triples, contact_ids, a.per_contact, a.pairs, a.min_len)
    if not pairs:
        sys.exit(f"no usable pairs for contacts {contact_ids} in {a.triples} "
                 f"(need context>=3 chars, reply>={a.min_len} chars)")

    print("=" * 72)
    print("  GraphRAG GROUNDING A/B — marginal effect of per-contact community summaries")
    print("=" * 72)
    print(f"  Contacts: {len(contact_ids)} ({', '.join(contact_ids)})")
    print(f"  Pairs:    {len(pairs)} (pooled, each generated with its own contact's grounding)")
    print(f"  Judge:    {eab.EVAL_MODEL} (blinded, randomized A/B)")
    print(f"  Model:    local agent turn, ON vs OFF grounding")
    print("=" * 72)

    st = {"on_wins": 0, "off_wins": 0, "ties": 0, "errors": 0,
          "decisions": 0, "leak_on": 0, "leak_off": 0}
    trials = []

    def snapshot():
        """Recompute aggregates and write out_json. Called after every pair so a
        long run that is interrupted still leaves a valid, up-to-date result."""
        d = st["decisions"]
        p, lo, hi = wilson_ci(st["on_wins"], d)
        if not d:
            verdict = "NO DECISIONS"
        elif lo > 0.5:
            verdict = "GROUNDING SUBSTANTIATED (CI above 50%)"
        elif hi < 0.5:
            verdict = "GROUNDING HARMFUL (CI below 50%)"
        else:
            verdict = "NOT SUBSTANTIATED (CI crosses 50%)"
        result = {
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "measurement": "graphrag_grounding_marginal_ab",
            "contacts": contact_ids, "n_pairs": len(pairs),
            "decisions": d, "on_wins": st["on_wins"], "off_wins": st["off_wins"],
            "ties": st["ties"], "errors": st["errors"],
            "leak_on": st["leak_on"], "leak_off": st["leak_off"],
            "on_win_rate": (p * 100 if d else None),
            "ci_lo": (lo * 100 if d else None), "ci_hi": (hi * 100 if d else None),
            "verdict": verdict, "commit": eab._git_commit(), "trials": trials,
        }
        os.makedirs(os.path.dirname(a.out_json), exist_ok=True)
        tmp = a.out_json + ".tmp"
        with open(tmp, "w") as f:
            json.dump(result, f, indent=2, ensure_ascii=False)
        os.replace(tmp, a.out_json)  # atomic — never leaves a half-written file
        return result, p, lo, hi, verdict

    # Graceful early stop: SIGTERM/SIGINT set a flag so the current pair finishes,
    # the snapshot is already on disk, and we fall through to the summary + gate.
    stop = {"flag": False}

    def _on_signal(signum, frame):
        stop["flag"] = True
        print(f"\n  (signal {signum} received — finishing current pair, then stopping)")
    import signal
    signal.signal(signal.SIGTERM, _on_signal)
    signal.signal(signal.SIGINT, _on_signal)

    for i, pair in enumerate(pairs):
        if stop["flag"]:
            print(f"  stopping early at pair {i} ({st['decisions']} decisions collected)")
            break
        incoming, real_seth, cid = pair["incoming"], pair["seth_reply"], pair["contact"]
        print(f"\n[{i+1}/{len(pairs)}] ({cid}) incoming: {incoming[:55]!r}")
        sys.stdout.flush()

        reply_on = gen_reply(cid, incoming, "on", a.timeout)
        reply_off = gen_reply(cid, incoming, "off", a.timeout)

        if not reply_on or not reply_off or reply_on.startswith("(error") or reply_off.startswith("(error"):
            print(f"  SKIP: empty/error reply (on={len(reply_on)}b off={len(reply_off)}b)")
            st["errors"] += 1
        else:
            deg_on, deg_off = _looks_degenerate(reply_on), _looks_degenerate(reply_off)
            if deg_on or deg_off:
                # Symmetric exclusion: can't bias the win-rate toward ON or OFF.
                st["leak_on"] += int(deg_on)
                st["leak_off"] += int(deg_off)
                st["errors"] += 1
                print(f"  SKIP: degenerate prompt-leak reply (on_leak={deg_on} off_leak={deg_off})")
                trials.append({"contact": cid, "incoming": incoming, "real_seth": real_seth,
                               "reply_on": reply_on, "reply_off": reply_off,
                               "outcome": "leak", "leak_on": deg_on, "leak_off": deg_off})
            elif reply_on == reply_off:
                print("  TIE: grounding produced byte-identical output (no marginal effect)")
                st["ties"] += 1
                trials.append({"contact": cid, "incoming": incoming, "real_seth": real_seth,
                               "reply_on": reply_on, "reply_off": reply_off, "outcome": "tie"})
            else:
                # Randomize A/B order so the judge can't learn a positional bias.
                on_is_a = random.random() < 0.5
                cand_a, cand_b = (reply_on, reply_off) if on_is_a else (reply_off, reply_on)
                time.sleep(0.3)
                try:
                    j = judge(incoming, real_seth, cand_a, cand_b)
                    choice = j.get("choice", "?")
                    on_won = (choice == "A") == on_is_a
                    st["decisions"] += 1
                    st["on_wins" if on_won else "off_wins"] += 1
                    print(f"  ON {'WON' if on_won else 'lost'} (judge picked {choice}, "
                          f"conf {j.get('confidence')}) — {j.get('reasoning','')[:80]}")
                    trials.append({
                        "contact": cid, "incoming": incoming, "real_seth": real_seth,
                        "reply_on": reply_on, "reply_off": reply_off,
                        "on_was": "A" if on_is_a else "B", "judge_choice": choice,
                        "on_won": on_won, "confidence": j.get("confidence"),
                        "reasoning": j.get("reasoning"),
                        "outcome": "on_win" if on_won else "off_win",
                    })
                except Exception as e:
                    print(f"  judge error: {e}")
                    st["errors"] += 1
        snapshot()  # checkpoint after every pair
        time.sleep(0.3)

    result, p, lo, hi, verdict = snapshot()
    d = st["decisions"]
    print(f"\n{'=' * 72}")
    print("  GROUNDING A/B RESULTS")
    print(f"{'=' * 72}")
    print(f"  Decisions (non-tie): {d}")
    print(f"  ON  wins:  {st['on_wins']}")
    print(f"  OFF wins:  {st['off_wins']}")
    print(f"  Ties (no effect): {st['ties']}")
    print(f"  Errors/skips: {st['errors']}  "
          f"(degenerate prompt-leak replies excluded: ON={st['leak_on']} OFF={st['leak_off']})")
    if d:
        print(f"\n  ON-win-rate: {p*100:.1f}%  (95% Wilson CI {lo*100:.1f}%–{hi*100:.1f}%)")
    print(f"  VERDICT: {verdict}")
    print(f"\n  Full results -> {a.out_json}")

    if a.gate:
        # Record grounding's marginal effect in the proxy half. This is a DISTINCT
        # metric from fool_rate (named via `measurement`), so a reader never
        # confuses grounding-win-rate with AI-vs-human fool-rate.
        if not d or d < gate.ENFORCE_MIN_PAIRS:
            gmode, gverdict = "ADVISORY", "ADVISORY"
        else:
            gmode = "ENFORCING"
            gverdict = "PASS" if lo > 0.5 else "FAIL"
        gate.write_proxy_half(gate.GATE_PATH, {
            "measurement": "graphrag_grounding_marginal_ab",
            "verdict": gverdict, "mode": gmode,
            "on_win_rate": (p * 100 if d else None),
            "ci_lo": (lo * 100 if d else None), "ci_hi": (hi * 100 if d else None),
            "n_decisions": d, "n_pairs": len(pairs),
            "ties": st["ties"], "contacts": contact_ids,
            # keep fool_rate keys present-but-null so downstream readers that
            # expect the schema don't KeyError; this proxy measures grounding.
            "fool_rate": None, "n_real_pairs": d,
        }, commit=eab._git_commit())
        print(f"  GATE proxy half written -> {gate.GATE_PATH} ({gmode}/{gverdict})")


if __name__ == "__main__":
    main()
