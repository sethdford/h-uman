#!/usr/bin/env python3
"""Guard the persona's `voice` / `voice_messages` blocks against silent loss.

Why: on 2026-09-20 h-uman had not sent a voice message for months even though
HU_ENABLE_CARTESIA, the Cartesia key, the "Seth" voice clone, and the iMessage
`voice_enabled` flag were all ON. A persona-refresh pass (between the May 10
`pre-v2-refresh` backup and the September `gutted-by-style-reanalyze` one)
rewrote ~/.human/personas/seth.json without the two blocks, and nothing
noticed because every GLOBAL switch still read ON — the daemon's voice path
gates on the PERSONA (daemon_voice_reply.c: persona->voice.voice_id and
persona->voice_messages.enabled).

What it does:
  * When the persona HAS both blocks: snapshot them next to the persona
    (.voice-guard-snapshot.json). Silent, exit 0.
  * When the persona LACKS them and a snapshot exists: back the persona up,
    re-inject the snapshot blocks, write atomically, report on stderr, exit 2
    (so a Claude Code PostToolUse hook surfaces it as feedback).
  * `--check`: report only, never write.
  * `--selftest`: exercise all branches in a temp dir.

Wired as a PostToolUse hook (Write|Edit|Bash) in .claude/settings.json.
"""
import argparse
import json
import os
import shutil
import sys
import tempfile
import time

DEFAULT_PERSONA = "~/.human/personas/seth.json"
SNAPSHOT_NAME = ".voice-guard-snapshot.json"
REQUIRED = ("voice", "voice_messages")


def has_voice_blocks(p):
    v = p.get("voice")
    vm = p.get("voice_messages")
    return (isinstance(v, dict) and bool(v.get("voice_id"))
            and isinstance(vm, dict) and "enabled" in vm)


def atomic_write_json(path, obj):
    d = os.path.dirname(path) or "."
    fd, tmp = tempfile.mkstemp(prefix=".guard-", dir=d)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(obj, f, indent=2, ensure_ascii=False)
            f.write("\n")
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def guard(persona_path, snapshot_path, check=False, err=sys.stderr):
    """Returns exit code: 0 healthy/snapshotted, 2 missing (repaired or not)."""
    if not os.path.exists(persona_path):
        return 0  # nothing to guard on this machine
    try:
        with open(persona_path, encoding="utf-8") as f:
            persona = json.load(f)
    except (OSError, ValueError) as e:
        print(f"PERSONA VOICE GUARD: {persona_path} unreadable ({e}); not touching it", file=err)
        return 2

    if has_voice_blocks(persona):
        snap = {k: persona[k] for k in REQUIRED}
        old = None
        if os.path.exists(snapshot_path):
            try:
                with open(snapshot_path, encoding="utf-8") as f:
                    old = {k: json.load(f).get(k) for k in REQUIRED}
            except (OSError, ValueError):
                old = None
        if old != snap and not check:
            atomic_write_json(snapshot_path, dict(snap, captured_at=int(time.time()),
                                                  source=os.path.abspath(persona_path)))
        return 0

    if not os.path.exists(snapshot_path):
        print(f"PERSONA VOICE GUARD: {persona_path} has no voice/voice_messages blocks and "
              f"no snapshot at {snapshot_path} — voice replies are OFF; restore the blocks "
              f"manually (see docs/voice-clone-setup.md)", file=err)
        return 2

    with open(snapshot_path, encoding="utf-8") as f:
        snap = json.load(f)
    missing = [k for k in REQUIRED if not isinstance(persona.get(k), dict)
               or (k == "voice" and not persona[k].get("voice_id"))
               or (k == "voice_messages" and "enabled" not in persona[k])]
    if check:
        print(f"PERSONA VOICE GUARD: {persona_path} lost {missing}; snapshot available "
              f"(run without --check to repair)", file=err)
        return 2

    backup = f"{persona_path}.bak-voice-guard-{time.strftime('%Y%m%d-%H%M%S')}"
    shutil.copy2(persona_path, backup)
    for k in missing:
        persona[k] = snap[k]
    atomic_write_json(persona_path, persona)
    print(f"PERSONA VOICE GUARD: {persona_path} was written WITHOUT {missing} — restored "
          f"from snapshot (voice_id={snap['voice'].get('voice_id')}, "
          f"frequency={snap['voice_messages'].get('frequency')}). Backup: {backup}. "
          f"Whatever rewrote the persona must preserve these blocks.", file=err)
    return 2


