#!/usr/bin/env python3
"""pro_clone — turn Seth's recordings into a Cartesia Pro Voice Clone and adopt it.

Why: the live voice is an instant clone from ~10 s of audio (April 2026);
timbre and personal habits cannot be fixed in code. Cartesia's Pro Voice
Clone trains on 30+ minutes of clean single-speaker audio (Startup plan,
up to ~3 h training) and is the only lever for "sounds like me". This script
is the whole pipeline so the day the recording exists, nothing else is needed.

Steps (each idempotent, each writes into --dir, default ~/.human/pro_clone):
  prepare  --in <files or dirs>...   validate + convert to 44.1 kHz mono WAV,
                                     sum duration, write manifest.json
  submit   --name "Seth Pro"         POST /datasets, upload every WAV
                                     (purpose=fine_tune), POST /fine-tunes,
                                     write job.json
  status                             GET /fine-tunes/{id}
  adopt    [--persona seth]          GET /fine-tunes/{id}/voices -> first id,
                                     back up the persona, set voice.voice_id
                                     (old id kept in voice.previous_voice_id)

Then A/B the new id against the old one with scripts/blind_ab/voice_ab.py
before trusting it (`--model` stays; the persona's voice_id is what changed).

API per docs.cartesia.ai (2026-09): datasets -> files -> fine-tunes -> voices.
"""
import argparse
import json
import mimetypes
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid

HOME = os.path.expanduser("~")
DEFAULT_DIR = os.path.join(HOME, ".human", "pro_clone")
API = "https://api.cartesia.ai"
API_VERSION = "2025-04-16"
MIN_MINUTES = 30.0
AUDIO_EXT = (".wav", ".m4a", ".mp3", ".caf", ".aiff", ".aif", ".flac", ".ogg")
PERSONA_DIR = os.path.join(HOME, ".human", "personas")


# ── pure helpers (unit-tested) ──────────────────────────────────────────

def parse_afinfo(text):
    """Duration (s), channels, sample rate from `afinfo` output; None if absent."""
    dur = re.search(r"estimated duration:\s*([\d.]+)\s*sec", text)
    fmt = re.search(r"Data format:\s*(\d+)\s*ch,\s*(\d+)\s*Hz", text)
    return {
        "duration_sec": float(dur.group(1)) if dur else None,
        "channels": int(fmt.group(1)) if fmt else None,
        "sample_rate": int(fmt.group(2)) if fmt else None,
    }


