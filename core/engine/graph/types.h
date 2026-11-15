#pragma once
// Engine graph IR: basic types (docs/engine/pipeline-architecture.md section 4.1).
#include <bitset>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace dapple::engine {

// 128-bit content key: two independent 64-bit lanes (collisions are treated as impossible, like the film cache keys).
struct Hash128 {
  std::uint64_t a = 0, b = 0;
  bool operator==(const Hash128& o) const { return a == o.a && b == o.b; }
  bool operator!=(const Hash128& o) const { return !(*this == o); }
  bool operator<(const Hash128& o) const { return a != o.a ? a < o.a : b < o.b; }
  bool zero() const { return a == 0 && b == 0; }
  std::string hex() const;
};
struct Hash128Hash {
  std::size_t operator()(const Hash128& h) const { return std::size_t(h.a ^ (h.b * 0x9E3779B97F4A7C15ull)); }
};

// Incremental hasher. Floats are hashed by bit pattern with -0 -> +0 and one canonical NaN.
class Hasher {
 public:
  void u64(std::uint64_t v) {
    a_ = rotl(a_ ^ (v * 0xFF51AFD7ED558CCDull), 29) * 0x9E3779B97F4A7C15ull + 0x52DCE729ull;
    b_ = (b_ ^ v) * 0xC4CEB9FE1A85EC53ull;
    b_ ^= b_ >> 31;
    b_ += 0x94D049BB133111EBull;
  }
  void i32(std::int32_t v) { u64(std::uint64_t(std::uint32_t(v)) | 0x100000000ull); }
  void f32(float v) {
    if (v == 0.f) v = 0.f;  // -0 -> +0
    std::uint32_t bits;
    if (v != v) bits = 0x7FC00000u;
    else std::memcpy(&bits, &v, 4);
    u64(bits);
  }
  void boolean(bool v) { u64(v ? 0xB001ull : 0xB000ull); }
  void str(std::string_view s) {
    u64(0x5700000000ull | s.size());
    std::size_t i = 0;
    for (; i + 8 <= s.size(); i += 8) {
      std::uint64_t w;
      std::memcpy(&w, s.data() + i, 8);
      u64(w);
    }
    std::uint64_t w = 0;
    if (i < s.size()) std::memcpy(&w, s.data() + i, s.size() - i);
    u64(w);
  }
  void hash(const Hash128& h) {
    u64(h.a);
    u64(h.b);
  }
  Hash128 finish() const {
    Hash128 h;
    h.a = fmix(a_ ^ (b_ >> 17));
    h.b = fmix(b_ ^ (a_ << 11) ^ 0xA5A5A5A5ull);
    return h;
  }

 private:
  static std::uint64_t rotl(std::uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
  static std::uint64_t fmix(std::uint64_t k) {
    k ^= k >> 33;
    k *= 0xFF51AFD7ED558CCDull;
    k ^= k >> 33;
    k *= 0xC4CEB9FE1A85EC53ull;
    k ^= k >> 33;
    return k;
  }
  std::uint64_t a_ = 0x243F6A8885A308D3ull, b_ = 0x13198A2E03707344ull;
};

using ParamKey = std::uint16_t;
using NodeId = std::uint32_t;
constexpr ParamKey kNoKey = 0xFFFF;
constexpr std::size_t kMaxKeys = 256;

// Which ParamKeys (sliders, groups, context keys) a node reads.
struct ParamMask {
  std::bitset<kMaxKeys> bits;
  ParamMask& add(ParamKey k) {
    bits.set(k);
    return *this;
  }
  ParamMask& add(const ParamMask& o) {
    bits |= o.bits;
    return *this;
  }
  bool has(ParamKey k) const { return bits.test(k); }
};

enum class Domain : std::uint8_t { Image, Table };
// Footprint class of a node: what part of its input one output pixel reads (the planner derives groups, halos and source windows from it).
//   Point     the same pixel                      Window    a neighbourhood (NodeBehaviour::halo)
//   Resample  a mapped neighbourhood (inputRect)  Global    the whole input (tables, frame-global solves of a window node: NodeDesc::solve)
//   Source    no input (the picture)              Sink      the 8-bit output
enum class Footprint : std::uint8_t { Point, Window, Resample, Global, Source, Sink };

// Colour space of an image port (docs/engine/pipeline-architecture.md 4.1). Graph::validate checks that every image input carries the
// space its consumer declares.
//   Source   camera / source linear RGB (the picture, resampled or not)    Scene    scene-linear working RGB (after the gain)
//   Display  display-encoded RGB (after the tone / colour stage or the film)  Film     film planes (exposure, density, print)
//   Any      a node that passes its input space through (declared on inputs only)
enum class Space : std::uint8_t { Any, Source, Scene, Display, Film };
// Precision of the image a node writes. The planner gives every sweep the widest precision of its nodes (Stage::precision), the GPU
// executor allocates the sweep's working image in it and the memory estimate counts it.
enum class Precision : std::uint8_t { F16, F32 };

// The node types (generated from engine/nodes/node_list.inc; the order enters the structure hash and the node keys: append only).
enum class NodeType : std::uint16_t {
#define DAPPLE_NODE(type, desc, gpu) type,
#include "engine/nodes/node_list.inc"
#undef DAPPLE_NODE
  Count
};
constexpr int kNodeTypeCount = int(NodeType::Count);

enum class OutputRole : std::uint8_t { Frame, Analysis, Table };

}  // namespace dapple::engine
