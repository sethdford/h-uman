# Fixture CMakeLists — accepted shape 1: the test is registered inside the
# same platform-macro block as its source, so it is excluded on Linux too.
if(APPLE)
    option(HU_HAS_IMESSAGE "Build iMessage channel" ON)
else()
    option(HU_HAS_IMESSAGE "Build iMessage channel" OFF)
endif()

if(HU_HAS_IMESSAGE)
    list(APPEND CHANNEL_SOURCES src/channels/imessage.c)
    list(APPEND HU_TEST_SOURCES tests/test_imessage_replay_guard.c)
endif()
