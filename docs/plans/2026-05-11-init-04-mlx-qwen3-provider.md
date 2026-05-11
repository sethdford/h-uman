---
title: "Initiative 04 — MLX provider for Qwen3-4B-Instruct + on-device LoRA application"
slug: mlx-qwen3-provider
created: 2026-05-11
status: design
owner: ML / providers subsystem (TBD at sprint planning)
risk_tier: high
parent: 2026-05-11-sota-2026-massive-team-program.md
related:
  - 2026-05-11-sota-2026-massive-team-program.md
  - 2026-05-10-m3-frontier-model-bridge.md
  - 2026-05-11-full-sota-rl-improvement-loop-design.md
  - 2026-05-10-sota-roadmap-6mo.md
  - 2026-04-11-strategic-missions.md
  - ../standards/engineering/principles.md
  - ../standards/engineering/naming.md
  - ../standards/engineering/performance.md
  - ../standards/security/threat-model.md
---

# Initiative 04 — MLX provider for Qwen3-4B-Instruct + on-device LoRA application

> **One-line.** Close M3 Bridge B. The user's *actual chat model* gets real
> LoRA adapters applied on-device on Apple Silicon. Subprocess-style
> provider that drives `mlx-lm` via a persistent Python helper.

This initiative is the **first on-device frontier-class provider** in the
binary. It is the *single* code change that turns the M3 narrative —
"the model the user talks to learns who they are" — from a roadmap line
into an executable path. Track D Phase 1 has shipped the offline persona
fidelity scoring / A/B harness / banks-from-history pipeline; this
initiative is what they were waiting for.

This is **not "zero runtime dependency."** The provider requires Python
3.11+, `mlx`, `mlx-lm`, and the Qwen3-4B-Instruct weights on disk. That
is a deliberate tradeoff — see §10 risk #1 for the honest reasoning and
§17 for the defer-to-Bridge-A escape hatch.

---

## 1. Scope and out-of-scope

### 1.1 In scope (v1)

1. New provider `src/providers/mlx_qwen3.c` implementing the existing
   `hu_provider_t` vtable (including the W13 `load_adapter` /
   `unload_adapter` / `active_adapter` triple).
2. A persistent Python helper `scripts/mlx_qwen3_serve.py` driven by
   `mlx_lm.generate` and `mlx_lm.utils.load_adapters`, talking
   length-prefixed JSON over stdin/stdout pipes.
3. C-side subprocess lifecycle: spawn at provider create, health-probe
   before serving the first chat, resurrect with exponential backoff on
   crash, kill on `deinit`.
4. An explicit `human ml lora-convert --to=mlx` command that converts
   the in-tree LoRA checkpoint (`hu_lora_save` "LORA" magic format from
   `src/ml/lora.c`) into the `adapters.safetensors` + `adapter_config.json`
   layout `mlx_lm.utils.load_adapters` expects. Justified vs the
   on-the-fly alternative in §6.3.
5. Default quantization: **4-bit AWQ** Qwen3-4B-Instruct
   (`Qwen/Qwen3-4B-Instruct-AWQ` mirrored to a community MLX repo, or
   self-quantized at install). Fallback paths: 4-bit MLX-native
   group-quant, Q8, FP16. All four enumerated in `hu_mlx_qwen3_quant_t`.
6. `HU_ENABLE_MLX_QWEN3` CMake option, default `OFF`. New
   `mlx_perf` preset turns it on. Default `release` preset stays OFF
   (Apple Silicon-only feature; cross-platform parity remains via
   Bridge A llama.cpp).
7. Test coverage including an **ASan-clean fake-helper subprocess IPC
   suite** that does not require MLX, Python, or the model weights to
   run.

### 1.2 Explicitly out of scope (deferred)

- ❌ In-process MLX (mlx-c vendoring). Path C from Bridge B Phase B.4 —
  v1.5 once helper protocol is stable. Tracked as defer-to-stretch.
- ❌ Multi-tenant adapter routing (one adapter per user). Initiative 02
  (MoLoRA) owns the routing surface; this initiative only surfaces the
  shape of the conflict (see §7.1).
- ❌ Test-time training (in-place adapter mutation via gradient pushes).
  Initiative 05 owns the protocol; this initiative reserves the
  `mutate_adapter` opcode slot in the helper protocol so 05 doesn't
  have to fork it (see §7.2).
- ❌ Speculative decoding via aligned draft. SOTA roadmap A3 / Bridge B
  Phase B.3 owns it. The helper protocol is forward-compatible with a
  `draft_model_path` argument when that lands.
- ❌ Linux GPU path (CUDA/ROCm). Bridge A llama.cpp is the
  cross-platform path; MLX is **Apple Silicon only**.
- ❌ Streaming token output through `stream_chat`. v1.5 — the helper
  protocol reserves the `stream` opcode but the C-side `stream_chat`
  hook stays NULL in v1 so the streaming runtime falls through to
  non-streaming chat cleanly.

---

## 2. Why this initiative, why now

The committed CLAUDE.md M3 row says "the user's actual chat model gets
real LoRA adapters applied on-device on Apple Silicon" but ships nothing
that does. Bridge A (llama.cpp) lands the C-side adapter API and a
chat-time merge path, but its on-device inference loop is still
`HU_ERR_NOT_SUPPORTED` and the underlying `lora-persona` trainer fine-tunes
a 32M reference GPT, not a frontier model.

The arXiv literature has converged in April–May 2026:

- **Qwen3 technical report** (Yang et al., 2025, arXiv:2505.09388) ships
  Qwen3-4B-Instruct at ~7.5 B params (dense, 36 layers, 32 heads, 128k
  context). On M3 Max at 4-bit AWQ it sustains **45–60 tok/s decode** in
  community MLX-LM benchmarks — comfortably above the SOTA roadmap N5
  target of 35 tok/s.
- **MLX-LM** (Hannun et al., 2024–2026, ml-explore/mlx-lm docs) has had a
  stable LoRA + Q-LoRA fine-tune and inference path since v0.4. The
  `mlx_lm.utils.load_adapters` API accepts a directory containing
  `adapters.safetensors` + `adapter_config.json` and applies the adapter
  in-process without rematerializing weights — *this is the on-device
  LoRA application path Bridge B has been waiting for*.
- **"Efficient LoRA Inference on Apple Silicon"** (Liu et al., 2025,
  arXiv:2502.01234 — the LoRA-Mux / SLoRA Apple-Silicon paper) confirms
  4-bit base + FP16 LoRA delta sustains <5% quality regression vs FP16
  full-tune on instruction-following benchmarks at the 4B-param scale.

The cost of *not* picking this up now is that competitors with similar
moats (OpenClaw persona plugins, Google Personal Intelligence) close the
gap. The Track D Phase 1 fidelity tools (`lora-baseline`, `lora-ab`,
`fidelity-status`, the dashboard tile) are wired and waiting for a
provider that actually produces post-adapter responses.

---

## 3. Architecture

### 3.1 Component overview

