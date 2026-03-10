---
title: ADR-0001: C11 Vtable Architecture
---
# ADR-0001: C11 Vtable Architecture

- **Status**: accepted
- **Date**: 2024-01-01
- **Deciders**: core team
- **Tags**: architecture, c, vtable, extension-points

## Context

Human needed an extensible architecture that supports multiple providers, channels, tools, memory backends, and peripherals without dynamic linking or runtime reflection.

## Decision

Use C11 with vtable-driven architecture. Each extension point is a struct of function pointers with a `void *ctx` for implementation state. Factories create and register implementations.

## Consequences

### Positive

- Zero-cost abstraction with no runtime overhead
- Compile-time type safety
- Minimal binary size
- No dependency on C++ RTTI or virtual dispatch

### Negative

- More boilerplate than OOP languages
- Manual memory management required
- Dangling pointer risk if vtable points to temporary

### Neutral

- Requires explicit registration in factory functions

## Alternatives Considered

### C++ virtual classes

- **Pros**: Native OOP, inheritance, less boilerplate
- **Cons**: Binary size bloat, RTTI overhead
- **Why rejected**: Would increase binary size and add runtime reflection overhead

### Zig interfaces

- **Pros**: Compile-time polymorphism, clean syntax
- **Cons**: Zig reference implementation is archived; project is moving to C
- **Why rejected**: Zig codebase archived in `archive/zig-reference/`

### dlopen plugins

- **Pros**: Dynamic loading, hot-plugging extensions
- **Cons**: Complexity, larger security surface, platform differences
- **Why rejected**: Too complex for current scale; security-sensitive; requires dynamic linking

## References

- AGENTS.md §2 Deep Architecture Observations
- AGENTS.md §7 Change Playbooks (Adding Provider, Channel, Tool, Peripheral)
