// test_logger.cpp
// -----------------------------------------------------------------------------
// Unit tests for the Logger module.
// This suite verifies that the Logger does not crash under normal usage,
// correctly prefixes log messages with severity levels, and writes them 
// reliably to a temporary log file without data corruption.
// -----------------------------------------------------------------------------

#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include <cstdio>

#include "logger.hpp"

// ── Helpers ───────────────────────────────────────────────────────────────────

// A simple helper function to scan a file for a specific substring.
// This is used to verify that our log messages actually made it to the disk.
static bool fileContains(const std::string& path, const std::string& needle) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// ── Test Fixture ──────────────────────────────────────────────────────────────

// The LoggerTest fixture sets up a clean environment before EACH test runs,
// and cleans up after EACH test finishes. This ensures tests are isolated.
//
// ISOLATION STRATEGY:
//   Logger::logFile is a process-global std::ofstream. To prevent the
//   unlinked-inode bug (where a removed file's stream remains open on the
//   old inode), we:
//     1. Use a unique log path per test (embedded with the test name).
//     2. Call Logger::close() in TearDown() BEFORE removing the file, so the
//        OS fully releases the file descriptor before it is unlinked.
class LoggerTest : public ::testing::Test {
protected:
    std::string tmpLog;

    void SetUp() override {
        // Build a unique path per test case using the test's own name.
        // This prevents any cross-test file descriptor sharing.
        // We write to /tmp/ so we don't pollute the project directory on Arch Linux.
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tmpLog = std::string("/tmp/arise_logger_") + info->name() + ".log";

        // Remove any stale file from a previous failed run
        std::remove(tmpLog.c_str());

        // Initialize the logger, which will close any previously open stream
        // (via the fix in Logger::init) before opening the new unique path.
        Logger::init(tmpLog);
    }

    void TearDown() override {
        // CRITICAL: close the stream BEFORE removing the file.
        // Without this, the global ofstream holds the inode open and
        // the next Logger::init() re-opens the same deleted inode.
        Logger::close();
        std::remove(tmpLog.c_str());
    }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

// Tests that a standard INFO level message successfully reaches the file.
TEST_F(LoggerTest, InfoWritesToFile) {
    Logger::info("test info message");
    EXPECT_TRUE(fileContains(tmpLog, "test info message"));
}

// Tests that a WARNING level message successfully reaches the file.
TEST_F(LoggerTest, WarnWritesToFile) {
    Logger::warn("test warn message");
    EXPECT_TRUE(fileContains(tmpLog, "test warn message"));
}

// Tests that an ERROR level message successfully reaches the file.
TEST_F(LoggerTest, ErrorWritesToFile) {
    Logger::error("test error message");
    EXPECT_TRUE(fileContains(tmpLog, "test error message"));
}

// Verifies that the logger automatically prepends the [INFO] tag.
TEST_F(LoggerTest, LogLevelPrefixIsPresent) {
    Logger::info("level-prefix-check");
    EXPECT_TRUE(fileContains(tmpLog, "[INFO]"));
}

// Verifies that the logger automatically prepends the [WARN] tag.
TEST_F(LoggerTest, WarnLevelPrefixIsPresent) {
    Logger::warn("warn-prefix-check");
    EXPECT_TRUE(fileContains(tmpLog, "[WARN]"));
}

// Verifies that the logger automatically prepends the [ERROR] tag.
TEST_F(LoggerTest, ErrorLevelPrefixIsPresent) {
    Logger::error("error-prefix-check");
    EXPECT_TRUE(fileContains(tmpLog, "[ERROR]"));
}

// Edge Case: Passing an empty string should be handled gracefully, not crash.
TEST_F(LoggerTest, EmptyMessageDoesNotCrash) {
    EXPECT_NO_FATAL_FAILURE(Logger::info(""));
    EXPECT_NO_FATAL_FAILURE(Logger::warn(""));
    EXPECT_NO_FATAL_FAILURE(Logger::error(""));
}

// Verifies that writing multiple times in a row doesn't overwrite previous logs.
TEST_F(LoggerTest, MultipleWritesDontCorruptFile) {
    Logger::info("first");
    Logger::warn("second");
    Logger::error("third");

    // We expect all three lines to exist in the same file simultaneously.
    EXPECT_TRUE(fileContains(tmpLog, "first"));
    EXPECT_TRUE(fileContains(tmpLog, "second"));
    EXPECT_TRUE(fileContains(tmpLog, "third"));
}