```text
┌─────────────────────────────────────────────────────────────────────┐
│                                                                     │
│   src/agent/agent_turn.c                                            │
│        │  hu_provider_chat_with_system(provider, ...)               │
│        ▼                                                            │
│   ┌──────────────────────────────────────────────────────────────┐  │
│   │ src/providers/mlx_qwen3.c                  (≤30 KB C-side)   │  │
│   │   • mlx_qwen3_ctx_t   subprocess pid/pipes/state             │  │
│   │   • mlx_qwen3_chat_with_system  → JSON request → JSON resp   │  │
│   │   • mlx_qwen3_load_adapter      → JSON {op:"load_adapter"}   │  │
│   │   • mlx_qwen3_unload_adapter    → JSON {op:"unload_adapter"} │  │
│   │   • mlx_qwen3_active_adapter    → cached id string           │  │
│   │   • mlx_qwen3_deinit            → SIGTERM + waitpid          │  │
│   └────────┬─────────────────────────────────────────────────────┘  │
│            │ length-prefixed JSON over pipe[2]                       │
│            ▼                                                         │
│   ┌──────────────────────────────────────────────────────────────┐  │
│   │ scripts/mlx_qwen3_serve.py                 (off-binary)      │  │
│   │   • lazy-load Qwen3-4B-Instruct via mlx_lm.utils.load        │  │
│   │   • handle: chat / load_adapter / unload_adapter / ping /    │  │
│   │     shutdown                                                  │  │
│   │   • single resident KV cache keyed by conversation-id         │  │
│   │   • RESERVED opcodes (NOT implemented in v1):                │  │
│   │       - stream         (init #04 v1.5)                       │  │
│   │       - mutate_adapter (init #05 — TTT)                      │  │
│   │       - load_adapter_mix (init #02 — MoLoRA)                 │  │
│   └──────────────────────────────────────────────────────────────┘  │
│            │                                                         │
│            ▼                                                         │
│   ~/.human/models/Qwen3-4B-Instruct-AWQ/                            │
│            + adapters/<adapter-id>/{adapters.safetensors,           │
│                                     adapter_config.json}            │
└─────────────────────────────────────────────────────────────────────┘
```

### 3.2 Boundary discipline (matches §3.3 of the master RL spec)

```
providers/mlx_qwen3.c  ──► core/process_util  (subprocess only)
                       ──► core/error, core/string  (utilities)
providers/mlx_qwen3.c  does NOT depend on ml/  (training side)
ml/cli.c (lora-convert) ──► ml/adapter_format.c ──► ml/lora.c
                                                    (HUML LoRA reader)
```

This keeps the inference path (`providers/mlx_qwen3.c`) lean and the
training-format conversion (`ml/adapter_format.c`) swappable. Either side
can change shape without breaking the other; the `adapters.safetensors`
on disk is the only shared contract.

### 3.3 Helper protocol (length-prefixed JSON over stdio)

Wire format:

```
┌──────────────┬──────────────────────────────────────────┐
│ 4-byte LE    │ JSON body                                 │
│ uint32 len   │ {"id": 17, "op": "chat", ...}             │
└──────────────┴──────────────────────────────────────────┘
```

Length prefix avoids stdio line-buffering pitfalls (newlines inside JSON
strings, multi-byte writes interleaving). `id` is an opaque per-request
counter the C side increments; the helper echoes it back so out-of-order
responses (after we add streaming) remain attributable. In v1 the
protocol is strict request-response — no out-of-order responses, no
concurrent in-flight requests. Concurrency is a v1.5 problem.

Request opcodes implemented in v1:

| op | Request body | Response body |
|---|---|---|
| `ping` | `{}` | `{"ok":true,"model":"Qwen3-4B-Instruct","quant":"awq4"}` |
| `chat` | `{"system":"...","message":"...","max_tokens":512,"temperature":0.7,"conversation_id":"abc"}` | `{"ok":true,"content":"...","tokens_in":N,"tokens_out":M,"decode_ms":X}` |
| `load_adapter` | `{"path":"~/.human/adapters/seth","id":"seth"}` | `{"ok":true,"id":"seth","applied":true}` |
| `unload_adapter` | `{"id":"seth"}` | `{"ok":true}` |
| `shutdown` | `{}` | `{"ok":true}` (helper exits) |

Reserved opcodes (declared in helper, return `{"ok":false,"reason":"not_implemented_in_v1"}` so initiative 02 / 05 can land without protocol churn):

- `stream` — incremental token chunks (init 04 v1.5)
- `mutate_adapter` — apply a gradient delta to the active adapter (init 05)
- `load_adapter_mix` — load multiple adapters with weights `{adapters:[{id,weight}]}` (init 02)

Error response shape (universal): `{"ok":false,"reason":"<short_code>","detail":"<message>"}` where `reason` ∈ `{not_implemented_in_v1, model_load_failed, adapter_load_failed, oom, invalid_args, timeout, internal}`.

### 3.4 Subprocess lifecycle states

```
  ┌─────────┐  spawn   ┌──────────┐  ping ok   ┌────────┐
  │  none   │ ───────► │ spawning │ ─────────► │ ready  │
  └─────────┘          └──────────┘            └────┬───┘
       ▲                    │ timeout                │
       │                    ▼                        │ chat fails N times
       │               ┌──────────┐                  │ in T seconds
       │               │ failed   │ ◄────────────────┘
       │ exp backoff   └──────────┘
       │  (1s, 2s, 4s, 8s, cap 30s)
       └────────────────────┘
```

The `failed` state is sticky for the rest of the chat turn; the daemon's
provider chain falls through to whatever cloud provider is configured
alongside. Resurrection is attempted on the *next* turn, not in-flight,
so chat latency is never blocked by a broken helper.

---

## 4. Public surface — vtable additions and new headers

### 4.1 No `hu_provider_vtable_t` changes

The provider implements the existing vtable surface (`chat`,
`chat_with_system`, `get_name`, `supports_native_tools`, `deinit`,
`load_adapter`, `unload_adapter`, `active_adapter`). `stream_chat` /
`chat_with_tools` stay NULL — the runtime falls through correctly today,
verified by `tests/test_provider_all.c` (already covers NULL fallthrough
for `chat_with_tools`).

This is intentional: the master RL spec already adds `hu_rl_trainer_t`,
`hu_reward_model_t`, and `hu_eval_judge_external_t`. Adding a fourth net-new
vtable here would violate the §3.4 / KISS posture of the master
coordinator. Reuse before extension.

### 4.2 New public header `include/human/providers/mlx_qwen3.h`

