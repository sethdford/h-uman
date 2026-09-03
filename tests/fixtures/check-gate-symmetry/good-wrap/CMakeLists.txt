# Fixture CMakeLists — mirrors the real tree's shape around commit 012ced741:
# the source is only compiled on macOS (platform macro gate), but the test is
# registered unconditionally.
if(APPLE)
    option(HU_HAS_IMESSAGE "Build iMessage channel" ON)
else()
    option(HU_HAS_IMESSAGE "Build iMessage channel" OFF)
endif()

if(HU_HAS_IMESSAGE)
    list(APPEND CHANNEL_SOURCES src/channels/imessage.c)
endif()

set(HU_TEST_SOURCES
    tests/test_imessage_replay_guard.c
)
