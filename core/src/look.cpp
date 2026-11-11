#include "dapple/look.h"

#include <cstdio>

#include "look_internal.h"

namespace dapple {

const std::array<LookInfo, 9>& looks() {
  // Names, tags, categories and default pad points: docs/design/requirements.md 4.12.
  static const std::array<LookInfo, 9> kLooks = {{
      {"std", "标准", "中性 · 不加倾向", "neutral", 0.50f, 0.50f},
      {"warm", "暖阳", "暖色 · 柔和", "warm", 0.56f, 0.30f},
      {"amber", "琥珀", "暖色 · 浓郁", "warm", 0.66f, 0.22f},
      {"rose", "玫瑰金", "暖粉 · 柔和", "warm", 0.50f, 0.36f},
      {"neutral", "淡雅", "低饱和 · 干净", "neutral", 0.38f, 0.50f},
      {"cool", "冷调", "冷色 · 安静", "cool", 0.44f, 0.74f},
      {"teal", "青橙", "冷暖互补 · 电影感", "cool", 0.62f, 0.62f},
      {"film", "胶片", "偏暖 · 灰黑位 · 颗粒", "film", 0.46f, 0.40f},
      {"bw", "黑白", "单色 · 高反差", "bw", 0.50f, 0.50f},
  }};
  return kLooks;
}

int lookIndex(const std::string& id) {
  const auto& l = looks();
  for (std::size_t i = 0; i < l.size(); ++i)
    if (id == l[i].id) return int(i);
  return -1;
}

std::string looksJson() {
  std::string j = "{\"looks\":[";
  const auto& l = looks();
  for (std::size_t i = 0; i < l.size(); ++i) {
    char buf[64];
    std::snprintf(buf, sizeof buf, ",\"padX\":%.2f,\"padY\":%.2f}", double(l[i].padX), double(l[i].padY));
    j += std::string(i ? "," : "") + "{\"id\":\"" + l[i].id + "\",\"name\":\"" + l[i].name + "\",\"tags\":\"" +
         l[i].tags + "\",\"category\":\"" + l[i].category + "\"" + buf;
  }
  return j + "]}";
}

// Recipes translate the prototype's CSS-filter placeholders (LOOKS in Main.dc.html) into OKLab
// operations: saturate() -> satMul, contrast() -> contrast, brightness() -> dL, sepia() -> tint
// (warm, hue 60), grayscale() -> gray, hue-rotate() -> hueRotDeg. Teal & orange gets a real split
// tone, film a lifted black point and grain.
const LookRecipe& lookRecipe(int index) {
  static const LookRecipe kRecipes[9] = {
      /* std     */ {},
      /* warm    */ {1.04f, 1.f, 0.f, 0.010f, 60.f},
      /* amber   */ {1.10f, 1.02f, 0.f, 0.020f, 62.f},
      /* rose    */ {1.02f, 1.f, 0.f, 0.012f, 12.f, 0.f, 0.f, 0.f, 0.f, -8.f},
      /* neutral */ {0.92f, 0.97f, 0.f},
      /* cool    */ {0.98f, 1.f, 0.f, 0.012f, 245.f},
      /* teal    */ {1.08f, 1.04f, 0.f, 0.004f, 62.f, 0.022f, 195.f, 0.020f, 65.f},
      /* film    */ {1.f, 0.94f, 0.015f, 0.010f, 70.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.030f, 20.f},
      /* bw      */ {1.f, 1.12f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
  };
  return kRecipes[index >= 0 && index < 9 ? index : 0];
}

}  // namespace dapple
