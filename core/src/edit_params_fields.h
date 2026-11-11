#pragma once
// Internal: the table of numeric EditParams fields shared by clamp(), operator+=, toJson, fromJson
// and the schema. Keys are the contract keys of docs/design/requirements.md 6.3.

#include <cstddef>
#include <string>
#include <vector>

#include "dapple/edit_params.h"

namespace dapple {

struct ParamField {
  std::string key;
  std::size_t offset;   // byte offset of the float inside EditParams
  float def, lo, hi;    // in internal units
  float scale;          // JSON value = internal * scale (exposure: EV * 20)
  const char* group;
  bool additive;        // operator+= adds (value - default); otherwise a non-default delta replaces
  float& ref(EditParams& p) const { return *reinterpret_cast<float*>(reinterpret_cast<char*>(&p) + offset); }
  const float& ref(const EditParams& p) const {
    return *reinterpret_cast<const float*>(reinterpret_cast<const char*>(&p) + offset);
  }
};

const std::vector<ParamField>& paramFields();

}  // namespace dapple
