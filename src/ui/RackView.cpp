// Effects rack (pedalboard): one card per effect; drag the title to reorder, knobs for parameters, power button bypasses.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/I18n.h"
#include "fx/Preset.h"
#include "ui/App.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace gs {

using namespace ui;

namespace {
constexpr int kCols = 3;
float knobRadius() { return 22.f * S(); }
float knobCellH() { return knobRadius() * 2.f + ImGui::GetFontSize() * 2.f + 12.f * S(); }
float choiceRowH() { return ImGui::GetFrameHeight() + ImGui::GetFontSize() + 12.f * S(); }
}  // namespace

float App::fxCardHeight(const Effect& fx, float) const {
    const float s = S();
    float h = 40.f * s;  // title bar
    int cells = 0;
    for (int i = 0; i < fx.numParams(); ++i) {
        if (fx.paramInfo(i).scale == ParamScale::Choice) h += choiceRowH();
        else ++cells;
    }
    h += ((cells + kCols - 1) / kCols) * knobCellH();
    h += fx.numMeters() * (ImGui::GetFontSize() + 8.f * s);
    if (fx.acceptsFile()) h += ImGui::GetFrameHeight() + ImGui::GetFontSize() + 10.f * s;
    return h + 12.f * s;
}

void App::drawFxCard(int idx, float cardW, float cardH) {
    const float s = S();
    FxSlot& slot = *engine_.chain()[idx];
    Effect& fx = *slot.fx;
    const ImU32 col = categoryColor(fx.category());
    const bool on = !fx.bypassed.load();
    ImGui::PushID((int)slot.id);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(on ? IM_COL32(46, 51, 59, 255) : IM_COL32(36, 39, 44, 255)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.f * s);
    ImGui::BeginChild("card", ImVec2(cardW, cardH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    // Title bar
    const float th = 34.f * s;
    dl->AddRectFilled(p, ImVec2(p.x + cardW, p.y + th), on ? col : withAlpha(col, 0.35f), 8.f * s, ImDrawFlags_RoundCornersTop);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 6 * s, p.y + 4 * s));
    if (PowerButton("pwr", on, 26.f * s, IM_COL32(255, 255, 255, 255))) fx.bypassed = on;
    ImGui::SameLine(0, 4 * s);
    // Title (drag to reorder)
    const float titleW = cardW - 74 * s;
    const ImVec2 tp = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("title", ImVec2(titleW, 26 * s));
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
        int payload = idx;
        ImGui::SetDragDropPayload("GS_FX", &payload, sizeof payload);
        ImGui::Text(tr("Move %s"), tr(fx.displayName()));
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GS_FX")) {
            int from = *(const int*)pl->Data;
            engine_.moveEffect(from, idx);
            curPreset_.clear();
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(0)) {
        for (auto& t : effectTypes())
            if (t.typeId == fx.typeId()) ImGui::SetTooltip(tr("%s\n(drag the title to change the order)"), tr(t.description.c_str()));
    }
    ImGui::PushFont(boldFont(), 0.f);
    dl->PushClipRect(tp, ImVec2(tp.x + titleW, tp.y + 26 * s), true);
    dl->AddText(ImVec2(tp.x + 2 * s, tp.y + 3 * s), on ? IM_COL32(20, 20, 22, 255) : P.textDim, tr(fx.displayName()));
    dl->PopClipRect();
    ImGui::PopFont();
    ImGui::SameLine(0, 2 * s);
    ImGui::SetCursorScreenPos(ImVec2(p.x + cardW - 34 * s, p.y + 4 * s));
    if (IconButton("menu", IconMenu, ImVec2(28 * s, 26 * s))) ImGui::OpenPopup("fxmenu");
    bool removed = false;
    if (ImGui::BeginPopup("fxmenu")) {
        if (ImGui::MenuItem(tr("Reset to defaults"))) fx.resetParamsToDefault();
        if (ImGui::MenuItem(on ? tr("Bypass (turn off)") : tr("Turn on"))) fx.bypassed = on;
        if (ImGui::MenuItem(tr("Move left"), nullptr, false, idx > 0)) engine_.moveEffect(idx, idx - 1);
        if (ImGui::MenuItem(tr("Move right"), nullptr, false, idx + 1 < (int)engine_.chain().size())) engine_.moveEffect(idx, idx + 1);
        if (ImGui::MenuItem(tr("Insert an effect after this..."))) {
            addFxInsertAt_ = idx + 1;
            addFxOpen_ = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Delete"))) removed = true;
        ImGui::EndPopup();
    }

    // Parameters
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + th + 6 * s));
    const float innerW = cardW - 16 * s;
    const float cellW = innerW / kCols;
    const float r = knobRadius();
    // Choice parameters first (full rows)
    for (int i = 0; i < fx.numParams(); ++i) {
        if (fx.paramInfo(i).scale != ParamScale::Choice) continue;
        ImGui::SetCursorScreenPos(ImVec2(p.x + 10 * s, ImGui::GetCursorScreenPos().y));
        if (ParamControl(fx, i, r, col, innerW - 4 * s)) curPreset_.clear();
        ImGui::Dummy(ImVec2(0, 2 * s));
    }
    int cell = 0;
    float rowY = ImGui::GetCursorScreenPos().y;
    for (int i = 0; i < fx.numParams(); ++i) {
        if (fx.paramInfo(i).scale == ParamScale::Choice) continue;
        const int c = cell % kCols;
        if (c == 0 && cell > 0) rowY += knobCellH();
        const float kw = std::max(r * 2.f + 14.f * s, TextWidth(tr(fx.paramInfo(i).name.c_str())) + 4.f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 8 * s + c * cellW + (cellW - kw) * 0.5f, rowY));
        if (ParamControl(fx, i, r, on ? col : P.textDim, cellW)) curPreset_.clear();
        ++cell;
    }
    float y = rowY + (cell > 0 ? knobCellH() : 0.f);
    // Meters
    for (int m = 0; m < fx.numMeters(); ++m) {
        const char* nm = fx.meterName(m);
        float v = fx.meterValue(m);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 10 * s, y));
        ImGui::TextDisabled("%s", tr(nm));
        const float labelW = std::max(84 * s, TextWidth(tr(nm)) + 10 * s);
        float bw = std::max(30 * s, innerW - 16 * s - labelW);
        ImVec2 bp(p.x + 10 * s + labelW, y + 5 * s);
        dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + 8 * s), IM_COL32(20, 22, 26, 255), 3 * s);
        // dB-valued meters (e.g. compressor gain reduction, positive) have "dB" in their name; others are 0..1
        bool isDb = std::strstr(nm, "dB") != nullptr;
        float t = isDb ? std::clamp(v / 24.f, 0.f, 1.f) : std::clamp(v, 0.f, 1.f);
        dl->AddRectFilled(bp, ImVec2(bp.x + bw * t, bp.y + 8 * s), isDb ? P.yellow : P.green, 3 * s);
        y += ImGui::GetFontSize() + 8 * s;
    }
    // File (cabinet IR)
    if (fx.acceptsFile()) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + 10 * s, y + 2 * s));
        if (ImGui::Button(tr("Load IR file..."), ImVec2(innerW - 4 * s, 0))) {
            irTargetSlot_ = (int)slot.id;
            fileDlg_.open(FileDialog::IR, tr("Choose a cabinet impulse response (WAV)"), "*.wav *.WAV", homeDir());
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Use any free guitar cabinet IR from the internet (WAV file), or just drop a WAV onto the window"));
        std::string lab = fx.fileLabel();
        ImGui::SetCursorScreenPos(ImVec2(p.x + 10 * s, ImGui::GetCursorScreenPos().y));
        ImGui::PushTextWrapPos(p.x + cardW - 8 * s);
        ImGui::TextDisabled("%s", lab.empty() ? tr("Using the built-in cabinet model") : lab.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!on) dl->AddRectFilled(ImVec2(p.x, p.y + th), ImVec2(p.x + cardW, p.y + cardH), IM_COL32(20, 22, 25, 90), 8.f * s,
                               ImDrawFlags_RoundCornersBottom);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopID();
    if (removed) {
        engine_.removeEffect(idx);
        curPreset_.clear();
    }
}

