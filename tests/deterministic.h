#pragma once
// Included by every test binary that hashes or compares frames: pins the timing dependent interactive paths off (core/src/determinism.h).
#include <cstdlib>
namespace {
[[maybe_unused]] const int kPinDeterministic = [] {
#ifdef _WIN32
  _putenv_s("DAPPLE_DETERMINISTIC", "1");
#else
  setenv("DAPPLE_DETERMINISTIC", "1", 1);
#endif
  return 1;
}();
}
