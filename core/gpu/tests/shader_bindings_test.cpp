// Desktop check of the binding strings of the Vulkan shader table (core/gpu/CMakeLists.txt, "name|source|defines|SIB..."): the device builds
// the descriptor set layout of a pipeline from that string (hal/vk/vk_device.cpp), so a shader that declares a binding the string does not
// list (or the other way round, or of another kind) is a pipeline whose layout does not match its SPIR-V. SwiftShader and lavapipe tolerate
// that; Adreno fails vkCreateComputePipelines with VK_ERROR_UNKNOWN (film_src and local_film_in after the portrait warp buffer was added).
// The check reads the embedded SPIR-V of every entry: binding 0 must be the uniform block (when present), bindings 1..n must be of the kinds
// in the string (I storage image, S sampled image, B storage buffer), no other binding may exist, all in descriptor set 0.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
struct ShaderBin {
  const char* name;
  const unsigned char* data;
  std::size_t size;
  const char* bindings;
};
#include "shader_table.inc"

struct Ty {
  std::uint32_t op = 0, a = 0, b = 0;  // OpTypeImage: a = sampled (1 sampled, 2 storage); pointer: a = storage class, b = pointee; array: a = element
};

// Kind letter of every resource variable of the module by binding: 'U' uniform block, 'B' storage buffer, 'I' storage image, 'S' sampled image.
bool bindingKinds(const ShaderBin& s, std::map<std::uint32_t, char>& out, std::string& err) {
  if (s.size < 20 || s.size % 4 != 0) return err = "not a SPIR-V module", false;
  std::vector<std::uint32_t> w(s.size / 4);
  std::memcpy(w.data(), s.data, s.size);
  if (w[0] != 0x07230203u) return err = "bad SPIR-V magic", false;
  std::map<std::uint32_t, Ty> ty;
  std::map<std::uint32_t, std::uint32_t> binding, set, bufferBlock, block;
  struct Var {
    std::uint32_t id, ptrTy, cls;
  };
  std::vector<Var> vars;
  for (std::size_t i = 5; i < w.size();) {
    const std::uint32_t op = w[i] & 0xffffu, n = w[i] >> 16;
    if (n == 0 || i + n > w.size()) return err = "truncated SPIR-V", false;
    const std::uint32_t* p = &w[i];
    if (op == 71 && n >= 3) {  // OpDecorate target decoration [value]
      if (p[2] == 33 && n >= 4) binding[p[1]] = p[3];
      if (p[2] == 34 && n >= 4) set[p[1]] = p[3];
      if (p[2] == 3) bufferBlock[p[1]] = 1;
      if (p[2] == 2) block[p[1]] = 1;
    } else if (op == 25 && n >= 9) ty[p[1]] = {op, p[7], 0};   // OpTypeImage ... Sampled
    else if (op == 27 && n >= 3) ty[p[1]] = {op, 0, 0};        // OpTypeSampledImage
    else if (op == 30 && n >= 2) ty[p[1]] = {op, 0, 0};        // OpTypeStruct
    else if ((op == 28 || op == 29) && n >= 3) ty[p[1]] = {op, p[2], 0};  // OpTypeArray / OpTypeRuntimeArray
    else if (op == 32 && n >= 4) ty[p[1]] = {op, p[2], p[3]};  // OpTypePointer
    else if (op == 59 && n >= 4) vars.push_back({p[2], p[1], p[3]});  // OpVariable
    i += n;
  }
  for (const Var& v : vars) {
    if (!binding.count(v.id)) continue;  // not a descriptor
    if (set.count(v.id) && set[v.id] != 0) return err = "descriptor set other than 0", false;
    std::uint32_t t = ty[v.ptrTy].b;
    while (ty.count(t) && (ty[t].op == 28 || ty[t].op == 29)) t = ty[t].a;  // arrays of descriptors
    const Ty& e = ty[t];
    char k = 0;
    if (e.op == 27) k = 'S';
    else if (e.op == 25) k = e.a == 2 ? 'I' : 'S';
    else if (e.op == 30) k = v.cls == 12 || bufferBlock.count(t) ? 'B' : 'U';  // StorageBuffer, or a BufferBlock struct in Uniform
    if (!k) return err = "unknown resource type at binding " + std::to_string(binding[v.id]), false;
    out[binding[v.id]] = k;
  }
  return true;
}
}  // namespace

int main() {
  int bad = 0;
  for (std::size_t i = 0; i < kShaderCount; ++i) {
    const ShaderBin& s = kShaders[i];
    std::map<std::uint32_t, char> kinds;
    std::string err;
    if (!bindingKinds(s, kinds, err)) {
      std::printf("FAIL %s: %s\n", s.name, err.c_str());
      ++bad;
      continue;
    }
    const std::string want = s.bindings;
    std::string got, why;
    for (const auto& [b, k] : kinds) {
      if (b == 0) {
        if (k != 'U') why += " binding 0 is not a uniform block;";
        continue;
      }
      if (b != got.size() + 1) why += " gap before binding " + std::to_string(b) + ";";
      got.push_back(k);
    }
    if (got != want) why += " string \"" + want + "\" but the shader declares \"" + got + "\";";
    if (!why.empty()) {
      std::printf("FAIL %s:%s\n", s.name, why.c_str());
      ++bad;
    }
  }
  std::printf("%zu shaders checked, %d mismatches\n", kShaderCount, bad);
  return bad ? 1 : 0;
}
