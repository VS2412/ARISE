add_executable(arise_unit_tests
    test_logger.cpp
    test_llm_parse.cpp
    test_agent_action.cpp
)

target_link_libraries(arise_unit_tests PRIVATE
    arise_core
    GTest::gtest_main
)

# Automatically discover and register all TEST() / TEST_F() cases with CTest
gtest_discover_tests(arise_unit_tests)
