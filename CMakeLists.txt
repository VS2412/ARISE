cmake_minimum_required(VERSION 3.20)
project(ai_agent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ── Dependencies ─────────────────────────────────────────────────────────────

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

# ── whisper.cpp ───────────────────────────────────────────────────────────────
# To use a local checkout, pass -DWHISPER_DIR=/path/to/whisper.cpp at configure
# time. This is purely opt-in and portable — no developer-specific path is
# ever embedded in the source tree.
# If WHISPER_DIR is not provided (the default), whisper.cpp v1.7.1 is fetched
# automatically via FetchContent and built as part of this project.
#
# Example:
#   cmake -S . -B build -DWHISPER_DIR=/home/me/whisper.cpp
set(WHISPER_DIR "OFF" CACHE PATH "Path to a local whisper.cpp checkout. Leave OFF to use FetchContent.")

if(NOT WHISPER_DIR STREQUAL "OFF" AND EXISTS "${WHISPER_DIR}/CMakeLists.txt")
    message(STATUS "ARISE: using local whisper.cpp at ${WHISPER_DIR}")
    add_subdirectory(${WHISPER_DIR} ${CMAKE_BINARY_DIR}/whisper EXCLUDE_FROM_ALL)
    # Use the exported target from whisper.cpp's own CMakeLists.
    # Both branches now link the same target for a consistent dependency graph.
    set(WHISPER_TARGET whisper)
else()
    if(NOT WHISPER_DIR STREQUAL "OFF")
        message(WARNING "ARISE: WHISPER_DIR='${WHISPER_DIR}' does not contain CMakeLists.txt — falling back to FetchContent")
    endif()
    message(STATUS "ARISE: fetching whisper.cpp v1.7.1 from GitHub")
    FetchContent_Declare(
        whisper_cpp
        GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
        GIT_TAG        v1.7.1
        GIT_SHALLOW    TRUE
    )
    set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(whisper_cpp)
    set(WHISPER_TARGET whisper)
endif()

# ── Core library (shared between the main binary and the test binary) ─────────
# Excludes main.cpp so tests can link against logic without a duplicate main().
add_library(arise_core STATIC
    src/daemon.cpp
    src/logger.cpp
    src/recorder.cpp
    src/transcriber.cpp
    src/llm.cpp
    src/executor.cpp
    src/tts.cpp
)

target_include_directories(arise_core PUBLIC
    src/
)

target_link_libraries(arise_core PUBLIC
    ${WHISPER_TARGET}
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

target_compile_options(arise_core PRIVATE -Wall -Wextra -O2)
target_compile_definitions(arise_core PRIVATE _POSIX_C_SOURCE=200809L)

# ── Main executable ───────────────────────────────────────────────────────────
add_executable(ai-agent src/main.cpp)
target_link_libraries(ai-agent PRIVATE arise_core)
target_compile_options(ai-agent PRIVATE -Wall -Wextra -O2)

# ── Unit Tests (Google Test) ──────────────────────────────────────────────────
enable_testing()

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)
# Prevent overriding the parent project's compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)
add_subdirectory(tests)