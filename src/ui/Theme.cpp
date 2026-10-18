#include "ui/Theme.h"

#include <algorithm>
#include <cstdio>

namespace gs::ui {

Palette P = {
    IM_COL32(30, 33, 38, 255),     // bg
    IM_COL32(40, 44, 51, 255),     // panel
    IM_COL32(50, 55, 63, 255),     // panel2
    IM_COL32(24, 26, 30, 255),     // frame
    IM_COL32(62, 68, 78, 255),     // border
    IM_COL32(243, 156, 51, 255),   // accent (orange)
    IM_COL32(255, 196, 92, 255),   // accent2
    IM_COL32(120, 214, 98, 255),   // green
    IM_COL32(236, 84, 72, 255),    // red
    IM_COL32(244, 208, 63, 255),   // yellow
    IM_COL32(74, 163, 223, 255),   // blue
    IM_COL32(170, 120, 240, 255),  // purple
    IM_COL32(64, 200, 210, 255),   // cyan
    IM_COL32(232, 234, 237, 255),  // text
    IM_COL32(140, 147, 158, 255),  // textDim
    IM_COL32(70, 76, 86, 255),     // grid
};

static ImFont* g_bold = nullptr;

static bool exists(const char* p) {
    FILE* f = std::fopen(p, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

bool loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const char* cjk = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
    const char* cjkBold = "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc";
    const char* droid = "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf";
    const char* wqy = "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc";
    const char* dejavu = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    const char* dejavuBold = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
    const char* sym2 = "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf";
    bool ok = true;
    auto addMain = [&](const char* ttc, const char* fallbackLatin) -> ImFont* {
        ImFont* f = nullptr;
        if (exists(ttc)) {
            ImFontConfig c;
            c.FontNo = 2;  // 3rd face in the TTC = Noto Sans CJK SC (Simplified Chinese)
            f = io.Fonts->AddFontFromFileTTF(ttc, 18.f, &c);
        } else if (exists(droid)) {
            f = io.Fonts->AddFontFromFileTTF(droid, 18.f);
        } else if (exists(wqy)) {
            f = io.Fonts->AddFontFromFileTTF(wqy, 18.f);
        }
        if (!f) {
            ok = false;
            f = io.Fonts->AddFontDefault();
        }
        // fallback fonts for missing glyphs (symbols such as \u2717 \u21bb \u23f8)
        ImFontConfig m;
        m.MergeMode = true;
        if (exists(fallbackLatin)) io.Fonts->AddFontFromFileTTF(fallbackLatin, 18.f, &m);
        if (exists(sym2)) io.Fonts->AddFontFromFileTTF(sym2, 18.f, &m);
        return f;
    };
    addMain(cjk, dejavu);
    g_bold = addMain(cjkBold, dejavuBold);
    return ok;
}

ImFont* boldFont() { return g_bold ? g_bold : ImGui::GetFont(); }

float S() { return ImGui::GetStyle().FontSizeBase / 18.f * ImGui::GetStyle().FontScaleMain; }

void applyTheme(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 6.f;
    st.ChildRounding = 6.f;
    st.FrameRounding = 4.f;
    st.PopupRounding = 6.f;
    st.GrabRounding = 4.f;
    st.TabRounding = 4.f;
    st.ScrollbarRounding = 6.f;
    st.WindowBorderSize = 0.f;
    st.ChildBorderSize = 1.f;
    st.FrameBorderSize = 0.f;
    st.WindowPadding = ImVec2(10, 10);
    st.FramePadding = ImVec2(8, 5);
    st.ItemSpacing = ImVec2(8, 7);
    st.ItemInnerSpacing = ImVec2(6, 5);
    st.ScrollbarSize = 14.f;
    st.GrabMinSize = 12.f;
    st.ScaleAllSizes(scale);
    st.FontSizeBase = 18.f * scale;
    st.FontScaleMain = 1.f;

    auto C = [](ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); };
    ImVec4* col = st.Colors;
    col[ImGuiCol_Text] = C(P.text);
    col[ImGuiCol_TextDisabled] = C(P.textDim);
    col[ImGuiCol_WindowBg] = C(P.bg);
    col[ImGuiCol_ChildBg] = C(P.panel);
    col[ImGuiCol_PopupBg] = C(IM_COL32(36, 40, 46, 250));
    col[ImGuiCol_Border] = C(P.border);
    col[ImGuiCol_FrameBg] = C(P.frame);
    col[ImGuiCol_FrameBgHovered] = C(IM_COL32(44, 48, 56, 255));
    col[ImGuiCol_FrameBgActive] = C(IM_COL32(54, 58, 66, 255));
    col[ImGuiCol_TitleBg] = C(P.panel);
    col[ImGuiCol_TitleBgActive] = C(P.panel2);
    col[ImGuiCol_MenuBarBg] = C(P.panel);
    col[ImGuiCol_ScrollbarBg] = C(IM_COL32(0, 0, 0, 0));
    col[ImGuiCol_ScrollbarGrab] = C(IM_COL32(70, 76, 86, 255));
    col[ImGuiCol_ScrollbarGrabHovered] = C(IM_COL32(90, 96, 108, 255));
    col[ImGuiCol_ScrollbarGrabActive] = C(P.accent);
    col[ImGuiCol_CheckMark] = C(P.accent);
    col[ImGuiCol_SliderGrab] = C(P.accent);
    col[ImGuiCol_SliderGrabActive] = C(P.accent2);
    col[ImGuiCol_Button] = C(IM_COL32(58, 64, 73, 255));
    col[ImGuiCol_ButtonHovered] = C(IM_COL32(74, 81, 92, 255));
    col[ImGuiCol_ButtonActive] = C(IM_COL32(200, 128, 40, 255));
    col[ImGuiCol_Header] = C(IM_COL32(64, 70, 80, 255));
    col[ImGuiCol_HeaderHovered] = C(IM_COL32(78, 85, 96, 255));
    col[ImGuiCol_HeaderActive] = C(IM_COL32(200, 128, 40, 255));
    col[ImGuiCol_Separator] = C(P.border);
    col[ImGuiCol_Tab] = C(P.panel);
    col[ImGuiCol_TabHovered] = C(IM_COL32(78, 85, 96, 255));
    col[ImGuiCol_TabSelected] = C(IM_COL32(64, 70, 80, 255));
    col[ImGuiCol_PlotHistogram] = C(P.accent);
    col[ImGuiCol_TableHeaderBg] = C(P.panel2);
    col[ImGuiCol_TableRowBgAlt] = C(IM_COL32(255, 255, 255, 8));
    col[ImGuiCol_DragDropTarget] = C(P.accent);
    col[ImGuiCol_NavCursor] = C(P.accent);
    col[ImGuiCol_ModalWindowDimBg] = C(IM_COL32(0, 0, 0, 140));
}

ImU32 withAlpha(ImU32 c, float a) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.w = std::clamp(a, 0.f, 1.f) * v.w;
    return ImGui::ColorConvertFloat4ToU32(v);
}