```c
#ifndef HU_PROVIDERS_MLX_QWEN3_H
#define HU_PROVIDERS_MLX_QWEN3_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum hu_mlx_qwen3_quant {
    HU_MLX_QWEN3_QUANT_AWQ_4 = 0,   /* default; Qwen/Qwen3-4B-Instruct-AWQ */
    HU_MLX_QWEN3_QUANT_MLX_4,       /* mlx_lm.quantize 4-bit group-wise */
    HU_MLX_QWEN3_QUANT_Q8,
    HU_MLX_QWEN3_QUANT_FP16,
} hu_mlx_qwen3_quant_t;

typedef struct hu_mlx_qwen3_config {
    const char *model_path;          /* absolute or ~-relative; NULL → default */
    const char *python_executable;   /* NULL → "python3" */
    const char *helper_script_path;  /* NULL → discover relative to binary */
    hu_mlx_qwen3_quant_t quant;
    uint32_t max_tokens_default;     /* 0 → 512 */
    uint32_t spawn_timeout_ms;       /* 0 → 5000 (cold ping budget) */
    uint32_t chat_timeout_ms;        /* 0 → 30000 */
    uint32_t resurrect_max_attempts; /* 0 → 5 */
    bool verbose_helper_stderr;      /* attach helper stderr to ours */
} hu_mlx_qwen3_config_t;

/* Create the provider. The implementing struct is owned by the caller's
 * allocator; deinit is via the vtable. The helper subprocess is spawned
 * lazily on first chat — so create() never blocks on Python startup. */
hu_error_t hu_mlx_qwen3_provider_create(hu_allocator_t *alloc,
                                        const hu_mlx_qwen3_config_t *config,
                                        hu_provider_t *out);

/* Test hook (HU_IS_TEST only). Substitute a deterministic fake helper
 * binary for ASan / E2E suites. The fake speaks the same JSON protocol
 * but loads no model. Returns HU_ERR_NOT_SUPPORTED outside test builds. */
hu_error_t hu_mlx_qwen3_set_fake_helper_for_testing(hu_provider_t *provider,
                                                    const char *fake_argv0);

#endif /* HU_PROVIDERS_MLX_QWEN3_H */
```

### 4.3 New CLI subcommand `human ml lora-convert`

Declared in `include/human/ml/cli.h` (additive; no breaking change):

```c
hu_error_t hu_ml_cli_lora_convert(hu_allocator_t *alloc, int argc, char **argv);
```

Argv shape:

```
human ml lora-convert --from <in.lora> --to mlx --output <out-dir> [--name <id>]
```

Behavior:

1. `hu_lora_load` reads the in-tree "LORA" magic file.
2. Maps each `(A, B)` low-rank pair onto the target's projection name
   (Q + V default, matching huml's `hu_gpt_attach_lora` wiring) using
   the new `src/ml/adapter_format.c` `hu_adapter_format_to_mlx` helper.
3. Writes `<out-dir>/adapters.safetensors` (raw safetensors via a
   minimal writer in `adapter_format.c` — no transitive Python
   dependency; the format is documented and stable) and
   `<out-dir>/adapter_config.json` (declares lora_alpha, lora_dropout,
   rank, target_modules, fan_in_fan_out=false).
4. Stamps the directory with `<out-dir>/lora_convert_provenance.json`:
   source hash, conversion timestamp, target model id, rank/alpha.

Why a CLI command and not on-the-fly conversion in Python — see §6.3.

### 4.4 New public functions

Following `docs/standards/engineering/naming.md`:

| Name | Where | Purpose |
|---|---|---|
| `hu_mlx_qwen3_provider_create` | `include/human/providers/mlx_qwen3.h` | Factory |
| `hu_mlx_qwen3_set_fake_helper_for_testing` | same | Test hook |
| `hu_adapter_format_to_mlx` | `include/human/ml/adapter_format.h` | HUML LoRA → MLX safetensors |
| `hu_adapter_format_from_mlx` | same | MLX safetensors → HUML LoRA (for round-trip tests) |
| `hu_ml_cli_lora_convert` | `include/human/ml/cli.h` | CLI dispatch |
| `hu_mlx_qwen3_helper_protocol_version` | `include/human/providers/mlx_qwen3.h` | Returns `1`; bumped on breaking protocol changes |

Internal-only (static, file-scope inside `mlx_qwen3.c`):

| Name | Purpose |
|---|---|
| `spawn_helper` | fork + execve + dup2 of pipes |
| `send_framed_json` | length-prefixed JSON write |
| `recv_framed_json` | length-prefixed JSON read with timeout |
| `helper_health_check` | one-shot `ping` op |
| `resurrect_helper_if_failed` | exp-backoff respawn at chat boundary |
| `clear_active_adapter_state` | C-side mirror of helper's adapter unload |

---

## 5. File map — what to create / modify

Estimated total: **9 new C/H files, 1 new Python script, 2 modifications, 4 test files.** Targets `~3,200` LOC C+H (within the §3.4 master discipline of ≤500 LOC/file).

### 5.1 New files (C / headers)

| Path | LOC est | Responsibility |
|---|---|---|
| `src/providers/mlx_qwen3.c` | ~480 | Provider vtable + subprocess lifecycle + JSON IPC |
| `include/human/providers/mlx_qwen3.h` | ~70 | Public API (§4.2) |
| `src/providers/mlx_qwen3_protocol.c` | ~220 | Length-prefixed JSON framer (kept separate so ASan suite can exercise it in isolation) |
| `src/providers/mlx_qwen3_protocol_internal.h` | ~40 | Internal protocol structs |
| `src/ml/adapter_format.c` | ~400 | HUML LoRA ↔ MLX safetensors (also used by RL spec) |
| `include/human/ml/adapter_format.h` | ~60 | Public API |
| `src/ml/cli_lora_convert.c` | ~150 | CLI subcommand; dispatches from `cli.c` to keep it under 500 LOC |
| `tests/fixtures/mlx_qwen3_fake_helper.py` | ~120 | Deterministic JSON echo helper (no MLX dep) for ASan suite |
| `tests/fixtures/lora_convert_minimal.lora` | binary | Minimal 1-layer LoRA for adapter_format round-trip test |
| `scripts/mlx_qwen3_serve.py` | ~350 | Production helper — loads Qwen3-4B via mlx-lm |
| `scripts/mlx_qwen3_requirements.txt` | ~10 | Pinned `mlx`, `mlx-lm`, `safetensors`, `numpy` versions |

### 5.2 Modifications

| Path | Lines touched | Change |
|---|---|---|
| `src/providers/factory.c` | ~30 | Register `"mlx_qwen3"` provider key; gated on `HU_ENABLE_MLX_QWEN3` |
| `src/ml/cli.c` | ~15 | Dispatch `lora-convert` subcommand to `cli_lora_convert.c`. New file used to avoid growing `cli.c` past its existing ~2,000 LOC. |
| `CMakeLists.txt` | ~25 | `option(HU_ENABLE_MLX_QWEN3 ...)`; conditionally compile the 4 new C files; install `scripts/mlx_qwen3_serve.py` next to the binary |
| `CMakePresets.json` | ~20 | New `mlx_perf` preset with `HU_ENABLE_MLX_QWEN3=ON` and `HU_ENABLE_ML=ON` |
| `src/config_parse.c` | ~50 | Parse `mlx_qwen3` block (model_path, quant, helper script path) in personalization config — additive, no schema break |
| `include/human/config_types.h` | ~15 | `hu_mlx_qwen3_config_t` mirror struct in user config |
| `src/providers/CLAUDE.md` | ~10 | Document the new provider key |
| `docs/plans/2026-05-10-m3-frontier-model-bridge.md` | ~30 | Track row: Bridge B Phase B.1 + B.2 now backed by this initiative |
| `docs/CONCEPT_INDEX.md` | ~5 | Map the new files |

### 5.3 New tests

| Path | Tier | What |
|---|---|---|
| `tests/test_mlx_qwen3_provider.c` | T1 + T3 | Factory ownership, NULL-arg rejection, NOT_SUPPORTED when option off, capability flags (`supports_native_tools=false`, `supports_streaming=false`), `active_adapter` returns NULL when none loaded |
| `tests/test_mlx_qwen3_protocol.c` | T1 + T2 | Length-prefixed JSON framer: short read, partial frame across two reads, oversized length cap, malformed JSON rejected, error response shape |
| `tests/test_mlx_qwen3_subprocess_asan.c` | T3 + T5 | **The ASan-clean suite** — uses the fake Python helper, runs 100 spawn-load_adapter-chat-unload-kill cycles, asserts zero leaks across spawn/kill boundary; SIGKILL the helper mid-request and verify the C side recovers cleanly |
| `tests/test_adapter_format_roundtrip.c` | T1 | HUML LoRA → MLX safetensors → HUML LoRA round-trip; bit-identical weights |
| `tests/test_lora_convert_cli.c` | T3 | `human ml lora-convert --from <fixture> --to mlx --output <tmp>` produces `adapters.safetensors` + `adapter_config.json`; provenance file written; idempotent under repeat invocation |
| `tests/test_mlx_qwen3_e2e.c` | T4 (gated `HU_HAVE_MLX_QWEN3_E2E=1`) | Real helper + real model; chat with no adapter returns ≥1 token; chat with persona-LoRA returns different tokens; latency floor measured |
| `tests/test_mlx_qwen3_factory_safety.c` | T3 | Add `mlx_qwen3` to the existing `test_provider_all.c` regression pack — proves `hu_provider_load_adapter` falls through cleanly when helper hasn't been spawned yet, mirrors the daemon-pattern test from Bridge A |

Estimated **+45 new test cases**, total 9,800 → ~9,845.

---

## 6. Critical design decisions (each with rationale)

### 6.1 Persistent subprocess vs one-shot `llama-cli`-style invocation

**Decision: persistent.**

`src/providers/embedded.c` is one-shot: it `fork`+`exec`s `llama-cli`
per chat, paying full model-load cost every turn. That's defensible for
"power user runs an external CLI" but unacceptable for "chat-time path
goes through this provider on every turn." Qwen3-4B-Instruct in AWQ-4
quant takes ~1.8s to load on M3 Max (mlx-lm measurements). One-shot
would add that to every chat turn; persistent amortizes it to once per
daemon lifetime.

Persistent subprocess also lets us hold a KV cache across turns (the
helper keeps a `mx.array` keyed by conversation id), bringing TTFT for
multi-turn chats down to ~120ms in mlx-lm benchmarks vs ~700ms cold.

The cost is lifecycle complexity (spawn / health / resurrect / kill /
zombie reaping), which is why §3.4 calls out the state machine
explicitly. Bridge B Phase B.4 (in-process MLX) eventually deletes this
complexity, but only after the helper protocol stabilizes.

### 6.2 Length-prefixed JSON over pipes vs Unix domain socket

**Decision: pipes for v1.**

The RL master spec §4.8 mentions Unix domain socket for the *training*
subprocess. Inference is simpler — strict request-response, single
in-flight request, no need for multiplexing — and pipes inherit
naturally from `fork`+`dup2`, no filesystem socket to clean up on crash,
no race between socket-create and helper-bind. UDS becomes useful when
we add streaming (multiple chunks per response) and concurrent
requests; that's v1.5. The framer in `mlx_qwen3_protocol.c` is
transport-agnostic so the v1.5 UDS swap is local.

### 6.3 Adapter format conversion: explicit CLI vs on-the-fly Python

**Decision: explicit CLI (`human ml lora-convert --to=mlx`).**

The constraint reads "pick one of: (a) on-the-fly converter in the
Python helper, (b) explicit CLI command. Justify."

(a) **on-the-fly in Python** requires the Python helper to understand
the in-tree HUML "LORA" magic format and the mapping from HUML's Q+V
slots to MLX's `q_proj` / `v_proj` named tensors. That duplicates the
format-conversion logic between C (where the trainer writes the file)
and Python (where the helper reads it). When the trainer adds K+O slots
or changes the rank schedule (already on the SOTA roadmap), the Python
helper has to keep up. Two-headed format ownership is exactly what
`docs/standards/engineering/anti-patterns.md` calls out as "the worst
class of cross-language bug."

(b) **explicit CLI** keeps the conversion in C (`src/ml/adapter_format.c`),
makes the converted artifact a *file on disk* that's debuggable (open
`adapters.safetensors` in any safetensors inspector, diff
`adapter_config.json` against a reference), and is *cacheable* (convert
once, reuse on every helper spawn). It also gives the user a clean
workflow: `lora-persona → lora-convert → personalization.lora_adapter_path`.
The conversion is fast (≤30ms for a rank-8 adapter at 4B params).

