// Tuner, looper, settings and the beginner welcome guide.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "audio/PulseBridge.h"
#include "common/I18n.h"
#include "ui/App.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace gs {

using namespace ui;

// ------------------------------------------------------------------ tuner

void App::drawTuner() {
    const float s = S();
    static const int tunings[3][6] = {{64, 59, 55, 50, 45, 40}, {63, 58, 54, 49, 44, 39}, {64, 59, 55, 50, 45, 38}};
    const char* tuningNames[3] = {tr("Standard E A D G B E"), tr("Half step down (Eb)"), tr("Drop D (string 6 down to D)")};
    static float smoothCents = 0.f;
    const int (&T)[6] = tunings[std::clamp(settings_.tuning, 0, 2)];

    SectionTitle(tr("Tuner"));
    ImGui::SameLine(140 * s);
    ImGui::SetNextItemWidth(260 * s);
    ImGui::Combo("##tuning", &settings_.tuning, tuningNames, 3);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160 * s);
    ImGui::SliderFloat("##a4", &settings_.a4, 430.f, 450.f, "A4 = %.0f Hz");
    ImGui::SameLine();
    ImGui::Checkbox(tr("Mute guitar while tuning"), &settings_.tunerMute);
    ImGui::Separator();

    const float hz = analyzer_.tunerHz();
    const bool valid = hz > 0;
    float m = valid ? 69.f + 12.f * std::log2(hz / settings_.a4) : 0.f;
    int note = valid ? (int)std::lround(m) : 0;
    float cents = valid ? (m - note) * 100.f : 0.f;
    if (valid) smoothCents += (cents - smoothCents) * std::min(1.f, ImGui::GetIO().DeltaTime * 12.f);
    // Nearest open string
    int nearest = 0;
    float nd = 1e9f;
    for (int i = 0; i < 6; ++i)
        if (valid && std::fabs(m - T[i]) < nd) {
            nd = std::fabs(m - T[i]);
            nearest = i;
        }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float gw = std::min(avail.x, 760.f * s);
    const ImVec2 c(p.x + avail.x * 0.5f, p.y + 260 * s);
    const float R = std::min(gw * 0.45f, 230.f * s);
    // Dial: -50..+50 cents mapped to 210..330 degrees
    const float a0 = 3.14159f * (210.f / 180.f), a1 = 3.14159f * (330.f / 180.f);
    dl->PathArcTo(c, R, a0, a1, 64);
    dl->PathStroke(IM_COL32(60, 66, 76, 255), 0, 10 * s);
    const float gz = (a1 - a0) * 0.05f;  // +/-5 cent green zone
    dl->PathArcTo(c, R, (a0 + a1) * 0.5f - gz, (a0 + a1) * 0.5f + gz, 16);
    dl->PathStroke(P.green, 0, 10 * s);
    for (int k = -50; k <= 50; k += 10) {
        float a = a0 + (a1 - a0) * (k + 50) / 100.f;
        ImVec2 d(std::cos(a), std::sin(a));
        dl->AddLine(ImVec2(c.x + d.x * (R - 22 * s), c.y + d.y * (R - 22 * s)), ImVec2(c.x + d.x * (R - 8 * s), c.y + d.y * (R - 8 * s)),
                    P.textDim, k == 0 ? 3.f : 1.5f);
        char b[16];
        std::snprintf(b, sizeof b, "%+d", k);
        if (k % 20 == 0 || k == -50 || k == 50)
            dl->AddText(ImVec2(c.x + d.x * (R + 18 * s) - TextWidth(b) * 0.5f, c.y + d.y * (R + 18 * s) - 8 * s), P.textDim, b);
    }
    if (valid) {
        float a = a0 + (a1 - a0) * (std::clamp(smoothCents, -50.f, 50.f) + 50.f) / 100.f;
        ImVec2 d(std::cos(a), std::sin(a));
        ImU32 col = std::fabs(smoothCents) <= 5 ? P.green : (std::fabs(smoothCents) <= 15 ? P.yellow : P.red);
        dl->AddLine(c, ImVec2(c.x + d.x * (R - 4 * s), c.y + d.y * (R - 4 * s)), col, 5 * s);
        dl->AddCircleFilled(c, 10 * s, col);
    } else {
        dl->AddCircleFilled(c, 10 * s, P.textDim);
    }
    // Note name
    ImGui::PushFont(boldFont(), ImGui::GetStyle().FontSizeBase * 5.f);
    std::string nn = valid ? midiToName(note) : "--";
    ImVec2 ts = ImGui::CalcTextSize(nn.c_str());
    ImU32 ncol = !valid ? P.textDim : (std::fabs(cents) <= 5 ? P.green : P.text);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - R * 0.62f - ts.y * 0.5f), ncol, nn.c_str());
    ImGui::PopFont();
    char info[96];
    if (valid) std::snprintf(info, sizeof info, tr("%.2f Hz    %+.1f cents"), hz, cents);
    else std::snprintf(info, sizeof info, "%s", tr("Pluck an open string..."));
    dl->AddText(ImVec2(c.x - TextWidth(info) * 0.5f, c.y + 18 * s), P.textDim, info);
    // Hint
    char hint[256] = "";
    ImU32 hcol = P.text;
    if (valid) {
        if (nd > 1.5f) {
            std::snprintf(hint, sizeof hint, tr("This is %s; the nearest open string is %s (string %d), %d semitones away"), nn.c_str(),
                          midiToName(T[nearest]).c_str(), nearest + 1, (int)std::lround(nd));
            hcol = P.yellow;
        } else {
            float off = (m - T[nearest]) * 100.f;
            if (std::fabs(off) <= 5) {
                std::snprintf(hint, sizeof hint, tr("String %d in tune \xe2\x9c\x93"), nearest + 1);
                hcol = P.green;
            } else if (off < 0) {
                std::snprintf(hint, sizeof hint, tr("String %d is %d cents flat: tighten the string a little (tune up)"), nearest + 1,
                              (int)std::lround(-off));
                hcol = P.yellow;
            } else {
                std::snprintf(hint, sizeof hint, tr("String %d is %d cents sharp: loosen the string a little (tune down)"), nearest + 1,
                              (int)std::lround(off));
                hcol = P.yellow;
            }
        }
    }
    ImGui::PushFont(boldFont(), ImGui::GetStyle().FontSizeBase * 1.3f);
    dl->AddText(ImVec2(c.x - ImGui::CalcTextSize(hint).x * 0.5f, c.y + 50 * s), hcol, hint);
    ImGui::PopFont();
    // The six strings
    ImGui::SetCursorScreenPos(ImVec2(c.x - 3 * 96 * s, c.y + 100 * s));
    for (int i = 5; i >= 0; --i) {
        ImGui::PushID(i);
        const bool isNear = valid && i == nearest && nd <= 1.5f;
        const bool ok = isNear && std::fabs((m - T[i]) * 100.f) <= 5;
        ImVec2 bp = ImGui::GetCursorScreenPos();
        const ImVec2 bs(88 * s, 70 * s);
        if (ImGui::InvisibleButton("str", bs)) practice_.audio().previewNote(T[i], i);
        dl->AddRectFilled(bp, ImVec2(bp.x + bs.x, bp.y + bs.y), isNear ? withAlpha(ok ? P.green : stringColor(i), 0.35f) : IM_COL32(40, 44, 51, 255), 8 * s);
        dl->AddRect(bp, ImVec2(bp.x + bs.x, bp.y + bs.y), stringColor(i), 8 * s, 0, isNear ? 2.5f : 1.f);
        char b1[32], b2[16];
        std::snprintf(b1, sizeof b1, tr("String %d"), i + 1);
        std::snprintf(b2, sizeof b2, "%s", midiToName(T[i]).c_str());
        dl->AddText(ImVec2(bp.x + bs.x * 0.5f - TextWidth(b1) * 0.5f, bp.y + 8 * s), P.textDim, b1);
        ImGui::PushFont(boldFont(), ImGui::GetStyle().FontSizeBase * 1.4f);
        dl->AddText(ImVec2(bp.x + bs.x * 0.5f - ImGui::CalcTextSize(b2).x * 0.5f, bp.y + 30 * s), P.text, b2);
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Click to hear the reference pitch %s"), b2);
        ImGui::PopID();
        ImGui::SameLine(0, 8 * s);
    }
    ImGui::NewLine();
    ImGui::SetCursorScreenPos(ImVec2(p.x, c.y + 190 * s));
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("%s", tr("How to use: pluck one open string at a time and let it ring. Needle in the green center = in tune. "
                                 "Left (flat): tighten the tuning peg; right (sharp): loosen it. Start with string 6 (the thickest), "
                                 "then check all strings once more. Click the boxes above to hear the reference pitches."));
    ImGui::PopTextWrapPos();
}

