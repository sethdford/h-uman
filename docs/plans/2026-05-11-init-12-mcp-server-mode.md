---
title: "Init 12 — MCP Server Mode (h-uman as the persona layer for every other agent)"
created: 2026-05-11
status: design done
parent: 2026-05-11-sota-2026-massive-team-program.md
related:
  - 2026-05-11-sota-2026-massive-team-program.md
  - 2026-05-10-master-follow-through-program.md
  - 2026-05-10-sota-roadmap-6mo.md
  - 2026-05-11-init-09-memory-trust-tiers.md
  - ../../CLAUDE.md
  - ../../AGENTS.md
  - ../guides/claude-code-features.md
  - ../standards/security/threat-model.md
  - ../standards/engineering/gateway-api.md
  - ../standards/engineering/principles.md
  - ../standards/engineering/naming.md
external_refs:
  - "MCP specification — https://modelcontextprotocol.io/spec (rev 2025-06-18)"
  - "Anthropic, ‘Model Context Protocol — open-sourcing the integration plumbing’, Nov 2024"
  - "Anthropic, ‘MCP for desktop and IDEs — Cursor, Claude Code, Copilot integration patterns’, March 2026"
  - "OWASP ASVS v4.0.3 §V13 (API and Web Service) for the TCP-exposed surface"
  - "RFC 6749 §1.4 (Bearer token usage) — applied to MCP `Authorization: Bearer` header"
---

# Init 12 — MCP Server Mode

> **One-line.** The h-uman daemon already runs a gateway. Add an MCP server endpoint
> that exposes the user's persona + (consented) memory + a curated subset of tools
> as MCP resources / tools / prompts. Cursor, Claude Code, GitHub Copilot, OpenAI
> Operator etc. become **clients** of h-uman. We flip from "another assistant"
> to "the persona layer that **every** assistant the user already runs subscribes to."

## 0. Why now

| Factor | Evidence |
|---|---|
| MCP is the de-facto standard | All four major IDE-coding agents (Cursor, Claude Code, Copilot in 1.99+, Operator preview) speak MCP natively as of April 2026 |
| We already have the **client** half | `docs/guides/claude-code-features.md` §1 ships h-uman as an MCP client; the protocol code, JSON-RPC dispatcher, transport, and `mcp_resources.h` registry already exist in-tree |
| Reverses our integration polarity | Today we lobby external agents to integrate us. As an MCP server we make ourselves trivial to consume; users self-integrate |
| Guards the moat | Persona-as-compiled-architecture (CLAUDE.md M1 row) is *only* a moat if downstream consumers can actually pick it up. MCP is the cheapest path |
| Cost of waiting | OpenClaw's SOUL.md plugins already have a parallel "expose persona" story (issue #2841 in their tracker, March 2026). If we don't ship MCP-server first, the persona-layer slot gets filled by an inferior implementation we then have to displace |

## 1. The hard truth about what already exists

A pre-existing partial implementation lives in-tree. Pretending it doesn't would
waste budget and produce naming collisions. The honest map:

| File | Status | Role in this design |
|---|---|---|
| `include/human/mcp_server.h` | Stub: declares `hu_mcp_host_t` (a non-vtable concrete type) | **Replaced**: the file path is reused for the new vtable surface; the old `hu_mcp_host_t` type is renamed to `hu_mcp_engine_t` and demoted to internal `include/human/mcp/engine.h` |
| `src/mcp_server.c` (`hu_mcp_host_*` family) | Working stdio JSON-RPC dispatcher, ~657 LOC | **Kept and refactored.** Becomes the *engine* the new vtable wraps. ~80 LOC delta to extract `hu_mcp_engine_handle_request(engine, req_json, peer, out)` so a policy layer can wrap each call |
| `include/human/mcp.h` (`hu_mcp_server_t`) | Pre-existing struct that is **actually a client connection** to an external MCP server (badly named) | **Renamed in M0**: `hu_mcp_server_t → hu_mcp_client_t` with one-release `typedef ... HU_DEPRECATED` shim so the symbol is freed for our new vtable |
| `src/mcp_transport_stdio.c` / `mcp_transport_http.c` / `mcp_transport_sse.c` | Working transports for the **client** | **Reused as-is.** New code adds a fourth transport (TCP-listener) on the **server** side; keeps existing client transports untouched |
| `src/security/mcp_audit.c` (`hu_mcp_audit_*`) | Static-analysis pass over **incoming** tool descriptions (red-team helper, not a wire-audit log) | **Kept unchanged.** New audit log uses the distinct `hu_mcp_server_audit_*` namespace to avoid confusion |
| `src/main.c::cmd_mcp` | Currently runs the stdio engine in pass-through mode with no consent / pairing / audit | **Replaced.** Becomes a subcommand dispatcher (`serve`, `peers`, `consent`, `audit`, `pair`) |

Bottom line: **we are not building from scratch; we are putting policy, transport,
and CLI scaffolding around code that already parses JSON-RPC correctly.** That's
how this initiative fits in 56 KB.

## 2. C-API surface (D1)

### 2.1 Public vtable — `include/human/mcp_server.h` (replaces stub)

