#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "common/I18n.h"
#include "ui/Theme.h"

namespace gs::ui {

static const float kPi = 3.14159265f;

float TextWidth(const char* s) { return ImGui::CalcTextSize(s).x; }

std::string formatParam(const ParamInfo& p, float v) {
    char buf[64];
    if (p.scale == ParamScale::Choice) {
        int i = std::clamp((int)std::lround(v), 0, (int)p.choices.size() - 1);
        return p.choices.empty() ? std::to_string(i) : tr(p.choices[i]);
    }
    if (p.scale == ParamScale::Toggle) return v > 0.5f ? tr("On") : tr("Off");
    const std::string& u = p.unit;
    if (u == "Hz") {
        if (v >= 1000.f) std::snprintf(buf, sizeof buf, "%.2f kHz", v / 1000.f);
        else std::snprintf(buf, sizeof buf, v < 10 ? "%.2f Hz" : "%.0f Hz", v);
    } else if (u == "ms") {
        if (v >= 1000.f) std::snprintf(buf, sizeof buf, "%.2f s", v / 1000.f);
        else std::snprintf(buf, sizeof buf, v < 10 ? "%.1f ms" : "%.0f ms", v);
    } else if (u == "s") {
        std::snprintf(buf, sizeof buf, "%.2f s", v);
    } else if (u == "dB") {
        std::snprintf(buf, sizeof buf, "%+.1f dB", v);
    } else if (u == "%") {
        std::snprintf(buf, sizeof buf, "%.0f%%", v);
    } else if (p.scale == ParamScale::Int) {
        std::snprintf(buf, sizeof buf, "%d%s%s", (int)std::lround(v), u.empty() ? "" : " ", u.c_str());
    } else {
        std::snprintf(buf, sizeof buf, "%.2f%s%s", v, u.empty() ? "" : " ", u.c_str());
    }
    return buf;
}

bool Knob(const char* id, const char* label, float* v, float vmin, float vmax, float vdef, bool logScale, float r,
          ImU32 color, const char* fmt, bool integer, const char* tooltip) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float s = S();
    const float lh = ImGui::GetFontSize();
    const float w = std::max(r * 2.f + 14.f * s, TextWidth(label) + 4.f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, r * 2.f + lh * 2.f + 6.f * s));
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    if (logScale && vmin <= 0) logScale = false;
    auto toN = [&](float x) {
        return logScale ? std::log(std::max(x, vmin) / vmin) / std::log(vmax / vmin) : (x - vmin) / (vmax - vmin);
    };
    auto fromN = [&](float t) { return logScale ? vmin * std::pow(vmax / vmin, t) : vmin + t * (vmax - vmin); };
    float t = std::clamp(toN(*v), 0.f, 1.f);
    bool changed = false;
    if (active && io.MouseDelta.y != 0.f) {
        t = std::clamp(t - io.MouseDelta.y * (io.KeyShift ? 0.0012f : 0.0055f), 0.f, 1.f);
        changed = true;
    }
    if (hovered) {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);  // the wheel turns the knob instead of scrolling
        if (io.MouseWheel != 0.f) {
            t = std::clamp(t + io.MouseWheel * (io.KeyShift ? 0.005f : 0.03f), 0.f, 1.f);
            changed = true;
        }
    }
    if (changed) {
        float nv = fromN(t);
        if (integer) nv = std::round(nv);
        *v = std::clamp(nv, vmin, vmax);
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *v = vdef;
        changed = true;
        t = std::clamp(toN(*v), 0.f, 1.f);
    }
    const ImVec2 c(p.x + w * 0.5f, p.y + r + 1.f);
    const float a0 = 0.75f * kPi, a1 = 2.25f * kPi, a = a0 + (a1 - a0) * t;
    dl->AddCircleFilled(c, r, IM_COL32(22, 24, 28, 255), 40);
    dl->PathArcTo(c, r - 3.f * s, a0, a1, 40);
    dl->PathStroke(IM_COL32(60, 65, 74, 255), 0, 3.f * s);
    dl->PathArcTo(c, r - 3.f * s, a0, a, 40);
    dl->PathStroke(active || hovered ? lerpColor(color, IM_COL32_WHITE, 0.25f) : color, 0, 3.f * s);
    const float rk = r - 7.5f * s;
    dl->AddCircleFilled(c, rk, IM_COL32(70, 75, 85, 255), 32);
    dl->AddCircle(c, rk, IM_COL32(95, 102, 114, 255), 32, 1.f);
    const ImVec2 d(std::cos(a), std::sin(a));
    dl->AddLine(ImVec2(c.x + d.x * rk * 0.25f, c.y + d.y * rk * 0.25f), ImVec2(c.x + d.x * (rk - 1.f), c.y + d.y * (rk - 1.f)),
                IM_COL32(240, 240, 240, 255), 2.2f * s);
    // Label + value
    const float lw = TextWidth(label);
    dl->AddText(ImVec2(c.x - lw * 0.5f, p.y + r * 2.f + 3.f * s), P.textDim, label);
    if (fmt && *fmt) {
        char vb[48];
        std::snprintf(vb, sizeof vb, fmt, *v);
        const float vw = TextWidth(vb);
        dl->AddText(ImVec2(c.x - vw * 0.5f, p.y + r * 2.f + 3.f * s + lh), active ? P.accent2 : P.text, vb);
    }
    if (hovered && !active && tooltip) ImGui::SetTooltip("%s", tooltip);
    return changed;
}