// ------------------------------------------------------------------ looper

void App::drawLooper() {
    const float s = S();
    Looper& lp = engine_.looper();
    SectionTitle(tr("Looper"));
    ImGui::TextDisabled("%s", tr("Record a chord part first; it keeps looping so you can practice melodies or improvise over it. "
                                 "It records the sound after the effects."));
    ImGui::Separator();
    const Looper::State st = lp.state();
    static const char* stName[] = {"Empty", "Recording", "Playing", "Overdubbing", "Stopped"};
    const char* mainLabel = st == Looper::Empty         ? tr("Start recording")
                            : st == Looper::Recording   ? tr("Stop recording and loop")
                            : st == Looper::Playing     ? tr("Overdub")
                            : st == Looper::Overdubbing ? tr("Stop overdub")
                                                        : tr("Play");
    ImU32 mainCol = (st == Looper::Empty || st == Looper::Playing) ? P.red : P.green;
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(withAlpha(mainCol, 0.75f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(mainCol));
    ImGui::PushFont(boldFont(), ImGui::GetStyle().FontSizeBase * 1.3f);
    if (ImGui::Button(mainLabel, ImVec2(260 * s, 90 * s))) lp.pressMain();
    ImGui::PopFont();
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::Button(tr("Stop"), ImVec2(130 * s, 90 * s))) lp.pressStop();
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear loop"), ImVec2(130 * s, 90 * s))) lp.pressClear();
    ImGui::Dummy(ImVec2(0, 10 * s));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float R = 70 * s;
    const ImVec2 c(p.x + R + 10 * s, p.y + R + 10 * s);
    dl->AddCircle(c, R, P.grid, 64, 8 * s);
    if (st != Looper::Empty) {
        float pos = st == Looper::Recording ? 1.f : lp.position();
        ImU32 col = (st == Looper::Recording || st == Looper::Overdubbing) ? P.red : P.green;
        dl->PathArcTo(c, R, -1.5708f, -1.5708f + pos * 6.2832f, 64);
        dl->PathStroke(col, 0, 8 * s);
    }
    char b[64];
    std::snprintf(b, sizeof b, tr("%.1f s"), lp.lengthSec());
    ImGui::PushFont(boldFont(), ImGui::GetStyle().FontSizeBase * 1.4f);
    dl->AddText(ImVec2(c.x - ImGui::CalcTextSize(b).x * 0.5f, c.y - 22 * s), P.text, b);
    ImGui::PopFont();
    const char* sn = tr(stName[st]);
    dl->AddText(ImVec2(c.x - TextWidth(sn) * 0.5f, c.y + 8 * s), P.textDim, sn);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 2 * R + 40 * s, p.y + 20 * s));
    ImGui::BeginGroup();
    float lvl = lp.level.load();
    ImGui::SetNextItemWidth(260 * s);
    if (ImGui::SliderFloat(tr("Loop volume"), &lvl, 0.f, 1.5f, "%.2f")) lp.level = lvl;
    ImGui::TextDisabled("%s", tr("Up to 90 seconds. Older layers get slightly quieter when overdubbing, so the loop doesn't keep getting louder."));
    ImGui::TextDisabled("%s", tr("Tip: turn on the metronome in the top bar before recording for a tighter loop."));
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 2 * R + 30 * s));
    ImGui::Dummy(ImVec2(1, 1));  // ImGui requires an item after moving the cursor
}