def selftest():
    import io
    good = {"core": {"identity": "x"},
            "voice": {"provider": "cartesia", "voice_id": "abc", "model": "m"},
            "voice_messages": {"enabled": True, "frequency": "rare"}}
    with tempfile.TemporaryDirectory() as d:
        pp = os.path.join(d, "seth.json")
        sp = os.path.join(d, SNAPSHOT_NAME)
        # 1. missing persona → no-op
        assert guard(pp, sp) == 0 and not os.path.exists(sp)
        # 2. healthy persona → snapshot captured, exit 0
        atomic_write_json(pp, good)
        assert guard(pp, sp) == 0 and os.path.exists(sp)
        snap = json.load(open(sp))
        assert snap["voice"]["voice_id"] == "abc" and snap["voice_messages"]["frequency"] == "rare"
        # 3. blocks dropped → --check reports, does not write
        gutted = {"core": {"identity": "y"}}
        atomic_write_json(pp, gutted)
        e = io.StringIO()
        assert guard(pp, sp, check=True, err=e) == 2 and "lost" in e.getvalue()
        assert json.load(open(pp)) == gutted
        # 4. blocks dropped → repair restores both, keeps other edits, makes backup, exit 2
        e = io.StringIO()
        assert guard(pp, sp, err=e) == 2 and "restored" in e.getvalue()
        fixed = json.load(open(pp))
        assert fixed["core"]["identity"] == "y"
        assert fixed["voice"]["voice_id"] == "abc" and fixed["voice_messages"]["enabled"] is True
        assert any(n.startswith("seth.json.bak-voice-guard-") for n in os.listdir(d))
        # 5. voice present but voice_id empty counts as missing → repaired
        atomic_write_json(pp, dict(gutted, voice={"provider": "cartesia", "voice_id": ""}))
        assert guard(pp, sp, err=io.StringIO()) == 2
        assert json.load(open(pp))["voice"]["voice_id"] == "abc"
        # 6. changed healthy blocks update the snapshot (frequency edits survive)
        atomic_write_json(pp, dict(good, voice_messages={"enabled": True, "frequency": "occasional"}))
        assert guard(pp, sp) == 0
        assert json.load(open(sp))["voice_messages"]["frequency"] == "occasional"
        # 7. no snapshot + missing → exit 2, no write
        os.unlink(sp)
        atomic_write_json(pp, gutted)
        e = io.StringIO()
        assert guard(pp, sp, err=e) == 2 and "no snapshot" in e.getvalue()
        assert json.load(open(pp)) == gutted
        # 8. corrupt persona → exit 2, untouched
        with open(pp, "w") as f:
            f.write("{not json")
        e = io.StringIO()
        assert guard(pp, sp, err=e) == 2 and "unreadable" in e.getvalue()
        assert open(pp).read() == "{not json"
    print("selftest OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--persona", default=DEFAULT_PERSONA)
    ap.add_argument("--snapshot", default=None,
                    help=f"default: {SNAPSHOT_NAME} beside the persona")
    ap.add_argument("--check", action="store_true", help="report only, never write")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        return 0
    persona = os.path.expanduser(a.persona)
    snapshot = os.path.expanduser(a.snapshot) if a.snapshot else \
        os.path.join(os.path.dirname(persona), SNAPSHOT_NAME)
    return guard(persona, snapshot, check=a.check)


if __name__ == "__main__":
    sys.exit(main())
