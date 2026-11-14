#pragma once
// Per-process temp names for the test binaries: ctest -j runs several processes of one binary at once, and a fixed name in the shared temp directory
// made them rewrite / delete each other's files (a torn LUT gave an unexplained golden mismatch under load, wave 8).
// The tag is process id + wall-clock ns + steady-clock ns; testUniqueTag() is stable inside one process (use it for a directory), and
// testUniqueTag("x") adds a per-call counter for several distinct names in one process.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#if defined(_WIN32)
#include <process.h>
#define DAPPLE_TEST_PID() static_cast<unsigned long>(_getpid())
#else
#include <unistd.h>
#define DAPPLE_TEST_PID() static_cast<unsigned long>(getpid())
#endif

inline std::string testUniqueTag() {
  static const std::string tag = std::to_string(DAPPLE_TEST_PID()) + "_" +
                                 std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "_" +
                                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  return tag;
}
inline std::string testUniqueTag(const char* what) {
  static std::atomic<unsigned> counter{0};
  return std::string(what) + "_" + testUniqueTag() + "_" + std::to_string(counter.fetch_add(1));
}
