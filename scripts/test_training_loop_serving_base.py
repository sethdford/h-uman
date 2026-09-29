#!/usr/bin/env python3
"""
C3 serving-base resolution verifier — pins training_loop.py's dynamic
base-model targeting (2026-07-26).

Production :8741 flipped to GLM-4.5-Air-4bit on 2026-07-26 while the C3
train-from-outcomes path hardcoded the gemma base — every auto-trained
adapter was un-loadable dead weight (gemma-shaped LoRA cannot bind to
GLM). These tests pin the fix:

  1. resolve_serving_base_model precedence: explicit --model-id override
     > live mlx-server process --model > config.json mlx_local.model
     > hardcoded gemma default. Malformed ps lines are skipped.
  2. resolve_serving_adapter: live process --adapter-path first (must
     exist on disk), config.json personalization.lora_adapter_path
     fallback, (None, reason) when unresolvable.
  3. base_model_tag + suffix_adapter_name: adapter names carry the base
     (auto-<ts>-glm vs auto-<ts>-gemma) so registry/promotion tooling
     can refuse cross-base swaps; suffixing is idempotent.
  4. training_config_for_model: nested lora_parameters form (the flat
     lora_scale/lora_rank/lora_alpha keys were silently IGNORED by
     mlx_lm — scale fell back to the catastrophic 20.0 default); GLM
     recipe parity with the proven glm-v5-config.yaml.

All tests are OFFLINE: the server probe is injected via ps_output and
config_path — no live server, no subprocess, no network.

Run:
  python3 scripts/test_training_loop_serving_base.py

Exit codes:
  0 — all assertions passed
  1 — at least one failure (printed inline)
"""
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TRAINING_LOOP = REPO_ROOT / "scripts" / "training_loop.py"


def _load_module():
    """Load training_loop.py as a module without invoking main()."""
    spec = importlib.util.spec_from_file_location("training_loop", TRAINING_LOOP)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


tl = _load_module()

_PASS = 0
_FAIL = 0

GLM = "mlx-community/GLM-4.5-Air-4bit"
GEMMA = "mlx-community/gemma-4-31b-it-4bit"

PS_GLM = (
    "  501 12345 python3 mlx-server.py --model mlx-community/GLM-4.5-Air-4bit "
    "--adapter-path /tmp/does-not-matter --port 8741\n"
)


def _ok(label: str, cond: bool) -> None:
    global _PASS, _FAIL
    if cond:
        _PASS += 1
        print(f"  PASS  {label}")
    else:
        _FAIL += 1
        print(f"  FAIL  {label}")


def _write_config(tmpdir: Path, payload: dict) -> Path:
    path = tmpdir / "config.json"
    path.write_text(json.dumps(payload))
    return path


def test_resolution_precedence():
    print("\n=== resolve_serving_base_model precedence ===")
    with tempfile.TemporaryDirectory() as td:
        tmpdir = Path(td)
        config = _write_config(tmpdir, {"mlx_local": {"model": "config-model"}})

        # 1. Explicit override wins over a live server AND config.
        model, source = tl.resolve_serving_base_model(
            ps_output=PS_GLM, config_path=config, override="override-model")
        _ok("override wins", model == "override-model")
        _ok("override source labelled", "explicit --model-id" in source)

        # 2. Live process wins over config.
        model, source = tl.resolve_serving_base_model(
            ps_output=PS_GLM, config_path=config)
        _ok("live process wins over config", model == GLM)
        _ok("process source labelled", "live mlx-server" in source)

        # 3. Config fallback when no server is running.
        model, source = tl.resolve_serving_base_model(
            ps_output="", config_path=config)
        _ok("config fallback", model == "config-model")
        _ok("config source labelled", "mlx_local.model" in source)

        # 4. Hardcoded default when neither exists.
        model, source = tl.resolve_serving_base_model(
            ps_output="", config_path=tmpdir / "missing.json")
        _ok("default fallback is gemma", model == tl.DEFAULT_BASE_MODEL)
        _ok("default source labelled", "hardcoded default" in source)

        # 5. Malformed ps line (--model with no value) falls through to config.
        bad_ps = "python3 mlx-server.py --port 8741 --model\n"
        model, _ = tl.resolve_serving_base_model(
            ps_output=bad_ps, config_path=config)
        _ok("malformed ps line skipped", model == "config-model")

        # 6. Unrelated processes mentioning --model don't match.
        noise_ps = "python3 other-server.py --model something-else\n"
        model, _ = tl.resolve_serving_base_model(
            ps_output=noise_ps, config_path=config)
        _ok("non-mlx-server process ignored", model == "config-model")

        # 7. Malformed config JSON falls through to default.
        broken = tmpdir / "broken.json"
        broken.write_text("{not json")
        model, _ = tl.resolve_serving_base_model(
            ps_output="", config_path=broken)
        _ok("broken config falls to default", model == tl.DEFAULT_BASE_MODEL)


