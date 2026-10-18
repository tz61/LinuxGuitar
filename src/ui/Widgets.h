#pragma once
// Custom widgets: knobs, level meters, power buttons, icon buttons, cents gauge, chord diagram, fretboard.
#include <imgui.h>

#include <string>

#include "fx/Effect.h"

namespace gs::ui {

enum Icon { IconPlay, IconPause, IconStop, IconLoop, IconPlus, IconMenu, IconFolder, IconRec, IconBack, IconWait, IconNote };

// Generic knob. Drag up/down to adjust (Shift = fine), mouse wheel works, double-click resets. Returns true if changed.
bool Knob(const char* id, const char* label, float* v, float vmin, float vmax, float vdef, bool logScale, float radius,
          ImU32 color, const char* fmt = "%.1f", bool integer = false, const char* tooltip = nullptr);
// Effect parameter control (knob, toggle or combo depending on the parameter type); labels are translated here
bool ParamControl(Effect& fx, int i, float radius, ImU32 color, float width);
std::string formatParam(const ParamInfo& p, float v);

// Level meter (-60..+3 dBFS). The caller keeps peakHold.
void Meter(float db, ImVec2 size, float* peakHold, bool clip, bool vertical = false);
bool PowerButton(const char* id, bool on, float size, ImU32 color);
bool IconButton(const char* id, Icon icon, ImVec2 size, bool active = false, ImU32 activeColor = 0, const char* tooltip = nullptr);
void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 col);
// Pill-shaped toggle
bool ToggleChip(const char* label, bool* v, ImU32 onColor = 0, const char* tooltip = nullptr);
void CentsGauge(float cents, bool valid, ImVec2 size, float greenZone = 5.f);
// Chord diagram (vertical, string 6 on the left). fretsLowToHigh[0] = string 6; -1 = muted, 0 = open
void ChordDiagram(ImDrawList* dl, ImVec2 pos, float w, float h, const int fretsLowToHigh[6], const char* name, ImU32 col);
// Horizontal fretboard with highlighted (string, fret) positions
struct FretMark {
    int string, fret;
    ImU32 col;
    const char* label;
};
void Fretboard(ImDrawList* dl, ImVec2 pos, ImVec2 size, int firstFret, int lastFret, const FretMark* marks, int n);
void HelpTip(const char* text);
void SectionTitle(const char* text, ImU32 col = 0);
// Keep the next widget (width w) on the same line if it fits, otherwise wrap (toolbars wrap when scaled up)
void SameLineIfFits(float w);
float TextWidth(const char* s);

}  // namespace gs::ui
