// Lightroom / Camera Raw preset import. We read only the crs: settings we can
// reproduce and report the rest, rather than pretending to be bit-exact.

#include "dapple/xmp_preset.h"

#include <cstdlib>
#include <map>
#include <regex>

#include "file_util.h"

namespace dapple {

namespace {

using Settings = std::map<std::string, std::string>;

// Decodes the predefined XML entities and numeric character references.
std::string decodeEntities(const std::string& in) {
  if (in.find('&') == std::string::npos) return in;
  std::string out;
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '&') { out += in[i]; continue; }
    const std::size_t semi = in.find(';', i);
    if (semi == std::string::npos || semi - i > 10) { out += in[i]; continue; }
    const std::string ent = in.substr(i + 1, semi - i - 1);
    unsigned cp = 0;
    bool ok = true;
    if (ent == "amp") cp = '&';
    else if (ent == "lt") cp = '<';
    else if (ent == "gt") cp = '>';
    else if (ent == "quot") cp = '"';
    else if (ent == "apos") cp = 0x27;
    else if (ent.size() > 1 && ent[0] == '#') {
      char* end = nullptr;
      const bool hex = ent[1] == 'x' || ent[1] == 'X';
      cp = unsigned(std::strtoul(ent.c_str() + (hex ? 2 : 1), &end, hex ? 16 : 10));
      ok = end && *end == 0 && cp > 0 && cp <= 0x10FFFF;
    } else ok = false;
    if (!ok) { out += in[i]; continue; }
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    i = semi;
  }
  return out;
}

// Removes the (potentially megabytes long) crs:RGBTable attribute so the
// regex scans stay fast. Returns whether one was present.
bool stripRgbTable(std::string& xml) {
  bool found = false;
  std::size_t pos = 0;
  while ((pos = xml.find("crs:RGBTable", pos)) != std::string::npos) {
    found = true;
    std::size_t q = xml.find_first_of("\"'", pos);
    if (q == std::string::npos) break;
    std::size_t e = xml.find(xml[q], q + 1);
    if (e == std::string::npos) break;
    xml.erase(pos, e + 1 - pos);
  }
  return found;
}

Settings collectScalars(const std::string& xml) {
  Settings s;
  // Attribute form: crs:Key="value"
  static const std::regex attr(R"rx(crs:([A-Za-z0-9]+)\s*=\s*(?:"([^"]*)"|'([^']*)'))rx");
  for (std::sregex_iterator it(xml.begin(), xml.end(), attr), end; it != end; ++it)
    s[(*it)[1]] = decodeEntities((*it)[2].matched ? (*it)[2].str() : (*it)[3].str());
  // Element form: <crs:Key>value</crs:Key>
  static const std::regex elem(R"rx(<crs:([A-Za-z0-9]+)>([^<]*)</crs:\1>)rx");
  for (std::sregex_iterator it(xml.begin(), xml.end(), elem), end; it != end; ++it) s[(*it)[1]] = decodeEntities((*it)[2]);
  return s;
}

std::vector<CurvePoint> readCurve(const std::string& xml, const std::string& key) {
  std::vector<CurvePoint> pts;
  const std::regex block("<crs:" + key + R"rx(>\s*<rdf:Seq>([\s\S]*?)</rdf:Seq>)rx");
  std::smatch m;
  if (!std::regex_search(xml, m, block)) return pts;
  const std::string body = m[1];
  static const std::regex li(R"rx(<rdf:li>\s*([-+0-9.]+)\s*,\s*([-+0-9.]+)\s*</rdf:li>)rx");
  for (std::sregex_iterator it(body.begin(), body.end(), li), end; it != end; ++it)
    pts.emplace_back(std::strtof((*it)[1].str().c_str(), nullptr), std::strtof((*it)[2].str().c_str(), nullptr));
  // A straight 0,0 -> 255,255 line is the identity; drop it.
  if (pts.size() == 2 && pts[0].first == 0 && pts[0].second == 0 && pts[1].first == 255 && pts[1].second == 255)
    pts.clear();
  return pts;
}

std::string readName(const std::string& xml, const Settings& s) {
  if (auto it = s.find("Name"); it != s.end() && !it->second.empty()) return it->second;
  static const std::regex alt(R"rx(<crs:Name>\s*<rdf:Alt>\s*<rdf:li[^>]*>([^<]*)</rdf:li>)rx");
  std::smatch m;
  if (std::regex_search(xml, m, alt)) return decodeEntities(m[1]);
  return {};
}

}  // namespace

