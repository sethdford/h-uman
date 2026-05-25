---
title: Error Code Reference
generated: true
source: include/human/core/error.h
---

# Error Code Reference

All error codes are defined in `include/human/core/error.h` as `hu_error_t`. Use `hu_error_string(err)` to get a human-readable string.

## General

| Code                       | Value | When to Use                                                  |
| -------------------------- | ----- | ------------------------------------------------------------ |
| `HU_OK`                    | 0     | Success — operation completed without error                  |
| `HU_ERR_OUT_OF_MEMORY`     | 1     | `malloc`, `realloc`, or arena allocation failed              |
| `HU_ERR_INVALID_ARGUMENT`  | 2     | NULL pointer, empty string, or out-of-range parameter passed |
| `HU_ERR_NOT_FOUND`         | 3     | Requested resource (file, key, record) does not exist        |
| `HU_ERR_ALREADY_EXISTS`    | 4     | Attempted to create something that already exists            |
| `HU_ERR_NOT_SUPPORTED`     | 5     | Feature or operation not available on this platform/build    |
| `HU_ERR_PERMISSION_DENIED` | 6     | Insufficient permissions for the requested operation         |
| `HU_ERR_TIMEOUT`           | 7     | Operation exceeded its time limit                            |
| `HU_ERR_IO`                | 8     | File system, network, or device I/O failure                  |
| `HU_ERR_PARSE`             | 9     | Generic parse failure (non-JSON specific)                    |

## Config

| Code                      | Value | When to Use                                               |
| ------------------------- | ----- | --------------------------------------------------------- |
| `HU_ERR_CONFIG_INVALID`   | 10    | Config JSON is syntactically valid but semantically wrong |
| `HU_ERR_CONFIG_NOT_FOUND` | 11    | Config file does not exist at expected path               |

## Provider

| Code                           | Value | When to Use                                        |
| ------------------------------ | ----- | -------------------------------------------------- |
| `HU_ERR_PROVIDER_UNAVAILABLE`  | 12    | Provider API is unreachable or returned 5xx        |
| `HU_ERR_PROVIDER_AUTH`         | 13    | API key invalid, expired, or missing               |
| `HU_ERR_PROVIDER_RATE_LIMITED` | 14    | Provider returned 429 rate limit response          |
| `HU_ERR_PROVIDER_RESPONSE`     | 15    | Provider returned unexpected or malformed response |

## Channel

| Code                            | Value | When to Use                                                 |
| ------------------------------- | ----- | ----------------------------------------------------------- |
| `HU_ERR_CHANNEL_SEND`           | 16    | Failed to send message through channel                      |
| `HU_ERR_CHANNEL_START`          | 17    | Channel listener/webhook failed to start                    |
| `HU_ERR_CHANNEL_NOT_CONFIGURED` | 18    | Channel referenced but not configured (missing token, etc.) |

## Tool

| Code                     | Value | When to Use                                               |
| ------------------------ | ----- | --------------------------------------------------------- |
| `HU_ERR_TOOL_EXECUTION`  | 19    | Tool executed but failed (command error, API error, etc.) |
| `HU_ERR_TOOL_VALIDATION` | 20    | Tool parameters failed validation before execution        |
| `HU_ERR_TOOL_NOT_FOUND`  | 21    | Requested tool name not registered in factory             |

## Memory

| Code                    | Value | When to Use                                         |
| ----------------------- | ----- | --------------------------------------------------- |
| `HU_ERR_MEMORY_STORE`   | 22    | Failed to write to memory backend                   |
| `HU_ERR_MEMORY_RECALL`  | 23    | Failed to read from memory backend                  |
| `HU_ERR_MEMORY_BACKEND` | 24    | Memory backend initialization or connection failure |

## Security

| Code                                  | Value | When to Use                                              |
| ------------------------------------- | ----- | -------------------------------------------------------- |
| `HU_ERR_SECURITY_COMMAND_NOT_ALLOWED` | 25    | Command blocked by security policy                       |
| `HU_ERR_SECURITY_HIGH_RISK_BLOCKED`   | 26    | High-risk operation blocked (requires approval)          |
| `HU_ERR_SECURITY_APPROVAL_REQUIRED`   | 27    | Operation needs explicit user approval before proceeding |
| `HU_ERR_SECURITY_RATE_LIMITED`        | 28    | Security rate limit exceeded (too many attempts)         |
| `HU_ERR_SECURITY_LOCKOUT`             | 29    | Account/pairing locked out due to too many failures      |

## Peripheral

| Code                                 | Value | When to Use                                      |
| ------------------------------------ | ----- | ------------------------------------------------ |
| `HU_ERR_PERIPHERAL_NOT_CONNECTED`    | 30    | Hardware peripheral not detected or disconnected |
| `HU_ERR_PERIPHERAL_IO`               | 31    | Read/write to peripheral failed                  |
| `HU_ERR_PERIPHERAL_FLASH_FAILED`     | 32    | Firmware flash operation failed                  |
| `HU_ERR_PERIPHERAL_DEVICE_NOT_FOUND` | 33    | Specified device not found on any bus            |

## Tunnel

| Code                          | Value | When to Use                                               |
| ----------------------------- | ----- | --------------------------------------------------------- |
| `HU_ERR_TUNNEL_START_FAILED`  | 34    | Tunnel provider failed to start (ngrok, cloudflare, etc.) |
| `HU_ERR_TUNNEL_URL_NOT_FOUND` | 35    | Tunnel started but public URL not returned                |