bool ParamControl(Effect& fx, int i, float r, ImU32 color, float width) {
    const ParamInfo& p = fx.paramInfo(i);
    const std::string name = tr(p.name);
    float v = fx.param(i);
    bool changed = false;
    ImGui::PushID(i);
    const float s = S();
    if (p.scale == ParamScale::Toggle) {
        const float h = r * 2.f + ImGui::GetFontSize() * 2.f + 6.f * s;
        ImGui::BeginGroup();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float bw = std::max(width, r * 2.f + 14.f * s);
        ImGui::InvisibleButton("tog", ImVec2(bw, h));
        if (ImGui::IsItemClicked()) {
            v = v > 0.5f ? 0.f : 1.f;
            changed = true;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c(pos.x + bw * 0.5f, pos.y + r);
        const bool on = v > 0.5f;
        dl->AddRectFilled(ImVec2(c.x - 16 * s, c.y - 9 * s), ImVec2(c.x + 16 * s, c.y + 9 * s),
                          on ? withAlpha(color, 0.9f) : IM_COL32(30, 32, 36, 255), 9 * s);
        dl->AddCircleFilled(ImVec2(on ? c.x + 7 * s : c.x - 7 * s, c.y), 6.5f * s, IM_COL32(235, 235, 235, 255));
        const float lw = TextWidth(name.c_str());
        dl->AddText(ImVec2(c.x - lw * 0.5f, pos.y + r * 2.f + 3.f * s), P.textDim, name.c_str());
        const char* st = on ? tr("On") : tr("Off");
        dl->AddText(ImVec2(c.x - TextWidth(st) * 0.5f, pos.y + r * 2.f + 3.f * s + ImGui::GetFontSize()), P.text, st);
        ImGui::EndGroup();
    } else if (p.scale == ParamScale::Choice) {
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", name.c_str());
        ImGui::SetNextItemWidth(width);
        int cur = std::clamp((int)std::lround(v), 0, std::max(0, (int)p.choices.size() - 1));
        if (ImGui::BeginCombo("##c", p.choices.empty() ? "" : tr(p.choices[cur].c_str()), ImGuiComboFlags_HeightLarge)) {
            for (int k = 0; k < (int)p.choices.size(); ++k)
                if (ImGui::Selectable(tr(p.choices[k].c_str()), k == cur)) {
                    v = (float)k;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        ImGui::EndGroup();
    } else {
        std::string val = formatParam(p, v);
        float tmp = v;
        char tip[256];
        std::snprintf(tip, sizeof tip, tr("%s: %s  (range %s ~ %s, double-click to reset to %s)"), name.c_str(), val.c_str(),
                      formatParam(p, p.minV).c_str(), formatParam(p, p.maxV).c_str(), formatParam(p, p.defV).c_str());
        // The knob draws no value text itself; we draw the formatted value (with units) underneath
        ImGui::BeginGroup();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        changed = Knob("k", name.c_str(), &tmp, p.minV, p.maxV, p.defV, p.scale == ParamScale::Log, r, color, nullptr,
                       p.scale == ParamScale::Int, tip);
        if (changed) v = tmp;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = std::max(r * 2.f + 14.f * s, TextWidth(name.c_str()) + 4.f);
        val = formatParam(p, changed ? v : fx.param(i));
        dl->AddText(ImVec2(pos.x + w * 0.5f - TextWidth(val.c_str()) * 0.5f, pos.y + r * 2.f + 3.f * s + ImGui::GetFontSize()),
                    ImGui::IsItemActive() ? P.accent2 : P.text, val.c_str());
        ImGui::EndGroup();
    }
    if (changed) fx.setParam(i, v);
    ImGui::PopID();
    return changed;
}

void Meter(float db, ImVec2 size, float* peakHold, bool clip, bool vertical) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    const float lo = -60.f, hi = 3.f;
    auto norm = [&](float d) { return std::clamp((d - lo) / (hi - lo), 0.f, 1.f); };
    const float t = norm(db);
    if (peakHold) {
        const float dt = ImGui::GetIO().DeltaTime;
        *peakHold = std::max(db, *peakHold - 20.f * dt);  // falls back at 20 dB/s
    }
    const ImVec2 q(p.x + size.x, p.y + size.y);
    dl->AddRectFilled(p, q, IM_COL32(16, 18, 21, 255), 3.f);
    const float yel = norm(-12.f), red = norm(-3.f);
    auto seg = [&](float a, float b, ImU32 col) {
        if (b <= a) return;
        if (vertical)
            dl->AddRectFilled(ImVec2(p.x + 1, q.y - 1 - (size.y - 2) * b), ImVec2(q.x - 1, q.y - 1 - (size.y - 2) * a), col, 2.f);
        else
            dl->AddRectFilled(ImVec2(p.x + 1 + (size.x - 2) * a, p.y + 1), ImVec2(p.x + 1 + (size.x - 2) * b, q.y - 1), col, 2.f);
    };
    seg(0.f, std::min(t, yel), IM_COL32(90, 200, 90, 255));
    seg(yel, std::min(t, red), IM_COL32(230, 200, 60, 255));
    seg(red, t, IM_COL32(240, 70, 60, 255));
    if (peakHold && *peakHold > lo) {
        float pt = norm(*peakHold);
        if (vertical) {
            float y = q.y - 1 - (size.y - 2) * pt;
            dl->AddLine(ImVec2(p.x + 1, y), ImVec2(q.x - 1, y), IM_COL32(255, 255, 255, 200), 2.f);
        } else {
            float x = p.x + 1 + (size.x - 2) * pt;
            dl->AddLine(ImVec2(x, p.y + 1), ImVec2(x, q.y - 1), IM_COL32(255, 255, 255, 200), 2.f);
        }
    }
    // 0 dB tick
    const float z = norm(0.f);
    if (vertical) dl->AddLine(ImVec2(p.x, q.y - size.y * z), ImVec2(q.x, q.y - size.y * z), IM_COL32(255, 255, 255, 60));
    else dl->AddLine(ImVec2(p.x + size.x * z, p.y), ImVec2(p.x + size.x * z, q.y), IM_COL32(255, 255, 255, 60));
    if (clip) dl->AddRect(p, q, IM_COL32(255, 60, 60, 255), 3.f, 0, 2.f);
}

bool PowerButton(const char* id, bool on, float size, ImU32 color) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    const float r = size * 0.42f;
    dl->AddCircleFilled(c, r, on ? withAlpha(color, 0.25f) : IM_COL32(25, 27, 31, 255), 24);
    const ImU32 col = on ? lerpColor(color, IM_COL32_WHITE, 0.2f) : IM_COL32(110, 115, 125, 255);
    dl->PathArcTo(c, r * 0.6f, -0.5f * kPi + 0.7f, 1.5f * kPi - 0.7f, 24);
    dl->PathStroke(col, 0, 2.f * S());
    dl->AddLine(ImVec2(c.x, c.y - r * 0.75f), ImVec2(c.x, c.y - r * 0.05f), col, 2.f * S());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", on ? tr("Click to bypass (turn this effect off)") : tr("Click to turn this effect on"));
    return clicked;
}

void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float sz, ImU32 col) {
    const float h = sz * 0.5f;
    switch (icon) {
        case IconPlay:
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.55f, c.y - h * 0.7f), ImVec2(c.x - h * 0.55f, c.y + h * 0.7f),
                                  ImVec2(c.x + h * 0.75f, c.y), col);
            break;
        case IconPause:
            dl->AddRectFilled(ImVec2(c.x - h * 0.6f, c.y - h * 0.65f), ImVec2(c.x - h * 0.15f, c.y + h * 0.65f), col, 1.5f);
            dl->AddRectFilled(ImVec2(c.x + h * 0.15f, c.y - h * 0.65f), ImVec2(c.x + h * 0.6f, c.y + h * 0.65f), col, 1.5f);
            break;
        case IconStop:
            dl->AddRectFilled(ImVec2(c.x - h * 0.55f, c.y - h * 0.55f), ImVec2(c.x + h * 0.55f, c.y + h * 0.55f), col, 2.f);
            break;
        case IconRec: dl->AddCircleFilled(c, h * 0.6f, col, 24); break;
        case IconBack:
            dl->AddRectFilled(ImVec2(c.x - h * 0.7f, c.y - h * 0.6f), ImVec2(c.x - h * 0.45f, c.y + h * 0.6f), col);
            dl->AddTriangleFilled(ImVec2(c.x + h * 0.6f, c.y - h * 0.6f), ImVec2(c.x + h * 0.6f, c.y + h * 0.6f),
                                  ImVec2(c.x - h * 0.4f, c.y), col);
            break;
        case IconLoop: {
            dl->PathArcTo(c, h * 0.6f, 0.2f, 2.f * kPi - 0.6f, 24);
            dl->PathStroke(col, 0, 2.f * S());
            ImVec2 tip(c.x + h * 0.6f * std::cos(0.2f), c.y + h * 0.6f * std::sin(0.2f));
            dl->AddTriangleFilled(ImVec2(tip.x - h * 0.3f, tip.y - h * 0.05f), ImVec2(tip.x + h * 0.3f, tip.y - h * 0.05f),
                                  ImVec2(tip.x, tip.y + h * 0.35f), col);
            break;
        }
        case IconPlus:
            dl->AddLine(ImVec2(c.x - h * 0.6f, c.y), ImVec2(c.x + h * 0.6f, c.y), col, 2.5f * S());
            dl->AddLine(ImVec2(c.x, c.y - h * 0.6f), ImVec2(c.x, c.y + h * 0.6f), col, 2.5f * S());
            break;
        case IconMenu:
            for (int k = -1; k <= 1; ++k) dl->AddCircleFilled(ImVec2(c.x, c.y + k * h * 0.45f), h * 0.13f, col);
            break;
        case IconFolder:
            dl->AddRectFilled(ImVec2(c.x - h * 0.7f, c.y - h * 0.4f), ImVec2(c.x + h * 0.7f, c.y + h * 0.5f), col, 2.f);
            dl->AddRectFilled(ImVec2(c.x - h * 0.7f, c.y - h * 0.55f), ImVec2(c.x - h * 0.1f, c.y - h * 0.3f), col, 2.f);
            break;
        case IconWait:
            dl->AddCircle(c, h * 0.62f, col, 24, 2.f * S());
            dl->AddLine(c, ImVec2(c.x, c.y - h * 0.4f), col, 2.f * S());
            dl->AddLine(c, ImVec2(c.x + h * 0.3f, c.y), col, 2.f * S());
            break;
        case IconNote:
            dl->AddCircleFilled(ImVec2(c.x - h * 0.2f, c.y + h * 0.4f), h * 0.28f, col);
            dl->AddLine(ImVec2(c.x + h * 0.06f, c.y + h * 0.4f), ImVec2(c.x + h * 0.06f, c.y - h * 0.6f), col, 2.f * S());
            dl->AddLine(ImVec2(c.x + h * 0.06f, c.y - h * 0.6f), ImVec2(c.x + h * 0.45f, c.y - h * 0.35f), col, 2.f * S());
            break;
    }
}

