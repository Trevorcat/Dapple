#pragma once
// DAPPLE_DETERMINISTIC=1 (set by the test binaries that hash or compare frames): every timing dependent path of the interactive tier is switched off, so a frame
// depends only on its own inputs: no previous-step film table or film match while a background worker computes the new one, no background median seeding.
#include <cstdlib>
namespace dapple {
inline bool deterministicMode() {
  static const bool on = [] {
    const char* e = std::getenv("DAPPLE_DETERMINISTIC");
    return e && e[0] == '1';
  }();
  return on;
}
}  // namespace dapple
