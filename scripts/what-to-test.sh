#!/usr/bin/env bash
# Maps changed source files to relevant test suite filter args.
# Usage: scripts/what-to-test.sh [file ...] | xargs ./build/human_tests
#   or:  ./build/human_tests $(scripts/what-to-test.sh src/tools/shell.c)
# With no args, uses git diff to detect changed files.
set -euo pipefail

files=("$@")
if [ ${#files[@]} -eq 0 ]; then
    mapfile -t files < <(git diff --name-only HEAD 2>/dev/null; git diff --cached --name-only 2>/dev/null)
fi

if [ ${#files[@]} -eq 0 ]; then
    echo "--suite=" >&2
    echo "No changed files detected." >&2
    exit 0
fi

suites=()

for f in "${files[@]}"; do
    case "$f" in
        src/core/json*.c|include/human/core/json*.h)
            suites+=("JSON") ;;
        src/core/string*.c|include/human/core/string*.h)
            suites+=("String") ;;
        src/core/allocator*.c|include/human/core/allocator*.h)
            suites+=("Allocator") ;;
        src/core/arena*.c|include/human/core/arena*.h)
            suites+=("Arena") ;;
        src/core/http*.c|include/human/core/http*.h)
            suites+=("HTTP") ;;
        src/core/slice*.c|include/human/core/slice*.h)
            suites+=("Slice") ;;
        src/config*.c|include/human/config*.h)
            suites+=("Config") ;;
        src/providers/*.c|include/human/providers/*.h)
            suites+=("Provider") ;;
        src/channels/*.c|include/human/channels/*.h)
            suites+=("Channel") ;;
        src/tools/*.c|include/human/tools/*.h)
            suites+=("Tools") ;;
        src/memory/*.c|include/human/memory/*.h)
            suites+=("Memory") suites+=("memory") ;;
        src/security/*.c|include/human/security*.h)
            suites+=("security") suites+=("Security") ;;
        src/gateway/*.c|include/human/gateway/*.h)
            suites+=("Gateway") suites+=("gateway") ;;
        src/agent/*.c|include/human/agent/*.h)
            suites+=("Agent") suites+=("agent") ;;
        src/persona/*.c|include/human/persona/*.h)
            suites+=("persona") suites+=("Persona") ;;
        src/behavior/*.c|include/human/behavior/*.h)
            suites+=("behavior") suites+=("Behavior") ;;
        src/runtime/*.c|include/human/runtime/*.h)
            suites+=("runtime") ;;
        src/sse/*.c|include/human/sse*.h)
            suites+=("SSE") ;;
        src/websocket/*.c|include/human/websocket*.h)
            suites+=("WebSocket") suites+=("websocket") ;;
        src/context/*.c|include/human/context/*.h)
            suites+=("context") ;;
        src/intelligence/*.c|include/human/intelligence/*.h)
            suites+=("intelligence") suites+=("skill") ;;
        src/music.c|include/human/music.h)
            suites+=("Music") ;;
        src/feeds/*.c|include/human/feeds/*.h)
            suites+=("feeds") ;;
        src/peripherals/*.c|include/human/peripherals/*.h)
            suites+=("Peripheral") ;;
        src/observability/*.c|include/human/observability*.h)
            suites+=("Observer") ;;
        tests/test_*.c)
            base=$(basename "$f" .c)
            base=${base#test_}
            suites+=("$base") ;;
    esac
done

if [ ${#suites[@]} -eq 0 ]; then
    echo "# No test mapping found for changed files. Run full suite." >&2
    exit 0
fi

unique_suites=($(printf '%s\n' "${suites[@]}" | sort -u))

for s in "${unique_suites[@]}"; do
    echo "--suite=$s"
done