void App::drawAddFxPopup() {
    if (addFxOpen_) {
        ImGui::OpenPopup("##addfx");
        addFxOpen_ = false;
    }
    const float s = S();
    ImGui::SetNextWindowSize(ImVec2(560 * s, 0));
    if (ImGui::BeginPopup("##addfx")) {
        SectionTitle(tr("Add effect"));
        ImGui::TextDisabled("%s", tr("Typical order: Noise Gate > Compressor > Drive > Amp > Cabinet > EQ > Modulation > Delay > Reverb"));
        ImGui::Separator();
        FxCategory last = (FxCategory)255;
        if (ImGui::BeginTable("fxt", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthFixed, 150 * s);
            ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch);
            for (const auto& t : effectTypes()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (t.category != last) {
                    last = t.category;
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(categoryColor(t.category)), "\xe2\x96\xa0 %s", tr(categoryName(t.category)));
                }
                ImGui::TableNextColumn();
                ImGui::PushID(t.typeId.c_str());
                if (ImGui::Selectable(tr(t.displayName.c_str()), false, ImGuiSelectableFlags_SpanAllColumns)) {
                    if (!engine_.addEffect(t.typeId, addFxInsertAt_)) toast(tr("Too many effects (24 at most)"), true);
                    curPreset_.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine(220 * s);
                ImGui::TextDisabled("%s", tr(t.description.c_str()));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndPopup();
    }
}

void App::drawRack() {
    const float s = S();
    // ---- Toolbar: presets ----
    SectionTitle(tr("Effect chain"));
    ImGui::SameLine(160 * s);
    ImGui::SetNextItemWidth(260 * s);
    if (ImGui::BeginCombo("##preset", curPreset_.empty() ? tr("(custom / not saved)") : tr(curPreset_.c_str()), ImGuiComboFlags_HeightLarge)) {
        bool sepDone = false;
        for (size_t i = 0; i < presets_.size(); ++i) {
            if (!presets_[i].builtin && !sepDone) {
                ImGui::SeparatorText(tr("My presets"));
                sepDone = true;
            }
            ImGui::PushID((int)i);
            if (ImGui::Selectable(tr(presets_[i].data.name.c_str()), presets_[i].data.name == curPreset_)) {
                std::string warn;
                engine_.loadPreset(presets_[i].data, &warn);
                curPreset_ = presets_[i].data.name;
                if (!warn.empty()) toast(warn, true);
            }
            if (ImGui::IsItemHovered() && !presets_[i].data.description.empty())
                ImGui::SetTooltip("%s", tr(presets_[i].data.description.c_str()));
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Save preset..."))) {
        std::snprintf(presetName_, sizeof presetName_, "%s", curPreset_.empty() ? tr("My tone") : curPreset_.c_str());
        ImGui::OpenPopup("##savepreset");
    }
    ImGui::SameLine();
    bool isUser = false;
    std::string userPath;
    for (auto& p : presets_)
        if (!p.builtin && p.data.name == curPreset_) {
            isUser = true;
            userPath = p.path;
        }
    ImGui::BeginDisabled(!isUser);
    if (ImGui::Button(tr("Delete preset"))) {
        std::remove(userPath.c_str());
        scanPresets();
        toast(std::string(tr("Deleted preset ")) + curPreset_);
        curPreset_.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear"))) {
        engine_.clearChain();
        curPreset_.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("+ Add effect"))) {
        addFxInsertAt_ = -1;
        addFxOpen_ = true;
    }
    // Latency info: the long form if it fits, otherwise the short one, otherwise nothing
    const double fxMs = engine_.fxLatencySamples() / engine_.sampleRate() * 1000.0;
    char lat[192];
    std::snprintf(lat, sizeof lat, tr("  Effects latency %.2f ms · total round trip about %.1f ms"), fxMs, engine_.roundTripMs());
    ImGui::SameLine();
    if (TextWidth(lat) > ImGui::GetContentRegionAvail().x) std::snprintf(lat, sizeof lat, tr("  Effects latency %.2f ms"), fxMs);
    if (TextWidth(lat) <= ImGui::GetContentRegionAvail().x) ImGui::TextDisabled("%s", lat);
    else ImGui::NewLine();
    if (ImGui::BeginPopupModal("##savepreset", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        SectionTitle(tr("Save preset"));
        ImGui::TextUnformatted(tr("Preset name:"));
        ImGui::SetNextItemWidth(300 * s);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##pn", presetName_, sizeof presetName_, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button(tr("Save"), ImVec2(120 * s, 0)) || enter) {
            saveCurrentPreset(presetName_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"), ImVec2(120 * s, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    drawAddFxPopup();
    ImGui::Separator();

    // ---- Pedalboard (wraps automatically) ----
    ImGui::BeginChild("##board", ImVec2(0, 0), ImGuiChildFlags_None);
    const float availW = ImGui::GetContentRegionAvail().x;
    const float cardW = 216.f * s, gap = 14.f * s;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    float x = 0, y = 0, rowH = 0;
    auto place = [&](float w, float h) {
        if (x > 0 && x + w > availW) {
            x = 0;
            y += rowH + gap;
            rowH = 0;
        }
        ImVec2 pos(origin.x + x, origin.y + y);
        x += w + gap;
        rowH = std::max(rowH, h);
        return pos;
    };
    // Input node
    {
        const float h = 120 * s, w = 86 * s;
        ImVec2 pos = place(w, h);
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(36, 40, 46, 255), 8 * s);
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 10 * s, pos.y + 10 * s));
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", tr("Guitar in"));
        Meter(engine_.inputPeakDb(), ImVec2(w - 20 * s, 8 * s), nullptr, engine_.inputClipped());
        ImGui::TextDisabled("%s", activeSource_.empty() ? "" : tr(CardMixer::sourceLabel(activeSource_)).c_str());
        ImGui::EndGroup();
    }
    const auto& chain = engine_.chain();
    for (int i = 0; i < (int)chain.size(); ++i) {
        const float h = fxCardHeight(*chain[i]->fx, cardW);
        ImVec2 pos = place(cardW, h);
        // Connector arrow
        dl->AddTriangleFilled(ImVec2(pos.x - gap + 2 * s, pos.y + 14 * s), ImVec2(pos.x - gap + 2 * s, pos.y + 26 * s),
                              ImVec2(pos.x - 3 * s, pos.y + 20 * s), P.textDim);
        ImGui::SetCursorScreenPos(pos);
        drawFxCard(i, cardW, h);
        if (i >= (int)engine_.chain().size()) break;  // deleted
    }
    // Add
    {
        const float w = 110 * s, h = 120 * s;
        ImVec2 pos = place(w, h);
        ImGui::SetCursorScreenPos(pos);
        if (ImGui::InvisibleButton("##add", ImVec2(w, h))) {
            addFxInsertAt_ = -1;
            addFxOpen_ = true;
        }
        bool hov = ImGui::IsItemHovered();
        dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), hov ? P.accent : P.border, 8 * s, 0, 2.f);
        DrawIcon(dl, IconPlus, ImVec2(pos.x + w * 0.5f, pos.y + h * 0.42f), 34 * s, hov ? P.accent : P.textDim);
        const char* t = tr("Add effect");
        dl->AddText(ImVec2(pos.x + w * 0.5f - TextWidth(t) * 0.5f, pos.y + h * 0.68f), hov ? P.text : P.textDim, t);
    }
    // Output node
    {
        const float h = 120 * s, w = 86 * s;
        ImVec2 pos = place(w, h);
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(36, 40, 46, 255), 8 * s);
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 10 * s, pos.y + 10 * s));
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", tr("Output"));
        Meter(engine_.outputPeakDb(), ImVec2(w - 20 * s, 8 * s), nullptr, engine_.outputLimited());
        ImGui::TextDisabled("%s", engine_.mute.load() ? tr("Muted") : "");
        ImGui::EndGroup();
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + y + rowH + gap));
    ImGui::Dummy(ImVec2(1, 1));
    if (chain.empty()) {
        ImGui::TextDisabled("%s", tr("The chain is empty = dry signal. Click \"Add effect\" or pick a preset above (try \"Practice Clean\" first)."));
    } else {
        ImGui::TextDisabled("%s", tr("Tip: drag knobs up/down (hold Shift for fine control) or use the mouse wheel; double-click resets. Drag a card title to reorder."));
    }
    ImGui::EndChild();
}

}  // namespace gs
