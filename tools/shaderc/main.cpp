// dapple_shaderc: host build tool (never shipped). SPIR-V (glslc output) -> ESSL 310 + MSL + manifest.
// docs/engine/pipeline-architecture.md section 7.1 step 3. Built over SPIRV-Cross (Apache-2.0, tools/_dl/spirv-cross).
//
// Usage: dapple_shaderc --name N --spv in.spv --out-dir D [--msl]
// Writes: D/N.essl (ESSL 310, spec constants as SPIRV_CROSS_CONSTANT_ID_n macros with defaults)
//         D/N.json (manifest: bindings per class, spec constants, workgroup size, shared bytes, ES violations)
//         D/N.metal, D/N_msl.h, D/N_msl_entry.inc (with --msl; Metal backend table entry)
//         D/N_gles.h (C++ header embedding essl + manifest as raw strings: g_N_essl, g_N_manifest)
// Bindings are renumbered PER RESOURCE CLASS (ubo, ssbo, image, sampled) in ascending Vulkan binding order, because
// ES 3.1 has separate unit spaces and small per-class minimums (6.4).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "spirv_glsl.hpp"
#include "spirv_msl.hpp"

using namespace spirv_cross;

static std::vector<uint32_t> readSpv(const std::string& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f) {
    std::cerr << "cannot open " << p << "\n";
    std::exit(2);
  }
  const size_t n = static_cast<size_t>(f.tellg());
  f.seekg(0);
  std::vector<uint32_t> v(n / 4);
  f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n));
  return v;
}
static void writeFile(const std::string& p, const std::string& s) {
  std::ofstream f(p, std::ios::binary);
  if (!f) {
    std::cerr << "cannot write " << p << "\n";
    std::exit(2);
  }
  f << s;
}
static const char* fmtName(spv::ImageFormat f) {
  switch (f) {
    case spv::ImageFormatRgba32f: return "rgba32f";
    case spv::ImageFormatRgba16f: return "rgba16f";
    case spv::ImageFormatR32f: return "r32f";
    case spv::ImageFormatRgba8: return "rgba8";
    case spv::ImageFormatRgba8Snorm: return "rgba8_snorm";
    case spv::ImageFormatR32ui: return "r32ui";
    case spv::ImageFormatR32i: return "r32i";
    case spv::ImageFormatRgba32ui: return "rgba32ui";
    case spv::ImageFormatRgba32i: return "rgba32i";
    case spv::ImageFormatRgba16ui: return "rgba16ui";
    case spv::ImageFormatRgba16i: return "rgba16i";
    case spv::ImageFormatRgba8ui: return "rgba8ui";
    case spv::ImageFormatRgba8i: return "rgba8i";
    case spv::ImageFormatRg32f: return "rg32f";
    case spv::ImageFormatR16f: return "r16f";
    case spv::ImageFormatRg16f: return "rg16f";
    case spv::ImageFormatUnknown: return "unknown";
    default: return "other";
  }
}
// storage formats valid for image2D in ES 3.1 (spec table of image unit formats)
static bool esStorageFormat(const std::string& f) {
  static const char* ok[] = {"rgba32f", "rgba16f", "r32f",     "rgba8",    "rgba8_snorm", "rgba32i", "rgba16i",
                             "rgba8i",  "r32i",    "rgba32ui", "rgba16ui", "rgba8ui",     "r32ui"};
  for (auto* o : ok)
    if (f == o) return true;
  return false;
}
static const char* dimName(spv::Dim d) {
  switch (d) {
    case spv::Dim2D: return "2D";
    case spv::Dim3D: return "3D";
    case spv::Dim1D: return "1D";
    case spv::DimCube: return "Cube";
    default: return "?";
  }
}
static std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

struct Res {
  std::string name, kind, access, format, dim;
  uint32_t id = 0, vk = 0, gl = 0;
  std::string diag;  // ES rule diagnostics
};

static uint64_t typeBytes(Compiler& c, const SPIRType& t) {
  uint64_t base = (t.width / 8) * t.vecsize * t.columns;
  if (t.basetype == SPIRType::Struct) base = c.get_declared_struct_size(t);
  uint64_t n = 1;
  for (size_t i = 0; i < t.array.size(); i++) {
    uint32_t sz = t.array[i];
    if (!t.array_size_literal[i]) sz = c.get_constant(t.array[i]).scalar();
    n *= sz;
  }
  return base * n;
}

