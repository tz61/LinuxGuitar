#pragma once
// UI colors (dark, FL Studio-like) and fonts (Noto Sans CJK SC + symbol fallbacks).
#include <imgui.h>

#include <cstdint>

#include "fx/Effect.h"
#include "practice/Practice.h"

namespace gs::ui {

struct Palette {
    ImU32 bg, panel, panel2, frame, border, accent, accent2, green, red, yellow, blue, purple, cyan, text, textDim, grid;
};
extern Palette P;

void applyTheme(float scale);
bool loadFonts();        // returns whether a CJK font was found
ImFont* boldFont();
float S();               // current UI scale (relative to an 18 px font)

ImU32 withAlpha(ImU32 c, float a);
ImU32 lerpColor(ImU32 a, ImU32 b, float t);
ImU32 stringColor(int s);          // 0 = string 1 ... 5 = string 6 (Rocksmith style)
ImU32 categoryColor(FxCategory c);
ImU32 judgeColor(Judge j);
const char* categoryName(FxCategory c);  // English; display through tr()

}  // namespace gs::ui