def test_multi_server_port_filter():
    """Regression: observed live 2026-07-26 — a gemma-8bit realtime server
    on :8747 was listed by `ps` BEFORE the production GLM server on :8741.
    First-match resolution trained the wrong base. Resolution must filter
    by the production port (the daemon's hot-swap target)."""
    print("\n=== multi-server port filter ===")
    two_servers = (
        f"python3 mlx-server.py --model {GEMMA} --port 8747 --realtime "
        f"--adapter-path /tmp/gemma-adapter\n"
        f"python3 mlx-server.py --model {GLM} --port 8741 --realtime "
        f"--adapter-path /tmp/glm-adapter\n"
    )
    with tempfile.TemporaryDirectory() as td:
        tmpdir = Path(td)
        missing = tmpdir / "missing.json"

        model, source = tl.resolve_serving_base_model(
            ps_output=two_servers, config_path=missing)
        _ok("production :8741 wins over ps-first :8747", model == GLM)
        _ok("source names the port", "8741" in source)

        # A server line with NO --port flag counts as the default 8741.
        no_port = f"python3 mlx-server.py --model {GLM} --realtime\n"
        model, _ = tl.resolve_serving_base_model(
            ps_output=no_port, config_path=missing)
        _ok("no --port flag treated as default 8741", model == GLM)

        # Only the non-production server running -> fall through to config.
        spare_only = f"python3 mlx-server.py --model {GEMMA} --port 8747\n"
        config = _write_config(tmpdir, {"mlx_local": {"model": GLM}})
        model, source = tl.resolve_serving_base_model(
            ps_output=spare_only, config_path=config)
        _ok("non-production server ignored, config wins", model == GLM)
        _ok("fell through to config source", "mlx_local.model" in source)

        # Adapter resolution applies the same filter.
        glm_adapter = tmpdir / "glm-adapter"
        glm_adapter.mkdir()
        gemma_adapter = tmpdir / "gemma-adapter"
        gemma_adapter.mkdir()
        both = (
            f"python3 mlx-server.py --model {GEMMA} --port 8747 "
            f"--adapter-path {gemma_adapter}\n"
            f"python3 mlx-server.py --model {GLM} --port 8741 "
            f"--adapter-path {glm_adapter}\n"
        )
        adapter, _ = tl.resolve_serving_adapter(
            ps_output=both, config_path=missing)
        _ok("adapter from production port", adapter == glm_adapter)

    # HU_MLX_BASE_URL overrides the production port (mirrors
    # lora_training_runner.c resolve_mlx_base_url).
    _ok("default production port is 8741",
        tl.production_mlx_port(env={}) == "8741")
    _ok("HU_MLX_BASE_URL port honored",
        tl.production_mlx_port(
            env={"HU_MLX_BASE_URL": "http://127.0.0.1:8743/v1"}) == "8743")


def test_resolve_serving_adapter():
    print("\n=== resolve_serving_adapter ===")
    with tempfile.TemporaryDirectory() as td:
        tmpdir = Path(td)
        live_adapter = tmpdir / "live-adapter"
        live_adapter.mkdir()
        config_adapter = tmpdir / "config-adapter"
        config_adapter.mkdir()
        config = _write_config(
            tmpdir, {"personalization": {"lora_adapter_path": str(config_adapter)}})

        # 1. Live process --adapter-path wins when it exists on disk.
        ps = f"python3 mlx-server.py --model {GLM} --adapter-path {live_adapter}\n"
        adapter, source = tl.resolve_serving_adapter(
            ps_output=ps, config_path=config)
        _ok("live adapter wins", adapter == live_adapter)
        _ok("live adapter source labelled", "live mlx-server" in source)

        # 2. Nonexistent process path falls through to config.
        ps = f"python3 mlx-server.py --model {GLM} --adapter-path {tmpdir}/gone\n"
        adapter, source = tl.resolve_serving_adapter(
            ps_output=ps, config_path=config)
        _ok("dead process path falls to config", adapter == config_adapter)
        _ok("config adapter source labelled",
            "personalization.lora_adapter_path" in source)

        # 3. Nothing resolvable -> (None, reason).
        adapter, source = tl.resolve_serving_adapter(
            ps_output="", config_path=tmpdir / "missing.json")
        _ok("unresolvable returns None", adapter is None)
        _ok("unresolvable has reason", len(source) > 0)


