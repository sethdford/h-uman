# vendored sqlite3

This directory vendors the SQLite 3.51.0 amalgamation sources used by Human.

Why vendored:

- deterministic builds across environments
- no external downloads during build
- integrity enforced by the CMake build system

Contents:

- `sqlite3.c`, `sqlite3.h`, `sqlite3ext.h` from SQLite 3.51.0 amalgamation

Upstream reference:

- SQLite source: https://sqlite.org/2025/sqlite-amalgamation-3510000.zip