def collect_inputs(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            for fn in sorted(os.listdir(p)):
                if fn.lower().endswith(AUDIO_EXT):
                    files.append(os.path.join(p, fn))
        elif p.lower().endswith(AUDIO_EXT):
            files.append(p)
    return files


def plan(manifest_items, min_minutes=MIN_MINUTES):
    """Total minutes and whether the set clears the Pro threshold."""
    total = sum(it.get("duration_sec") or 0.0 for it in manifest_items)
    return {"files": len(manifest_items), "minutes": round(total / 60.0, 2),
            "enough": total / 60.0 >= min_minutes, "min_minutes": min_minutes}


def multipart(fields, file_field, filename, data, content_type="audio/wav"):
    """(body_bytes, content_type_header) for one file + text fields."""
    boundary = "----h-uman-" + uuid.uuid4().hex
    parts = []
    for k, v in fields.items():
        parts.append(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n"
                     .encode())
    parts.append((f"--{boundary}\r\nContent-Disposition: form-data; name=\"{file_field}\"; "
                  f"filename=\"{filename}\"\r\nContent-Type: {content_type}\r\n\r\n").encode())
    parts.append(data)
    parts.append(f"\r\n--{boundary}--\r\n".encode())
    return b"".join(parts), f"multipart/form-data; boundary={boundary}"


def fine_tune_body(name, dataset_id, model_id, language, description):
    return {"name": name, "description": description, "language": language,
            "model_id": model_id, "dataset": dataset_id}


def adopt_persona(persona_json, new_voice_id):
    """Return the updated persona dict: voice.voice_id <- new, old kept."""
    p = json.loads(json.dumps(persona_json))
    voice = p.setdefault("voice", {})
    old = voice.get("voice_id")
    if old and old != new_voice_id:
        voice["previous_voice_id"] = old
    voice["voice_id"] = new_voice_id
    voice.setdefault("provider", "cartesia")
    return p


# ── io ──────────────────────────────────────────────────────────────────

def api_key():
    k = os.environ.get("CARTESIA_API_KEY")
    if k:
        return k
    with open(os.path.join(HOME, ".human", "config.json")) as f:
        cfg = json.load(f)
    for prov in cfg.get("providers", []):
        if prov.get("name") == "cartesia" and prov.get("api_key"):
            return prov["api_key"]
    raise SystemExit("no Cartesia key: set CARTESIA_API_KEY or providers.cartesia.api_key")


def http(method, path, body=None, content_type="application/json", key=None):
    data = None
    if body is not None:
        data = body if isinstance(body, (bytes, bytearray)) else json.dumps(body).encode()
    req = urllib.request.Request(API + path, data=data, method=method, headers={
        "X-API-Key": key or api_key(), "Cartesia-Version": API_VERSION,
        "Content-Type": content_type})
    try:
        with urllib.request.urlopen(req, timeout=600) as r:
            raw = r.read()
    except urllib.error.HTTPError as e:
        raise SystemExit(f"{method} {path} -> HTTP {e.code}: {e.read()[:300]!r}")
    return json.loads(raw) if raw else {}


def afinfo(path):
    r = subprocess.run(["afinfo", path], capture_output=True, text=True)
    return parse_afinfo(r.stdout)


def to_wav(src, dst):
    r = subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@44100", "-c", "1", src, "-o", dst],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"afconvert failed on {src}: {r.stderr.strip()[:200]}")


# ── commands ────────────────────────────────────────────────────────────

def cmd_prepare(a):
    files = collect_inputs(a.inputs)
    if not files:
        raise SystemExit("no audio files found")
    wav_dir = os.path.join(a.dir, "wav")
    os.makedirs(wav_dir, exist_ok=True)
    items = []
    for i, src in enumerate(files, 1):
        info = afinfo(src)
        dst = os.path.join(wav_dir, f"{i:03d}_{os.path.splitext(os.path.basename(src))[0]}.wav")
        to_wav(src, dst)
        out = afinfo(dst)
        items.append({"source": src, "wav": dst, "duration_sec": out["duration_sec"],
                      "source_channels": info["channels"], "source_rate": info["sample_rate"]})
        print(f"{os.path.basename(src)}: {out['duration_sec'] or 0:.1f}s -> {os.path.basename(dst)}")
    summary = plan(items)
    with open(os.path.join(a.dir, "manifest.json"), "w") as f:
        json.dump({"items": items, "summary": summary, "prepared_at": int(time.time())}, f, indent=1)
    print(f"{summary['files']} files, {summary['minutes']} min "
          f"({'enough' if summary['enough'] else 'NOT enough'} for Pro; need {MIN_MINUTES:.0f})")
    return 0 if (summary["enough"] or a.allow_short) else 2


def cmd_submit(a):
    with open(os.path.join(a.dir, "manifest.json")) as f:
        man = json.load(f)
    if not man["summary"]["enough"] and not a.allow_short:
        raise SystemExit("manifest is under 30 minutes; re-run prepare with more audio or --allow-short")
    key = api_key()
    ds = http("POST", "/datasets", {"name": a.name, "description": a.description}, key=key)
    ds_id = ds.get("id") or ds.get("dataset_id")
    if not ds_id:
        raise SystemExit(f"dataset create returned no id: {ds}")
    print(f"dataset {ds_id}")
    for it in man["items"]:
        with open(it["wav"], "rb") as f:
            data = f.read()
        body, ctype = multipart({"purpose": "fine_tune"}, "file", os.path.basename(it["wav"]), data)
        http("POST", f"/datasets/{ds_id}/files", body, content_type=ctype, key=key)
        print(f"  uploaded {os.path.basename(it['wav'])} ({len(data) // 1024} KB)")
    ft = http("POST", "/fine-tunes",
              fine_tune_body(a.name, ds_id, a.model, a.language, a.description), key=key)
    ft_id = ft.get("id")
    if not ft_id:
        raise SystemExit(f"fine-tune create returned no id: {ft}")
    with open(os.path.join(a.dir, "job.json"), "w") as f:
        json.dump({"dataset_id": ds_id, "fine_tune_id": ft_id, "submitted_at": int(time.time()),
                   "name": a.name, "model": a.model}, f, indent=1)
    print(f"fine-tune {ft_id} submitted; poll with: pro_clone.py status --dir {a.dir}")
    return 0


def _job(a):
    with open(os.path.join(a.dir, "job.json")) as f:
        return json.load(f)


def cmd_status(a):
    job = _job(a)
    ft = http("GET", f"/fine-tunes/{job['fine_tune_id']}")
    print(json.dumps({k: ft.get(k) for k in ("id", "status", "name", "created_at")}, indent=1))
    return 0 if ft.get("status") == "completed" else 3


def cmd_adopt(a):
    job = _job(a)
    voices = http("GET", f"/fine-tunes/{job['fine_tune_id']}/voices").get("data") or []
    if not voices:
        raise SystemExit("fine-tune has no voices yet (status not completed?)")
    new_id = voices[0]["id"]
    path = os.path.join(PERSONA_DIR, f"{a.persona}.json")
    with open(path) as f:
        persona = json.load(f)
    backup = f"{path}.bak-pre-pro-clone-{time.strftime('%Y%m%d-%H%M%S')}"
    shutil.copy2(path, backup)
    updated = adopt_persona(persona, new_id)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(updated, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, path)
    with open(os.path.join(a.dir, "job.json"), "w") as f:
        job["adopted_voice_id"] = new_id
        job["adopted_at"] = int(time.time())
        json.dump(job, f, indent=1)
    print(f"persona {a.persona}: voice_id {persona.get('voice', {}).get('voice_id')} -> {new_id} "
          f"(backup {backup}). Restart the daemon, then A/B it with voice_ab.py.")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dir", default=DEFAULT_DIR)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--in", dest="inputs", nargs="+", required=True)
    p.add_argument("--allow-short", action="store_true")
    s = sub.add_parser("submit")
    s.add_argument("--name", default="Seth Pro")
    s.add_argument("--description", default="h-uman persona voice (pro clone)")
    s.add_argument("--model", default="sonic-3.6")
    s.add_argument("--language", default="en")
    s.add_argument("--allow-short", action="store_true")
    sub.add_parser("status")
    ad = sub.add_parser("adopt")
    ad.add_argument("--persona", default="seth")
    a = ap.parse_args(argv)
    os.makedirs(a.dir, exist_ok=True)
    return {"prepare": cmd_prepare, "submit": cmd_submit, "status": cmd_status,
            "adopt": cmd_adopt}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