bool IconButton(const char* id, Icon icon, ImVec2 size, bool active, ImU32 activeColor, const char* tooltip) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = active ? (activeColor ? activeColor : P.accent) : (held ? IM_COL32(80, 86, 96, 255) : hov ? IM_COL32(70, 76, 86, 255) : IM_COL32(56, 62, 71, 255));
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, 5.f * S());
    DrawIcon(dl, icon, ImVec2(p.x + size.x * 0.5f, p.y + size.y * 0.5f), std::min(size.x, size.y) * 0.62f,
             active ? IM_COL32(20, 20, 20, 255) : P.text);
    if (hov && tooltip) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

bool ToggleChip(const char* label, bool* v, ImU32 onColor, const char* tooltip) {
    if (!onColor) onColor = P.accent;
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float s = S();
    const ImVec2 size(ts.x + 30.f * s, ts.y + 10.f * s);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(label, size);
    if (clicked) *v = !*v;
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y),
                      *v ? withAlpha(onColor, hov ? 0.45f : 0.32f) : (hov ? IM_COL32(64, 70, 80, 255) : IM_COL32(48, 53, 61, 255)),
                      size.y * 0.5f);
    dl->AddCircleFilled(ImVec2(p.x + 11.f * s, p.y + size.y * 0.5f), 4.5f * s, *v ? onColor : IM_COL32(100, 105, 115, 255));
    dl->AddText(ImVec2(p.x + 20.f * s, p.y + 5.f * s), *v ? P.text : P.textDim, label);
    if (hov && tooltip) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

