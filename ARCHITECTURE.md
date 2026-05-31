# Architecture

_not quite human._ human is a C11 autonomous AI assistant runtime. This document provides the structural overview — see `AGENTS.md` for the full engineering protocol.

## System Topology

```mermaid
graph TB
    subgraph external [External Interfaces]
        CLI[CLI Terminal]
        Gateway[HTTP/WS Gateway]
        Channels[31 Messaging Channels]
    end

    subgraph core [Agent Core]
        Dispatcher[Dispatcher]
        AgentLoop[Agent Loop]
        Planner[Planner / DAG]
        ToolRouter[Tool Router]
        PromptBuilder[Prompt Builder]
    end

    subgraph intelligence [Intelligence Layer]
        Providers[50+ AI Providers]
        Persona[Persona System]
        Memory[Memory Subsystem]
        Context[Context Manager]
    end

    subgraph execution [Execution Layer]
        Tools[87 Tools]
        Runtime[Runtime Adapters]
        Security[Security / Policy]
    end

    subgraph storage [Storage]
        SQLite[(SQLite)]
        VectorStore[(Vector Store)]
        SecretVault[(Secret Vault)]
    end

    CLI --> Dispatcher
    Gateway --> Dispatcher
    Channels --> Dispatcher

    Dispatcher --> AgentLoop
    AgentLoop --> Planner
    AgentLoop --> PromptBuilder
    Planner --> ToolRouter

    PromptBuilder --> Persona
    PromptBuilder --> Context
    PromptBuilder --> Memory
    AgentLoop --> Providers

    ToolRouter --> Security
    ToolRouter --> Tools
    Tools --> Runtime

    Memory --> SQLite
    Memory --> VectorStore
    Security --> SecretVault
```

## Request Flow

A message entering the system follows this path:

```mermaid
sequenceDiagram
    participant User
    participant Channel
    participant Dispatcher
    participant Agent
    participant Provider
    participant ToolRouter
    participant Tool

    User->>Channel: Send message
    Channel->>Dispatcher: hu_channel_message_t
    Dispatcher->>Agent: agent_turn

    Agent->>Agent: Build prompt (persona + context + memories)
    Agent->>Provider: chat(request)
    Provider-->>Agent: response (may include tool_calls)

    alt Tool calls requested
        Agent->>ToolRouter: Route tool calls
        ToolRouter->>ToolRouter: Policy check
        ToolRouter->>Tool: execute(params)
        Tool-->>ToolRouter: hu_tool_result_t
        ToolRouter-->>Agent: Results
        Agent->>Provider: chat(with tool results)
        Provider-->>Agent: Final response
    end

    Agent->>Channel: send(response)
    Channel->>User: Deliver message
```

## Module Dependency Graph

Dependencies flow inward — implementations depend on contracts, never on each other.

```mermaid
graph TD
    subgraph contracts [Vtable Contracts - include/human/]
        ProviderH["provider.h"]
        ChannelH["channel.h"]
        ToolH["tools.h"]
        MemoryH["memory.h"]
        RuntimeH["runtime.h"]
        SecurityH["security.h"]
        PersonaH["persona.h"]
    end

    subgraph coreLib [Core Libraries - src/core/]
        Allocator["allocator"]
        Arena["arena"]
        JSON["json"]
        StringLib["string"]
        HTTP["http"]
        Error["error"]
    end

    subgraph implementations [Implementations - src/]
        ProvImpl["providers/ - 50+"]
        ChanImpl["channels/ - 31"]
        ToolImpl["tools/ - 87"]
        MemImpl["memory/ - 10 engines"]
        RTImpl["runtime/ - 5 adapters"]
        SecImpl["security/ - 11 sandboxes"]
        PersImpl["persona/ - 17 files"]
    end

    subgraph agentCore [Agent Core - src/agent/]
        AgentImpl["agent loop, planner, DAG, dispatcher"]
    end

    ProvImpl --> ProviderH
    ChanImpl --> ChannelH
    ToolImpl --> ToolH
    MemImpl --> MemoryH
    RTImpl --> RuntimeH
    SecImpl --> SecurityH
    PersImpl --> PersonaH

    ProvImpl --> HTTP
    ProvImpl --> JSON
    ChanImpl --> HTTP
    ChanImpl --> JSON
    ToolImpl --> SecurityH
    MemImpl --> JSON

    AgentImpl --> ProviderH
    AgentImpl --> ChannelH
    AgentImpl --> ToolH
    AgentImpl --> MemoryH
    AgentImpl --> PersonaH
    AgentImpl --> SecurityH

    ProviderH --> Allocator
    ChannelH --> Allocator
    ToolH --> Allocator
    MemoryH --> Allocator
    HTTP --> Allocator
    JSON --> Allocator
    StringLib --> Allocator
```