```c
#ifndef HU_MCP_SERVER_H
#define HU_MCP_SERVER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/mcp_resources.h"
#include "human/tool.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_json_value;

/* Identity of the connected peer; passed through every vtable call so the
 * implementation can apply per-peer consent, rate limits, and audit. The
 * pointer is borrowed; the server owns the storage. */
typedef struct hu_mcp_peer {
    char id[40];           /* "<client_name>:<sha256[:16]>" — never the raw token */
    char client_name[64];  /* e.g. "cursor", "claude-code", "copilot" */
    char client_version[32];
    bool paired;           /* false for stdio subprocess; true after TCP pair */
    int64_t paired_at;     /* unix epoch */
} hu_mcp_peer_t;

/* Vtable. Each method receives the peer so consent / rate-limit / audit is
 * uniform. Methods mirror the MCP spec one-to-one; resource subscription
 * uses `subscribe`/`unsubscribe` plus an out-of-band `notify_changed`
 * the implementation calls when the underlying resource mutates. */
typedef struct hu_mcp_server_vtable {
    hu_error_t (*list_resources)(void *ctx, hu_allocator_t *alloc,
                                 const hu_mcp_peer_t *peer,
                                 hu_mcp_resource_t **out, size_t *out_count);

    hu_error_t (*read_resource)(void *ctx, hu_allocator_t *alloc,
                                const hu_mcp_peer_t *peer, const char *uri,
                                hu_mcp_resource_content_t *out);

    hu_error_t (*list_tools)(void *ctx, hu_allocator_t *alloc,
                             const hu_mcp_peer_t *peer,
                             hu_tool_t **out, size_t *out_count);

    hu_error_t (*call_tool)(void *ctx, hu_allocator_t *alloc,
                            const hu_mcp_peer_t *peer, const char *name,
                            const struct hu_json_value *args,
                            hu_tool_result_t *out);

    hu_error_t (*list_prompts)(void *ctx, hu_allocator_t *alloc,
                               const hu_mcp_peer_t *peer,
                               hu_mcp_prompt_t **out, size_t *out_count);

    hu_error_t (*get_prompt)(void *ctx, hu_allocator_t *alloc,
                             const hu_mcp_peer_t *peer, const char *name,
                             const struct hu_json_value *args,
                             char **out_text, size_t *out_len);

    hu_error_t (*subscribe)(void *ctx, const hu_mcp_peer_t *peer,
                            const char *uri);
    hu_error_t (*unsubscribe)(void *ctx, const hu_mcp_peer_t *peer,
                              const char *uri);

    void (*destroy)(void *ctx);
} hu_mcp_server_vtable_t;

typedef struct hu_mcp_server {
    void *ctx;
    const hu_mcp_server_vtable_t *vtable;
} hu_mcp_server_t;

/* Notification side-channel. Called by the implementation; the server
 * fans out `notifications/resources/updated` to every peer that
 * subscribed to `uri`. */
hu_error_t hu_mcp_server_notify_changed(hu_mcp_server_t *srv, const char *uri);

/* ── Default implementation factory ────────────────────────────────── */

struct hu_persona;
struct hu_personal_model;
struct hu_memory;
struct hu_pairing_guard;

typedef struct hu_mcp_server_options {
    const struct hu_persona *persona;            /* required */
    const struct hu_personal_model *personal;    /* nullable; gates memory:// resources */
    struct hu_memory *memory;                    /* nullable; gates memory:// resources */
    hu_tool_t *tools;                            /* nullable; gates tools the server may expose */
    size_t tool_count;
    struct hu_pairing_guard *pairing;            /* nullable for stdio; required for TCP */
    const char *consent_path;                    /* default "~/.human/mcp_consent.json" */
    const char *audit_path;                      /* default "~/.human/private/mcp_audit.log" */
    const char *peers_path;                      /* default "~/.human/mcp_peers.json" */
    uint32_t default_rate_per_sec;               /* default 10 */
    uint32_t default_rate_burst;                 /* default 20 */
    bool require_pairing;                        /* default true (false only when transport=stdio) */
} hu_mcp_server_options_t;

hu_error_t hu_mcp_server_default_create(hu_allocator_t *alloc,
                                        const hu_mcp_server_options_t *opts,
                                        hu_mcp_server_t *out);

void hu_mcp_server_destroy(hu_mcp_server_t *srv);

/* Run a single-connection, single-thread loop on the given transport. */
struct hu_mcp_transport;  /* existing struct from human/mcp_transport.h */
hu_error_t hu_mcp_server_run_transport(hu_mcp_server_t *srv,
                                       struct hu_mcp_transport *transport,
                                       const hu_mcp_peer_t *peer);

/* Run a TCP-listener loop. POSIX only (HU_GATEWAY_POSIX). Blocks until SIGTERM
 * or hu_mcp_server_request_stop. */
hu_error_t hu_mcp_server_run_tcp(hu_mcp_server_t *srv, const char *bind_host,
                                 uint16_t port);

void hu_mcp_server_request_stop(hu_mcp_server_t *srv);

#endif /* HU_MCP_SERVER_H */
```

Naming conformance: `hu_mcp_server_*` follows `docs/standards/engineering/naming.md`
(snake_case, `hu_<module>_<action>`, type `hu_<name>_t`).

### 2.2 Consent surface — `include/human/mcp/consent.h` (new)

```c
typedef enum hu_mcp_consent_decision {
    HU_MCP_CONSENT_DENY,
    HU_MCP_CONSENT_ALLOW,
    HU_MCP_CONSENT_PROMPT  /* CLI-only; daemon treats as DENY with audit-warning */
} hu_mcp_consent_decision_t;

typedef struct hu_mcp_consent hu_mcp_consent_t;

hu_error_t hu_mcp_consent_load(hu_allocator_t *alloc, const char *path,
                               hu_mcp_consent_t **out);
void hu_mcp_consent_destroy(hu_mcp_consent_t *consent);

/* Path-style match: exact > longest-prefix glob > default_policy.
 * peer may be NULL (apply default rules). Returns DENY when consent==NULL. */
hu_mcp_consent_decision_t hu_mcp_consent_resource(const hu_mcp_consent_t *consent,
                                                  const hu_mcp_peer_t *peer,
                                                  const char *uri);
hu_mcp_consent_decision_t hu_mcp_consent_tool(const hu_mcp_consent_t *consent,
                                              const hu_mcp_peer_t *peer,
                                              const char *tool_name);

/* Returns the per-peer rate-limit override or default (rps, burst). */
void hu_mcp_consent_rate_limit(const hu_mcp_consent_t *consent,
                               const hu_mcp_peer_t *peer,
                               uint32_t *out_rps, uint32_t *out_burst);

/* Programmatic edit + atomic save (used by `human mcp consent edit` and tests). */
hu_error_t hu_mcp_consent_set_resource(hu_mcp_consent_t *consent,
                                       const char *uri,
                                       hu_mcp_consent_decision_t decision);
hu_error_t hu_mcp_consent_save(const hu_mcp_consent_t *consent,
                               const char *path);

/* Default consent built from an empty file. Identity-shaped persona fields
 * are allowed; everything else (memory, contacts, goals, banks, tools) is
 * deny by default. Used to seed `~/.human/mcp_consent.json` on first run. */
void hu_mcp_consent_seed_defaults(hu_mcp_consent_t *consent);
```

### 2.3 Audit log surface — `include/human/mcp/server_audit.h` (new)

```c
typedef struct hu_mcp_server_audit hu_mcp_server_audit_t;

hu_error_t hu_mcp_server_audit_open(hu_allocator_t *alloc, const char *path,
                                    size_t rotate_bytes, size_t generations,
                                    hu_mcp_server_audit_t **out);
void hu_mcp_server_audit_close(hu_mcp_server_audit_t *log);

/* One NDJSON line per call. Body bytes are NEVER logged — only
 * size, latency, peer, method, uri, error code. */
hu_error_t hu_mcp_server_audit_record(hu_mcp_server_audit_t *log,
                                      const hu_mcp_peer_t *peer,
                                      const char *method,
                                      const char *uri_or_name,  /* may be NULL */
                                      bool ok,
                                      int rpc_error_code,       /* 0 if ok */
                                      size_t response_bytes,
                                      uint64_t latency_us);
```

