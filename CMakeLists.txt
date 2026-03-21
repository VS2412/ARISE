cmake_minimum_required(VERSION 3.20)
project(ai_agent)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# path to your whisper.cpp build
set(WHISPER_DIR /home/Aurelius/Documents/AdoVs/whisper.cpp)

add_executable(ai-agent
    src/main.cpp
    src/daemon.cpp
    src/logger.cpp
    src/recorder.cpp
    src/transcriber.cpp
)

target_include_directories(ai-agent PRIVATE
    ${WHISPER_DIR}/include
    ${WHISPER_DIR}/ggml/include
)

target_link_libraries(ai-agent PRIVATE
    ${WHISPER_DIR}/build/src/libwhisper.so
)

target_compile_options(ai-agent PRIVATE -Wall -Wextra -O2)
target_compile_definitions(ai-agent PRIVATE _POSIX_C_SOURCE=200809L)