void CentsGauge(float cents, bool valid, ImVec2 size, float green) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    const ImVec2 q(p.x + size.x, p.y + size.y);
    dl->AddRectFilled(p, q, IM_COL32(18, 20, 24, 255), 4.f);
    const float cx = p.x + size.x * 0.5f;
    const float gw = size.x * 0.5f * green / 50.f;
    dl->AddRectFilled(ImVec2(cx - gw, p.y + 2), ImVec2(cx + gw, q.y - 2), IM_COL32(60, 140, 70, 120), 2.f);
    for (int k = -50; k <= 50; k += 10) {
        float x = cx + size.x * 0.5f * k / 50.f * 0.96f;
        float th = (k == 0) ? size.y * 0.5f : size.y * 0.25f;
        dl->AddLine(ImVec2(x, q.y - th), ImVec2(x, q.y - 2), IM_COL32(120, 125, 135, 255), 1.f);
    }
    if (valid) {
        float c = std::clamp(cents, -50.f, 50.f);
        float x = cx + size.x * 0.5f * c / 50.f * 0.96f;
        ImU32 col = std::fabs(cents) <= green ? P.green : (std::fabs(cents) <= 15.f ? P.yellow : P.red);
        dl->AddRectFilled(ImVec2(x - 3.f * S(), p.y + 3), ImVec2(x + 3.f * S(), q.y - 3), col, 2.f);
    }
}

