---
title: "Design — Story D: Live LoRA evaluation under HU_ENABLE_LLAMACPP"
sprint: 1
story: D
created: 2026-05-11
status: ready_for_implementation
authored_by: tech-lead
---

# Design for Story D — Live LoRA evaluation under `HU_ENABLE_LLAMACPP`

> **Bottom line up front.** This story should ship as **DESCOPE_OK
> (blocker category B)**. The orchestrator script exists, the baseline
> persona fixture exists, but no provider in the current build can answer
> a single chat call against a GGUF + LoRA adapter pair, so paths (a) and
> (b) both terminate with the orchestrator's exit-2 ("no usable
> responses") code well before any `status.json` with a positive `delta`
> can be produced. Spending an implementer cycle on a real-GGUF download
> would burn 30+ minutes and end with the same exit 2.
>
> The implementer should write the descope rationale, ship the four
> evidence files listed below, and move on.

## 1. Status assessment

For each AC-D.1 variant, REACHABLE / NOT REACHABLE based on the
investigation. Be unsparing.

### Path (a) — real GGUF: **NOT REACHABLE**

Five independent blockers — every one of them on its own is fatal, and
they compound.

| # | Blocker | Evidence |
|---|---|---|
| a1 | `HU_ENABLE_LLAMACPP` is `OFF` by default and no preset flips it on. | `CMakeLists.txt:46` (`option(HU_ENABLE_LLAMACPP ... OFF)`); the `rl_sota` preset referenced in `docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md` is **not present** in `CMakePresets.json` (greppable preset names: `dev`, `test`, `release`, `fuzz`, `minimal`, `minimal-release`, `integration`). |
| a2 | Even with `HU_ENABLE_LLAMACPP=ON` the chat path is a deliberate `HU_ERR_NOT_SUPPORTED` stub. | `src/providers/llamacpp.c:107–139` — the comment says "For now even the linked build is a stub so the binary builds cleanly without the upstream API drift hitting us." |
| a3 | No vendored llama.cpp source tree. | `ls third_party/` → `No such file or directory`. |
| a4 | No system-installed `libllama` available for the `find_package`/pkg-config/manual-prefix discovery chain in `CMakeLists.txt:1607–1660` to find. | `ls /opt/homebrew/include/llama.h /usr/local/include/llama.h /opt/homebrew/lib/libllama* 2>&1` → all not found. `which llama-cli llama-server` → not in PATH. |
| a5 | No GGUF file on disk anywhere a provider could find it. | `find ~/.human/models tests/fixtures -name '*.gguf'` → empty. The only model under `~/.human/models/` is the Gemma `safetensors` shards (~17 GB total) — wrong format for both `huml` (consumes HUML checkpoints) and `llamacpp` (consumes GGUF). |

Even if blockers a1–a4 were waived (vendor llama.cpp, build it, ship a
real chat path), blocker a5 alone forces a 4–5 GB GGUF download. The 30-
minute time budget is broken before `wget` finishes.

There is also a sixth, non-provider blocker that affects paths (a) AND
(b) regardless of whether the chat call succeeds:

| a6 | AC-D.4's schema check is wrong. The orchestrator's `status.json` is produced by `human ml fidelity-status` (`scripts/lora-runner-ab.sh:186–188`). That command emits the JSON shape `{ persona, fingerprint_source, baseline:{scored,mean,min,max}, ab:{available,before_mean,after_mean,delta,scored_before,scored_after} }` (`src/ml/cli.c:2339–2370`). The keys AC-D.4 requires (`delta`, `baseline_score`, `candidate_score`, `run_id` at the **root**) do not exist. `run_id` is not emitted anywhere. So even a hypothetically-successful path (a) run would FAIL the AC-D.4 jq check `jq 'has("delta") and has("baseline_score") and has("candidate_score") and has("run_id")'` → `false`. |

### Path (b) — synthetic GGUF: **NOT REACHABLE**

A synthetic GGUF doesn't help, because the orchestrator never reads the
GGUF directly — it pipes responses through a `hu_provider_t.chat()`
call. The `--adapter <path>` argument flows into
`hu_provider_load_adapter` (`src/providers/helpers.c:148`), which
dispatches to the active provider's `load_adapter` vtable hook:

| Provider | `load_adapter` hook | Outcome with a synthetic GGUF |
|---|---|---|
| `llamacpp` (`src/providers/llamacpp.c:194`) | Returns `HU_ERR_NOT_SUPPORTED` when `HU_LLAMACPP_LINKED=0` (the current state). When linked, calls `llama_adapter_lora_init` which tries to parse the GGUF — a 1-byte fixture would crash the magic-bytes check; a hand-crafted minimal GGUF would still fail tensor-descriptor validation. | Step 2 aborts on adapter-load failure (`src/ml/cli.c:2148–2159`); orchestrator returns the provider's error code without writing `status.json`. |
| `huml` (`src/providers/huml.c:375`) | Loads a HUML LoRA file — **not GGUF**. Has its own header check (`adapter dim mismatch` rejection). | A synthetic GGUF would fail the HUML header check; same abort-without-status-json path. |
| `embedded` (`src/providers/embedded.c:88`) | No `load_adapter` hook (vtable lookup returns `HU_ERR_NOT_SUPPORTED`). Also requires `llama-cli` in PATH (not present). | Step 2 aborts; no `status.json`. |
| Cloud providers (openai, anthropic, …) | The script's own header (`scripts/lora-runner-ab.sh:33–36, 47–49`) explicitly notes cloud providers don't support `load_adapter`. | Step 2 aborts. |

Even if step 2 were somehow waived (e.g., reusing step 1's responses for
both sides), step 1 itself would still fail because there is no provider
in the current build that can answer `chat_with_system` against a local
model:

- `llamacpp.chat_with_system` — `HU_ERR_NOT_SUPPORTED` regardless of link
  state (lines 125–138).
- `huml.chat_with_system` — needs a checkpoint at the path passed via
  `huml.checkpoint_path`. None present (see investigation: only `.bin`
  files under `~/.human/` are tokenizer training data, not HUML
  checkpoints).
- `embedded.chat_with_system` — POSIX-only and shells out to
  `llama-cli`, not in PATH.
- Cloud providers — would work, but then the GGUF/LoRA fixture is
  unused, the adapter load step (step 2) fails on `HU_ERR_NOT_SUPPORTED`,
  and the entire premise of "live LoRA evaluation" collapses to "we
  asked GPT-4 the same prompts twice".

A synthetic GGUF therefore satisfies neither the spirit nor the letter
of AC-D.1(b), and AC-D.4's schema mismatch (a6 above) blocks it even on
a hypothetical success.

### Path (DESCOPE_OK): **REACHABLE**

This is the only viable path. AC-D.1(DESCOPE_OK), AC-D.2, and AC-D.5
are achievable in <15 minutes with no external dependencies.

## 2. Recommended path

**Pursue DESCOPE_OK.** Justification, ranked by weight:

1. **Provider-side stub is intentional and time-bounded by a separate
   plan.** `docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md` is the
   accepted multi-week workstream that turns
   `llamacpp.chat_with_system` from a stub into a real sampling/kv-
   cache/decode loop. Story D was authored knowing this gap exists —
   the DESCOPE_OK branch is precisely the escape valve for "the
   subsystem isn't ready yet".
2. **A real-GGUF download wastes 30+ minutes and ends in the same exit
   2.** Even if the implementer downloaded TinyLlama-1.1B Q4_0
   (~668 MB), there is no provider in the current build to consume it.
   The orchestrator would still abort at step 1 or step 2.
3. **AC-D.4's schema check is a known wart that would block paths (a)
   and (b) regardless.** The implementer would need to either change
   `human ml fidelity-status` to add root-level
   `delta`/`baseline_score`/`candidate_score`/`run_id` (out-of-scope per
   the sprint's "Out of scope" §: *"Any change to the C provider or ML
   training code"*) or pass a hand-crafted JSON, which defeats the
   point.
4. **Story B is the load-bearing customer of this story's success
   path.** Story B (canonical write path) lands the publish hook in
   `lora-runner-ab.sh` such that *whenever* the chat path becomes real,
   the dashboard tile lights up automatically. Story D's value to the
   sprint is precisely "verify or document the gap"; documenting the gap
   is what DESCOPE_OK does.
5. **The descope artifact is itself useful.** A precise, dated rationale
   that names blocker (B) plus the secondary AC-D.4 schema issue gives
   the team a concrete checklist for the next sprint when llama.cpp
   chat lands. That artifact is the deliverable.

## 3. Path (a) — real GGUF: how the implementer would do it (NOT
   recommended; here so the descope rationale can cite it)