### 2.4 Discovery surface — `include/human/mcp/discovery.h` (new)

```c
typedef struct hu_mcp_discovery {
    const char *transport;          /* "stdio" or "tcp" */
    const char *host;               /* "127.0.0.1" for tcp; ignored for stdio */
    uint16_t port;
    const char *stdio_command;      /* abs path to `human` binary */
    const char *const *stdio_args;
    size_t stdio_args_count;
    bool pairing_required;
    const char *const *advertised_resources;
    size_t advertised_resources_count;
    const char *const *advertised_tools;
    size_t advertised_tools_count;
} hu_mcp_discovery_t;

hu_error_t hu_mcp_discovery_write(hu_allocator_t *alloc, const char *path,
                                  const hu_mcp_discovery_t *info);
```

## 3. Files to create / modify (D2)

### 3.1 New files

| # | Path | Est. LOC | Purpose |
|---|---|---|---|
| 1 | `include/human/mcp_server.h` | 90 | Vtable + factory + transport runner (replaces stub) |
| 2 | `include/human/mcp/engine.h` | 30 | Internal-only re-export of the existing `hu_mcp_host_t` engine, renamed `hu_mcp_engine_t` |
| 3 | `include/human/mcp/consent.h` | 60 | Consent file API (above) |
| 4 | `include/human/mcp/server_audit.h` | 35 | Audit log API (above) |
| 5 | `include/human/mcp/discovery.h` | 35 | Discovery file writer (above) |
| 6 | `include/human/mcp/peer.h` | 25 | `hu_mcp_peer_t` (re-exported from `mcp_server.h` for internal callers) |
| 7 | `src/mcp/server.c` | 600 | Default vtable impl: wraps engine, applies consent, writes audit, enforces rate limit |
| 8 | `src/mcp/server_consent.c` | 320 | Consent load/save/match (JSON, glob `*` suffix only — no full glob to keep simple) |
| 9 | `src/mcp/server_persona_resource.c` | 280 | Materializes `human://persona/*` URIs from `hu_persona_t` + `hu_personal_model_t` |
| 10 | `src/mcp/server_audit.c` | 220 | NDJSON writer with size-based rotation |
| 11 | `src/mcp/server_rate_limit.c` | 170 | Token-bucket rate limiter built on `hu_rate_tracker_t` for the per-peer key |
| 12 | `src/mcp/server_discovery.c` | 150 | `~/.human/mcp_server.json` writer + small Cursor / Claude-Code installer hooks |
| 13 | `src/mcp/server_transport_tcp.c` | 380 | TCP-listener loop reusing `src/gateway/` socket primitives + framing per MCP spec (Content-Length headers) |
| 14 | `src/mcp/cli.c` | 410 | Subcommand dispatcher (`serve`, `peers list/revoke`, `consent show/edit`, `audit tail`, `pair`) |
| 15 | `tests/test_mcp_server_consent.c` | 240 | Consent decision matrix |
| 16 | `tests/test_mcp_server_pairing.c` | 200 | Pairing flow + 401 path |
| 17 | `tests/test_mcp_server_audit.c` | 180 | Rotation + body-bytes-not-logged |
| 18 | `tests/test_mcp_server_rate_limit.c` | 160 | Per-peer rps + burst |
| 19 | `tests/test_mcp_server_persona_resource.c` | 220 | Persona → MCP URI round-trip |
| 20 | `tests/test_mcp_server_tcp_e2e.c` | 280 | End-to-end TCP handshake + resources/list + resources/read |
| 21 | `tests/test_mcp_server_discovery.c` | 120 | Discovery JSON shape + installer dry-run |
| 22 | `tests/test_mcp_server_jsonrpc_dispatch.c` | 200 | Method routing, malformed input, notifications-vs-requests |
| 23 | `fuzz/fuzz_mcp_server_jsonrpc.c` | 80 | libFuzzer harness over `hu_mcp_engine_handle_request` |

**New code total:** ≈ 4 240 LOC (3 100 product + 1 140 test + 80 fuzz).

### 3.2 Files modified

| # | Path | Δ LOC | Why |
|---|---|---|---|
| M1 | `src/mcp_server.c` | +60 / −10 | Extract `hu_mcp_engine_handle_request(engine, req_json, peer, out_response)`; rename type `hu_mcp_host_t → hu_mcp_engine_t` (typedef shim left behind for one release) |
| M2 | `include/human/mcp.h` | +6 | `hu_mcp_server_t → hu_mcp_client_t` rename + `typedef` shim with `HU_DEPRECATED` |
| M3 | `src/mcp.c`, `src/mcp_manager.c`, `src/mcp_tool_wrapper.c`, `src/mcp_registry.c` | +0 / +12 lines total | Update all `hu_mcp_server_t` call sites to `hu_mcp_client_t` (compat shim makes this gradual) |
| M4 | `src/main.c::cmd_mcp` | +30 / −60 | Replace inline body with call to new `hu_mcp_cli_run` from `src/mcp/cli.c`; preserve the legacy stdio behavior as the implicit `serve --transport=stdio` default so `human mcp` (one-arg form) keeps working for existing IDE configs |
| M5 | `CMakeLists.txt` | +15 | Add the 9 new sources; add `HU_ENABLE_MCP_SERVER_TCP` option (default `ON`) gating `server_transport_tcp.c` and the TCP test for the embedded build |
| M6 | `src/security/pairing.c` | +0 | Reused unmodified — the new flow constructs a fresh `hu_pairing_guard_t` per server |
| M7 | `src/gateway/cp_admin.c` (`metrics.snapshot`) | +12 | Add four counters: `mcp_server.connections`, `.requests`, `.audit_bytes`, `.consent_denials` |
| M8 | `ui/src/demo-gateway.ts` | +20 | Mirror new metrics fields per `.cursor/rules/gateway-protocol.mdc` |
| M9 | `~/.human/mcp_consent.json` schema | new | Documented in §4.3 below |
| M10 | `docs/guides/claude-code-features.md` | +60 | Add §8 "MCP Server Mode" pointing here |
| M11 | `scripts/install-cursor-mcp.sh` | new (40 LOC) | One-command import into Cursor settings — reads `~/.human/mcp_server.json` |
| M12 | `scripts/install-claude-code-mcp.sh` | new (40 LOC) | Same for Claude Code's `~/.claude/mcp.json` |

## 4. Data flow

### 4.1 Lifecycle

