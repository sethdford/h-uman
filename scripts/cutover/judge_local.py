#!/usr/bin/env python3
"""Local cross-family judge for the cut-over kit: scripts/blind_ab/synthetic_judge.py,
run unchanged except for the same three patches as the operator's
judge_crossfamily.py wrapper:

  1. loopback only: any --endpoint or request URL that is not 127.0.0.1 /
     localhost / [::1] is refused, and so is --api (cloud judges ship text);
  2. reasoning_effort="none" is injected into every request body (Gemma 4's
     thinking otherwise eats the 1024-token budget and returns no content);
  3. a pause after every call (--pace-s, default 2 s), so the judge never
     saturates the machine production inference runs on.

Usage (the kit calls it per arm):
  python3 judge_local.py SHEET --out JUDGED --harness-dir scripts/blind_ab \
      --endpoint http://127.0.0.1:11434/v1/chat/completions --model gemma4-26b-mmap

It never scores and never writes a gate file: the kit scores the judged sheet
itself (cutover_report.py) and never calls score.py --emit-gate.
"""
import argparse
import json
import os
import sys
import time
import urllib.parse
import urllib.request

LOOPBACK = {"127.0.0.1", "localhost", "::1"}


def is_loopback(url):
    try:
        u = urllib.parse.urlsplit(url)
    except ValueError:
        return False
    return u.scheme in ("http", "https") and (u.hostname or "").lower() in LOOPBACK


def install(sj, pace_s):
    """Patch synthetic_judge's module (sj) in place: loopback-only requests
    carrying reasoning_effort=none, and a pause after every call."""
    real_request = sj.urllib.request.Request

    def request(url, data=None, headers=None, **kw):
        if not is_loopback(url):
            raise RuntimeError("refusing non-loopback judge URL")
        if data:
            body = json.loads(data)
            body["reasoning_effort"] = "none"
            data = json.dumps(body).encode()
        return real_request(url, data=data, headers=headers or {}, **kw)

    class _Shim:
        Request = staticmethod(request)
        urlopen = staticmethod(urllib.request.urlopen)

    sj.urllib = type("_U", (), {"request": _Shim})
    real_call = sj.call

    def paced(*a, **k):
        try:
            return real_call(*a, **k)
        finally:
            if pace_s > 0:
                time.sleep(pace_s)

    sj.call = paced


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--harness-dir", required=True)
    ap.add_argument("--pace-s", type=float, default=2.0)
    ap.add_argument("--endpoint", default=None)
    ap.add_argument("--api", default=None)
    own, rest = ap.parse_known_args(argv)
    if own.api is not None:
        print("refusing: --api (the cut-over judge is local OpenAI-compatible only)",
              file=sys.stderr)
        return 2
    if not own.endpoint or not is_loopback(own.endpoint):
        print("refusing: --endpoint must be a loopback URL", file=sys.stderr)
        return 2
    sys.path.insert(0, os.path.abspath(own.harness_dir))
    import synthetic_judge as sj  # noqa: E402
    install(sj, own.pace_s)
    sys.argv = ["synthetic_judge.py"] + rest + ["--endpoint", own.endpoint]
    sj.main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
