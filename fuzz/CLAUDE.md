# fuzz/ — LibFuzzer Harnesses

Fuzz testing for security-critical parsers and input handling.

## Commands

```bash
cmake -B build-fuzz -DSC_ENABLE_FUZZ=ON -DCMAKE_C_COMPILER=clang
cmake --build build-fuzz -j$(nproc)
./build-fuzz/fuzz_json_parse corpus/json -max_total_time=60
```

## Rules

- Harnesses must not crash or trigger ASan/UBSan errors
- Seed corpora live in `fuzz/corpus/<harness>/`
- Each harness targets one parser or input handler
- Keep harnesses small and focused — one entry point per file
- New harnesses must be added to both `CMakeLists.txt` and CI (`ci.yml` fuzz job)

## Existing Harnesses

| Harness | Target |
|---------|--------|
| `fuzz_json_parse` | JSON parser |
| `fuzz_config_load` | Config loader |
| `fuzz_base64` | Base64 codec |
| `fuzz_url_encode` | URL encoder |
| `fuzz_persona_json` | Persona JSON parser |
| `fuzz_sse` | SSE stream parser |
| `fuzz_http_parse` | HTTP request parser |
| `fuzz_tool_params` | Tool parameter parser |