```
┌────────────────────────────────────────────────────────────────────┐
│                        h-uman daemon                                │
│                                                                    │
│  ┌──────────┐   ┌────────────────────┐   ┌─────────────────────┐  │
│  │ persona  │   │ personal_model     │   │ tools (curated)     │  │
│  │ (always) │   │ (gated by consent) │   │ (per-tool consent)  │  │
│  └────┬─────┘   └─────────┬──────────┘   └──────────┬──────────┘  │
│       │                   │                         │             │
│       └─────────┬─────────┴─────────────────────────┘             │
│                 │                                                 │
│         ┌───────▼─────────────┐    ┌──────────────────────────┐   │
│         │ hu_mcp_server_t     │◄───┤ hu_mcp_consent_t         │   │
│         │ (vtable)            │    │ ~/.human/mcp_consent.json│   │
│         │                     │    └──────────────────────────┘   │
│         │ ┌─────────────────┐ │    ┌──────────────────────────┐   │
│         │ │ rate_limiter    │◄┼────┤ per-peer overrides       │   │
│         │ └─────────────────┘ │    └──────────────────────────┘   │
│         │ ┌─────────────────┐ │    ┌──────────────────────────┐   │
│         │ │ pairing_guard   │ │───►│ ~/.human/mcp_peers.json  │   │
│         │ └─────────────────┘ │    └──────────────────────────┘   │
│         │ ┌─────────────────┐ │    ┌──────────────────────────┐   │
│         │ │ audit log       │─┼───►│ ~/.human/private/        │   │
│         │ └─────────────────┘ │    │   mcp_audit.log          │   │
│         │ ┌─────────────────┐ │    └──────────────────────────┘   │
│         │ │ engine          │ │                                   │
│         │ │ (JSON-RPC)      │ │                                   │
│         │ └─────────────────┘ │                                   │
│         └─────┬───────────┬───┘                                   │
│               │           │                                       │
│       ┌───────▼─┐    ┌────▼──────────┐                            │
│       │ stdio   │    │ TCP listener  │  ← reuses src/gateway/     │
│       │ (1 peer)│    │ (N peers)     │     socket primitives      │
│       └────┬────┘    └────┬──────────┘                            │
└────────────┼──────────────┼───────────────────────────────────────┘
             │              │
             ▼              ▼
   ┌───────────────┐  ┌─────────────────────────────────────┐
   │ Cursor / IDE  │  │ Claude Code / Copilot / Operator    │
   │ (subprocess)  │  │ (peer, paired via `human mcp pair`) │
   └───────────────┘  └─────────────────────────────────────┘
```

### 4.2 Per-request flow

```
client request (JSON-RPC over transport)
    │
    ▼
[1] transport.recv() → request JSON
    │
    ▼
[2] dispatch on method:
       initialize / ping / notifications/initialized → engine direct
       resources/* / tools/* / prompts/* / *subscribe → policy gate
    │
    ▼
[3] policy gate (only for non-handshake methods)
    │
    ├─ rate_limiter.try_acquire(peer) ──► RPC error -32000 "rate limited"
    ├─ consent.lookup(peer, uri/tool) ──► RPC error -32601 "denied by consent"
    └─ allow
    │
    ▼
[4] engine.handle_request(req, peer) — existing JSON-RPC dispatcher,
       unchanged except it now receives `peer` so handlers can attribute
    │
    ▼
[5] response JSON
    │
    ▼
[6] audit.record(peer, method, uri, ok, code, bytes, latency_us)
    │
    ▼
[7] transport.send(response)
```

`notifications/resources/updated` flows the other way: `hu_mcp_server_notify_changed("human://personal-model/style")` is called from `src/memory/personal_model.c`'s save path; the server walks its per-URI subscriber set and emits a notification on each peer's transport.

### 4.3 Consent file (`~/.human/mcp_consent.json`)

```jsonc
{
  "version": 1,
  "default_policy": "deny",

  // Resource decisions. Exact match wins; otherwise longest-prefix
  // glob match; otherwise default_policy. Glob is suffix `*` only
  // (KISS — no full glob engine).
  "resources": {
    "human://persona/identity":              "allow",
    "human://persona/traits":                "allow",
    "human://persona/communication-rules":   "allow",
    "human://persona/vocabulary":            "allow",
    "human://persona/values":                "allow",
    "human://persona/decision-style":        "allow",
    "human://persona/example-bank/*":        "deny",
    "human://memory/*":                      "deny",
    "human://contacts/*":                    "deny",
    "human://goals/*":                       "deny"
  },

  // Tool decisions. Default deny; opt-in per tool name.
  "tools": {
    "persona_lookup":   "allow",
    "memory_recall":    "deny",
    "channel_send":     "deny",
    "shell":            "deny",
    "file_write":       "deny"
  },

  // Per-peer overrides. Peer ID is `client_name:sha256(token)[:16]`.
  "peer_overrides": {
    "cursor:7f3c1a2bd9e8f4ab": {
      "resources": { "human://memory/recent-style": "allow" }
    }
  },

  // Per-peer rate-limit overrides (rps, burst).
  "rate_limits": {
    "default":                      { "rps": 10, "burst": 20 },
    "cursor:7f3c1a2bd9e8f4ab":      { "rps": 50, "burst": 100 }
  }
}
```

Schema is loaded with `hu_mcp_consent_load`, validated, and on first-run-with-no-file
the daemon writes the seed defaults shown above (the six `persona/*` allows, all
else deny). The user is told *exactly* what got allowed in the daemon log:

```
[mcp-server] seeded ~/.human/mcp_consent.json
            allow: human://persona/identity
            allow: human://persona/traits
            allow: human://persona/communication-rules
            allow: human://persona/vocabulary
            allow: human://persona/values
            allow: human://persona/decision-style
            (everything else default-deny — run `human mcp consent edit` to expand)
```

### 4.4 Audit log (`~/.human/private/mcp_audit.log`)

NDJSON, one line per request:

```json
{"ts":1715478912,"peer":"cursor:7f3c1a2bd9e8f4ab","client":"cursor","method":"resources/read","uri":"human://persona/identity","ok":true,"code":0,"bytes":487,"us":1240}
{"ts":1715478913,"peer":"cursor:7f3c1a2bd9e8f4ab","client":"cursor","method":"resources/read","uri":"human://memory/recent-style","ok":false,"code":-32601,"bytes":58,"us":140}
{"ts":1715478914,"peer":"copilot:1a2b3c4d5e6f7890","client":"copilot","method":"tools/call","uri":"persona_lookup","ok":true,"code":0,"bytes":312,"us":2100}
```

What is **never** logged:

- Resource body content (only `bytes` count)
- Tool arguments (only `name` and result size)
- The pairing token (`peer.id` is `sha256(token)[:16]`, not the token itself)
- Persona text, memory content, user identity beyond the abstract peer id

Rotation: at 10 MB the live file moves to `mcp_audit.log.1`, the prior `.1`
becomes `.2`, etc., up to `.5`. Hard cap: 50 MB total. Past `.5` is deleted.

