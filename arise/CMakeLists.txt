## ARISE — successor cognitive OS layer.
##
## Builds two artifacts on top of dependencies already present for ai-agent:
##   * arise-core   static library (memory cortex, identity store, util)
##   * arise        CLI binary used to drive the cortex from the shell
## A third optional artifact (arise-tests) is built when GTest is present.
##
## ARIA's binary is untouched; both live side by side until ARISE replaces
## the daemon entirely in a later phase.

find_package(GTest CONFIG)

add_library(arise-core STATIC
    util/paths.cpp
    util/log.cpp
    util/embedding_client.cpp
    util/vision_client.cpp
    cortex/memory_cortex.cpp
    cortex/identity.cpp
    cortex/salience.cpp
    cortex/goals.cpp
    cortex/goal_scheduler.cpp
    cortex/sub_agent.cpp
    cortex/watcher.cpp
    cortex/curator.cpp
    cortex/orchestrator.cpp
    cortex/spawn_handle.cpp
    cortex/critic.cpp
    cortex/researcher.cpp
    cortex/coder.cpp
    cortex/sandbox.cpp
    cortex/tool_registry.cpp
    cortex/forge_tool.cpp
    cortex/suggestion.cpp
    cortex/feedback_db.cpp
    cortex/proactive.cpp
    cortex/device_store.cpp
    cortex/federation_router.cpp
    cortex/speech.cpp
    cortex/piper_engine.cpp
    cortex/persona_prompt.cpp
    cortex/privacy_filter.cpp
    cortex/trace_replay.cpp
    cortex/trace_scorer.cpp
    cortex/training_curator.cpp
    cortex/adapter_registry.cpp
    blackboard/blackboard.cpp
    perception/phash.cpp
    perception/system_snapshot.cpp
    perception/privacy_gate.cpp
    perception/audio_scene.cpp
    perception/mic_capture.cpp
    perception/perception.cpp
)
target_include_directories(arise-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ONNXRUNTIME_INCLUDE_DIRS}
)
target_link_directories(arise-core PUBLIC ${ONNXRUNTIME_LIBRARY_DIRS})
target_link_libraries(arise-core PUBLIC
    SQLite3::SQLite3
    CURL::libcurl
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    ${ONNXRUNTIME_LIBRARIES}
    pthread
)
target_compile_features(arise-core PUBLIC cxx_std_20)
target_compile_options(arise-core PRIVATE -Wall -Wextra -O2)

add_executable(arise cli/arise_cli.cpp)
target_link_libraries(arise PRIVATE arise-core)
target_compile_options(arise PRIVATE -Wall -Wextra -O2)

if(GTest_FOUND)
    enable_testing()
    add_executable(arise-tests
        tests/test_memory_cortex.cpp
        tests/test_blackboard.cpp
        tests/test_perception.cpp
        tests/test_phase2_commit2.cpp
        tests/test_goals.cpp
        tests/test_goal_scheduler.cpp
        tests/test_phase4_commit1.cpp
        tests/test_phase4_commit2.cpp
        tests/test_phase5_commit1.cpp
        tests/test_phase5_commit2.cpp
        tests/test_phase6_commit1.cpp
        tests/test_phase7_commit1.cpp
        tests/test_phase8_commit1.cpp
    )
    target_link_libraries(arise-tests PRIVATE
        arise-core
        GTest::gtest
        GTest::gtest_main
    )
    target_compile_options(arise-tests PRIVATE -Wall -Wextra -O2)
    include(GoogleTest)
    gtest_discover_tests(arise-tests
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 30
    )
else()
    message(WARNING "GTest not found; arise-tests target skipped")
endif()