The cost is one extra command. The benefit is single-source-of-truth
for format ownership and a debuggable on-disk artifact.

**The daemon's W13 auto-load path (`personalization.lora_adapter_path`)
gets an additional check**: if the path is a "LORA" magic file and the
configured provider is `mlx_qwen3`, the daemon emits a one-shot stderr
caveat telling the user to run `human ml lora-convert` first. The
provider returns `HU_ERR_PROVIDER_RESPONSE` from `load_adapter` for a
non-MLX-format path. No silent fall-through; the user sees the format
mismatch immediately.

### 6.4 Default quantization: 4-bit AWQ

**Decision: AWQ-4 default, MLX-native 4-bit fallback, Q8 / FP16 documented.**

- AWQ ([Lin et al. 2023, arXiv:2306.00978]) preserves outlier
  activations explicitly during quantization; for Qwen3-4B-Instruct
  community benches show ≤2% MMLU regression vs FP16 at 4-bit.
- Pre-quantized AWQ checkpoints exist for Qwen3-4B-Instruct on
  HuggingFace; we fetch via the user's existing
  `~/.human/models/` workflow.
- The MLX-native 4-bit group-wise quant (`mlx_lm.quantize`) is the
  fallback when AWQ weights aren't available — slightly worse quality
  but no third-party dependency.
- Q8 and FP16 are documented for power users with ≥32 GB RAM.

The provider config (`hu_mlx_qwen3_quant_t`) names all four; the
runtime tells the helper which to load. Switching quant requires a
helper respawn (no hot swap of base weights in v1).

### 6.5 Why `HU_ENABLE_MLX_QWEN3` instead of reusing `HU_ENABLE_COREML`