### 4.5 Pairing flow

```
1. User runs `human mcp pair` on the daemon host.
2. Daemon prints an 8-digit code, e.g. "32840197", valid 10 minutes.
3. External client (Cursor / Claude Code / etc.) connects via TCP, sends
   POST-style JSON-RPC method `pair` with `{"code":"32840197","client":"cursor","client_version":"0.42.1"}`.
4. Daemon's `hu_pairing_guard_attempt_pair` validates with constant-time compare.
5. On success the daemon returns `{"token":"zc_<64hex>", "peer_id":"cursor:7f3c1a2bd9e8f4ab"}`.
6. Client persists the token (Cursor: in its own secret store; Claude Code: in `~/.claude/mcp.json` with file-mode 0600).
7. Subsequent connects send `Authorization: Bearer zc_<...>` in the connection-init frame; daemon validates against `hu_pairing_guard_is_authenticated`.
8. After 5 failed attempts: 10 minute lockout (existing `hu_pairing_guard` semantics).
```

For **stdio** transport the IDE forks the `human mcp serve --transport=stdio`
binary as a subprocess. Since the parent process owns the daemon's lifetime
and stdin/stdout, pairing is bypassed: the parent already has full process
trust. This matches how every other MCP server handles stdio.

### 4.6 Persona → MCP resource bridge

`src/mcp/server_persona_resource.c` materializes the consented persona surface:

| URI | Source field(s) | Default consent |
|---|---|---|
| `human://persona/identity` | `hu_persona_t.identity`, `name` | allow |
| `human://persona/traits` | `traits[]`, `principles[]` | allow |
| `human://persona/communication-rules` | `communication_rules[]` | allow |
| `human://persona/vocabulary` | `preferred_vocab[]`, `avoided_vocab[]`, `slang[]` | allow |
| `human://persona/values` | `values[]` | allow |
| `human://persona/decision-style` | `decision_style` | allow |
| `human://persona/example-bank/{channel}` | per-channel `hu_persona_example_bank_t` | deny |
| `human://persona/voice-rhythm` | `voice_rhythm` | deny |
| `human://memory/recent-style` | `hu_personal_model_t.communication_style` last 50 obs | deny |
| `human://memory/topics/recent` | `hu_personal_model_t.topics[]` | deny |
| `human://memory/facts/{tier}` | tier-filtered facts | deny |
| `human://contacts/{contact_id}` | `hu_contact_profile_t` | deny |
| `human://goals/{id}` | `hu_personal_goal_t` | deny |

Returned MIME is `application/json` with a stable schema documented in
`docs/standards/engineering/gateway-api.md` §"MCP server resources".

### 4.7 Tool exposure

`hu_mcp_server_options_t.tools` is the **opt-in subset**. The daemon does
**not** automatically expose every tool the agent has. Operators construct
the list explicitly in C (or via config), and the consent file is the
second gate.

Recommended starter set for v1:

| Tool name | What it does | Default consent |
|---|---|---|
| `persona_lookup` | Returns persona-derived guidance for a draft message ("would the user say it like this?") | allow |
| `style_score` | Scores a candidate text for fidelity vs the user's style observations | allow |
| `recent_commits_style` | Returns the user's last N commit-message styles (for IDEs) | deny |
| `memory_recall` | Tier-filtered recall (only above a configurable trust threshold) | deny |

Tools with side effects on real channels (`channel_send`, `imessage_send`,
`telegram_send`, `shell`, `file_write`) are **never** exposed in v1 even with
consent allow — the flag returns `HU_ERR_NOT_SUPPORTED` from
`hu_mcp_server_default_create` and the audit log records an attempt. That's
the "not Copilot's pager" guarantee in code.

### 4.8 Discovery

`~/.human/mcp_server.json`:

```json
{
  "version": 1,
  "name": "human",
  "description": "h-uman — your persona, your memory, your voice",
  "transports": [
    {
      "type": "stdio",
      "command": "/usr/local/bin/human",
      "args": ["mcp", "serve", "--transport=stdio"]
    },
    {
      "type": "tcp",
      "host": "127.0.0.1",
      "port": 8421,
      "pairing_required": true,
      "pair_command": "human mcp pair"
    }
  ],
  "advertised_resources": [
    "human://persona/identity",
    "human://persona/traits",
    "human://persona/communication-rules",
    "human://persona/vocabulary",
    "human://persona/values",
    "human://persona/decision-style"
  ],
  "advertised_tools": ["persona_lookup", "style_score"]
}
```

Installer hooks (`scripts/install-cursor-mcp.sh`, `scripts/install-claude-code-mcp.sh`)
read this file and merge it into the IDE's settings. The rule:
**we never write the IDE config silently.** The script prints what it will change
and waits for `[y/N]`.

## 5. CLI surface

```
$ human mcp serve [--transport=stdio|tcp] [--bind=127.0.0.1] [--port=8421]
$ human mcp pair                          # prints one-time code
$ human mcp peers list                    # peer_id, client, paired_at, last_seen
$ human mcp peers revoke <peer_id>        # removes from ~/.human/mcp_peers.json
$ human mcp consent show                  # pretty-print effective decisions
$ human mcp consent edit                  # opens $EDITOR; validates schema before save
$ human mcp consent allow <uri>           # one-shot edit + atomic save
$ human mcp consent deny <uri>            # one-shot edit + atomic save
$ human mcp audit tail [--lines=200]      # tail audit log; redacted for human reading
$ human mcp install cursor                # runs scripts/install-cursor-mcp.sh
$ human mcp install claude-code           # runs scripts/install-claude-code-mcp.sh
```

The single-arg legacy form `human mcp` continues to work (defaults to
`serve --transport=stdio`) so existing IDE configs that spawn `human mcp` as
a subprocess are unbroken.

## 6. Test plan (D3)

### 6.1 Unit (deterministic, all in `human_tests`)