If the team later changes its mind and the llama.cpp chat path is real,
this is the cheapest model + acquisition recipe. Documented for
completeness; **do not download as part of this story.**

| Field | Value |
|---|---|
| Model | `tinyllama-1.1b-chat-v1.0.Q4_0.gguf` |
| Size | ~668 MB (Q4_0 quantization) |
| URL | `https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_0.gguf` |
| License | Apache-2.0 (TinyLlama base) — permits redistribution and commercial use; no click-through |
| Tokenizer | LLaMA SentencePiece, embedded in the GGUF |
| Why this one | Smallest license-permissive GGUF that exercises the full sampling loop; widely available across mirrors; commonly used in llama.cpp CI. |
| Where to put it | `~/.human/models/tinyllama-1.1b-chat-v1.0.Q4_0.gguf` (out-of-tree). **Never commit to `tests/fixtures/`** — 668 MB blows past Git LFS economics and the AGENTS.md performance budget. |
| `.gitignore` line | `~/.human/models/` is already outside the repo; no `.gitignore` change needed. |

**Acquisition command** (POSIX):

```bash
mkdir -p ~/.human/models
curl --fail --location --output ~/.human/models/tinyllama-1.1b-chat-v1.0.Q4_0.gguf \
  https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_0.gguf
sha256sum ~/.human/models/tinyllama-1.1b-chat-v1.0.Q4_0.gguf  # record in evidence/D/
```

**LoRA adapter** for the same base would also need to be in GGUF format
(llama.cpp's `llama_adapter_lora_init` rejects PEFT safetensors). The
existing `~/.human/adapters/persona/adapters.safetensors` is HF/PEFT
format and would need conversion via `llama.cpp/convert-lora-to-ggml.py`
— another 5+ minute step.

**Why this isn't recommended for THIS sprint:** even after both
artifacts are in place, the chat path in `src/providers/llamacpp.c` is
still a stub (blocker a2). Until the work in
`docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md` lands, downloading
the GGUF accomplishes nothing. The download step belongs to that plan,
not to this sprint.

## 4. Path (b) — synthetic GGUF: how it would be authored (NOT
   recommended; here so the descope rationale can cite it)

A 1-byte fixture would not survive llama.cpp's GGUF magic-byte check
(four-byte `GGUF` magic at offset 0, GGUF version `uint32`, then a
tensor count and metadata KV table). The minimum bytes-on-disk shape
that even *parses* is the GGUF header + a zero-tensor manifest — about
~80 bytes including a tiny metadata table — and that would be rejected
the moment llama.cpp tries to allocate KV cache against a model with
zero tensors.

Two ways path (b) could in theory satisfy "exists ≥ 1 byte":

- **Stub-provider mode** in `lora-runner-ab.sh`: a `--synthetic` flag
  that short-circuits the actual `human ml lora-runner` invocation and
  emits canned `before.json`/`after.json` files with deterministic
  per-example responses (one set tuned to score 0.4 against the
  synthetic fingerprint, the other tuned to 0.6, giving a reliably
  positive delta of ~0.2). This **sidesteps the provider stack entirely**
  but requires modifying the orchestrator script — and that
  modification does **not** belong to Story D, it belongs to a future
  story explicitly about "synthetic CI gate for the orchestrator".
- **Hand-crafted minimal GGUF generator** at `scripts/gen-synthetic-
  gguf.py`: would emit a file that passes `llama_model_load_from_file`'s
  magic + header parse, then fails at tensor allocation (no real
  weights). Useful for a unit test of the LOADING path, useless for
  testing the CHAT path. Fails AC-D.1(b) because no `status.json` is
  ever produced.