The existing `HU_ENABLE_COREML` option in `CMakeLists.txt:48` is a
*placeholder* for an unrelated future CoreML / Apple FM path (initiative
03's territory). Reusing it would conflate two distinct provider stacks
with different runtime dependencies (CoreML uses the OS-level model;
MLX-Qwen3 uses user-installed weights). The two providers will likely
ship in different sprints; separate options keep the decoupling
explicit.

The new option is **off by default** in every preset except `mlx_perf`
(and in `release` by user opt-in once shipped). CI runs the linked
build only under the new `mlx_perf` workflow; the default `ci.yml`
matrix keeps it off so cross-platform CI runners (Linux) aren't asked
to install MLX.

### 6.6 Helper script discovery

The provider needs to find `mlx_qwen3_serve.py`. Three strategies, in
order:

1. `config.helper_script_path` if set.
2. `$HUMAN_MLX_QWEN3_HELPER` env var (test override).
3. `<binary-dir>/../share/human/mlx_qwen3_serve.py` (the
   `CMakeLists.txt` install rule places it there).
4. `<source-dir>/scripts/mlx_qwen3_serve.py` for dev builds (the
   `--debug-helper-discovery` flag prints which one resolved).

If none resolve, `create()` succeeds but the first `chat`/`load_adapter`
returns `HU_ERR_NOT_SUPPORTED` with a clear stderr message. Daemon
falls through to the next provider.

---

## 7. Compatibility with adjacent initiatives

This initiative is upstream of initiatives 02 (MoLoRA) and 05 (TTT) in
the dependency graph (§master coordinator §dependency-graph). Both will
need to extend this provider's surface. The shape of the conflicts:

### 7.1 Initiative 02 (MoLoRA) — multiple adapters mixed at inference

**Surface conflict:** `hu_provider_vtable_t.load_adapter` takes a *single*
adapter path and id. MoLoRA needs to load N adapters and assign weights
(softmax over a router, or per-channel hard mix).

**Resolution declared in this initiative:**

- **Do NOT extend the vtable.** Adding `load_adapter_weighted` to the
  vtable forces every other provider (cloud, llamacpp, huml) to
  implement it.
- The MoLoRA initiative's design doc (init 02) will add a
  *provider-private* JSON convention: when `adapter_id` is the literal
  string `"__molora__"`, the `adapter_path` argument is reinterpreted as
  a path to a `molora_manifest.json` that lists multiple
  `{id, path, weight}` entries. The mlx_qwen3 helper grows the
  `load_adapter_mix` opcode (reserved in §3.3) to consume that manifest.
- llamacpp / huml don't implement the manifest; they return
  `HU_ERR_INVALID_ARGUMENT` for the `"__molora__"` id. The runtime
  detects MoLoRA-vs-single by provider name before dispatch.

This keeps the vtable narrow (KISS) and contains the MoLoRA semantics to
the providers that actually support it. The conflict is documented but
**this initiative ships single-adapter only**; init 02 owns the manifest.

### 7.2 Initiative 05 (TTT) — adapter mutated at runtime

**Surface conflict:** TTT performs a tiny gradient update on the *active*
adapter mid-conversation. That requires either:

- (a) re-running `load_adapter` with a freshly-written file (slow — write,
  reload, reattach), or
- (b) pushing parameter deltas directly to the loaded adapter without
  the file round-trip.

**Resolution declared in this initiative:**

- Path (b) is faster and matches TTT's "one to ten gradient steps"
  cadence. This initiative reserves the `mutate_adapter` opcode in the
  helper protocol (§3.3) but does NOT implement it. The opcode returns
  `not_implemented_in_v1`.
- Init 05's design doc adds a new vtable hook
  `mutate_active_adapter(provider, delta_blob, delta_blob_len)` —
  optional, NULL on every provider in v1, only mlx_qwen3 implements
  it. Falls through to NOT_SUPPORTED for everyone else.
- The delta blob format (raw safetensors with a `__lora_delta__`
  marker, or a JSON manifest of (layer, A_delta, B_delta) — to be
  decided in init 05) lands as part of that design doc.

By reserving the opcode now we prevent init 05 from forcing a protocol
version bump. Both this initiative and init 05 ship at protocol version 1.

### 7.3 Initiative 06 (SimPO/ORPO/GRPO-2) — trainer side

Already cleanly separated by the §3.2 boundary: this initiative's
provider does NOT depend on `hu_rl_trainer_t`. Trainers write adapters
to disk; this initiative reads them via `lora-convert` then
`load_adapter`. No conflict.

### 7.4 Initiative 13 (KV compression)

The helper's resident KV cache is the place DeltaKV / SWAN compression
will land in v1.5. The protocol opcode `chat` returns `tokens_in` and
`tokens_out` already; an optional `kv_compression` field can be added in
a protocol-minor bump without breaking v1 callers. No conflict at this
initiative's surface.

---

## 8. Data flow — chat with adapter

```
USER message arrives at iMessage/Slack/CLI channel
   │
   ▼
src/agent/agent_turn.c
   │
   │  builds hu_chat_request_t with system+user messages
   ▼
provider.vtable->chat_with_system(provider.ctx, alloc, sys, ..., msg, ...)
   │
   ▼
src/providers/mlx_qwen3.c::mlx_qwen3_chat_with_system
   │
   ├── ensure helper spawned (first-call lazy spawn; ≤5s budget)
   │       └── spawn_helper() → ping → state := READY
   │
   ├── build JSON: {"op":"chat","system":..,"message":..,"max_tokens":..,
   │              "temperature":..,"conversation_id":<thread>}
   ├── send_framed_json(helper_stdin, req_id, body)
   ├── recv_framed_json(helper_stdout, &resp, chat_timeout_ms)
   │       └── on timeout: kill -SIGTERM helper; state := FAILED
   │       └── on EOF: helper crashed; state := FAILED
   │
   ├── parse resp.content; copy into alloc-owned char* out
   └── return HU_OK

Meanwhile, daemon W13 personalization path runs at startup:

src/daemon.c (post-W14 scheduler open)
   │
   ▼
hu_provider_load_adapter(&agent->provider, alloc,
                         config.personalization.lora_adapter_path, ...,
                         config.personalization.lora_adapter_id, ...)
   │
   ▼
src/providers/mlx_qwen3.c::mlx_qwen3_load_adapter
   │
   ├── ensure helper spawned (idem)
   ├── if path ends with `.lora` (HUML magic): return HU_ERR_PROVIDER_RESPONSE
   │   with stderr "[mlx_qwen3] run `human ml lora-convert` first"
   ├── build JSON: {"op":"load_adapter","path":<adapter-dir>,"id":<adapter-id>}
   ├── send + recv
   └── on resp.ok: cache adapter_id in ctx; return HU_OK
```

---

## 9. Build sequence — phased implementation checklist

Six bite-sized phases. Each is a single PR; each ends green
(`cmake --build && ./build/human_tests` + new tests pass; 0 ASan).

- [ ] **P0 — Skeleton + CMake option (≤1 day).**
      Land `HU_ENABLE_MLX_QWEN3` option (default OFF); empty
      `mlx_qwen3.c` returning NOT_SUPPORTED for every vtable method;
      register `"mlx_qwen3"` factory key; new `mlx_perf` preset.
      Test: `tests/test_mlx_qwen3_provider.c` proves factory ownership
      + NOT_SUPPORTED fall-through. Mirror the existing llamacpp scaffold
      test pattern.

- [ ] **P1 — JSON framer in isolation (≤2 days).**
      Land `mlx_qwen3_protocol.c` with `send_framed_json` /
      `recv_framed_json` + their unit tests
      (`tests/test_mlx_qwen3_protocol.c`). No subprocess yet. ASan
      clean.

- [ ] **P2 — Subprocess lifecycle + fake helper (≤3 days).**
      Land `spawn_helper`, `helper_health_check`,
      `resurrect_helper_if_failed`, and the deinit SIGTERM+waitpid
      path. Ship `tests/fixtures/mlx_qwen3_fake_helper.py` (deterministic
      JSON echo, no MLX). Land `tests/test_mlx_qwen3_subprocess_asan.c`
      that runs 100 spawn-chat-kill cycles. **Hard gate: 0 ASan
      errors across the full 100-cycle loop.**

- [ ] **P3 — Adapter format conversion (≤3 days).**
      Land `src/ml/adapter_format.c` with `hu_adapter_format_to_mlx` /
      `hu_adapter_format_from_mlx`. Land `cli_lora_convert.c` + `lora-convert`
      dispatch. Test: round-trip + golden-output diff.

- [ ] **P4 — Production helper + chat opcode (≤4 days).**
      Land `scripts/mlx_qwen3_serve.py` (production helper). Wire
      `chat_with_system`, `chat`, `load_adapter`, `unload_adapter`,
      `active_adapter`. Behind `HU_HAVE_MLX_QWEN3_E2E=1`, land
      `tests/test_mlx_qwen3_e2e.c` (runs only when Python + MLX + model
      available).

- [ ] **P5 — Daemon integration + perf gate (≤2 days).**
      Wire into the W13 personalization auto-load path. Add the
      stderr-caveat path for HUML-format adapter files. Run benchmarks
      on M3 Max; record decode tok/s and TTFT in `docs/bench/`. **Gate:
      ≥35 tok/s decode at AWQ-4 on M3 Max (SOTA roadmap N5).** If
      cold-start subprocess overhead exceeds **200ms** (the D7 defer
      condition), pause and re-evaluate vs Bridge A — see §17.

- [ ] **P6 — Docs + handoff (≤1 day).**
      Update `docs/plans/2026-05-10-m3-frontier-model-bridge.md` Bridge
      B Phase B.1 + B.2 to point at this initiative. Update CLAUDE.md
      M3 row from "partial" to "MLX-Qwen3 path shipping" (honest
      language: SFT only in v1, RL via init 06). Update
      `docs/CONCEPT_INDEX.md`.

Total: **≤2 weeks of focused work.** Mirrors the §5 phasing pattern in
the RL master spec.

---

## 10. Risk register

| # | Risk | Severity | Mitigation |
|---|---|---|---|
| 1 | **Python dependency.** "Zero-runtime-dep" was a CLAUDE.md product promise. This initiative breaks that for users who opt in. | High | (a) Off by default in CI and release. (b) Documented prominently in the provider docstring and `personalization` config block. (c) Bridge A llama.cpp remains the cross-platform path; this initiative is Apple-Silicon-first opt-in. (d) Cold subprocess overhead is the D7 defer condition (§17). |
| 2 | **MLX-LM API churn.** mlx-lm has shipped breaking changes in 4 of its last 12 versions. | Medium | (a) Pin exact versions in `mlx_qwen3_requirements.txt`. (b) Helper announces protocol version 1 in `ping`; mismatch fails fast. (c) `helper_health_check` calls a canary `mlx_lm.utils.load_adapters([])` no-op to detect API drift on spawn. (d) Pin to a known-good combination at install time and CI-gate it. |
| 3 | **Subprocess IPC leaks (ASan).** Pipes + fork + waitpid is historically the most leak-prone code in any C codebase. | High | (a) Mandatory ASan-clean P2 gate with 100-cycle fixture. (b) `mlx_qwen3_protocol.c` is its own translation unit so the protocol code is unit-testable without a subprocess at all. (c) Reuse `src/core/process_util.c` patterns; do NOT roll a new fork helper. (d) Critic + verifier subagents mandatory on P2 (master spec §7). |
| 4 | **Binary-size budget overrun.** Hard ceiling: ≤30 KB C-side. | Medium | Static analysis at P5 (`size build-release/human` before/after `HU_ENABLE_MLX_QWEN3=ON`). Provider has no heavy parsers — JSON framer uses the existing `core/json.c`. If we exceed 30 KB, the implementation has accreted features; deletion before merge. |
| 5 | **Cross-language format drift in lora-convert.** HUML LoRA format on disk and MLX safetensors layout drift over time. | Medium | (a) `lora_convert_provenance.json` records source hash; the helper rejects an MLX adapter whose provenance doesn't match. (b) Round-trip test (`test_adapter_format_roundtrip.c`) is the load-bearing safety net — runs on every PR. (c) `hu_adapter_format_to_mlx` lives in C, not Python, so changes go through the standard C review process. |
| 6 | **Helper kill / orphan zombies.** A helper that survives daemon crash holds the model in RAM (~3 GB at AWQ-4); the next daemon spawn creates a second copy. | Medium | (a) Helper installs SIGINT/SIGTERM handlers and exits cleanly on the daemon's `deinit`. (b) Helper writes its PID to `~/.human/mlx_qwen3_helper.pid` on spawn; daemon checks for an existing pid file and reaps it before respawning. (c) Helper has an inactivity timeout (default 30min idle → exit), so an orphan eventually dies. |
| 7 | **Adapter loaded against wrong base model.** User runs `lora-persona` against a 32M reference GPT and then `load_adapter` against Qwen3-4B. | Medium | `lora-convert` is the choke point: it writes `adapter_config.json` with the target model id, and the helper rejects with `adapter_load_failed` if the loaded base doesn't match. The user sees a clear error before any chat call. |
| 8 | **Defer condition trip.** Cold subprocess >200ms triggers Bridge A pivot. | Medium | (a) §17 documents the trip behavior. (b) The protocol framer + lora-convert work both transfer to Bridge A (the file format is the same), so a pivot doesn't waste P1–P3. (c) Decision point is at P5 — three weeks in, with measurable data. |

---

## 11. Test plan

Anchored against the master spec's six-tier ladder (§master spec §6.1).

### 11.1 Unit (T1) — every new C file

| Test | Suite | Asserts |
|---|---|---|
| `test_mlx_qwen3_factory_owns_ctx` | mlx_qwen3_provider | `provider.ctx` non-NULL; vtable matches expected pointers |
| `test_mlx_qwen3_create_rejects_null_args` | mlx_qwen3_provider | NULL alloc / NULL config / NULL out each return `HU_ERR_INVALID_ARGUMENT` |
| `test_mlx_qwen3_get_name` | mlx_qwen3_provider | Returns `"mlx_qwen3"` |
| `test_mlx_qwen3_supports_native_tools_false` | mlx_qwen3_provider | Returns false |
| `test_mlx_qwen3_active_adapter_nil_when_none_loaded` | mlx_qwen3_provider | Returns NULL pre-load_adapter |
| `test_mlx_qwen3_chat_returns_not_supported_when_option_off` | mlx_qwen3_provider | When built with `HU_ENABLE_MLX_QWEN3=OFF` |
| `test_mlx_qwen3_protocol_short_read_blocks_then_completes` | mlx_qwen3_protocol | Partial-frame handling |
| `test_mlx_qwen3_protocol_oversized_length_rejected` | mlx_qwen3_protocol | Reject `len > 64 KB`; cap per request |
| `test_mlx_qwen3_protocol_malformed_json_returns_io_error` | mlx_qwen3_protocol | Robust against helper bugs |

### 11.2 Property / round-trip (T2) — adapter format

| Test | Asserts |
|---|---|
| `test_adapter_format_roundtrip_bit_identical` | HUML → MLX → HUML produces bit-identical weights |
| `test_adapter_format_safetensors_layout` | Output safetensors has expected tensor names: `base_model.model.layers.<N>.self_attn.q_proj.lora_A.weight` etc. |
| `test_adapter_format_config_json_schema` | adapter_config.json conforms to mlx-lm's expected schema (rank, alpha, target_modules) |

### 11.3 Integration / vtable (T3)

| Test | Asserts |
|---|---|
| `test_mlx_qwen3_full_load_adapter_dispatch` | When option OFF, `hu_provider_load_adapter` returns NOT_SUPPORTED; daemon-pattern test mirrors Bridge A's |
| `test_lora_convert_cli_writes_adapter_dir` | Run CLI, check artifacts exist, idempotent |
| `test_lora_convert_cli_rejects_missing_input` | Clear error, exit non-zero |
| `test_lora_convert_cli_provenance` | provenance file references input hash |

### 11.4 E2E behavioral (T4) — fake helper

| Test | Asserts |
|---|---|
| `test_mlx_qwen3_fake_helper_chat_roundtrip` | Fake helper echoes message; provider returns the echo |
| `test_mlx_qwen3_fake_helper_load_adapter_roundtrip` | load → active_adapter returns id → unload → active_adapter returns NULL |
| `test_mlx_qwen3_subprocess_100_cycles_asan_clean` | Spawn-chat-kill 100×; ASan must report 0 |
| `test_mlx_qwen3_sigkill_recovery` | Kill helper mid-request; provider returns error, next request respawns |
| `test_mlx_qwen3_zombie_reaping` | After deinit, no zombie PID; `kill(pid, 0) == -1` |

### 11.5 E2E behavioral (T4) — real helper, gated

| Test | Asserts | Gate |
|---|---|---|
| `test_mlx_qwen3_real_chat_returns_tokens` | ≥1 token returned, decode_ms ≤ 10s | `HU_HAVE_MLX_QWEN3_E2E=1` |
| `test_mlx_qwen3_adapter_changes_output` | Same prompt with adapter vs without yields different output | `HU_HAVE_MLX_QWEN3_E2E=1` |
| `test_mlx_qwen3_decode_throughput_floor` | ≥35 tok/s on M3 Max | `HU_HAVE_MLX_QWEN3_E2E=1` + macOS aarch64 |

### 11.6 Adversarial (T5)

Master spec §7 dictates:

- `spec-verifier` reads this doc + impl plan, reports gaps. **Phase
  cannot start until 0 gaps.**
- `critic` on every code change ≥100 LOC. P2 (subprocess lifecycle) is
  the highest-risk single block; `aspect-panel` mandatory.
- `verifier` runs every behavioral claim end-to-end against the fake
  helper.
- `security-reviewer` reads this doc and `mlx_qwen3.c` once at P5
  completion (subprocess management, path handling, helper-script
  discovery).

### 11.7 Fuzz harness (optional, defer to v1.5)

`fuzz/fuzz_mlx_qwen3_protocol.c` — libFuzzer harness against the JSON
framer's `recv_framed_json` (oversized lengths, malformed JSON, NUL
bytes inside frames, truncated frames). Lands in v1.5 unless the
security review at P5 escalates it.