Result<PresetImport> parseXmpPreset(const std::string& xmlIn) {
  if (xmlIn.find("crs:") == std::string::npos) return fail("no Camera Raw settings (crs:) found");
  std::string xml = xmlIn;
  const bool hasRgbTable = stripRgbTable(xml);

  const Settings s = collectScalars(xml);
  PresetImport out;
  out.name = readName(xml, s);
  EditParams& p = out.params;

  auto num = [&](const char* key, float& dst) {
    if (auto it = s.find(key); it != s.end()) dst = std::strtof(it->second.c_str(), nullptr);
  };
  auto nonZero = [&](const char* key) {
    auto it = s.find(key);
    return it != s.end() && std::strtof(it->second.c_str(), nullptr) != 0.f;
  };

  num("Exposure2012", p.exposure);
  num("Contrast2012", p.contrast);
  num("Highlights2012", p.highlights);
  num("Shadows2012", p.shadows);
  num("Whites2012", p.whites);
  num("Blacks2012", p.blacks);
  num("Vibrance", p.vibrance);
  num("Saturation", p.saturation);
  num("IncrementalTemperature", p.temperature);
  num("IncrementalTint", p.tint);

  num("ParametricShadows", p.parametricShadows);
  num("ParametricDarks", p.parametricDarks);
  num("ParametricLights", p.parametricLights);
  num("ParametricHighlights", p.parametricHighlights);
  num("ParametricShadowSplit", p.parametricShadowSplit);
  num("ParametricMidtoneSplit", p.parametricMidtoneSplit);
  num("ParametricHighlightSplit", p.parametricHighlightSplit);

  static const char* kBands[] = {"Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};
  for (int i = 0; i < kHslBandCount; ++i) {
    num((std::string("HueAdjustment") + kBands[i]).c_str(), p.hsl[i].hue);
    num((std::string("SaturationAdjustment") + kBands[i]).c_str(), p.hsl[i].saturation);
    num((std::string("LuminanceAdjustment") + kBands[i]).c_str(), p.hsl[i].luminance);
  }

  // Split toning (legacy) and the newer colour grading share our grade model.
  num("SplitToningShadowHue", p.gradeShadows.hue);
  num("SplitToningShadowSaturation", p.gradeShadows.saturation);
  num("SplitToningHighlightHue", p.gradeHighlights.hue);
  num("SplitToningHighlightSaturation", p.gradeHighlights.saturation);
  num("SplitToningBalance", p.gradeBalance);
  num("ColorGradeMidtoneHue", p.gradeMidtones.hue);
  num("ColorGradeMidtoneSat", p.gradeMidtones.saturation);

  num("ColorGradeGlobalHue", p.gradeGlobal.hue);
  num("ColorGradeGlobalSat", p.gradeGlobal.saturation);
  num("ColorGradeGlobalLum", p.gradeGlobal.luminance);
  num("ColorGradeShadowLum", p.gradeShadows.luminance);
  num("ColorGradeMidtoneLum", p.gradeMidtones.luminance);
  num("ColorGradeHighlightLum", p.gradeHighlights.luminance);
  num("ColorGradeBlending", p.gradeBlend);

  num("Clarity2012", p.clarity);
  num("Texture", p.texture);
  num("Dehaze", p.dehaze);
  num("GrainAmount", p.grain);
  num("Sharpness", p.sharpen);
  if (auto it = s.find("SharpenRadius"); it != s.end()) p.sharpRadius = std::strtof(it->second.c_str(), nullptr) * 10.f;
  num("LuminanceSmoothing", p.nrLuma);
  num("ColorNoiseReduction", p.nrColor);

  num("PostCropVignetteAmount", p.vignette);

  p.toneCurve = readCurve(xml, "ToneCurvePV2012");
  p.toneCurveRed = readCurve(xml, "ToneCurvePV2012Red");
  p.toneCurveGreen = readCurve(xml, "ToneCurvePV2012Green");
  p.toneCurveBlue = readCurve(xml, "ToneCurvePV2012Blue");

  // Honest reporting of what we do not reproduce.
  if (auto it = s.find("WhiteBalance"); it != s.end() && it->second != "As Shot")
    out.unsupported.push_back("WhiteBalance=" + it->second + " (absolute Kelvin; kept as shot)");
  static const char* kUnsupported[] = {
      "ShadowTint", "RedHue", "RedSaturation", "GreenHue", "GreenSaturation", "BlueHue", "BlueSaturation",
      "PostCropVignetteMidpoint", "PostCropVignetteFeather", "PostCropVignetteRoundness",
      "PostCropVignetteHighlightContrast", "VignetteAmount", "LensManualDistortionAmount"};
  for (const char* key : kUnsupported)
    if (nonZero(key)) out.unsupported.push_back(key);
  if (auto it = s.find("CameraProfile"); it != s.end() && !it->second.empty() && it->second != "Adobe Standard")
    out.unsupported.push_back("CameraProfile=" + it->second + " (approximated by Dapple base rendering)");
  if (hasRgbTable || xml.find("<crs:Look>") != std::string::npos)
    out.unsupported.push_back("Look/RGBTable (creative profile) - export it as a .cube LUT instead");

  return out;
}

Result<PresetImport> loadXmpPreset(const std::filesystem::path& path) {
  auto text = readTextFile(path);
  if (!text) return std::unexpected(text.error());
  auto preset = parseXmpPreset(*text);
  if (preset && preset->name.empty()) preset->name = pathToUtf8(path.stem());
  return preset;
}

}  // namespace dapple