Neither approach delivers a real `status.json` with `delta > 0` from
real chat output. Both are deferred work that should be authored
explicitly, not smuggled into Story D.

## 5. DESCOPE_OK — recommended (this is the path the implementer ships)

### Blocker category: **B**

Per AC-D.5: *"(B) `HU_ENABLE_LLAMACPP` CMake flag is not yet wired to a
real llama.cpp backend"*. This is the precise truth: the flag exists,
the CMake discovery chain exists, but neither a vendored llama.cpp nor a
system libllama is reachable, and the chat-path stub is intentional per
`src/providers/llamacpp.c:125–138` and tracked in
`docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md`.

A secondary independent issue worth recording in the rationale (not a
new descope category — still B):

- AC-D.4's schema check (`jq 'has("delta") and has("baseline_score") and
  has("candidate_score") and has("run_id")'`) does not match the actual
  `fidelity-status` output shape. The implementer cannot fix this within
  the sprint's "out of scope: Any change to the C provider or ML
  training code" guardrail. Fixing AC-D.4 belongs to a follow-up.

### What the descope rationale must contain (AC-D.1(DESCOPE_OK) + AC-D.5)

The rationale doc at `sprints/sprint-1/evidence/D/descope-rationale.md`
must be ≥ 10 lines and contain:

1. **The precise error or missing prerequisite** — quote
   `src/providers/llamacpp.c:5–9, 125–138` (chat_with_system always
   returns `HU_ERR_NOT_SUPPORTED`); cite the absence of vendored
   `third_party/llama.cpp/`, of system libllama, and of any GGUF under
   `tests/fixtures/` or `~/.human/models/`.
2. **The exact command attempted** — show the `cmake --preset dev` build
   plus the orchestrator invocation that produced the failure
   (intentionally trip step 1's empty-response check to capture exit
   code 2 and the first 20 lines of stderr).
3. **The exit code and first 20 lines of stderr** — captured into
   `run-log.txt`.
4. **A recommended follow-up action** — *"Land Phase 2 of
   `docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md` (real
   sampling/kv-cache/decode loop in `llamacpp_chat_with_system`), then
   re-open Story D with a fresh real-GGUF run."*
5. **The blocker category line** — must literally include the string
   `Category B` (or `blocker category: B`) so AC-D.5's
   `grep -E "blocker category|Category [A-C]"` matches.
6. **The AC-D.4 schema mismatch note** — `fidelity-status` emits
   `.ab.delta`, not root `.delta`; emits `.baseline.mean`, not
   `baseline_score`; never emits `run_id`. This blocks paths (a)/(b)
   independent of the chat-path stub.

## 6. Evidence file plan

All under `sprints/sprint-1/evidence/D/`:

| File | Required by | Purpose | Approximate size |
|---|---|---|---|
| `descope-rationale.md` | AC-D.1(DESCOPE_OK), AC-D.5 | The actual descope decision: blocker, command, exit code, first-20-stderr, follow-up. ≥ 10 lines. Must contain `Category B`. | ~60 lines |
| `run-log.txt` | AC-D.2 | Captured stdout+stderr of the orchestrator invocation that demonstrates the failure (exit 2, "no usable provider" message). The descope-rationale.md cites line numbers from this file. | ~50 lines |
| `status.json` | **NOT** created — path (a)/(b) only | n/a in DESCOPE_OK path. | — |
| `build-log.txt` | AC-D.3 | **NOT** strictly required in the DESCOPE_OK path** (AC-D.3 says "if path (a) or (b) is taken"). Still recommended: capture `cmake -DHU_ENABLE_LLAMACPP=ON` configure output to demonstrate "llama.cpp provider: enabled (no vendored upstream and libllama not found …)" status message — this is the strongest single piece of evidence for blocker category B. | ~30 lines |

The `evidence/D/` directory already exists (it was pre-created) and is
empty per the investigation; the implementer just needs to write these
files.

### File-by-file content sketch (for the implementer)

**`run-log.txt`** — produced by literally running the orchestrator with
no provider configured, capturing the exit-2 path:

```bash
mkdir -p sprints/sprint-1/evidence/D
{
  echo "=== command ==="
  echo "HOME=$(pwd)/sprints/sprint-1/evidence/D/sandbox \\"
  echo "  bash scripts/lora-runner-ab.sh \\"
  echo "    --persona lora_baseline_fixture \\"
  echo "    --adapter tests/fixtures/lora_baseline_persona.json \\"
  echo "    --output-dir sprints/sprint-1/evidence/D/sandbox/out"
  echo
  echo "=== output ==="
  mkdir -p sprints/sprint-1/evidence/D/sandbox/.human/personas
  cp tests/fixtures/lora_baseline_persona.json \
     sprints/sprint-1/evidence/D/sandbox/.human/personas/lora_baseline_fixture.json
  HOME="$(pwd)/sprints/sprint-1/evidence/D/sandbox" \
    bash scripts/lora-runner-ab.sh \
      --persona lora_baseline_fixture \
      --adapter tests/fixtures/lora_baseline_persona.json \
      --output-dir "$(pwd)/sprints/sprint-1/evidence/D/sandbox/out" 2>&1 || true
  echo
  echo "=== exit code: $? ==="
} > sprints/sprint-1/evidence/D/run-log.txt
```

(The `--adapter tests/fixtures/lora_baseline_persona.json` is a placeholder
satisfying the `[[ -f "$ADAPTER" ]]` check at `lora-runner-ab.sh:101`;
since step 1 fails with empty responses before adapter load is reached,
the adapter file's contents are irrelevant. Use a small non-empty file.)

Expected content: a `[lora-runner-ab] FAIL: …/before.json has no non-
empty responses — provider unreachable?` line, exit 2.

**`build-log.txt`** — captures the configure-time confirmation that the
flag exists but no real backend is available:

```bash
( cd /tmp && rm -rf hu-llamacpp-probe && mkdir hu-llamacpp-probe && cd hu-llamacpp-probe
  cmake -S /Users/sethford/Documents/h-uman -B . \
    -DHU_ENABLE_LLAMACPP=ON 2>&1 | grep -i 'llama.cpp provider' ) \
  > sprints/sprint-1/evidence/D/build-log.txt
```

Expected exact line: `-- llama.cpp provider: enabled (no vendored
upstream and libllama not found; vtable will return NOT_SUPPORTED until
libllama is linked)` — direct quote from `CMakeLists.txt:1657`. This is
the smoking gun for blocker category B.

**`descope-rationale.md`** — written by the implementer using the
template in §5 above. Must literally contain `Category B`. Must include
the AC-D.4 schema mismatch note. Must reference both `build-log.txt`
and `run-log.txt`.

## 7. Risk + alternates

| Scenario | Probability | Impact | Mitigation / fallback |
|---|---|---|---|
| Implementer attempts path (a) anyway, downloads TinyLlama, hits the chat-stub. | Low (this design names the trap explicitly) | 30 min wasted. | Design says **DO NOT download**. Verifier checks: if any GGUF appears under `~/.human/models/` or `tests/fixtures/` after the implementer ran, push back. |
| Implementer attempts path (b), hand-crafts a minimal GGUF, hits the same chat-stub. | Low | 60+ min wasted; risk of committing a binary fixture nobody can re-generate. | Design says **DO NOT author a synthetic GGUF**. The synthetic-CI-gate idea is for a future story. |
| `cmake -DHU_ENABLE_LLAMACPP=ON` configure fails on the implementer's machine for an unrelated reason. | Low | `build-log.txt` is empty/missing. | The `build-log.txt` capture line is wrapped so failure doesn't kill the script; falls back to plain `cmake -DHU_ENABLE_LLAMACPP=ON ..` output. The descope-rationale's blocker statement does not depend on the configure succeeding — the source-code comments at `src/providers/llamacpp.c:5–9, 125–138` are sufficient evidence. |
| `~/.human/last_fidelity_ab.json` is accidentally written by the run-log.txt capture (Story B's write path lands first and fires). | Low | Pollutes the user's real `~/.human/`. | The run-log.txt capture sets `HOME=$(pwd)/sprints/sprint-1/evidence/D/sandbox`, isolating any write. After Story B lands, also pass `--no-publish` (Story B adds this flag). If Story B isn't merged first, no publish happens — original script doesn't write the canonical file. |
| AC-D.4 schema mismatch is interpreted by the verifier as a Story D bug. | Medium | Sprint review pushback. | The rationale calls this out explicitly as a separate, documented issue with a follow-up recommendation. The DESCOPE_OK ACs (D.1(DESCOPE_OK), D.2, D.5) do not depend on AC-D.4. |

### Fallback if the design's recommendation is rejected

If sprint leadership insists on path (a) despite the chat-stub, the
fallback is a **two-PR plan**:

1. PR-D-prep: vendor `third_party/llama.cpp/` at a pinned tag (the plan
   doc names the b3000+ API as the target), add `rl_sota` preset, leave
   chat as a stub.
2. PR-D-real: implement the real `llamacpp_chat_with_system`
   sampling/decode loop per
   `docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md` Tasks 5–8.

Both PRs together exceed any single-sprint budget. The "two PR" name is
just a trace; the actual scope is Phases 1–4 of that plan, sized at
multiple weeks. This is the explicit reason DESCOPE_OK exists in this
story.

## 8. Time budget estimate

| Path | Steps | Wall-clock estimate | Outcome |
|---|---|---|---|
| **DESCOPE_OK (recommended)** | Run the no-provider orchestrator capture (≈30 s); run the cmake configure capture (≈45 s); author `descope-rationale.md` (~10 min); commit (~2 min). | **~15 minutes.** | All four ACs (D.1(DESCOPE_OK), D.2, D.5; D.3/D.4 marked N/A in the path) PASS. |
| Path (a) — real GGUF | Verify `wget`/`curl` available (1 min); download TinyLlama Q4_0 668 MB at ~50 MB/s real bandwidth (~13 min); convert PEFT adapter to GGUF (5–10 min, requires Python + llama.cpp checkout); discover blocker a2 chat-stub (5 min); revert to DESCOPE_OK (15 min). | **~40 minutes wasted, then ship DESCOPE_OK anyway.** | Path (a) ACs FAIL on chat-stub. AC-D.4 also FAILS on schema mismatch. |
| Path (b) — synthetic GGUF | Author `scripts/gen-synthetic-gguf.py` (~20 min); run it (1 min); hit provider stub same as path (a) (5 min); revert to DESCOPE_OK (15 min). | **~45 minutes wasted, then ship DESCOPE_OK.** | Path (b) ACs FAIL on provider stack. AC-D.4 also FAILS. |

**Hard cap.** Per the prompt, the live path budget is 30 min; both (a)
and (b) blow past it. DESCOPE_OK fits inside the budget with margin.

## Acceptance criteria mapping

| AC | Status under recommended path | Evidence the verifier captures |
|---|---|---|
| AC-D.1(a) — real GGUF | **N/A** (not pursued; rationale documents why) | `descope-rationale.md` blocker category B explanation |
| AC-D.1(b) — synthetic GGUF | **N/A** (not pursued; rationale documents why) | `descope-rationale.md` provider-stack-mismatch explanation |
| AC-D.1(DESCOPE_OK) | **PASS** | `wc -l sprints/sprint-1/evidence/D/descope-rationale.md \| awk '$1 >= 10'` prints a line count ≥ 10 |
| AC-D.2 | **PASS** | `ls sprints/sprint-1/evidence/D/` lists `descope-rationale.md`, `run-log.txt`, `build-log.txt` |
| AC-D.3 | **N/A** (path (a)/(b) not pursued) | n/a — `build-log.txt` is included anyway as supporting evidence for the descope, not as an AC-D.3 PASS claim |
| AC-D.4 | **N/A** (no `status.json` produced) | n/a; the rationale documents the schema mismatch that would have blocked (a)/(b) anyway |
| AC-D.5 | **PASS** | `grep -E "blocker category\|Category [A-C]" sprints/sprint-1/evidence/D/descope-rationale.md` returns the `Category B` line |

`RESULT_tech-lead=READY`