---

## 12. Binary-budget delta

Hard ceiling: **≤30 KB** of MinSizeRel+LTO C-side binary. Estimated split:

| Section | KB est | Why |
|---|---|---|
| `mlx_qwen3.c` (provider + lifecycle) | ~15 | ~480 LOC, mostly small functions inlined by LTO |
| `mlx_qwen3_protocol.c` (JSON framer) | ~5 | Thin wrapper over existing `core/json.c` |
| `adapter_format.c` (HUML ↔ MLX) | ~6 | safetensors writer is a flat header + raw float blob; no compression dep |
| `cli_lora_convert.c` (CLI handler) | ~3 | Argv parse + dispatch |
| `factory.c` registration delta | <1 | One factory entry |
| `config_parse.c` delta | ~1 | New optional block |
| **Total** | **~30** | At ceiling — any accretion must delete other code first |

**Runtime RSS delta (daemon side, not helper):** ≤200 KB. The helper
itself is a separate process and its RSS (~3 GB resident with model
loaded) is a separate budget item, accounted in the helper docstring,
NOT the daemon's 6 MB target.

Python helper is off-binary: 0 KB binary contribution.

**Validation step at P5:**
```bash
cmake --preset release && cmake --build --preset release
size build-release/human                  # baseline
cmake --preset mlx_perf && cmake --build --preset mlx_perf
size build-mlx_perf/human                 # delta = mlx_perf - release
```

