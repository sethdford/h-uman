"""Hermetic tests for scripts/datasets/fetch_*.sh.

No network: every fetch is pointed at a file:// URL in tmp_path through the
test-only overrides HU_FETCH_TEST_URL / HU_FETCH_TEST_SHA256, and the target
directory is HU_DATASETS_DIR=tmp_path — never ~/.human.
"""
import hashlib
import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / "scripts" / "datasets"


def _run(script, env_extra, tmp_path):
    env = {k: v for k, v in os.environ.items() if not k.startswith("HU_")}
    env.update({"HU_DATASETS_DIR": str(tmp_path / "datasets")}, **env_extra)
    return subprocess.run(["bash", str(SCRIPTS / script)], env=env,
                          capture_output=True, text=True)


def _src(tmp_path, name, data):
    p = tmp_path / "upstream" / name
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
    return p, hashlib.sha256(data).hexdigest()


def test_pinned_constants_are_real_sha256_and_revisions():
    for script in ("fetch_locomo.sh", "fetch_msc.sh", "fetch_soc2508.sh"):
        text = (SCRIPTS / script).read_text()
        assert "PINNED_SHA256=" in text and "PINNED_REVISION=" in text, script
        sha = text.split("PINNED_SHA256=")[1].split()[0].strip('"')
        assert len(sha) == 64 and all(c in "0123456789abcdef" for c in sha), script


def test_locomo_fetch_verifies_writes_provenance_and_is_idempotent(tmp_path):
    src, sha = _src(tmp_path, "locomo10.json", b'[{"sample_id": "x"}]')
    env = {"HU_FETCH_TEST_URL": src.as_uri(), "HU_FETCH_TEST_SHA256": sha}
    r = _run("fetch_locomo.sh", env, tmp_path)
    assert r.returncode == 0, r.stderr
    dest = tmp_path / "datasets" / "locomo"
    assert (dest / "locomo10.json").read_bytes() == src.read_bytes()
    prov = (dest / "PROVENANCE").read_text()
    assert sha in prov and "CC BY-NC 4.0" in prov
    src.unlink()  # a second run must not need the source at all
    r2 = _run("fetch_locomo.sh", env, tmp_path)
    assert r2.returncode == 0 and "already present" in r2.stdout


def test_checksum_mismatch_refuses_and_writes_nothing(tmp_path):
    src, _ = _src(tmp_path, "locomo10.json", b"tampered")
    env = {"HU_FETCH_TEST_URL": src.as_uri(), "HU_FETCH_TEST_SHA256": "0" * 64}
    r = _run("fetch_locomo.sh", env, tmp_path)
    assert r.returncode == 2 and "checksum mismatch" in r.stderr
    assert not (tmp_path / "datasets" / "locomo" / "locomo10.json").exists()


def test_corrupted_local_copy_is_refetched(tmp_path):
    src, sha = _src(tmp_path, "locomo10.json", b'[{"good": 1}]')
    dest = tmp_path / "datasets" / "locomo"
    dest.mkdir(parents=True)
    (dest / "locomo10.json").write_bytes(b"bitrot")
    env = {"HU_FETCH_TEST_URL": src.as_uri(), "HU_FETCH_TEST_SHA256": sha}
    r = _run("fetch_locomo.sh", env, tmp_path)
    assert r.returncode == 0, r.stderr
    assert (dest / "locomo10.json").read_bytes() == b'[{"good": 1}]'


def test_msc_fetch_extracts_only_session5(tmp_path):
    import io
    import tarfile
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tf:
        for name, data in [("msc/msc_dialogue/session_5/valid.txt", b'{"a":1}\n'),
                           ("msc/msc_dialogue/session_5/test.txt", b'{"a":2}\n'),
                           ("msc/msc_dialogue/session_2/train.txt", b'{"big":1}\n')]:
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            tf.addfile(ti, io.BytesIO(data))
    src, sha = _src(tmp_path, "msc_v0.1.tar.gz", buf.getvalue())
    env = {"HU_FETCH_TEST_URL": src.as_uri(), "HU_FETCH_TEST_SHA256": sha}
    r = _run("fetch_msc.sh", env, tmp_path)
    assert r.returncode == 0, r.stderr
    dest = tmp_path / "datasets" / "msc"
    assert (dest / "session_5" / "valid.txt").read_text() == '{"a":1}\n'
    assert (dest / "session_5" / "test.txt").exists()
    assert not list(dest.rglob("session_2"))


@pytest.mark.parametrize("script", ["fetch_locomo.sh", "fetch_msc.sh", "fetch_soc2508.sh"])
def test_test_override_needs_both_url_and_sha(script, tmp_path):
    src, _ = _src(tmp_path, "x", b"x")
    r = _run(script, {"HU_FETCH_TEST_URL": src.as_uri()}, tmp_path)
    assert r.returncode == 1 and "HU_FETCH_TEST_SHA256" in r.stderr
