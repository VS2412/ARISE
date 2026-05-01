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
    cortex/memory_cortex.cpp
    cortex/identity.cpp
)
target_include_directories(arise-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(arise-core PUBLIC
    SQLite3::SQLite3
    CURL::libcurl
    nlohmann_json::nlohmann_json
    pthread
)
target_compile_features(arise-core PUBLIC cxx_std_20)
target_compile_options(arise-core PRIVATE -Wall -Wextra -O2)

add_executable(arise cli/arise_cli.cpp)
target_link_libraries(arise PRIVATE arise-core)
target_compile_options(arise PRIVATE -Wall -Wextra -O2)

if(GTest_FOUND)
    enable_testing()
    add_executable(arise-tests tests/test_memory_cortex.cpp)
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