int main(int argc, char** argv) {
  std::string name, spv, outDir;
  bool msl = false, pingpong = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto nxt = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << a << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--name") name = nxt();
    else if (a == "--spv") spv = nxt();
    else if (a == "--out-dir") outDir = nxt();
    else if (a == "--msl") msl = true;
    else if (a == "--pingpong") pingpong = true;  // last sampled binding is the read copy of the (write-only) first image
    else {
      std::cerr << "unknown arg " << a << "\n";
      return 2;
    }
  }
  if (name.empty() || spv.empty() || outDir.empty()) {
    std::cerr << "usage: dapple_shaderc --name N --spv f.spv --out-dir D [--msl] [--pingpong]\n";
    return 2;
  }

  try {
    const auto words = readSpv(spv);
    struct PeekGLSL : CompilerGLSL {
      using CompilerGLSL::CompilerGLSL;
      ParsedIR& get_ir() { return ir; }
    };
    PeekGLSL gl(words);
    ShaderResources r = gl.get_shader_resources();
    std::vector<Res> res;
    auto add = [&](const Resource& x, const char* kind) {
      Res o;
      o.id = x.id;
      o.name = gl.get_name(x.id);
      o.kind = kind;
      if (o.name.empty()) o.name = gl.get_name(x.base_type_id);
      o.vk = gl.get_decoration(x.id, spv::DecorationBinding);
      const SPIRType& t = gl.get_type(x.type_id);
      const std::string k = kind;
      if (k == "image") {
        o.dim = dimName(t.image.dim);
        o.format = fmtName(t.image.format);
        const bool nr = gl.has_decoration(x.id, spv::DecorationNonReadable), nw = gl.has_decoration(x.id, spv::DecorationNonWritable);
        o.access = nr ? "write" : nw ? "read" : "readwrite";
        if (!esStorageFormat(o.format)) o.diag += "format " + o.format + " is not an ES 3.1 storage format; ";
        if (o.access == "readwrite" && o.format != "r32f" && o.format != "r32i" && o.format != "r32ui")
          o.diag += "read-write image with format " + o.format + " (ES 3.1 allows r32f/r32i/r32ui only); ";
      } else if (k == "sampled") {
        o.dim = dimName(t.image.dim);
        o.access = "read";
      } else if (k == "ssbo") {
        const Bitset fl = gl.get_buffer_block_flags(x.id);
        o.access = fl.get(spv::DecorationNonWritable) ? "read" : fl.get(spv::DecorationNonReadable) ? "write" : "readwrite";
      } else {
        o.access = "read";
      }
      res.push_back(o);
    };
    for (auto& x : r.uniform_buffers) add(x, "ubo");
    for (auto& x : r.storage_buffers) add(x, "ssbo");
    for (auto& x : r.storage_images) add(x, "image");
    for (auto& x : r.sampled_images) add(x, "sampled");
    for (auto& x : r.separate_images) add(x, "sampled");
    if (!r.separate_samplers.empty()) {
      std::cerr << name << ": separate samplers are not supported by the portable subset (use combined sampler2D)\n";
      return 3;
    }
    std::map<std::string, std::vector<Res*>> byClass;
    for (auto& o : res) byClass[o.kind].push_back(&o);
    for (auto& kv : byClass) {
      auto& v = kv.second;
      std::sort(v.begin(), v.end(), [](Res* a, Res* b) { return a->vk < b->vk; });
      uint32_t n = 0;
      for (Res* p : v) {
        p->gl = n++;
        gl.set_decoration(p->id, spv::DecorationBinding, p->gl);
        gl.unset_decoration(p->id, spv::DecorationDescriptorSet);
      }
    }
    // spec constants
    struct SC {
      uint32_t cid;
      std::string name, type;
      int64_t def;
    };
    std::vector<SC> scs;
    for (auto& sc : gl.get_specialization_constants()) {
      const SPIRConstant& c = gl.get_constant(sc.id);
      const SPIRType& t = gl.get_type(c.constant_type);
      SC s;
      s.cid = sc.constant_id;
      s.name = gl.get_name(sc.id);
      s.type = t.basetype == SPIRType::Boolean ? "bool" : t.basetype == SPIRType::UInt ? "uint" : t.basetype == SPIRType::Float ? "float" : "int";
      s.def = t.basetype == SPIRType::Float ? (int64_t)c.scalar_f32() : (int64_t)c.scalar_i32();
      scs.push_back(s);
    }
    SpecializationConstant wsc[3];
    gl.get_work_group_size_specialization_constants(wsc[0], wsc[1], wsc[2]);
    uint32_t lsz[3] = {gl.get_execution_mode_argument(spv::ExecutionModeLocalSize, 0), gl.get_execution_mode_argument(spv::ExecutionModeLocalSize, 1),
                       gl.get_execution_mode_argument(spv::ExecutionModeLocalSize, 2)};
    for (int i = 0; i < 3; i++)
      if (wsc[i].id) lsz[i] = gl.get_constant(wsc[i].id).scalar();
    uint64_t sharedBytes = 0;
    gl.get_ir().for_each_typed_id<SPIRVariable>([&](uint32_t, SPIRVariable& v) {
      if (v.storage == spv::StorageClassWorkgroup) sharedBytes += typeBytes(gl, gl.get_type(v.basetype));
    });

    CompilerGLSL::Options o;
    o.es = true;
    o.version = 310;
    o.vulkan_semantics = false;
    o.enable_420pack_extension = false;
    o.fragment.default_float_precision = CompilerGLSL::Options::Highp;
    o.fragment.default_int_precision = CompilerGLSL::Options::Highp;
    gl.set_common_options(o);
    gl.build_combined_image_samplers();
    const std::string essl = gl.compile();

    std::ostringstream m;
    m << "{\n  \"name\": \"" << name << "\",\n  \"workgroup\": [" << lsz[0] << ", " << lsz[1] << ", " << lsz[2] << "],\n  \"workgroupSpecIds\": ["
      << (wsc[0].id ? (int)wsc[0].constant_id : -1) << ", " << (wsc[1].id ? (int)wsc[1].constant_id : -1) << ", "
      << (wsc[2].id ? (int)wsc[2].constant_id : -1) << "],\n  \"sharedBytes\": " << sharedBytes << ",\n";
    m << "  \"specConstants\": [";
    for (size_t i = 0; i < scs.size(); i++)
      m << (i ? ", " : "") << "{\"id\": " << scs[i].cid << ", \"name\": \"" << esc(scs[i].name) << "\", \"type\": \"" << scs[i].type
        << "\", \"default\": " << scs[i].def << "}";
    m << "],\n  \"bindings\": [\n";
    std::vector<Res> sorted = res;
    std::sort(sorted.begin(), sorted.end(), [](const Res& a, const Res& b) { return a.vk < b.vk; });
    for (size_t i = 0; i < sorted.size(); i++) {
      const Res& x = sorted[i];
      m << "    {\"name\": \"" << esc(x.name) << "\", \"kind\": \"" << x.kind << "\", \"vkBinding\": " << x.vk << ", \"glBinding\": " << x.gl
        << ", \"access\": \"" << x.access << "\"";
      if (!x.format.empty()) m << ", \"format\": \"" << x.format << "\"";
      if (!x.dim.empty()) m << ", \"dim\": \"" << x.dim << "\"";
      m << "}" << (i + 1 < sorted.size() ? "," : "") << "\n";
    }
    m << "  ],\n  \"esViolations\": [";
    bool first = true;
    for (auto& x : sorted)
      if (!x.diag.empty()) {
        m << (first ? "" : ", ") << "\"" << esc(x.name + ": " + x.diag) << "\"";
        first = false;
      }
    if (lsz[0] * lsz[1] * lsz[2] > 128) {
      m << (first ? "" : ", ") << "\"workgroup " << lsz[0] * lsz[1] * lsz[2] << " invocations > 128 guaranteed (needs caps)\"";
      first = false;
    }
    if (sharedBytes > 16384) {
      m << (first ? "" : ", ") << "\"shared " << sharedBytes << " B > 16 KB guaranteed\"";
      first = false;
    }
    m << "]\n}\n";
    const std::string manifest = m.str();

    writeFile(outDir + "/" + name + ".essl", essl);
    writeFile(outDir + "/" + name + ".json", manifest);
    std::string hdr = "// generated by dapple_shaderc from " + name + ".spv - do not edit\n#pragma once\nstatic const char g_" + name +
                      "_essl[] = R\"DAPPLE_ESSL(" + essl + ")DAPPLE_ESSL\";\nstatic const char g_" + name + "_manifest[] = R\"DAPPLE_JSON(" +
                      manifest + ")DAPPLE_JSON\";\n";
    writeFile(outDir + "/" + name + "_gles.h", hdr);
    {
      // table entry for hal/gles: {name, essl, signature (I storage image, S sampled image, B storage buffer in Vulkan binding
      // order after the uniform block), GL unit per signature char, spec constants, local size, shared bytes}
      std::string sig;
      std::ostringstream glb, sid, sbool, sdef;
      for (auto& x : sorted) {
        if (x.kind == "ubo") continue;
        sig += x.kind == "image" ? 'I' : x.kind == "sampled" ? 'S' : 'B';
        glb << x.gl << ",";
      }
      for (auto& s : scs) {
        sid << s.cid << ",";
        sbool << (s.type == "bool" ? 1 : 0) << ",";
        sdef << s.def << ",";
      }
      std::ostringstream e;
      e << "{\"" << name << "\", g_" << name << "_essl, \"" << sig << "\", {" << glb.str() << "}, {" << sid.str() << "}, {" << sbool.str() << "}, {"
        << sdef.str() << "}, " << scs.size() << ", {" << lsz[0] << "," << lsz[1] << "," << lsz[2] << "}, {" << (wsc[0].id ? (int)wsc[0].constant_id : -1)
        << "," << (wsc[1].id ? (int)wsc[1].constant_id : -1) << "}, " << sharedBytes << ", " << (pingpong ? 1 : 0) << "},\n";
      writeFile(outDir + "/" + name + "_entry.inc", e.str());
    }

    if (msl) {
      // Metal (docs/ios/metal-backend.md): the Vulkan binding number is the Metal argument index of every resource class
      // (buffer / texture / sampler index spaces are separate in MSL and the Vulkan numbers are unique across classes);
      // spec constants become function constants with the same id; the entry point is `k_<name>`.
      CompilerMSL cm(words);
      cm.rename_entry_point("main", "k_" + name, spv::ExecutionModelGLCompute);
      CompilerMSL::Options mo;
      mo.platform = CompilerMSL::Options::iOS;
      mo.set_msl_version(2, 3);
      mo.enable_decoration_binding = true;
      cm.set_msl_options(mo);
      const std::string msrc = cm.compile();
      writeFile(outDir + "/" + name + ".metal", msrc);
      writeFile(outDir + "/" + name + "_msl.h",
                "// generated by dapple_shaderc from " + name + ".spv - do not edit\n#pragma once\nstatic const char g_" + name +
                    "_msl[] = R\"DAPPLE_MSL(" + msrc + ")DAPPLE_MSL\";\n");
      std::string sig;
      for (auto& x : sorted) {
        if (x.kind == "ubo") continue;
        sig += x.kind == "image" ? 'I' : x.kind == "sampled" ? 'S' : 'B';
      }
      if (scs.size() > 12) {
        std::cerr << name << ": more than 12 spec constants\n";
        return 3;
      }
      std::ostringstream sid, sty, sdf;
      for (auto& sc : scs) {
        sid << sc.cid << ",";
        sty << (sc.type == "bool" ? 2 : sc.type == "uint" ? 1 : sc.type == "float" ? 3 : 0) << ",";
        sdf << sc.def << ",";
      }
      std::ostringstream e;
      e << "{\"" << name << "\", DAPPLE_MSL_SRC(" << name << "), \"" << sig << "\", {" << sid.str() << "}, {" << sty.str() << "}, {" << sdf.str() << "}, "
        << scs.size() << ", {" << lsz[0] << "," << lsz[1] << "," << lsz[2] << "}, {" << (wsc[0].id ? (int)wsc[0].constant_id : -1) << ","
        << (wsc[1].id ? (int)wsc[1].constant_id : -1) << "," << (wsc[2].id ? (int)wsc[2].constant_id : -1) << "}, " << sharedBytes << "},\n";
      writeFile(outDir + "/" + name + "_msl_entry.inc", e.str());
    }
  } catch (const std::exception& e) {
    std::cerr << name << ": " << e.what() << "\n";
    return 1;
  }
  return 0;
}
