# Fixture CMakeLists — the source is registered UNCONDITIONALLY, but its body
# (src/channels/imessage.c in this fixture) is fully wrapped in
# `#if HU_HAS_IMESSAGE` with no #else stub, so it still exports nothing on
# Linux. The gate lives in the C file, not in CMake.
if(APPLE)
    option(HU_HAS_IMESSAGE "Build iMessage channel" ON)
else()
    option(HU_HAS_IMESSAGE "Build iMessage channel" OFF)
endif()

list(APPEND CHANNEL_SOURCES src/channels/imessage.c)

set(HU_TEST_SOURCES
    tests/test_imessage_replay_guard.c
)
