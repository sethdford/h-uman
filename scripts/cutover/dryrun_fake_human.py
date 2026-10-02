#!/usr/bin/env python3
"""A stand-in for `human replay`, for the cut-over kit's dry run on machines
(or CI jobs) without a C build. Same CLI subset and the same row shape as
src/app/cli_replay.c: reads one turn per line from --in, makes a director call
and a reply call to the loopback --endpoint, writes one row per turn to --out.

It "contains" these gates, so the kit's binary-strings gate detection finds
them, and it puts each one's env value into the reply request, so a gate arm
changes the request (reply_fp) like a real gate would:
"""
import hashlib
import json
import os
import sys
import urllib.request

GATES = ("HU_THREAD_CONTEXT", "HU_LENGTH_POLICY", "HU_DIRECTOR_V2")


def _post(endpoint, body):
    req = urllib.request.Request(endpoint.rstrip("/") + "/chat/completions",
                                 data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)["choices"][0]["message"]["content"]


def main(argv):
    if not argv or argv[0] != "replay":
        print("usage: dryrun_fake_human.py replay --in F --out F --arm A --endpoint URL",
              file=sys.stderr)
        return 2
    if "--help" in argv:
        print("usage: human replay --in F --out F --arm NAME --endpoint URL")
        return 0
    opts = {}
    it = iter(argv[1:])
    for a in it:
        if a.startswith("--"):
            opts[a[2:]] = next(it, "")
    gate_env = ",".join(f"{g}={os.environ.get(g, 'off')}" for g in GATES)
    out = []
    with open(opts["in"]) as f:
        turns = [json.loads(line) for line in f if line.strip()]
    for t in turns:
        inbound = "\n".join(t["inbound_bubbles"])
        director_v2 = os.environ.get("HU_DIRECTOR_V2", "off")
        d = json.loads(_post(opts["endpoint"], {
            "model": opts.get("model", "fake-model"), "cutover_role": "director",
            "messages": [{"role": "user", "content": f"New message from them: {inbound} "
                                                     f"[director_v2={director_v2}]"}]}))
        body = {"model": opts.get("model", "fake-model"), "temperature": 0,
                "messages": [{"role": "system", "content": f"gates: {gate_env}"},
                             {"role": "user", "content": inbound}]}
        fp = hashlib.sha256(json.dumps(body, sort_keys=True).encode()).hexdigest()[:16]
        if d.get("action") == "tapback":
            row = {"id": t["id"], "arm": opts.get("arm"), "action": "tapback", "bubbles": [],
                   "bubble_count": 0, "reply_fp": None}
        else:
            text = _post(opts["endpoint"], body)
            bubbles = [b for b in text.split(", ") if b] if os.environ.get(
                "HU_LENGTH_POLICY") == "live" else [text]
            row = {"id": t["id"], "arm": opts.get("arm"), "action": "text", "bubbles": bubbles,
                   "bubble_count": len(bubbles), "reply_fp": fp}
        row.update({"director_action": d.get("action"), "channel_outbound_calls": 0})
        out.append(row)
    fd = os.open(opts["out"], os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        for row in out:
            f.write(json.dumps(row) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