## Extension Points

All extension is done via vtable implementation + factory registration:

| Extension Point | Vtable            | Factory                   | Guide                  |
| --------------- | ----------------- | ------------------------- | ---------------------- |
| AI Provider     | `hu_provider_t`   | `src/providers/factory.c` | `AGENTS.md` 7.1        |
| Channel         | `hu_channel_t`    | `src/channels/channel_catalog.c`   | `AGENTS.md` 7.2        |
| Tool            | `hu_tool_t`       | `src/tools/factory.c`     | `AGENTS.md` 7.3        |
| Memory Engine   | `hu_memory_t`     | `src/memory/factory.c`    | `src/memory/CLAUDE.md` |
| Runtime         | `hu_runtime_t`    | `src/runtime/factory.c`   | `AGENTS.md` 7.5        |
| Peripheral      | `hu_peripheral_t` | `src/peripherals/`        | `AGENTS.md` 7.4        |

Pattern for every extension:

1. Implement the vtable struct with `void *ctx` + function pointers
2. Register in the module's `factory.c`
3. Callers OWN the implementing struct — never return a vtable pointing to a temporary

## ML Subsystem

Location: `src/ml/`, `include/human/ml/`. Gated behind `HU_ENABLE_ML`.

Components: BPE tokenizer, GPT model, MuonAdamW optimizer, data loader, training loop, experiment loop. Config-driven: `hu_experiment_config_t` parameterizes model architecture and optimizer. Same vtable-driven approach as the rest of the project (`hu_model_t`, `hu_ml_optimizer_t`).

## HuLa (agent program IR)

HuLa is a JSON program tree for tool orchestration (`call`, `seq`, `par`, `branch`, `loop`, `delegate`, `emit`) executed by `src/agent/hula.c` with policy checks and trace logging. **Compiler mode** (`hu_hula_compiler_chat_compile_execute` in `hula_compiler.c`), **native mode** (`<hula_program>` in assistant text), and a **direct IR** path from tool calls integrate in `agent_turn.c` when `agent.hula_enabled` is set (default **on** after config merge). **`delegate`** uses `hu_hula_exec_set_spawn` with the session `agent_pool` and `hu_spawn_config_apply_parent_agent` when a pool exists (POSIX non-test). Traces can persist under `~/.human/hula_traces/`; `hula_emergence.c` scans n-grams and can promote hot paths to SkillForge-style manifests. Operator guide: `docs/guides/hula.md`. Full spec: `docs/plans/2026-03-22-hula-language.md`.

## Voice pipeline

Realtime and streaming voice layers add **semantic end-of-turn** detection (`semantic_eot.c`, Phoenix-VAD–style), **LLM turn signals** (`turn_signal.c` — yield/hold/backchannel tokens), and **emotion-aware TTS**: **emotion–voice mapping** (`emotion_voice_map.c`) plus **audio emotion** from STT-side features (`audio_emotion.c`), alongside existing session, WebRTC, and streaming TTS paths.

## Memory

Beyond vector and graph recall, a **multi-dimensional graph index** (`graph_index.c`, MAGMA-inspired) supports **spreading activation**–style linkage and reranking; **entropy-based gating** (`entropy_gate.c` under `memory/retrieval/`) filters low-signal chunks before they enter context.

## Security

Tooling adds **deep argument inspection** (`arg_inspector.c`, AEGIS-style) before policy decisions, and **chain-of-thought audit** (`cot_audit.c`) to score reasoning traces for misuse patterns.

## Agent

The loop can **deduplicate system prompts** via a **prompt cache** (`prompt_cache.c`), reuse **cross-turn tool results** with **TTL** (`tools/cache_ttl.c`), exchange messages through **ACP** (`agent_comm.c`) and an **ACP–mailbox bridge** (`acp_bridge.c`), prune **KV cache** entries with attention-aware policy (`kv_cache.c`), and apply **PersonaFuse** per-channel overlays (`persona_fuse.c`) without reloading full personas.

## Humanness Frontiers

Twelve subsystems that close the gap from "AI with personality" to "genuinely feels like a person." All follow the same pattern: C struct for state, per-turn compute, prompt builder, injection into `agent_turn.c` via `hu_prompt_config_t`.

```mermaid
flowchart LR
    subgraph compute [Per-Turn Compute]
        A[Somatic] --> B[Presence]
        B --> C[Attachment]
        C --> D[Rupture-Repair]
        D --> E[Micro-Expression]
    end
    subgraph memory [Memory-Backed]
        F[Narrative Self]
        G[Relational Episodes]
        H[Growth Narrative]
        I[Genuine Boundaries]
        J[Novelty Detection]
    end
    subgraph delivery [Post-Generation]
        K[Message Choreography]
        L[Creative Voice]
    end
    compute --> Prompt["System Prompt"]
    memory --> Prompt
    L --> Prompt
    Prompt --> LLM
    LLM --> K
    K --> Channel["channel→send"]
```