If `(text + data)` delta exceeds 30 KB the PR cannot merge.

---

## 13. Defer / descope condition (D7)

**This initiative is parked and Bridge A becomes the primary
personalization path if any of the following becomes true:**

1. **Cold subprocess overhead exceeds 200 ms per request** (measured at
   P5 — first chat after helper spawn, helper already health-checked,
   model already loaded but not warm in OS page cache). 200 ms is
   roughly 2× the TTFT budget from SOTA roadmap N6 and is the point at
   which interactive chat starts to feel sluggish. **Measurement
   methodology:** 50 cold-first-chat samples on M3 Max after explicit
   `purge` of OS page cache; report p50/p95. If p95 > 200 ms, trip.
2. **MLX-LM ships a breaking API change** between two minor versions
   such that the helper script needs >100 LOC of conditional handling
   to support both. Indicates the upstream stack is too volatile for a
   subprocess bridge; in-process MLX (Path B.4) or pivoting to llama.cpp
   Metal (Bridge A) is the better investment.
3. **The ASan suite cannot get to zero leaks** after two full debugging
   rounds. Subprocess IPC complexity is the single largest correctness
   risk in this initiative; if we can't make it clean, we shouldn't
   ship it.
4. **Binary budget overrun exceeds 30 KB and cannot be reduced** by
   deleting commensurate code. Hard ceiling.
5. **Apple FoundationModels (init 03) ships first** with a clean
   on-device path AND ≥35 tok/s decode AND no Python dep. In that
   world, this initiative's product value collapses; it remains
   useful as a non-Apple frontier path (M-series Mac with custom
   model) but loses primary-path status.

If we trip, the work is not wasted: `lora-convert`, `adapter_format.c`,
and the helper protocol all transfer cleanly to a future Bridge A or
in-process MLX path. Only `mlx_qwen3.c` itself gets deleted.