void ChordDiagram(ImDrawList* dl, ImVec2 pos, float w, float h, const int f[6], const char* name, ImU32 col) {
    const float s = S();
    int maxF = 0, minF = 99;
    for (int i = 0; i < 6; ++i)
        if (f[i] > 0) {
            maxF = std::max(maxF, f[i]);
            minF = std::min(minF, f[i]);
        }
    int first = (maxF <= 4) ? 1 : minF;
    const int nFrets = 4;
    const float top = pos.y + ImGui::GetFontSize() * 2.2f, left = pos.x + 14.f * s;
    const float gw = w - 28.f * s, gh = h - (top - pos.y) - 6.f * s;
    const float dx = gw / 5.f, dy = gh / nFrets;
    if (name && *name) {
        ImGui::PushFont(boldFont(), ImGui::GetStyle().FontSizeBase * 1.3f);
        float tw = ImGui::CalcTextSize(name).x;
        dl->AddText(ImVec2(pos.x + w * 0.5f - tw * 0.5f, pos.y), col, name);
        ImGui::PopFont();
    }
    if (first == 1) dl->AddRectFilled(ImVec2(left - 1, top - 4.f * s), ImVec2(left + gw + 1, top), P.text);
    else {
        char b[16];
        std::snprintf(b, sizeof b, "%d", first);
        dl->AddText(ImVec2(pos.x - 2.f * s, top + dy * 0.2f), P.textDim, b);
    }
    for (int i = 0; i <= nFrets; ++i) dl->AddLine(ImVec2(left, top + i * dy), ImVec2(left + gw, top + i * dy), P.grid, 1.5f);
    for (int i = 0; i < 6; ++i) dl->AddLine(ImVec2(left + i * dx, top), ImVec2(left + i * dx, top + gh), P.textDim, 1.2f);
    for (int i = 0; i < 6; ++i) {
        const float x = left + i * dx;
        if (f[i] < 0) {
            const float r = 4.f * s;
            dl->AddLine(ImVec2(x - r, top - 13.f * s - r), ImVec2(x + r, top - 13.f * s + r), P.red, 2.f);
            dl->AddLine(ImVec2(x - r, top - 13.f * s + r), ImVec2(x + r, top - 13.f * s - r), P.red, 2.f);
        } else if (f[i] == 0) {
            dl->AddCircle(ImVec2(x, top - 13.f * s), 4.5f * s, P.text, 16, 1.6f);
        } else {
            const float y = top + (f[i] - first + 0.5f) * dy;
            dl->AddCircleFilled(ImVec2(x, y), std::min(dx, dy) * 0.32f, stringColor(5 - i), 20);
        }
    }
}