| Feature | Module | Purpose |
| ------- | ------ | ------- |
| Somatic State | `persona/somatic.c` | Energy, social battery, focus — day-to-day variation |
| Message Choreography | `agent/choreography.c` | Multi-message delivery with human rhythm |
| Narrative Self | `persona/narrative_self.c` | Persistent "who I am" identity narrative |
| Novelty Detection | `cognition/novelty.c` | Authentic surprise on genuinely new info |
| Attachment Dynamics | `cognition/attachment.c` | Bowlby/Ainsworth relationship modeling |
| Rupture-Repair | `cognition/rupture_repair.c` | Detect and repair conversational missteps |
| Relational Episodes | `memory/relational_episode.c` | Memories tagged with relational texture |
| Presence Gradient | `cognition/presence.c` | Variable attention depth from casual to deep |
| Micro-Expression | `persona/micro_expression.c` | Dynamic style modulation (length, punctuation, emoji) |
| Creative Voice | `persona/creative_voice.c` | Persona-shaped metaphors and worldview |
| Growth Narrative | `agent/growth_narrative.c` | Track and surface relationship evolution |
| Genuine Boundaries | `persona/genuine_boundaries.c` | Value-driven personal stances |

## Humanness Context

Per-turn humanness context (`src/agent/humanness.c`, `include/human/agent/humanness.h`) builds a voice/style snapshot for the current turn. Integrated in `hu_agent_turn` — built fresh each turn, freed on next turn or agent deinit. Includes voice profile adaptation based on conversation history.

## Apple Intelligence Provider

`src/providers/apple.c` integrates Apple Foundation Models via the on-device inference API. Tool call parsing, streaming support, and graceful fallback when not running on Apple Silicon. Registered in `src/providers/factory.c`.

## On-Device Server

`apps/shared/HumanKit/Sources/HumanOnDeviceServer/` provides a localhost-only HTTP server exposing an OpenAI-compatible `/v1/chat/completions` endpoint for on-device inference. Bearer token authentication required. Loopback-only binding. Request size capped at 10MB.

## Music Teasers

`src/multimodal/music.c` and `src/context/conversation.c` implement proactive music sharing. The daemon detects music-related conversation context, prompts the LLM for song suggestions, validates Apple Music/Spotify URLs, and sends them through the channel. Subject to moderation checks.

## Paperclip Integration

`src/paperclip/` provides a client for the Paperclip task management API. Includes checkout, update, comment, and list operations with JSON-escaped payloads and authenticated requests.

## Evaluation

**Turing-style scoring** (`turing_score.c`) implements an **18-dimension** heuristic (S2S taxonomy) for naturalness and conversational quality, complementing the broader eval harness in `src/eval/eval.c` and adversarial suites.

## Key Directories

```
src/
  agent/       Agent loop, planner, DAG, context, prompt assembly
  channels/    31 messaging channel implementations
  providers/   50+ AI provider implementations
  tools/       87 tool implementations
  memory/      Memory engines, retrieval, vector, lifecycle
  ml/          ML training (BPE, GPT, MuonAdamW, experiment loop) — HU_ENABLE_ML
  security/    Policy, pairing, secrets, 11 sandbox backends
  gateway/     HTTP/WebSocket server, JSON-RPC control protocol
  persona/     Persona profiles, prompt builder, examples, adaptation
  runtime/     Native, Docker, WASM, Cloudflare, GCE adapters
  core/        Allocator, arena, JSON, string, HTTP, error

include/human/  Public C headers (vtable definitions)
tests/          425+ test files, 8,900+ tests
fuzz/           33 libFuzzer harnesses
ui/             LitElement web dashboard
website/        Astro marketing site
apps/           iOS, macOS, Android native apps + shared HumanKit
```

## Build Targets

| Preset    | Purpose                          | Command                  |
| --------- | -------------------------------- | ------------------------ |
| `dev`     | Development (ASan, all features) | `cmake --preset dev`     |
| `test`    | Testing (no ASan, fast)          | `cmake --preset test`    |
| `release` | Production (MinSizeRel + LTO)    | `cmake --preset release` |
| `fuzz`    | Fuzz testing (Clang, libFuzzer)  | `cmake --preset fuzz`    |
| `minimal` | CLI-only, no optional deps       | `cmake --preset minimal` |

## Performance Profile

| Metric                | Measured       |
| --------------------- | -------------- |
| Binary size (release) | ~2075 KB       |
| Cold start            | 4-27 ms        |
| Peak RSS              | ~5.7 MB        |
| Test throughput       | 700+ tests/sec |