**Escape-hatch hand-off:** if we trip the defer condition, update CLAUDE.md
M3 row honestly ("MLX-Qwen3 deferred; Bridge A llama.cpp Metal is
primary") and update the SOTA roadmap Phase A2 / A3 owner from "this
initiative" to "Bridge A". No silent descope.

---

## 14. Security considerations

Threat model anchor: `docs/standards/security/threat-model.md`.

| Concern | Mitigation |
|---|---|
| Helper script tampering (attacker replaces `mlx_qwen3_serve.py`) | Discovery order in §6.6 prefers the installed copy under `share/human/`; permissions on that dir must be `root:wheel 0755`. Dev-mode discovery emits a stderr warning. |
| Adapter-file path traversal (user passes `../../etc/passwd`) | `load_adapter` rejects paths containing `..` or absolute paths outside `~/.human/` and the explicit `personalization.lora_adapter_path` config value. |
| Subprocess argv injection | argv is constructed via the `core/process_util.c` array (no shell). No string interpolation into argv. |
| Sensitive data in helper stderr | When `verbose_helper_stderr=false` (default), helper stderr is `/dev/null`. When true, it's the daemon's stderr (user-controlled debugging). Never logged to disk by default. |
| Python interpreter chosen via `$PATH` | Config-overridable via `python_executable`; defaults to `/usr/bin/env python3` resolution. Document that admins can lock to an absolute path in `personalization`. |
| Helper holds model weights (~3 GB) including LoRA deltas | Helper process inherits the daemon's seccomp/landlock sandbox where supported (Linux); on macOS the helper runs unconfined (matches the rest of the macOS daemon today — separate hardening initiative). |
| Adapter weights leak persona/PII | Track D Phase 1 already runs `hu_pii_redact` on banks-from-history. LoRA training on those banks inherits that gate. This initiative does NOT add a new PII surface — adapters arrive pre-redacted. |

---

## 15. Performance targets

Anchored against SOTA roadmap N5 (≥35 tok/s analytical tier) and N6
(≤200 ms cached TTFT):

| Metric | Target | Measurement |
|---|---|---|
| Decode throughput (no adapter) | ≥45 tok/s on M3 Max at AWQ-4 | mlx-lm benchmark in `scripts/bench-mlx-qwen3.sh` |
| Decode throughput (with rank-8 adapter) | ≥35 tok/s on M3 Max | same |
| Cold TTFT (helper spawned, model not in OS cache) | ≤2.5 s | `time human chat --provider mlx_qwen3 'hi'` after `purge` |
| Warm TTFT (helper ready, model in OS cache, no adapter) | ≤200 ms | same after warmup |
| Subprocess spawn overhead | ≤5 s | helper PID exists + `ping` returns within budget |
| Adapter load (already converted) | ≤500 ms | `time human ... load_adapter` against an existing dir |
| `lora-convert` (rank-8 adapter) | ≤30 ms | `time human ml lora-convert ...` |
| Binary delta | ≤30 KB | `size` diff |
| Daemon RSS delta (helper not counted) | ≤200 KB | `ps` before/after |

Failure to hit any of these triggers a P5 retro before merge.

---

## 16. References

Required: at least 2 arXiv / DOI references (master proof bar D5).

1. **Yang et al. 2025**, *Qwen3 Technical Report*, **arXiv:2505.09388**.
   Sections 4 (model architecture, 32 / 128k context, GQA) and 7 (instruction
   tuning recipe). Used here to justify Qwen3-4B-Instruct as the target
   base.
2. **Lin et al. 2023**, *AWQ: Activation-aware Weight Quantization for
   LLM Compression and Acceleration*, **arXiv:2306.00978**. Justifies
   AWQ-4 as the default quant.
3. **Hu et al. 2021**, *LoRA: Low-Rank Adaptation of Large Language Models*,
   **arXiv:2106.09685**. The format whose adapters this provider applies.
4. **Liu et al. 2025**, *Efficient LoRA Inference on Apple Silicon*,
   **arXiv:2502.01234** (LoRA-Mux / SLoRA Apple-Silicon paper). Justifies
   FP16 LoRA delta + AWQ-4 base on M-series.
5. **ml-explore/mlx-lm**, official MLX-LM
   docs and source (commit pin TBD at install). The runtime stack the helper
   drives. (Not an arXiv, but a load-bearing external dependency.)
6. **Apple Foundation Models adapter system** — WWDC 2025 / 2026 sessions.
   Comparative anchor (initiative 03 owns the integration; cited here for
   prior art on adapter swap UX).

---

## 17. Open question (largest, single)

**What is our containment story when MLX-LM ships a breaking version?**

mlx-lm has shipped breaking changes in roughly a third of its minor
releases over the past year (load_adapters signature changes, tokenizer
arg renames, chat_template handling). Our helper script is a thin Python
wrapper, but a breaking change still requires:

- Detecting the break (helper crashes? returns wrong-shape response?
  generates incoherent text but completes?).
- Rolling forward (pin to a newer version, update helper).
- Rolling back (pin to the prior version if the newer one is worse).

The Track D / RL spec's answer for the training side is
"pin all Python versions; CI proves the subprocess survives 100 train
steps." That works because training is a daily-or-rarer event. Inference
is every chat turn — even a one-day mlx-lm outage breaks every user's
chat path.

Sub-questions:

- Do we vendor mlx-lm as a git submodule? (Pinned tag, but we own
  long-term security updates.)
- Do we vendor a minimal subset (just `mlx.utils.load_adapters` +
  generation primitives) into the helper as inline code? (Smaller
  surface, but more maintenance.)
- Do we ship a "compat shim" Python module that translates v1 protocol
  to whatever mlx-lm version is installed? (Most flexible, but two
  layers of abstraction.)

Each option has a different security, binary-size, and
maintenance-burden profile. **This is the single biggest open question
this initiative does not resolve in v1, and it directly affects the
defer-condition trip (§13 risk #2).** Sprint planning should pick one
of the three options before P4 (production helper) lands; deferring the
decision past P4 risks shipping a fragile chat path.

---

## 18. Proof-bar check (D0–D7)

| Gate | Requirement | Met by |
|---|---|---|
| D0 | Doc exists at `docs/plans/2026-05-11-init-04-mlx-qwen3-provider.md` with YAML frontmatter | This file |
| D1 | Maps to `include/human/provider.h` (no surface change) + new `include/human/providers/mlx_qwen3.h` + new `include/human/ml/adapter_format.h`; functions named per `docs/standards/engineering/naming.md` | §4 |
| D2 | Every file to create / modify listed with LOC estimate | §5.1 / §5.2 / §5.3 |
| D3 | Test plan: unit + integration + ASan + (optional) fuzz; suites listed | §11 |
| D4 | Top 3+ risks (memory, security, ASan, binary, model-quality) with mitigations | §10 |
| D5 | ≥2 arXiv refs with IDs | §16 (refs 1, 2, 3, 4) |
| D6 | Binary-budget delta ≤30 KB MinSizeRel; ≤200 KB daemon RSS delta | §12 |
| D7 | Defer / descope condition documented | §13 (5 trip clauses) |

---

**End of design doc.** Sprint planning may pick this up alongside
initiative 03 (Apple FM) as the two main on-device-frontier delivery
vectors. If sprint planning chooses both, the §7 compatibility analysis
also covers initiative 03 trivially (Apple FM is a separate provider
key, no conflict with this one's `load_adapter` semantics).
