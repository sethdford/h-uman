# Sprint 1 Story D — Descope rationale

**Decision:** Story D ships as **DESCOPE_OK** under blocker
**Category B** (per AC-D.5): *the `HU_ENABLE_LLAMACPP` CMake flag is
not yet wired to a real, end-to-end-functional llama.cpp backend that
the `lora-runner-ab.sh` orchestrator can drive*. Descope-OK is an
explicitly-supported AC-D.1 outcome — the story closes with this
rationale + the captured run/build evidence in this directory.

## Command attempted

```
HOME=$(mktemp -d) bash scripts/lora-runner-ab.sh \
    --persona lora_baseline_fixture \
    --no-publish --keep
```

Exit code observed: **2** (orchestrator's own "empty response set"
guard fires because the live-inference chat path returns
`HU_ERR_NOT_SUPPORTED`, not because of an external network failure).
The first 20 lines of stderr are reproduced in `run-log.txt`.

## Six independent blockers

Each one is fatal on its own. Removing any single blocker is not
sufficient to make the live-LoRA path runnable; the cumulative gap
is what makes Category B the honest answer.

1. **`HU_ENABLE_LLAMACPP=OFF` by default** — `CMakeLists.txt:46`.
   The `rl_sota` CMake preset would enable it, but configure-time
   output (`build-log.txt`) shows the provider banner reads "vtable
   will return NOT_SUPPORTED until libllama is linked", confirming
   the flag is not enough.
2. **Chat path is an intentional `HU_ERR_NOT_SUPPORTED` stub** —
   `src/providers/llamacpp.c:107-139`. Even with libllama linked,
   the `llamacpp_chat_with_system` vtable hook returns
   `HU_ERR_NOT_SUPPORTED` unconditionally. This is *deliberate* —
   the C side has not yet implemented the inference loop — so this
   is the load-bearing blocker, not a configuration issue.
3. **No vendored `third_party/llama.cpp/`** — the Phase-1 RL plan
   (`docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md`) intends to
   vendor it, but at the time of writing this story it is not yet
   on disk in the form the build system can consume.
4. **No system libllama** — `which llama-cli` returns nothing,
   `/opt/homebrew/include/llama.h` does not exist, and
   `/usr/local/lib/libllama*` returns no matches. macOS dev
   environment has no fallback.
5. **No GGUF model on disk** — `find ~/.human/models tests/fixtures
   -name '*.gguf'` returns empty. The only assets in `~/.human/models`
   are HuggingFace safetensors (wrong format for llama.cpp).
6. **AC-D.4 schema mismatch (secondary)** — even if all five above
   were resolved, AC-D.4 demands the `status.json` contain top-level
   `delta`, `baseline_score`, `candidate_score`, `run_id` keys, but
   `human ml fidelity-status` (the actual writer) emits `.ab.delta`,
   `.baseline.mean`, and never emits `run_id`. This is documented as
   a follow-up below.

## Why path (a) was not attempted

A real-GGUF download (TinyLlama 1.1B Q4_0, Apache-2.0, ~668 MB) was
explicitly named "do not download" in design D §3 because blocker
**#2** (the chat path is a `HU_ERR_NOT_SUPPORTED` stub) makes the
download wasted effort — even with the model on disk, the orchestrator
would still exit 2 at step 1.

## Why path (b) was not attempted

A synthetic GGUF doesn't help because `lora-runner-ab.sh` pipes
through `hu_provider_t.chat()` rather than reading the GGUF directly.
Every provider's `load_adapter` hook either returns
`HU_ERR_NOT_SUPPORTED` (cloud, embedded, current llamacpp) or
rejects non-HUML/non-GGUF formats (huml). Path (b) would fail
*upstream* of the synthetic file ever being consumed.

## What the evidence in this directory proves

- `run-log.txt` — captures the orchestrator's behavior on a clean
  sandboxed `HOME`. Exit code 2 at step 1 demonstrates the live
  path is unreachable for reasons internal to the C provider, not
  external to the script.
- `build-log.txt` — captures `cmake -DHU_ENABLE_LLAMACPP=ON`
  configure output. The banner explicitly notes the vtable returns
  NOT_SUPPORTED until libllama is linked.
- `acs.log` — runs the AC-D.1 (DESCOPE_OK), AC-D.2, AC-D.5
  verifier commands against this rationale + the directory state
  and reports PASS for all three.

## AC-D.4 schema mismatch follow-up

The AC-D.4 jq predicate

```
has("delta") and has("baseline_score") and has("candidate_score") and has("run_id")
```

was specified against the *imagined* `status.json` schema, not the
shape `human ml fidelity-status` actually writes
(`src/ml/cli.c:2339-2370`). The actual emission is:

```
{
  "persona": "...",
  "fingerprint_source": "personal_model" | "synthetic",
  "baseline": { "scored": N, "mean": F, "min": F, "max": F },
  "ab": { "available": bool, "before_mean": F, "after_mean": F,
          "delta": F, "scored_before": N, "scored_after": N }
}
```

Two valid resolutions:

a) **Reshape the AC** — accept `.ab.delta` and `.baseline.mean` as
   the canonical schema; drop `run_id` (the gateway has no need for
   one because the file is the canonical "last A/B" snapshot).
b) **Add the missing keys to fidelity-status output** — emit
   top-level `delta`, `baseline_score`, `candidate_score`, `run_id`
   for AC-D.4 compatibility, then update the gateway and tile to
   read either shape.

Recommended: (a) for next sprint — the file is already shaped for
the dashboard tile and adding aliases just to satisfy a verifier
predicate is contortion without value. Captured here so a future
spec author doesn't re-introduce the predicate without reading this.

## Recommended next step

Phase 2 of `docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md`
implements the chat path against vendored llama.cpp. Once that
phase lands, re-attempt this story as path (a) with TinyLlama
1.1B Q4_0 (or the chosen Phase-1 model). Until then, the live
LoRA evaluation cannot prove a non-zero delta because the
inference doesn't actually execute on local hardware.

`Category B blocker confirmed.`
`Category B` is the honest answer.