| Suite | Test | Asserts |
|---|---|---|
| `mcp_server_consent` | `consent_default_deny_blocks_memory_when_no_file` | `hu_mcp_consent_resource(NULL,...)` → DENY |
| `mcp_server_consent` | `consent_seeded_defaults_allow_persona_six` | The exact six persona URIs, no more |
| `mcp_server_consent` | `consent_glob_suffix_matches_subpaths` | `"persona/example-bank/*"` matches `.../telegram` |
| `mcp_server_consent` | `consent_peer_override_beats_default` | Peer-scoped allow wins vs default deny |
| `mcp_server_consent` | `consent_save_atomic_on_disk_full` | Disk-full → `.tmp` blocked; prior file untouched |
| `mcp_server_pairing` | `tcp_unpaired_connect_returns_minus32000` | No `Authorization` header → RPC error |
| `mcp_server_pairing` | `stdio_subprocess_skips_pairing` | `require_pairing=false` path |
| `mcp_server_pairing` | `pair_then_connect_succeeds` | Round-trip with the existing pairing guard |
| `mcp_server_pairing` | `lockout_after_5_failures` | Reuses existing `HU_PAIR_LOCKOUT_SECS` semantics; verifies same lockout window applies to MCP |
| `mcp_server_audit` | `audit_log_rotates_at_threshold` | Synthetic 10 MB write triggers rename |
| `mcp_server_audit` | `audit_log_never_contains_resource_body` | Greps log for known persona substring; fails if found |
| `mcp_server_audit` | `audit_log_never_contains_token` | Greps log for `zc_`; fails if found |
| `mcp_server_audit` | `audit_log_records_consent_denial` | Denied call appears with code -32601 |
| `mcp_server_rate_limit` | `default_10rps_blocks_eleventh_within_one_second` | Token-bucket math correct |
| `mcp_server_rate_limit` | `per_peer_override_doubles_throughput` | Peer override overrides default |
| `mcp_server_persona_resource` | `persona_identity_round_trip_via_read_resource` | Engine output matches struct field |
| `mcp_server_persona_resource` | `personal_model_recent_style_returns_50_obs` | Bounded; oldest dropped first |
| `mcp_server_persona_resource` | `tool_with_side_effects_refused_at_construct` | `channel_send` in opts → `HU_ERR_NOT_SUPPORTED` |
| `mcp_server_jsonrpc_dispatch` | `notification_no_response_emitted` | `id`-less request: zero bytes out |
| `mcp_server_jsonrpc_dispatch` | `unknown_method_returns_minus32601` | Method-not-found |
| `mcp_server_jsonrpc_dispatch` | `malformed_json_returns_minus32700` | Parse error |
| `mcp_server_discovery` | `discovery_file_written_with_correct_endpoint` | JSON shape + advertised list |

### 6.2 Integration (the `mcp-server-e2e` suite, ~12 tests)

| Test | Flow |
|---|---|
| `tcp_handshake_lists_persona` | Pair → TCP connect → `initialize` → `resources/list` returns the six persona URIs |
| `tcp_read_persona_identity` | After above, `resources/read` returns valid JSON parseable as `hu_persona_t.identity` |
| `tcp_read_unconsented_returns_denied` | `resources/read uri=human://memory/recent-style` → -32601 |
| `tcp_subscribe_emits_notification_on_personal_model_save` | Subscribe → in-process call to `hu_personal_model_save` → notification arrives |
| `tcp_concurrent_two_peers_no_interleave` | Two peers pinning two different URIs; verify notification routing |
| `stdio_loopback_full_lifecycle` | spawn `human mcp serve --transport=stdio` in a pipe, run all six handshake steps |
| `cursor_simulator_listed_in_peers` | Test fixture impersonates Cursor; appears in `peers list` |
| `audit_log_after_e2e_has_correct_count` | After the full lifecycle, audit line count matches request count |
| `consent_edit_round_trip_e2e` | `human mcp consent allow human://memory/recent-style` → next read succeeds |
| `pairing_revoke_invalidates_token` | After revoke, the same token returns -32000 |
| `rate_limit_per_peer_e2e` | Burst over the configured peer cap → first denial within ±1 request |
| `discovery_install_dry_run_prints_diff` | `install cursor --dry-run` prints proposed Cursor settings diff and does not write |

### 6.3 Fuzz (libFuzzer, in `fuzz/`)

| Harness | Target |
|---|---|
| `fuzz_mcp_server_jsonrpc.c` | Feeds arbitrary bytes into `hu_mcp_engine_handle_request` via a stub transport. ASan + UBSan + 60-second budget per CI run. Goal: no crashes, no leaks, no reads past `len`. Reuses already-fuzzed `hu_json_parse` so the marginal compute is small |

### 6.4 Adversarial / red-team (`mcp-server-redteam` suite)

| Test | Threat |
|---|---|
| `consent_glob_cannot_promote_deny_to_allow` | Adversary writes `"human://persona/identity": "deny"` then tries glob `"human://*": "allow"` — exact match must still win |
| `peer_id_collision_resistance` | Two clients with name `cursor` get different `peer_id`s (sha256 of token differs) |
| `audit_log_truncate_does_not_lose_active_entries` | SIGKILL during write; live file is either complete-line-N or complete-line-N+1, never partial |
| `path_traversal_in_resource_uri` | `human://persona/../../etc/passwd` rejected by URI parser |
| `oversize_request_body_rejected` | >64 KB JSON-RPC body returns -32600, no allocation past 64 KB |
| `notification_storm_does_not_oom_peer` | Resource churn → outbound queue depth bounded at 64; drop-oldest with metric increment |
| `pairing_constant_time_compare` | Timing-safe per `hu_pairing_guard_constant_time_eq` (already proven in security tests) |

## 7. Risk register (D4)