ImU32 lerpColor(ImU32 a, ImU32 b, float t) {
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    t = std::clamp(t, 0.f, 1.f);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t,
                                                 x.w + (y.w - x.w) * t));
}

ImU32 stringColor(int s) {
    static const ImU32 c[6] = {
        IM_COL32(186, 110, 255, 255),  // string 1 (e) purple
        IM_COL32(64, 220, 132, 255),   // string 2 (B) green
        IM_COL32(255, 160, 40, 255),   // string 3 (G) orange
        IM_COL32(70, 168, 255, 255),   // string 4 (D) blue
        IM_COL32(255, 214, 64, 255),   // string 5 (A) yellow
        IM_COL32(255, 80, 80, 255),    // string 6 (E) red
    };
    return (s >= 0 && s < 6) ? c[s] : P.textDim;
}

ImU32 categoryColor(FxCategory c) {
    switch (c) {
        case FxCategory::Dynamics: return IM_COL32(43, 179, 163, 255);
        case FxCategory::Drive: return IM_COL32(240, 140, 46, 255);
        case FxCategory::Amp: return IM_COL32(204, 82, 60, 255);
        case FxCategory::Cabinet: return IM_COL32(166, 116, 74, 255);
        case FxCategory::EQ: return IM_COL32(108, 142, 164, 255);
        case FxCategory::Filter: return IM_COL32(118, 196, 74, 255);
        case FxCategory::Modulation: return IM_COL32(96, 126, 255, 255);
        case FxCategory::Delay: return IM_COL32(47, 181, 214, 255);
        case FxCategory::Reverb: return IM_COL32(150, 110, 220, 255);
        case FxCategory::Pitch: return IM_COL32(214, 93, 177, 255);
        default: return IM_COL32(138, 143, 152, 255);
    }
}

const char* categoryName(FxCategory c) {
    switch (c) {
        case FxCategory::Dynamics: return "Dynamics";
        case FxCategory::Drive: return "Drive & Distortion";
        case FxCategory::Amp: return "Amp";
        case FxCategory::Cabinet: return "Cabinet";
        case FxCategory::EQ: return "EQ";
        case FxCategory::Filter: return "Filter";
        case FxCategory::Modulation: return "Modulation";
        case FxCategory::Delay: return "Delay";
        case FxCategory::Reverb: return "Reverb";
        case FxCategory::Pitch: return "Pitch";
        default: return "Utility";
    }
}

ImU32 judgeColor(Judge j) {
    switch (j) {
        case Judge::Perfect: return IM_COL32(110, 240, 120, 255);
        case Judge::Good: return IM_COL32(150, 220, 90, 255);
        case Judge::Ok: return IM_COL32(220, 210, 80, 255);
        case Judge::WrongPitch: return IM_COL32(255, 120, 60, 255);
        case Judge::Miss: return IM_COL32(120, 120, 130, 255);
        default: return P.text;
    }
}

}  // namespace gs::ui