def test_base_model_tag_and_suffix():
    print("\n=== base_model_tag + suffix_adapter_name ===")
    _ok("GLM tag", tl.base_model_tag(GLM) == "glm")
    _ok("gemma tag", tl.base_model_tag(GEMMA) == "gemma")
    _ok("unknown base sanitized",
        tl.base_model_tag("mlx-community/Qwen3-30B-A3B-4bit") == "qwen3-30b-a3b-4bit")

    out = Path("/tmp/adapters/auto-1785053731")
    _ok("glm suffix applied",
        tl.suffix_adapter_name(out, "glm").name == "auto-1785053731-glm")
    _ok("gemma suffix applied",
        tl.suffix_adapter_name(out, "gemma").name == "auto-1785053731-gemma")
    already = Path("/tmp/adapters/auto-1785053731-glm")
    _ok("suffix idempotent",
        tl.suffix_adapter_name(already, "glm") == already)
    _ok("suffix stays in same parent",
        tl.suffix_adapter_name(out, "glm").parent == out.parent)


def test_training_config_selection():
    print("\n=== training_config_for_model ===")
    glm_cfg = tl.training_config_for_model(GLM, iters=500, scale=2.0)
    gemma_cfg = tl.training_config_for_model(GEMMA, iters=500, scale=2.0)

    for name, cfg in (("glm", glm_cfg), ("gemma", gemma_cfg)):
        # The flat keys were silently ignored by mlx_lm -> scale 20.0
        # (adapter auto-manual-1785053731-INVALID-scale20-gemma). Only the
        # nested lora_parameters form is honored.
        _ok(f"{name}: no ignored flat lora keys",
            not any(k in cfg for k in ("lora_scale", "lora_rank", "lora_alpha")))
        _ok(f"{name}: nested lora_parameters present", "lora_parameters" in cfg)
        _ok(f"{name}: scale pinned to 2.0",
            cfg["lora_parameters"]["scale"] == 2.0)
        _ok(f"{name}: rank 8", cfg["lora_parameters"]["rank"] == 8)
        _ok(f"{name}: num_layers 8", cfg["num_layers"] == 8)
        _ok(f"{name}: batch_size 1", cfg["batch_size"] == 1)
        _ok(f"{name}: lr 1e-5", cfg["learning_rate"] == 1e-5)
        _ok(f"{name}: max_seq_length 2048", cfg["max_seq_length"] == 2048)
        _ok(f"{name}: model recorded", cfg["model"] in (GLM, GEMMA))

    # GLM recipe parity with the proven glm-v5-config.yaml
    # (seth-glm-air-v5-20260725-093742: val 7.08->2.94, peak 62.9 GB).
    _ok("glm: grad_checkpoint on", glm_cfg.get("grad_checkpoint") is True)
    _ok("glm: optimizer adam", glm_cfg["optimizer"] == "adam")
    _ok("glm: seed pinned", glm_cfg.get("seed") == 42)
    _ok("glm: steps_per_report 10", glm_cfg["steps_per_report"] == 10)

    # Gemma keeps the original recipe (no GLM-only knobs leak across).
    _ok("gemma: no grad_checkpoint", "grad_checkpoint" not in gemma_cfg)
    _ok("gemma: optimizer adamw", gemma_cfg["optimizer"] == "adamw")
    _ok("gemma: no seed override", "seed" not in gemma_cfg)


