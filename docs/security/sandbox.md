---
title: Sandbox Isolation System
description: Multi-tier sandbox for isolating child processes spawned by the agent
updated: 2026-03-02
---

# Sandbox Isolation System

human provides a multi-tier sandbox system for isolating child processes spawned by the agent.
The system is designed to be cross-platform, composable, and secure-by-default.

## Quick Start

```json
// ~/.human/config.json
{
  "security": {
    "sandbox": "auto",
    "sandbox_config": {
      "enabled": true,
      "net_proxy": {
        "enabled": true,
        "deny_all": true,
        "allowed_domains": ["api.openai.com", "api.anthropic.com"]
      }
    }
  }
}
```

Check what's available on your system:

```bash
human sandbox
```

## Backends

### Tier 1: OS-native kernel sandboxes (near-zero overhead)

| Backend          | OS          | Mechanism                                  | Config value         |
| ---------------- | ----------- | ------------------------------------------ | -------------------- |
| Seatbelt         | macOS       | Sandbox profiles (SBPL) via `sandbox-exec` | `"seatbelt"`         |
| Landlock         | Linux 5.13+ | Kernel LSM filesystem ACLs                 | `"landlock"`         |
| seccomp-BPF      | Linux       | Syscall filtering via BPF                  | `"seccomp"`          |
| Landlock+seccomp | Linux       | Combined FS ACLs + syscall filtering       | `"landlock+seccomp"` |
| AppContainer     | Windows     | Job Objects + capability-based isolation   | `"appcontainer"`     |

### Tier 2: User-space namespace sandboxes

| Backend    | OS    | Mechanism                 | Config value   |
| ---------- | ----- | ------------------------- | -------------- |
| Bubblewrap | Linux | User namespaces (`bwrap`) | `"bubblewrap"` |
| Firejail   | Linux | User namespaces + seccomp | `"firejail"`   |

### Tier 3: Virtualization

| Backend     | OS                     | Mechanism                                 | Config value    |
| ----------- | ---------------------- | ----------------------------------------- | --------------- |
| Firecracker | Linux (KVM)            | Hardware-isolated microVM, sub-200ms boot | `"firecracker"` |
| Docker      | Any (Docker installed) | Container isolation                       | `"docker"`      |

### Tier 4: Cross-platform capability-based

| Backend | OS                    | Mechanism                      | Config value |
| ------- | --------------------- | ------------------------------ | ------------ |
| WASI    | Any (wasmtime/wasmer) | WebAssembly capability sandbox | `"wasi"`     |

## Auto-detection

When `"sandbox": "auto"` (the default), human selects the strongest available backend:

**macOS**: Seatbelt → Docker → WASI → noop

**Linux**: Landlock+seccomp → Bubblewrap → Firejail → Firecracker → Docker → WASI → noop

**Windows**: AppContainer → WASI → noop

## How It Works

Sandbox backends implement the `hu_sandbox_vtable_t` interface with two isolation mechanisms:

1. **`wrap_command`** — Prefixes the child argv with a sandbox tool
   (e.g., `sandbox-exec -p <profile> ...`, `bwrap --ro-bind ...`, `docker run ...`).
   Used by: Seatbelt, Bubblewrap, Firejail, Docker, WASI, Firecracker.

2. **`apply`** — Called in the child process after `fork()`, before the command runs.
   Applies kernel-level restrictions programmatically. Used by: Landlock, seccomp, Landlock+seccomp, AppContainer.

Both mechanisms can be active simultaneously (e.g., Landlock+seccomp uses `apply` for kernel
restrictions and passes through `wrap_command`).

## Network Proxy

The network proxy is an independent layer that composes with any sandbox backend.
When enabled, it sets `HTTP_PROXY`/`HTTPS_PROXY` environment variables in the child process
to route traffic through a filtering proxy. Allowed domains bypass the proxy via `NO_PROXY`.

```json
{
  "security": {
    "sandbox_config": {
      "net_proxy": {
        "enabled": true,
        "deny_all": true,
        "proxy_addr": "http://127.0.0.1:0",
        "allowed_domains": [
          "api.openai.com",
          "api.anthropic.com",
          "*.example.com"
        ]
      }
    }
  }
}
```

Wildcard subdomain matching is supported: `"*.example.com"` matches `api.example.com`,
`sub.example.com`, etc.

When `deny_all` is true and no `proxy_addr` is specified, a dead proxy (`127.0.0.1:0`) is used,
effectively blocking all HTTP/HTTPS traffic except to allowed domains.

## Composition

The sandbox and network proxy are independent and stack automatically:

- **Seatbelt + net proxy**: macOS kernel FS isolation + domain-based network filtering
- **Landlock+seccomp + net proxy**: Linux kernel FS + syscall isolation + network filtering
- **Docker + net proxy**: Container isolation + domain allowlist
- **WASI + net proxy**: Capability sandbox + network filtering

The spawn path applies them in order:

1. `wrap_command` transforms the argv (sandbox tool prefix)
2. In the child process after `fork()`:
   a. Network proxy env vars are set (`HTTP_PROXY`, `HTTPS_PROXY`, `NO_PROXY`)
   b. Sandbox `apply` callback runs (kernel restrictions)
3. `execvp` runs the sandboxed command

## Config Reference

```json
{
  "security": {
    "sandbox": "auto|none|seatbelt|landlock|seccomp|landlock+seccomp|bubblewrap|firejail|firecracker|docker|wasi|appcontainer",
    "sandbox_config": {
      "enabled": true,
      "backend": "auto",
      "firejail_args": ["--private-dev", "--nogroups"],
      "net_proxy": {
        "enabled": false,
        "deny_all": true,
        "proxy_addr": "http://127.0.0.1:0",
        "allowed_domains": ["api.openai.com"]
      }
    }
  }
}
```

## CLI

```bash
human sandbox          # Show sandbox status, available backends, active config
```

Output includes:

- Current configuration (backend, enabled state)
- Available backends on this OS with availability status
- Active sandbox details (name, type, description)
- Network proxy configuration