// ------------------------------------------------------------------ settings

void App::drawSettings() {
    const float s = S();
    const EngineStatus st = engine_.status();
    CardMixer& mx = engine_.mixer();

    if (ImGui::CollapsingHeader(tr("Audio device & latency"), ImGuiTreeNodeFlags_DefaultOpen)) {
        static std::vector<AlsaDeviceInfo> devs;
        static double devTime = -10;
        if (ImGui::GetTime() - devTime > 5) {
            devs = AlsaDuplex::listDevices();
            devTime = ImGui::GetTime();
        }
        ImGui::SetNextItemWidth(380 * s);
        if (ImGui::BeginCombo(tr("Sound card"), settings_.device.c_str())) {
            for (auto& d : devs) {
                if (!d.canCapture || !d.canPlayback) continue;
                std::string lab = d.id + "  " + d.name;
                if (ImGui::Selectable(lab.c_str(), d.id == settings_.device)) settings_.device = d.id;
            }
            ImGui::EndCombo();
        }
        HelpTip(tr("A device that can both record and play. The motherboard's built-in sound card is usually hw:PCH,0. "
                   "If you get a USB audio interface later, choose it here."));
        const int rates[] = {44100, 48000, 96000};
        ImGui::SetNextItemWidth(160 * s);
        if (ImGui::BeginCombo(tr("Sample rate"), std::to_string(settings_.rate).c_str())) {
            for (int r : rates)
                if (ImGui::Selectable(std::to_string(r).c_str(), r == settings_.rate)) settings_.rate = r;
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 30 * s);
        const int periods[] = {16, 32, 64, 128, 256};
        ImGui::SetNextItemWidth(160 * s);
        if (ImGui::BeginCombo(tr("Period (frames)"), std::to_string(settings_.period).c_str())) {
            for (int pr : periods)
                if (ImGui::Selectable(std::to_string(pr).c_str(), pr == settings_.period)) settings_.period = pr;
            ImGui::EndCombo();
        }
        HelpTip(tr("Frames processed per cycle. Smaller = lower latency but more frequent CPU wake-ups. "
                   "On this machine 16 frames gives about 3.5-4 ms round trip and is stable."));
        ImGui::SetNextItemWidth(330 * s);
        ImGui::SliderFloat(tr("Safety margin"), &settings_.safetyMs, 0.1f, 4.f, "%.2f ms");
        HelpTip(tr("At startup the app measures the smallest buffer the sound card really needs and adds this margin. "
                   "It also adds a little automatically after a dropout (xrun). Increase it if you hear many dropouts."));
        ImGui::Checkbox(tr("Ask PulseAudio to release the sound card (recommended)"), &settings_.reserveDevice);
        HelpTip(tr("Uses the standard device reservation protocol (same as JACK) so PulseAudio temporarily releases the card; "
                   "it gets the card back automatically when this app exits."));
        if (ImGui::Button(tr("Apply and restart audio"), ImVec2(240 * s, 0))) restartAudio();
        ImGui::SameLine();
        if (ImGui::Button(tr("Reset dropout counter"))) engine_.resetXruns();
        ImGui::Spacing();
        if (ImGui::BeginTable("status", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            auto row = [&](const char* k, const std::string& v, ImU32 col = 0) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", k);
                ImGui::TableNextColumn();
                if (col) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%s", v.c_str());
                else ImGui::TextUnformatted(v.c_str());
            };
            row(tr("Status"), st.simulated ? tr("No sound card (no audio input or output)") : (st.running ? tr("Running") : tr("Not running")),
                st.simulated ? P.red : P.green);
            if (!st.error.empty()) row(tr("Error"), st.error, P.red);
            row(tr("Format"), st.format);
            row(tr("Real-time scheduling"), st.rtMsg, st.rtPriority > 0 ? P.green : P.yellow);
            row(tr("Device reservation"), st.reserveMsg);
            char b[256];
            if (st.calibrated) {
                std::snprintf(b, sizeof b, tr("FIFO prefetch %u frames, calibration needs %u frames, current prefill %d frames"), st.fifoFrames,
                              st.usedFrames, engine_.prefillFrames());
                row(tr("Auto-calibration"), b);
            }
            std::snprintf(b, sizeof b, tr("%.2f ms  (prefill %.2f ms + effects %.2f ms + AD/DA ~0.6 ms)"), engine_.roundTripMs(),
                          engine_.prefillFrames() / engine_.sampleRate() * 1000.0, engine_.fxLatencySamples() / engine_.sampleRate() * 1000.0);
            row(tr("Round-trip latency"), b, engine_.roundTripMs() < 8 ? P.green : P.yellow);
            std::snprintf(b, sizeof b, tr("mean %.1f%%  peak %.1f%% (share of each period used by the audio thread)"), engine_.cpuLoad() * 100.f,
                          engine_.cpuPeak() * 100.f);
            row("CPU", b);
            row(tr("Dropouts (xruns)"), std::to_string(engine_.xruns()), engine_.xruns() > 0 ? P.yellow : 0);
            ImGui::EndTable();
        }
        if (st.rtPriority == 0 && !st.simulated)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.yellow), "%s",
                               tr("No real-time priority: run scripts/setup_realtime.sh (needs sudo, then log in again)."));
    }

    if (ImGui::CollapsingHeader(tr("Input (connecting the guitar)"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("%s", tr("Use a 6.35 mm to 3.5 mm adapter and plug the guitar cable into the blue \"line in\" jack on the back of "
                                     "the PC (best) or the pink mic jack. It is detected automatically once plugged in."));
        ImGui::PopTextWrapPos();
        if (!mx.isOpen()) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.yellow), "%s",
                               tr("Cannot access the sound card mixer (no-device mode or unsupported device)"));
        } else {
            if (mx.jackPlugged("Front Headphone") && mx.jackPlugged("Front Mic")) {
                ImGui::PushTextWrapPos(0.f);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.yellow), "%s",
                                   tr("Note: the front panel reports both \"headphones\" and \"mic\" plugged in, so it is probably a "
                                      "headphone/mic combo jack. A normal guitar plug only touches the headphone contacts there and "
                                      "cannot be recorded. Please use the blue line-in jack on the back instead."));
                ImGui::PopTextWrapPos();
            }
            for (size_t i = 0; i < inputSources_.size(); ++i) {
                bool plugged = i < jackState_.size() && jackState_[i];
                bool active = inputSources_[i] == activeSource_;
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(active ? P.green : (plugged ? P.text : P.textDim)), "%s %s %s",
                                   active ? "\xe2\x97\x8f" : "\xe2\x97\x8b", tr(CardMixer::sourceLabel(inputSources_[i])).c_str(),
                                   plugged ? tr("(plugged in)") : tr("(not plugged in)"));
                if (i + 1 < inputSources_.size()) ImGui::SameLine(0, 24 * s);
            }
            ImGui::SetNextItemWidth(300 * s);
            std::string cur = settings_.inputSource == "auto" ? tr("Auto (by jack detection)") : tr(CardMixer::sourceLabel(settings_.inputSource));
            if (ImGui::BeginCombo(tr("Input source"), cur.c_str())) {
                if (ImGui::Selectable(tr("Auto (by jack detection)"), settings_.inputSource == "auto")) {
                    settings_.inputSource = "auto";
                    applyInputSettings(true);
                }
                for (auto& src : inputSources_)
                    if (ImGui::Selectable(tr(CardMixer::sourceLabel(src)).c_str(), settings_.inputSource == src)) {
                        settings_.inputSource = src;
                        applyInputSettings(true);
                    }
                ImGui::EndCombo();
            }
            ImGui::Checkbox(tr("Let this app manage the hardware gain"), &settings_.applyHwGain);
            HelpTip(tr("When on, this app sets the sound card's hardware gain (PulseAudio may change it; we change it back automatically)."));
            ImGui::BeginDisabled(!settings_.applyHwGain);
            const int bmax = mx.boostMax(activeSource_);
            if (bmax > 0) {
                ImGui::SetNextItemWidth(300 * s);
                if (ImGui::SliderInt(tr("Hardware boost"), &settings_.boost, 0, bmax, "+%d0 dB")) applyInputSettings(true);
                HelpTip(tr("Analog preamp boost. Guitar signals are small; +10 to +20 dB is typical."));
            }
            float lo = -17.f, hi = 30.f;
            if (mx.captureRangeDb(lo, hi)) {
                ImGui::SetNextItemWidth(300 * s);
                if (ImGui::SliderFloat(tr("Capture gain"), &settings_.captureDb, lo, hi, "%.2f dB")) applyInputSettings(true);
            }
            ImGui::EndDisabled();
        }
        const char* chNames[] = {tr("Auto (use whichever channel has signal)"), tr("Left channel"), tr("Right channel"), tr("Mix both channels")};
        ImGui::SetNextItemWidth(300 * s);
        ImGui::Combo(tr("Input channel"), &settings_.inputChannel, chNames, 4);
        ImGui::SameLine();
        ImGui::TextDisabled(tr("L %.0f dB / R %.0f dB"), engine_.inputLeftDb(), engine_.inputRightDb());
        HelpTip(tr("A mono guitar plug in a stereo jack usually puts the signal on one channel only; auto mode picks the one with signal."));
        ImGui::SetNextItemWidth(300 * s);
        ImGui::SliderFloat(tr("Digital input gain"), &settings_.inputGainDb, -24.f, 24.f, "%.1f dB");
        ImGui::Spacing();
        // Auto gain
        if (agState_ == 1) {
            float t = (float)(ImGui::GetTime() - agStart_) / 6.f;
            ImGui::ProgressBar(std::min(t, 1.f), ImVec2(300 * s, 0), tr("Measuring: play normally, including strums..."));
            ImGui::SameLine();
            ImGui::Text(tr("Peak %.1f dBFS"), agPeak_);
        } else {
            if (ImGui::Button(tr("Auto-set gain (play for 6 s)"), ImVec2(300 * s, 0))) {
                agState_ = 1;
                agStart_ = ImGui::GetTime();
                agPeak_ = -120;
            }
            HelpTip(tr("Click, then play for 6 seconds at your normal strength (single notes + strums). The app sets the input to a "
                       "good level (peaks around -9 dBFS)."));
            if (!agMsg_.empty()) {
                ImGui::PushTextWrapPos(0.f);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(agOk_ ? P.green : P.yellow), "%s", agMsg_.c_str());
                ImGui::PopTextWrapPos();
            }
        }
        ImGui::TextUnformatted(tr("Current input:"));
        ImGui::SameLine();
        Meter(engine_.inputPeakDb(), ImVec2(300 * s, 12 * s), &inPeakHold_, engine_.inputClipped());
        ImGui::SameLine();
        ImGui::TextDisabled(tr("%.1f dBFS  noise floor %.0f dB"), engine_.inputPeakDb(), analyzer_.noiseDb());
        if (ImGui::GetTime() - signalSeenAt_ < 3.0)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.green), "%s", tr("\xe2\x9c\x93 Guitar signal detected"));
        else
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.yellow), "%s",
                               tr("No guitar signal detected - try playing. If the meter doesn't move: check the guitar's volume knob "
                                  "and that the cable is in an input jack (blue line in / pink mic)."));
    }

    if (ImGui::CollapsingHeader(tr("Output & system audio"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* outNames[] = {tr("Speakers (green line-out jack on the back)"), tr("Headphones (front panel jack)"),
                                  tr("Both speakers and headphones")};
        ImGui::SetNextItemWidth(320 * s);
        if (ImGui::Combo(tr("Output to"), &settings_.outputTarget, outNames, 3)) applyOutputSettings(true);
        HelpTip(tr("As soon as anything is plugged into the front panel, the system assumes headphones and mutes the speakers on the "
                   "back. Choose the output here and this app keeps it that way while it runs."));
        if (mx.isOpen()) {
            const bool lo = mx.jackPlugged("Line Out Front"), hp = mx.jackPlugged("Front Headphone");
            ImGui::TextDisabled(tr("Rear speaker jack: %s    Front headphone jack: %s"), lo ? tr("plugged in") : tr("not plugged in"),
                                hp ? tr("plugged in") : tr("not plugged in"));
            if (hp && settings_.outputTarget != 1) {
                ImGui::PushTextWrapPos(0.f);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.yellow), "%s",
                                   tr("Something is plugged into the front headphone jack. That jack is an output: if it is the guitar, "
                                      "the computer cannot record it. Please plug the guitar into the blue line-in jack on the back "
                                      "(next to the green speaker jack)."));
                ImGui::PopTextWrapPos();
            }
        }
        ImGui::SetNextItemWidth(300 * s);
        ImGui::SliderFloat(tr("Master volume"), &settings_.masterDb, -40.f, 6.f, "%.1f dB");
        ImGui::SetNextItemWidth(300 * s);
        ImGui::SliderFloat(tr("Guitar volume"), &settings_.guitarDb, -30.f, 12.f, "%.1f dB");
        float lo, hi;
        if (mx.isOpen() && mx.masterRangeDb(lo, hi)) {
            float hm = mx.masterDb();
            ImGui::SetNextItemWidth(300 * s);
            if (ImGui::SliderFloat(tr("Sound card master volume"), &hm, lo, hi, "%.1f dB")) mx.setMasterDb(hm);
            HelpTip(tr("The sound card's own output volume (normally controlled by the system volume). Raise it if the speakers are too quiet."));
        }
        bool sa = settings_.systemAudio;
        if (ImGui::Checkbox(tr("System audio passthrough (play videos/music while practicing)"), &sa)) {
            settings_.systemAudio = sa;
            if (sa && !st.simulated) {
                engine_.pulse().start(st.rate, sysAudioMsg_);
                toast(sysAudioMsg_);
            } else {
                engine_.pulse().stop();
                sysAudioMsg_ = tr("System audio passthrough off (other programs' audio comes back when this app exits)");
            }
        }
        HelpTip(tr("While this app owns the sound card, browsers and other programs would be paused. When on, a virtual output "
                   "\"GuitarStudio-SystemAudio\" becomes the default and its sound is mixed in; the original setup is restored on exit."));
        ImGui::SetNextItemWidth(300 * s);
        ImGui::SliderFloat(tr("System audio volume"), &settings_.systemAudioDb, -40.f, 6.f, "%.1f dB");
        if (!sysAudioMsg_.empty()) ImGui::TextDisabled("%s", sysAudioMsg_.c_str());
        ImGui::Checkbox(tr("Safety limiter (protects your ears from feedback and loud pops)"), &limiterUi_);
        engine_.limiterOn = limiterUi_;
    }

    if (ImGui::CollapsingHeader(tr("Practice"), ImGuiTreeNodeFlags_DefaultOpen)) {
        drawMetronomeOptions(300 * s);
        ImGui::SetNextItemWidth(300 * s);
        ImGui::SliderFloat(tr("Latency calibration"), &settings_.latencyOffsetMs, -60.f, 60.f, "%+.0f ms");
        HelpTip(tr("If you are always judged \"late\" although you feel on time, increase this (or decrease it in the opposite case). Usually keep 0."));
        ImGui::SetNextItemWidth(300 * s);
        ImGui::SliderFloat(tr("Judging tolerance"), &settings_.tolerance, 0.6f, 1.6f, "%.2f");
        ImGui::Checkbox(tr("Start automatically after importing a song"), &settings_.autoStart);
        ImGui::SetNextItemWidth(300 * s);
        ImGui::SliderInt(tr("Beats per bar (standalone metronome)"), &settings_.beatsPerBar, 1, 12);
        if (opt_.simulate || st.simulated) {
            bool demo = practice_.audio().demoPlayer.load();
            if (ImGui::Checkbox(tr("Demo: simulated player (no-device mode)"), &demo)) practice_.audio().demoPlayer = demo;
        }
    }

    if (ImGui::CollapsingHeader(tr("Interface"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* langs[] = {"English", "\xe4\xb8\xad\xe6\x96\x87 (Chinese)"};
        int lang = settings_.language;
        ImGui::SetNextItemWidth(300 * s);
        if (ImGui::Combo(tr("Language"), &lang, langs, 2)) setLanguage(lang);
        // While dragging only the number changes; it is applied on release (otherwise the slider resizes under the mouse)
        static float pct = -1;
        if (!ImGui::IsAnyItemActive() || pct < 0) pct = settings_.uiScale * 100.f;
        ImGui::SetNextItemWidth(300 * s);
        ImGui::SliderFloat(tr("UI size"), &pct, 60.f, 250.f, "%.0f%%");
        if (ImGui::IsItemDeactivatedAfterEdit()) setUiScale(pct / 100.f);
        HelpTip(tr("Shortcuts: Ctrl + (plus) zooms in, Ctrl - zooms out, Ctrl 0 resets to 150%; or hold Ctrl and scroll the mouse wheel. "
                   "The bottom-left corner has - / + buttons too."));
        for (int v : {100, 125, 150, 175, 200}) {
            char b[16];
            std::snprintf(b, sizeof b, "%d%%", v);
            if (ImGui::Button(b, ImVec2(64 * s, 0))) setUiScale(v / 100.f);
            ImGui::SameLine();
        }
        char autoLab[64];
        std::snprintf(autoLab, sizeof autoLab, tr("Match the screen (%.0f%%)"), opt_.dpiScale * 100.f);
        if (ImGui::Button(autoLab)) setUiScale(opt_.dpiScale);
    }

    if (ImGui::CollapsingHeader(tr("Help & tips"))) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::BulletText("%s", tr("Connection: guitar > 6.35 mm guitar cable > 6.35-to-3.5 mm adapter > blue line-in jack on the back "
                                   "(or pink mic). Speakers go into the green output."));
        ImGui::BulletText("%s", tr("A computer sound card has a lower input impedance than a guitar wants, so the tone is a bit darker and "
                                   "the signal smaller - that's normal. For better sound, get an inexpensive USB guitar interface "
                                   "(with a Hi-Z input) and select it under \"Sound card\" above."));
        ImGui::BulletText("%s", tr("Feedback / too loud: click \"Mute\" in the top-right corner. With high-gain tones the guitar may squeal "
                                   "near the speakers; use the noise gate or turn the volume down."));
        ImGui::BulletText("%s", tr("Shortcuts: Space start/pause, Esc back to start, Left/Right previous/next bar, Up/Down speed, L loop, "
                                   "W wait mode, M metronome, R reference, F1-F5 switch pages, Ctrl+O import song, Ctrl+V paste tab."));
        ImGui::BulletText("%s", tr("Lower/steadier latency: run scripts/setup_realtime.sh to set up real-time permissions; you can also add "
                                   "preempt=full threadirqs to the kernel boot parameters."));
        ImGui::BulletText("%s", tr("Song formats: .gtab (this app's text format, see the examples in assets/songs), Guitar Pro 3/4/5/6/7, "
                                   "MusicXML, MIDI and text tabs."));
        ImGui::PopTextWrapPos();
        if (ImGui::Button(tr("Open config folder"))) runProgram({"xdg-open", configDir()}, nullptr, 3000);
        ImGui::SameLine();
        if (ImGui::Button(tr("Open song folder"))) runProgram({"xdg-open", userSongDir()}, nullptr, 3000);
        ImGui::SameLine();
        if (ImGui::Button(tr("Show the beginner guide again"))) welcomeShown_ = false;
    }
}