def test_real_path_suffix_and_symlink():
    """End-to-end (offline) contract of the real-training path: the adapter
    dir gets the base suffix, a compat symlink preserves the C dispatcher's
    hot-swap path (<requested>/adapters.safetensors), and lineage/registry
    record the resolved base. Training + all real-world sinks are stubbed."""
    print("\n=== real path: suffix + compat symlink ===")
    import sqlite3
    from types import SimpleNamespace

    with tempfile.TemporaryDirectory() as td:
        tmpdir = Path(td)

        # Fixture DB + outcomes JSONL. TWO resolvable user/assistant pairs:
        # this test is about base-suffix/symlink routing, not batch size, but
        # train_from_outcomes now refuses anything under
        # MIN_TRAINABLE_OUTCOMES (a 1-outcome batch cannot be split into
        # train/valid, so its verdict is INCONCLUSIVE before it runs).
        db_path = tmpdir / "memory.db"
        conn = sqlite3.connect(str(db_path))
        conn.execute("CREATE TABLE messages (id INTEGER PRIMARY KEY, "
                     "session_id TEXT, role TEXT, content BLOB, created_at INTEGER)")
        pairs = [("you up?", "yeah what's good"),
                 ("still on for tomorrow?", "yep 10am works")]
        for i, (p, r) in enumerate(pairs):
            conn.execute("INSERT INTO messages VALUES (?,'s','user',?,?)",
                         (i * 2 + 1, p, i * 2 + 1))
            conn.execute("INSERT INTO messages VALUES (?,'s','assistant',?,?)",
                         (i * 2 + 2, r, i * 2 + 2))
        conn.commit()
        conn.close()
        jsonl = tmpdir / "outcomes.jsonl"
        jsonl.write_text("".join(json.dumps({
            "t": i + 1, "l": 10, "pt": 5, "ct": 5, "m": 1, "a": 1, "g": 0,
            "ph": tl.fnv1a_64(p.encode()),
            "rh": tl.fnv1a_64(r.encode()),
        }) + "\n" for i, (p, r) in enumerate(pairs)))

        requested_out = tmpdir / "auto-42"

        # Stub every real-world sink + the trainer itself.
        lineage: list[dict] = []
        registry: list[dict] = []
        saved = {k: getattr(tl, k) for k in (
            "resolve_serving_base_model", "resolve_serving_adapter",
            "run_mlx_lora_training", "append_lineage_entry",
            "dpo_results", "adapter_registry")}

        def stub_train(resolved, adapter_out, iters=500, scale=2.0, model=None):
            adapter_out.mkdir(parents=True, exist_ok=True)
            (adapter_out / "adapters.safetensors").write_bytes(b"stub")
            stub_train.calls.append({"adapter_out": adapter_out, "model": model,
                                     "scale": scale})
            return 0, 1.0, 2.0
        stub_train.calls = []

        try:
            tl.resolve_serving_base_model = (
                lambda **kw: (GLM, "test-injected"))
            tl.resolve_serving_adapter = (
                lambda **kw: (tmpdir / "serving-ref", "test-injected"))
            tl.run_mlx_lora_training = stub_train
            tl.append_lineage_entry = lineage.append
            tl.dpo_results = SimpleNamespace(
                append_result=lambda *a, **kw: None,
                load_recent=lambda *a, **kw: [],
                regression_verdict=lambda *a, **kw: "PASS",
                get_git_commit=lambda: "test",
                parse_mlx_losses=lambda out: (None, None),
            )
            tl.adapter_registry = SimpleNamespace(
                record_training=lambda **kw: registry.append(kw))

            rc = tl.train_from_outcomes(jsonl, requested_out, db_path,
                                        dry_run=False)
        finally:
            for k, v in saved.items():
                setattr(tl, k, v)

        suffixed = tmpdir / "auto-42-glm"
        _ok("returns 0", rc == 0)
        _ok("trained into suffixed dir",
            stub_train.calls and stub_train.calls[0]["adapter_out"] == suffixed)
        _ok("trainer got the resolved base",
            stub_train.calls[0]["model"] == GLM)
        _ok("trainer got scale 2.0", stub_train.calls[0]["scale"] == 2.0)
        _ok("suffixed adapter exists",
            (suffixed / "adapters.safetensors").exists())
        _ok("compat symlink at requested path", requested_out.is_symlink())
        # THE C contract: lora_training_runner.c hot-swaps this exact path.
        _ok("C hot-swap path resolves",
            (requested_out / "adapters.safetensors").exists())
        _ok("lineage records resolved model",
            lineage and lineage[-1].get("model") == GLM)
        _ok("lineage records base tag", lineage[-1].get("base_tag") == "glm")
        _ok("lineage adapter_path is suffixed",
            lineage[-1].get("adapter_path", "").endswith("-glm"))
        _ok("registry adapter carries base tag",
            registry and registry[0]["adapter_id"].endswith("-glm"))
        _ok("registry metrics record base model",
            registry[0]["metrics"].get("base_model") == GLM)


def main():
    print("C3 serving-base resolution verifier")
    test_resolution_precedence()
    test_multi_server_port_filter()
    test_resolve_serving_adapter()
    test_base_model_tag_and_suffix()
    test_training_config_selection()
    test_real_path_suffix_and_symlink()
    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