void Fretboard(ImDrawList* dl, ImVec2 pos, ImVec2 size, int firstFret, int lastFret, const FretMark* marks, int n) {
    const float s = S();
    const int nf = std::max(1, lastFret - firstFret + 1);
    const float labelW = 22.f * s;
    const float x0 = pos.x + labelW, x1 = pos.x + size.x - 4.f * s;
    const float y0 = pos.y + 4.f * s, y1 = pos.y + size.y - ImGui::GetFontSize() - 2.f * s;
    const float fw = (x1 - x0) / nf, sh = (y1 - y0) / 5.f;
    dl->AddRectFilled(ImVec2(x0, y0 - 4.f * s), ImVec2(x1, y1 + 4.f * s), IM_COL32(58, 42, 30, 255), 4.f);
    // Fret position markers
    for (int f = firstFret; f <= lastFret; ++f) {
        float cx = x0 + (f - firstFret + 0.5f) * fw;
        if (f == 3 || f == 5 || f == 7 || f == 9 || f == 15 || f == 17)
            dl->AddCircleFilled(ImVec2(cx, (y0 + y1) * 0.5f), 4.f * s, IM_COL32(200, 190, 170, 120));
        if (f == 12) {
            dl->AddCircleFilled(ImVec2(cx, y0 + sh * 1.5f), 4.f * s, IM_COL32(200, 190, 170, 120));
            dl->AddCircleFilled(ImVec2(cx, y0 + sh * 3.5f), 4.f * s, IM_COL32(200, 190, 170, 120));
        }
        char b[16];
        std::snprintf(b, sizeof b, "%d", f);
        dl->AddText(ImVec2(cx - TextWidth(b) * 0.5f, y1 + 5.f * s), P.textDim, b);
    }
    for (int f = 0; f <= nf; ++f) {
        float x = x0 + f * fw;
        bool nut = (firstFret + f == 1) && f == 0;
        dl->AddLine(ImVec2(x, y0 - 4.f * s), ImVec2(x, y1 + 4.f * s), nut ? IM_COL32(230, 225, 210, 255) : IM_COL32(170, 170, 175, 255),
                    nut ? 4.f * s : 1.5f);
    }
    static const char* names[6] = {"e", "B", "G", "D", "A", "E"};
    const float labelSize = std::min(ImGui::GetFontSize(), sh * 1.05f);  // shrink labels when strings are close together
    for (int st = 0; st < 6; ++st) {
        float y = y0 + st * sh;
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), IM_COL32(200, 200, 205, 200), 1.f + st * 0.35f * s);
        if (labelSize >= 7.f) dl->AddText(nullptr, labelSize, ImVec2(pos.x + 4.f * s, y - labelSize * 0.5f), stringColor(st), names[st]);
    }
    for (int i = 0; i < n; ++i) {
        const FretMark& m = marks[i];
        if (m.string < 0 || m.string > 5) continue;
        float y = y0 + m.string * sh;
        float x;
        if (m.fret == 0) x = x0 - 10.f * s;
        else if (m.fret < firstFret || m.fret > lastFret) continue;
        else x = x0 + (m.fret - firstFret + 0.5f) * fw;
        float r = std::min(sh * 0.48f, 11.f * s);
        if (m.fret == 0) dl->AddCircle(ImVec2(x, y), r * 0.8f, m.col, 20, 2.5f);
        else dl->AddCircleFilled(ImVec2(x, y), r, m.col, 20);
        if (m.label && m.fret > 0) {
            float tw = TextWidth(m.label);
            dl->AddText(ImVec2(x - tw * 0.5f, y - ImGui::GetFontSize() * 0.5f), IM_COL32(20, 20, 20, 255), m.label);
        }
    }
}

void SameLineIfFits(float w) {
    const float lastX2 = ImGui::GetItemRectMax().x;
    const float maxX = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (lastX2 + ImGui::GetStyle().ItemSpacing.x + w <= maxX) ImGui::SameLine();
}

void HelpTip(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void SectionTitle(const char* text, ImU32 col) {
    ImGui::PushFont(boldFont(), 0.f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col ? col : P.accent2));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

}  // namespace gs::ui
