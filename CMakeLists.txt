cmake_minimum_required(VERSION 3.20)
project(ai_agent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FVAD REQUIRED libfvad)

set(WHISPER_DIR /home/Aurelius/Documents/AdoVs/whisper.cpp)

add_executable(ai-agent
    src/main.cpp
    src/daemon.cpp
    src/logger.cpp
    src/recorder.cpp
    src/transcriber.cpp
    src/llm.cpp
    src/executor.cpp
    src/tts.cpp
    src/context.cpp
    src/memory.cpp
    src/intent.cpp
    src/screen.cpp
    src/timer.cpp
)

target_include_directories(ai-agent PRIVATE
    ${WHISPER_DIR}/include
    ${WHISPER_DIR}/ggml/include
    ${FVAD_INCLUDE_DIRS}
)

target_link_directories(ai-agent PRIVATE
    ${WHISPER_DIR}/build/src
    ${WHISPER_DIR}/build/ggml/src
)

target_link_libraries(ai-agent PRIVATE
    whisper ggml
    CURL::libcurl
    nlohmann_json::nlohmann_json
    SQLite3::SQLite3
    ${FVAD_LIBRARIES}
)

target_compile_options(ai-agent PRIVATE -Wall -Wextra -O2 ${FVAD_CFLAGS_OTHER})
target_compile_definitions(ai-agent PRIVATE _POSIX_C_SOURCE=200809L)