| # | Risk | Severity | Mitigation |
|---|---|---|---|
| **R1** | **Persona / memory leak via too-permissive default consent.** A user who never edits the consent file shouldn't accidentally export their inner-world content to Cursor. | **High** | Default = deny except the six identity-shaped persona URIs. Memory, contacts, goals, banks, voice-rhythm all default-deny. Daemon log explicitly enumerates what was allowed on first run so the user sees it. Test `consent_seeded_defaults_allow_persona_six` is a regression guard against future drift |
| **R2** | **Naming collision.** `hu_mcp_server_t` already exists in `mcp.h` as a *client* struct. Introducing the new vtable with the same name without coordination would break every existing MCP client call site | High (correctness) | M0 precondition (Sprint preamble): rename `hu_mcp_server_t → hu_mcp_client_t`, leave `typedef hu_mcp_client_t hu_mcp_server_t HU_DEPRECATED` for one release. Tests `M3` updates all internal callers. The new vtable lands only after this rename merges. **api-contract-watcher** subagent verifies no external symbol breaks |
| **R3** | **TCP attack surface on localhost.** Any process on the user's machine can connect | High | Bind `127.0.0.1` by default; require pairing on TCP; per-peer rate limit; per-call audit; same threat model section as gateway. macOS / Linux process credential check (`SO_PEERCRED` on Linux, `LOCAL_PEERCRED` on Darwin) recorded in audit but not used as auth — pairing is the auth |
| **R4** | **Audit log fills disk on noisy peer.** A misbehaving Copilot could spam reads | Medium | 10 MB rotation × 5 generations = 50 MB hard cap. `metrics.snapshot.mcp_server.audit_bytes` exposed so the dashboard surfaces growth. Per-peer rate limit defaults to 10 rps before the audit even sees the request |
| **R5** | **Binary size blows the 56 KB budget.** | Medium | Estimated 50 KB (see §8). `HU_ENABLE_MCP_SERVER_TCP=OFF` build option drops `server_transport_tcp.c` (~8 KB) for embedded targets. Post-merge `size build-release/human` measurement is a release-gate row |
| **R6** | **Fuzz target finds a JSON-RPC parser OOB.** | Medium | Reuses already-fuzzed `hu_json_parse`. New fuzzer covers the *dispatch* layer where method names + parameter shapes vary. ASan-clean is the gate; any finding blocks merge. 60-second/run CI budget |
| **R7** | **Subscription notification storm.** A churning resource (e.g. personal-model save loop) saturates an external peer | Low | Per-peer outbound queue depth ≤ 64 (drop-oldest, increment `mcp_server.dropped_notifications`). Coalescing window: notifications for the same URI within 100 ms collapse into one |
| **R8** | **External agent expects synchronous tool execution; ours may take seconds.** | Low | Tool descriptors carry an `mcp_capabilities` extension field declaring `interactive=true` so external agents know to expect latency. Default tool exposure is `persona_lookup` / `style_score` — both <50 ms in practice |
| **R9** | **Memory poisoning via exposed memory facets** (Init #09 cross-coupling) | High | This initiative depends on Init #09 (memory trust tiers) for the `human://memory/facts/{tier}` URI to even be safe to expose. Until Init #09 ships, *all* `memory://` URIs default to deny; the consent file pre-allow list does not include any memory entry. Documented as a hard cross-init dependency in §10 |

## 8. Binary budget (D6)

| Component | LOC | Estimated KB (MinSizeRel + LTO, aarch64) |
|---|---|---|
| `src/mcp/server.c` (vtable + dispatch + policy gate) | 600 | 14 |
| `src/mcp/server_consent.c` (JSON load/save + matcher) | 320 | 7 |
| `src/mcp/server_persona_resource.c` (URI → JSON materializer) | 280 | 6 |
| `src/mcp/server_audit.c` (NDJSON + rotation) | 220 | 4 |
| `src/mcp/server_rate_limit.c` (token bucket map) | 170 | 2 |
| `src/mcp/server_discovery.c` (JSON writer + installer hook) | 150 | 3 |
| `src/mcp/server_transport_tcp.c` (gated by `HU_ENABLE_MCP_SERVER_TCP`) | 380 | 8 |
| `src/mcp/cli.c` (subcommand router, edit-in-EDITOR, atomic save) | 410 | 6 |
| Public + internal headers (≈ 275 LOC, all inline) | — | 0 |
| `src/mcp_server.c` engine refactor (rename + extract) | +60 / −10 net | 0 (no new code) |
| **Total C-side add** | **2 530 product LOC** | **≈ 50 KB** |

**Headroom against the 56 KB budget: 6 KB.** The CMake option
`HU_ENABLE_MCP_SERVER_TCP=OFF` drops the 8 KB TCP transport for embedded
targets (Arduino, RPi, STM32) where MCP server makes no sense anyway.

**Runtime RSS impact:** ≤ 200 KB peak.

| Footprint source | Bytes |
|---|---|
| `hu_mcp_consent_t` (≤ 256 entries × 320 B) | 80 KB |
| Per-peer rate-limit window (≤ 16 peers × token bucket + 64-slot ring) | 8 KB |
| Per-peer outbound notification queue (≤ 16 × 64 × 256 B) | 256 KB worst case — bounded by drop-oldest |
| Audit log ring buffer (in-memory before flush, 16 KB) | 16 KB |
| Engine reuse (no new RSS) | 0 |
| **Total peak** | **≈ 110 KB steady, ≈ 360 KB worst-case during notification storm** |

Well below the 6 MB AGENTS.md ceiling.

**Cold-start delta:** ≤ 2 ms (consent file open + parse on serve startup; lazy on
first connect for everything else).

## 9. Defer / descope condition (D7)

> **If, by the end of Sprint+1 after this initiative ships, we cannot demonstrate
> at least two concrete external-agent integrations actually consuming
> `human://persona/identity` (Cursor + one of Claude Code, GitHub Copilot, or
> OpenAI Operator), descope to stdio-only.** Concretely:
>
> - Drop `src/mcp/server_transport_tcp.c` and the `HU_ENABLE_MCP_SERVER_TCP` option.
> - Drop `human mcp peers` and `human mcp pair` subcommands.
> - Drop the TCP advertised half of `~/.human/mcp_server.json` (keep stdio).
> - Keep everything else: vtable, consent, audit, persona-resource bridge.
>
> The persona-resource + consent + audit triad is the *differentiating moat*; the
> TCP transport is convenience. Stdio alone still hits Cursor, Claude Code, and
> Copilot via the standard "spawn server as subprocess" pattern that all three
> support — we lose only the "single daemon serves N IDEs concurrently" topology.
>
> **Re-evaluation criterion** for re-introducing TCP: at least two production users
> (not the maintainer) demonstrably running multiple MCP-consuming clients
> simultaneously, asking for the daemon-of-daemons topology.
>
> A second descope axis: if the **consent UX is too painful** (users skip editing
> the consent file → the server is functionally identity-only), descope by
> shipping a `human mcp consent wizard` interactive flow that walks the user
> through each high-value memory facet with a one-line explanation. This is a
> **scope grow** in feature surface but a **scope shrink** in operator burden.

## 10. Cross-initiative coupling

| Other init | Coupling | Discharge |
|---|---|---|
| **Init #09 (memory trust tiers)** | Hard dependency for `human://memory/facts/{tier}` to be safe to expose. Without trust tiers, exposing memory at all risks leaking poisoned facts to Cursor's planner | This design defaults *all* `memory://` URIs to deny. After Init #09 ships, a follow-up adds `human://memory/facts/trusted` (tier=user-direct only) to the consent seed defaults |
| **Init #11 (proactivity + typing)** | Shares the `hu_personal_model_t` save path. If Init #11 lands first, its `proactivity_gate` writes will trigger MCP `notifications/resources/updated` — desirable but worth flagging | The notification side-channel (`hu_mcp_server_notify_changed`) is called from the existing `hu_personal_model_save` so any writer (Init #11 included) gets it for free |
| **Init #07 (ThinkPRM verifier)** | Eventually `human://persona/style-score` could be implemented by ThinkPRM. v1 keeps it as a heuristic | No coupling at v1; future enhancement |
| **Init #02 (MoLoRA channels)** | Per-channel example banks become more meaningful — each channel-specific bank is a real expert. Worth re-revisiting `human://persona/example-bank/*` default consent after Init #02 | None at v1 — banks remain default-deny |

## 11. Build sequence (phased checklist)

### Phase 0 — Precondition rename (1 PR, ≈ 1 day)

- [ ] Rename `hu_mcp_server_t → hu_mcp_client_t` in `include/human/mcp.h`
- [ ] Add `typedef hu_mcp_client_t hu_mcp_server_t HU_DEPRECATED` shim with one-release lifetime
- [ ] Update internal callers (`src/mcp.c`, `src/mcp_manager.c`, `src/mcp_tool_wrapper.c`, `src/mcp_registry.c`)
- [ ] Run **api-contract-watcher** to confirm no external surface broke
- [ ] Tests: existing MCP-client tests must continue green

### Phase 1 — Engine extraction (1 PR, ≈ 1 day)

- [ ] Rename `hu_mcp_host_t → hu_mcp_engine_t` in `src/mcp_server.c` + the existing header
- [ ] Add `hu_mcp_engine_handle_request(engine, peer_or_null, req_json, req_len, out_resp, out_len)` factoring out the per-method dispatch
- [ ] Move `hu_mcp_engine_t` declaration to internal `include/human/mcp/engine.h`
- [ ] Re-route the existing `hu_mcp_host_run` (legacy stdio loop) through the new function
- [ ] Tests: existing MCP host tests, plus a new `mcp_engine_handle_request_known_methods` regression

### Phase 2 — Consent + audit + rate-limit (1 PR, ≈ 2 days)

- [ ] `include/human/mcp/{consent,server_audit,peer}.h`
- [ ] `src/mcp/server_{consent,audit,rate_limit}.c`
- [ ] `tests/test_mcp_server_{consent,audit,rate_limit}.c`
- [ ] Atomicity test for consent save (mirrors `test_personal_model_atomic_save.c`)
- [ ] Audit log rotation test
- [ ] Audit log "never contains body" greppable regression

### Phase 3 — Vtable + default impl + persona-resource bridge (1 PR, ≈ 3 days)

- [ ] Replace `include/human/mcp_server.h` stub with the vtable + factory
- [ ] `src/mcp/{server,server_persona_resource}.c`
- [ ] `tests/test_mcp_server_{persona_resource,jsonrpc_dispatch}.c`
- [ ] Subscriber registry + `hu_mcp_server_notify_changed` hooked from `hu_personal_model_save` and `hu_persona_save`
- [ ] Ship behind `HU_ENABLE_MCP_SERVER` CMake option (default `ON`); no-op factory when off

### Phase 4 — TCP transport + pairing (1 PR, ≈ 2 days)

- [ ] `src/mcp/server_transport_tcp.c` (reuses `src/gateway/` socket primitives)
- [ ] Pairing token round-trip + `human mcp pair`
- [ ] `tests/test_mcp_server_pairing.c` + `test_mcp_server_tcp_e2e.c`
- [ ] Adversarial: lockout, oversize body, path traversal in URI

### Phase 5 — CLI + discovery + installers (1 PR, ≈ 1 day)

- [ ] `src/mcp/cli.c` (`serve`, `peers`, `consent`, `audit tail`, `pair`)
- [ ] `src/mcp/server_discovery.c` + `~/.human/mcp_server.json`
- [ ] `scripts/install-{cursor,claude-code}-mcp.sh`
- [ ] Update `src/main.c::cmd_mcp` to dispatch
- [ ] `tests/test_mcp_server_discovery.c`

### Phase 6 — Fuzz + observability + docs (1 PR, ≈ 1 day)

- [ ] `fuzz/fuzz_mcp_server_jsonrpc.c` + CMake wiring
- [ ] `metrics.snapshot.mcp_server.{connections,requests,audit_bytes,consent_denials,dropped_notifications}`
- [ ] `ui/src/demo-gateway.ts` mirror per `.cursor/rules/gateway-protocol.mdc`
- [ ] `docs/guides/claude-code-features.md` §8 cross-link
- [ ] Concept index + standards-drift script updates

### Phase 7 — End-to-end demo (sprint exit gate)

- [ ] Cursor on the maintainer's machine consumes `human://persona/identity` and renders it in the agent context inspector
- [ ] Claude Code on the same machine consumes the same URI in parallel — single daemon, two peers
- [ ] Audit log shows both peers, distinct peer IDs, ≥ 1 second of clean traffic
- [ ] `human mcp peers list` reports both
- [ ] `human mcp consent show` reports the seed defaults still in effect

## 12. References (D5)

1. **MCP Specification** — Anthropic, *Model Context Protocol*, rev `2025-06-18`. https://modelcontextprotocol.io/spec — authoritative method names, JSON-RPC framing, capability negotiation.
2. **Anthropic announcement** — *Open-sourcing Model Context Protocol*, Nov 2024. https://www.anthropic.com/news/model-context-protocol — design rationale and reference servers.
3. **Anthropic "MCP for desktop and IDEs"**, March 2026 — recommended patterns for stdio-spawned servers vs daemon-of-daemons TCP topology, including the discovery-file convention this design follows.
4. **Cursor MCP integration docs** (`docs.cursor.com/mcp`, April 2026) — settings file shape consumed by `scripts/install-cursor-mcp.sh`.
5. **Claude Code MCP integration docs** (`docs.anthropic.com/claude-code/mcp`, April 2026) — `~/.claude/mcp.json` shape.
6. **OWASP ASVS v4.0.3 §V13** — API & Web Service security verification. Used as the checklist for the TCP transport (rate-limit, auth on every method, audit of access decisions).
7. **RFC 6749 §1.4** — *Bearer Token Usage*. Defines the `Authorization: Bearer <token>` semantics applied to MCP TCP transport here.
8. **OpenClaw issue #2841** (March 2026) — competing "expose persona via plugin" design from the OpenClaw community. Justifies the time-pressure of the *Why now* section.
9. **CLAUDE.md M1 row** (in-tree) — the persona-as-compiled-architecture moat this initiative protects.
10. **`docs/standards/security/threat-model.md`** — gateway threat model the TCP transport explicitly inherits.

## 13. Synthesis hooks (back to umbrella plan)

When `2026-05-11-sota-2026-massive-team-program.md` runs its synthesis pass:

- Status row 12 → flip from `dispatched` to `design done`.
- Cross-init-API conflict to flag in the synthesis: **`hu_mcp_server_t` rename**
  collides with whatever `mcp_manager.c` callers will look like after Init #09
  adds `trust_tier` filtering. Pre-coordinate the rename in Sprint SOTA-2026-01
  preamble (Phase 0 above).
- Recommended sprint slot: **SOTA-2026-02** (after Init #09 lands so
  `human://memory/facts/trusted` becomes safe to expose). Until then ship
  persona-only (Phase 0–7), reserve memory exposure for the follow-up sprint.

---

**End of Init 12 design doc.**