// ------------------------------------------------------------------ beginner guide

void App::drawWelcome() {
    const float s = S();
    const std::string title = std::string(tr("Welcome to Guitar Studio")) + "###welcome";
    if (!welcomeShown_) {
        welcomeShown_ = true;
        ImGui::OpenPopup(title.c_str());
    }
    ImGui::SetNextWindowSize(ImVec2(680 * s, 0));
    if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::PushTextWrapPos(0.f);
        SectionTitle(tr("Start practicing in three steps"));
        ImGui::Spacing();
        ImGui::TextUnformatted(tr("1. Connect: guitar cable + 6.35-to-3.5 mm adapter into the blue \"line in\" jack on the back (or the "
                                  "pink mic jack). It is detected automatically. Don't use the front headphone jack (it's an output)."));
        ImGui::TextUnformatted(tr("2. Set the level: Settings > \"Auto-set gain\", play normally for 6 seconds. Then tune all six strings "
                                  "with the Tuner."));
        ImGui::TextUnformatted(tr("3. Practice: on the Practice page pick lesson 1 on the left and press Space. When a note reaches the "
                                  "orange line, play that string and fret; it turns green when you get it right."));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("Tip: start at 50%-70% speed, or turn on \"Wait mode\" (the song only continues when you play the "
                                     "right note). Guitar Pro / MusicXML / MIDI files from the internet can simply be dropped onto the window."));
        const EngineStatus st = engine_.status();
        ImGui::Spacing();
        if (st.simulated)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.red), tr("Note: the sound card is not open (%s). Please check the settings."),
                               st.error.c_str());
        else
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.green), tr("Sound card ready, round-trip latency about %.1f ms."),
                               engine_.roundTripMs());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button(tr("Let's go!"), ImVec2(170 * s, 0))) {
            tab_ = Tab::Practice;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Tune first"), ImVec2(170 * s, 0))) {
            tab_ = Tab::Tuner;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Set up input"), ImVec2(170 * s, 0))) {
            tab_ = Tab::Settings;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

}  // namespace gs
