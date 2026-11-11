#pragma once

#include <array>
#include <string>

namespace dapple {

// The nine built-in colour styles. Their recipes live in the engine (renderer.cpp); this is the
// catalogue the UI lists (ids/names/tags match docs/design/requirements.md 4.12).
struct LookInfo {
  const char* id;        // "std" "warm" "amber" "rose" "neutral" "cool" "teal" "film" "bw"
  const char* name;      // UI name (UTF-8, Chinese)
  const char* tags;      // UI tags
  const char* category;  // "neutral" "warm" "cool" "film" "bw"
  float padX, padY;      // default colour-pad point
};

const std::array<LookInfo, 9>& looks();

// Index into looks() or -1.
int lookIndex(const std::string& id);

// {"looks":[{"id","name","tags","category","padX","padY"},...]}
std::string looksJson();

}  // namespace dapple
