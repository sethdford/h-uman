# Fuzz Testing Harnesses

Security-critical parsing paths are exercised by LibFuzzer harnesses. These harnesses ensure the JSON parser, config loader, URL encoder, base64 decoder, and tool param parsers do not crash or leak on arbitrary input.

## Prerequisites

- Clang compiler (LLVM libFuzzer is built into Clang)
- Optionally: Homebrew LLVM on macOS ARM64 for `__hash_memory` symbol resolution

## Build

Build and run fuzz targets:

```bash
cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DSC_ENABLE_FUZZ=ON -DSC_ENABLE_ALL_CHANNELS=ON
cmake --build build-fuzz -j$(nproc)

# Run a fuzzer (Ctrl+C to stop):
./build-fuzz/fuzz_json
./build-fuzz/fuzz_url_encode
./build-fuzz/fuzz_base64
./build-fuzz/fuzz_config
```

Each fuzzer creates a `crash-*` or `timeout-*` file for reproducible bugs.

On macOS ARM64 with Homebrew LLVM:

```bash
cmake -B build-fuzz -DSC_ENABLE_FUZZ=ON -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang -DSC_ENABLE_ALL_CHANNELS=ON
cmake --build build-fuzz -j$(nproc)
```

## Run

Each harness is a standalone executable. Run with a corpus directory to fuzz, or without to use ephemeral input:

```bash
# Fuzz JSON parser (simple)
./fuzz_json corpus/json/

# Fuzz JSON parser (with accessors)
./fuzz_json_parse corpus/json/

# Fuzz URL encoder
./fuzz_url_encode corpus/url/

# Fuzz base64url decoder (requires SC_ENABLE_GMAIL or SC_ENABLE_ALL_CHANNELS)
./fuzz_base64 corpus/base64/

# Fuzz config parser
./fuzz_config corpus/config/

# Fuzz tool params (report tool execute)
./fuzz_tool_params corpus/tool_params/
```

Run for a fixed duration (e.g., 60 seconds):

```bash
./fuzz_json -max_total_time=60 corpus/json/
```

## Seed Corpus

Create minimal valid inputs to bootstrap coverage:

```bash
mkdir -p corpus/json corpus/config corpus/tool_params corpus/url corpus/base64

# JSON
echo '{}' > corpus/json/empty.json
echo '{"a":1,"b":[2,3]}' > corpus/json/object_array.json
echo '"string"' > corpus/json/string.json

# Config
echo '{}' > corpus/config/empty.json
echo '{"default_provider":"openai"}' > corpus/config/minimal.json

# Tool params (report tool expects action)
echo '{"action":"template"}' > corpus/tool_params/template.json
echo '{"action":"create","title":"Test"}' > corpus/tool_params/create.json

# URL encode
echo 'hello world' > corpus/url/simple.txt
echo 'a+b=c&d=e' > corpus/url/query.txt

# Base64url
echo 'SGVsbG8gV29ybGQ' > corpus/base64/simple.txt
```

## Harnesses

| Harness            | Target                      | Description                                                          |
| ------------------ | --------------------------- | -------------------------------------------------------------------- |
| `fuzz_json`        | `sc_json_parse`             | Parses bytes as JSON                                                 |
| `fuzz_json_parse`  | `sc_json_parse` + accessors | Parses bytes as JSON, exercises object/array/string/number accessors |
| `fuzz_url_encode`  | `sc_web_search_url_encode`  | URL-encodes arbitrary bytes                                          |
| `fuzz_base64`      | `base64url_decode`          | Decodes base64url (Gmail body format)                                |
| `fuzz_config`      | `sc_config_parse_json`      | Parses bytes as config JSON in-memory                                |
| `fuzz_config_load` | `sc_config_parse_json`      | Same as fuzz_config (arena-based)                                    |
| `fuzz_tool_params` | `report` tool `execute`     | Parses JSON, passes to report tool (no I/O)                          |

## Expected Behavior

- **No crashes** on any input (including malformed JSON, truncated data, huge inputs).
- **No ASan leaks** — all allocations freed. Harnesses use `sc_system_allocator()` and ensure `sc_json_free`, `sc_config_deinit`, `sc_tool_result_free`, and tool deinit are called.
- **Graceful failure** — parse errors return without crashing.
- Inputs larger than 64 KiB are rejected by harness size limit to avoid OOM.

## CI / OSS-Fuzz

For continuous fuzzing, run harnesses in CI with a timeout and corpus. To integrate with OSS-Fuzz, add a `Dockerfile` and `build.sh` that build with `-DSC_ENABLE_FUZZ=ON` and run the harnesses.