## Gateway

| Code                            | Value | When to Use                                        |
| ------------------------------- | ----- | -------------------------------------------------- |
| `HU_ERR_GATEWAY_RATE_LIMITED`   | 36    | Gateway rate limit exceeded for this client        |
| `HU_ERR_GATEWAY_BODY_TOO_LARGE` | 37    | Request body exceeds maximum allowed size          |
| `HU_ERR_GATEWAY_AUTH`           | 38    | Gateway authentication failed (bad token, expired) |

## Crypto

| Code                    | Value | When to Use                                            |
| ----------------------- | ----- | ------------------------------------------------------ |
| `HU_ERR_CRYPTO_ENCRYPT` | 39    | Encryption operation failed                            |
| `HU_ERR_CRYPTO_DECRYPT` | 40    | Decryption failed (bad key, corrupted data, wrong tag) |
| `HU_ERR_CRYPTO_HMAC`    | 41    | HMAC computation or verification failed                |

## JSON

| Code                | Value | When to Use                                                             |
| ------------------- | ----- | ----------------------------------------------------------------------- |
| `HU_ERR_JSON_PARSE` | 42    | JSON syntax error during parsing                                        |
| `HU_ERR_JSON_TYPE`  | 43    | JSON value is not the expected type (e.g., expected string, got number) |
| `HU_ERR_JSON_DEPTH` | 44    | JSON nesting exceeds maximum allowed depth                              |

## Internal

| Code                       | Value | When to Use                                           |
| -------------------------- | ----- | ----------------------------------------------------- |
| `HU_ERR_INTERNAL`          | 45    | Internal error that should not happen (bug indicator) |
| `HU_ERR_SUBAGENT_TOO_MANY` | 46    | Maximum number of concurrent sub-agents exceeded      |
| `HU_ERR_CANCELLED`         | 47    | Operation was cancelled by user or system             |

## Fleet (spawn pool)

| Code                            | Value | When to Use                                                    |
| ------------------------------- | ----- | -------------------------------------------------------------- |
| `HU_ERR_FLEET_DEPTH_EXCEEDED`   | 49    | Nested spawn would exceed `agent.fleet_max_spawn_depth`        |
| `HU_ERR_FLEET_SPAWN_CAP`        | 50    | Lifetime spawn count hit `agent.fleet_max_total_spawns`        |
| `HU_ERR_FLEET_BUDGET_EXCEEDED`  | 51    | Shared session cost is at/above `agent.fleet_budget_usd`       |

`HU_ERR_LIMIT_REACHED` (value 48) sits outside the Fleet group — see "General-purpose limits" below.

## General-purpose limits

| Code                   | Value | When to Use                                            |
| ---------------------- | ----- | ------------------------------------------------------ |
| `HU_ERR_LIMIT_REACHED` | 48    | A fixed cap (tags, queue depth, etc.) was exceeded     |

## Persona (encryption-at-rest, US-8.2)

These errors surface from the persona load/save and migration paths in `src/persona/persona_crypt.c` and `src/persona/persona_io.c`. Operators see them when the on-disk persona file is in an unexpected state during migration to encrypted-at-rest format.

| Code                   | Value | When to Use                                                                          |
| ---------------------- | ----- | ------------------------------------------------------------------------------------ |
| `HU_ERR_LEGACY_REFUSED` | 52    | The refuse-path loader saw an encrypted file after the migration cutoff             |
| `HU_ERR_INVALID_FORMAT` | 53    | On-disk bytes do not match any recognized persona format (magic header mismatch)    |
| `HU_ERR_IO_BUSY`        | 54    | Atomic write target slot already exists — likely a concurrent migration in flight   |
| `HU_ERR_DECRYPT_FAILED` | 55    | libsodium MAC verification failed (wrong key, tampered ciphertext, or truncated)    |

## Usage Guidelines

- Return the most specific error code that applies (e.g., `HU_ERR_TOOL_VALIDATION` over `HU_ERR_INVALID_ARGUMENT` in tool code)
- Never invent new error codes without adding them to `error.h` — check this list first
- Use `HU_ERR_NOT_SUPPORTED` for platform-specific features on unsupported platforms (never silent 0)
- Security errors should be specific — `COMMAND_NOT_ALLOWED` vs `HIGH_RISK_BLOCKED` vs `LOCKOUT` convey different user actions
- `HU_ERR_INTERNAL` indicates a bug — investigate rather than handling gracefully

### Module-specific patterns (SQLite / normalization)

These helpers use the general codes above (`HU_ERR_IO`, `HU_ERR_INVALID_ARGUMENT`, `HU_ERR_OUT_OF_MEMORY`); behavior notes for callers:

- **`hu_sql_txn_begin`** — returns `HU_ERR_IO` when SQLite reports `SQLITE_BUSY` or other lock contention while starting a transaction.
- **`hu_sql_txn_commit`** — returns `HU_ERR_IO` on commit failure; the transaction remains active so the caller can roll back.
- **`hu_normalize_confusables`** — returns `HU_ERR_INVALID_ARGUMENT` when `input`, `out`, or `out_len` is NULL, or when `out_cap` is zero.

### Sentinel Value

`HU_ERR_COUNT` is a sentinel marking the end of the enum. It is **not** a valid error code
and must never be returned from any function. Use `0 .. HU_ERR_COUNT - 1` for valid error codes.
`hu_error_string()` returns a non-empty string for any value including invalid ones.
