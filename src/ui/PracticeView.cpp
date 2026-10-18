// Practice page: song library, transport, song overview, tab highway, pitch comparison, next-note hint, stats and summary.
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

namespace {

std::string fmtTime(double sec) {
    if (sec < 0) sec = 0;
    int m = (int)(sec / 60), s = (int)sec % 60;
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", m, s);
    return b;
}

std::string stringName(int s) {
    char b[32];
    std::snprintf(b, sizeof b, tr("String %d"), s + 1);
    return b;
}

std::string fretName(int fret) {
    if (fret == 0) return tr("open");
    char b[32];
    std::snprintf(b, sizeof b, tr("fret %d"), fret);
    return b;
}

// Chord diagram: prefer the chord library, otherwise derive it from the event's notes
void eventShape(const SongTimeline& tl, const TimelineEvent& e, int out[6]) {
    if (!e.chordName.empty() && chordShape(e.chordName, out)) return;
    for (int i = 0; i < 6; ++i) out[i] = -1;
    for (int k = 0; k < e.noteCount; ++k) {
        const TimelineNote& n = tl.notes[e.firstNote + k];
        if (n.string >= 0 && n.string < 6) out[5 - n.string] = n.fret;
    }
}

const char* kStarOn = "\xe2\x98\x85";   // black star
const char* kStarOff = "\xe2\x98\x86";  // white star

}  // namespace

// ------------------------------------------------------------------ song library

void App::drawLibrary(float w) {
    const float s = S();
    SectionTitle(tr("Song library"));
    ImGui::SameLine(w - 44 * s);
    if (ImGui::SmallButton("<<")) settings_.showLibrary = false;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Hide the song library"));
    const float bw = (w - 30 * s) * 0.5f;
    if (ImGui::Button(tr("Import song file"), ImVec2(bw, 0)))
        fileDlg_.open(FileDialog::Song, tr("Import song"),
                      "*.gtab *.gp *.gp3 *.gp4 *.gp5 *.gpx *.xml *.musicxml *.mxl *.mid *.midi *.txt *.tab", homeDir());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Supports Guitar Pro (.gp3/.gp4/.gp5/.gpx/.gp), MusicXML, MIDI, text tabs (.txt) and this app's .gtab\n"
                                   "You can also drop files onto the window (Ctrl+O)"));
    ImGui::SameLine();
    if (ImGui::Button(tr("Paste tab"), ImVec2(bw, 0))) pasteSong();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Copy some text tab from a web page (the e|---0---| kind) and click here to import it (Ctrl+V)"));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", tr("Search..."), libFilter_, sizeof libFilter_);
    ImGui::BeginChild("##songs", ImVec2(0, 0), ImGuiChildFlags_None);
    auto& es = library_.entries();
    std::string toDelete;
    for (int pass = 0; pass < 2; ++pass) {
        bool header = false;
        for (size_t i = 0; i < es.size(); ++i) {
            const SongEntry& e = es[i];
            if (e.builtin != (pass == 0)) continue;
            if (libFilter_[0] && e.title.find(libFilter_) == std::string::npos && e.artist.find(libFilter_) == std::string::npos)
                continue;
            if (!header) {
                ImGui::SeparatorText(pass == 0 ? tr("Lessons") : tr("My songs"));
                header = true;
            }
            ImGui::PushID((int)i);
            const bool cur = e.path == songPath_;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float rowH = ImGui::GetFontSize() * 2.3f;
            if (ImGui::Selectable("##row", cur, 0, ImVec2(0, rowH))) {
                if (!cur) openSong(e.path, false);
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem(tr("Open and start practicing"))) openSong(e.path, true);
                if (!e.builtin && ImGui::MenuItem(tr("Remove from library"))) toDelete = e.path;
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered() && (!e.desc.empty() || !e.error.empty())) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24);
                if (!e.error.empty()) ImGui::TextColored(ImVec4(1, .4f, .3f, 1), "%s", e.error.c_str());
                ImGui::TextUnformatted(loc(e.desc).c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImGui::PushFont(cur ? boldFont() : nullptr, 0.f);
            dl->PushClipRect(p, ImVec2(p.x + w - 70 * s, p.y + rowH), true);
            dl->AddText(ImVec2(p.x + 4 * s, p.y + 2 * s), cur ? P.accent2 : P.text, loc(e.title).c_str());
            dl->PopClipRect();
            ImGui::PopFont();
            char info[96];
            std::string stars;
            for (int k = 0; k < 5; ++k) stars += k < e.level ? kStarOn : kStarOff;
            std::snprintf(info, sizeof info, "%s  %d BPM  %d/%d", e.level > 0 ? stars.c_str() : "", (int)std::lround(e.bpm), e.num, e.den);
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.8f);
            dl->AddText(ImVec2(p.x + 4 * s, p.y + rowH * 0.52f), P.textDim, info);
            ImGui::PopFont();
            if (e.best >= 0) {
                char b[16];
                std::snprintf(b, sizeof b, "%d%%", (int)std::lround(e.best * 100));
                ImU32 c = e.best >= 0.9f ? P.green : (e.best >= 0.7f ? P.yellow : P.textDim);
                const float bwid = TextWidth(b) + 12 * s;
                const ImVec2 b0(p.x + w - 30 * s - bwid, p.y + rowH * 0.25f);
                dl->AddRectFilled(b0, ImVec2(b0.x + bwid, b0.y + rowH * 0.5f), withAlpha(c, 0.25f), 6 * s);
                dl->AddText(ImVec2(b0.x + 6 * s, b0.y + (rowH * 0.5f - ImGui::GetFontSize()) * 0.5f), c, b);
            }
            ImGui::PopID();
        }
    }
    if (!toDelete.empty()) {
        library_.remove(toDelete);
        toast(tr("Removed from the library"));
    }
    ImGui::Dummy(ImVec2(0, 6 * s));
    if (ImGui::SmallButton(tr("Open song folder"))) runProgram({"xdg-open", userSongDir()}, nullptr, 3000);
    ImGui::EndChild();
}

// ------------------------------------------------------------------ header / transport

void App::drawSongHeader() {
    const float s = S();
    if (!settings_.showLibrary) {
        if (ImGui::SmallButton(tr(">> Library"))) settings_.showLibrary = true;
        ImGui::SameLine();
    }
    if (!song_) {
        ImGui::TextDisabled("%s", tr("No song open yet. Pick a lesson on the left, or import / drop a song file."));
        return;
    }
    ImGui::PushFont(boldFont(), ImGui::GetStyle().FontSizeBase * 1.4f);
    ImGui::TextUnformatted(song_->title.empty() ? baseName(songPath_).c_str() : loc(song_->title).c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    const TimeSig& ts = song_->timeSigAt(0);
    char capo[48] = "";
    if (song_->capo > 0) std::snprintf(capo, sizeof capo, tr("Capo %d"), song_->capo);
    ImGui::TextDisabled("  %s   %d BPM \xc3\x97 %.0f%%   %d/%d   %s", loc(song_->artist).c_str(),
                        song_->tempo.empty() ? 0 : (int)std::lround(song_->tempo[0].bpm), settings_.speed * 100.f, ts.num, ts.den, capo);
    if (tracks_.size() > 1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220 * s);
        std::string curName = tr("Auto-selected track");
        for (auto& t : tracks_)
            if (t.index == track_) curName = t.name;
        if (ImGui::BeginCombo("##track", curName.c_str())) {
            for (auto& t : tracks_) {
                char lab[256];
                std::snprintf(lab, sizeof lab, tr("%s  (%d notes)%s"), t.name.c_str(), t.noteCount, t.isGuitarLike ? "" : " *");
                if (ImGui::Selectable(lab, t.index == track_)) openSong(songPath_, false, t.index);
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("This file has several tracks; choose the one to practice"));
    }
    if (!song_->description.empty()) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(IM_COL32(170, 178, 190, 255)), "%s", loc(song_->description).c_str());
        ImGui::PopTextWrapPos();
    }
}

void App::drawMetronomeOptions(float width) {
    ImGui::SetNextItemWidth(width);
    PercentSlider(tr("Metronome volume"), &settings_.metronomeVol);
    HelpTip(tr("Also the volume of the standalone metronome in the top bar."));
    ImGui::SetNextItemWidth(width);
    ImGui::SliderInt(tr("Count-in beats"), &settings_.countInBeats, 1, 8);
    HelpTip(tr("How many clicks the metronome gives before the song starts (and after a pause), when \"Count-in\" is on."));
    ImGui::SetNextItemWidth(width);
    ImGui::SliderInt(tr("Ending beats"), &settings_.endBeats, 1, 8);
    HelpTip(tr("How many beats after the last note the run ends and your score is shown."));
}

void App::drawTransport() {
    const float s = S();
    PracticeAudio& pa = practice_.audio();
    auto tl = practice_.timeline();
    const TransportState st = pa.state();
    const bool playing = st == TransportState::Playing || st == TransportState::CountIn;
    const ImVec2 bs(44 * s, 36 * s);
    ImGui::BeginDisabled(!tl);
    if (IconButton("##stop", IconBack, bs, false, 0, tr("Back to the start (Esc)"))) practice_.stop();
    ImGui::SameLine();
    char startTip[128];
    if (settings_.countIn) std::snprintf(startTip, sizeof startTip, tr("Start (Space): %d beats of count-in first"), settings_.countInBeats);
    else std::snprintf(startTip, sizeof startTip, "%s", tr("Start (Space)"));
    if (IconButton("##play", playing ? IconPause : IconPlay, ImVec2(58 * s, 36 * s), true, playing ? P.yellow : P.green,
                   playing ? tr("Pause (Space)") : startTip)) {
        if (playing) practice_.pause();
        else practice_.play();
    }
    ImGui::SameLine();
    bool loop = pa.loopOn.load();
    if (IconButton("##loop", IconLoop, bs, loop, P.cyan, tr("Loop a section (L). Shift+drag on the overview bar above to choose the range"))) {
        if (!loop && tl && pa.loopB.load() <= pa.loopA.load() + 0.1) {
            int b = tl->barAt(pa.position());
            pa.loopA.store(tl->barTimes.empty() ? 0 : tl->barTimes[b]);
            pa.loopB.store(b + 2 < (int)tl->barTimes.size() ? tl->barTimes[b + 2] : tl->length);
        }
        pa.loopOn = !loop;
    }
    ImGui::EndDisabled();
    SameLineIfFits(TextWidth(tr("Speed")) + 160 * s);
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s", tr("Speed"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150 * s);
    float pct = settings_.speed * 100.f;
    if (ImGui::SliderFloat("##speed", &pct, 25.f, 150.f, "%.0f%%")) settings_.speed = std::round(pct) / 100.f;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Practice slower (Up/Down arrows change it by 5%). Beginners: start at 50%-70% and speed up once it feels easy"));
    ImGui::EndGroup();
    auto chipW = [&](const char* l) { return TextWidth(l) + 30 * s; };
    SameLineIfFits(chipW(tr("Wait mode")));
    ToggleChip(tr("Wait mode"), &settings_.waitMode, P.purple, tr("Only continue when you play the right note (W). Great for learning a new song"));
    SameLineIfFits(chipW(tr("Metronome")));
    ToggleChip(tr("Metronome"), &settings_.metronome, P.cyan, tr("Click along with the song (M)"));
    SameLineIfFits(chipW(tr("Reference")));
    ToggleChip(tr("Reference"), &settings_.reference, P.green, tr("A synthesized guitar plays the song so you can hear how it should sound (R)"));
    SameLineIfFits(chipW(tr("Count-in")));
    char countInTip[128];
    std::snprintf(countInTip, sizeof countInTip, tr("Click %d beats before starting (change it under \"Volume / judging...\")"),
                  settings_.countInBeats);
    ToggleChip(tr("Count-in"), &settings_.countIn, P.yellow, countInTip);
    SameLineIfFits(TextWidth(tr("Volume / judging...")) + 2 * ImGui::GetStyle().FramePadding.x);
    if (ImGui::Button(tr("Volume / judging..."))) ImGui::OpenPopup("practice_opts");
    if (ImGui::BeginPopup("practice_opts")) {
        SectionTitle(tr("Practice options"));
        drawMetronomeOptions(200 * s);
        ImGui::SetNextItemWidth(200 * s);
        PercentSlider(tr("Reference volume"), &settings_.referenceVol);
        ImGui::SetNextItemWidth(200 * s);
        ImGui::SliderFloat(tr("Judging tolerance"), &settings_.tolerance, 0.6f, 1.6f, "%.2f");
        HelpTip(tr("1.0 = normal (perfect <= 45 ms, good <= 100 ms, window +/-200 ms). Higher is more forgiving, lower is stricter."));
        ImGui::SetNextItemWidth(200 * s);
        ImGui::SliderFloat(tr("Highway zoom"), &settings_.zoom, 0.5f, 2.5f, "%.2f");
        ImGui::Checkbox(tr("Start automatically after importing a song"), &settings_.autoStart);
        ImGui::EndPopup();
    }
    // Status text + time
    SameLineIfFits(TextWidth(tr("Playing")) + TextWidth("0:00 / 0:00   bar 00") + 60 * s);
    if (tl) {
        const double pos = pa.position();
        std::string stTxt;
        ImU32 col = P.textDim;
        switch (st) {
            case TransportState::CountIn: {
                char b[48];
                std::snprintf(b, sizeof b, tr("Count-in %d"), (int)std::ceil(pa.countInLeft()));
                stTxt = b;
                col = P.yellow;
                break;
            }
            case TransportState::Playing:
                stTxt = pa.holding() ? tr("Waiting for you...") : tr("Playing");
                col = pa.holding() ? P.purple : P.green;
                break;
            case TransportState::Paused: stTxt = tr("Paused"); break;
            case TransportState::Finished: stTxt = tr("Finished"); break;
            default: stTxt = tr("Ready (press Space)");
        }
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%s", stTxt.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled(tr("%s / %s   bar %d"), fmtTime(pos / std::max(0.1f, settings_.speed)).c_str(),
                            fmtTime(tl->length / std::max(0.1f, settings_.speed)).c_str(), tl->barAt(pos) + 1);
    }
}

// ------------------------------------------------------------------ song overview bar

void App::drawTimeline(float h) {
    auto tl = practice_.timeline();
    const float s = S();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##timeline", ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(24, 26, 30, 255), 4 * s);
    if (!tl || tl->length <= 0) return;
    PracticeAudio& pa = practice_.audio();
    auto X = [&](double t) { return p.x + (float)(t / tl->length) * w; };
    // Loop range
    if (pa.loopB.load() > pa.loopA.load())
        dl->AddRectFilled(ImVec2(X(pa.loopA.load()), p.y), ImVec2(X(pa.loopB.load()), p.y + h),
                          withAlpha(P.cyan, pa.loopOn.load() ? 0.28f : 0.10f), 3 * s);
    for (size_t b = 0; b < tl->barTimes.size(); ++b) {
        float x = X(tl->barTimes[b]);
        dl->AddLine(ImVec2(x, p.y + h * 0.65f), ImVec2(x, p.y + h), IM_COL32(80, 86, 96, 255));
    }
    for (size_t i = 0; i < tl->events.size() && i < results_.size(); ++i) {
        const auto& e = tl->events[i];
        if (!e.judged) continue;
        float x = X(e.time);
        ImU32 c = results_[i].judge == Judge::Pending ? IM_COL32(120, 126, 136, 255) : judgeColor(results_[i].judge);
        dl->AddRectFilled(ImVec2(x, p.y + 4 * s), ImVec2(x + std::max(1.5f, 2 * s), p.y + h * 0.6f), c);
    }
    // Section names, each clipped to its own section so that short sections don't overlap
    for (size_t k = 0; k < tl->sections.size(); ++k) {
        const float x = X(tl->sections[k].first);
        const float xEnd = k + 1 < tl->sections.size() ? X(tl->sections[k + 1].first) - 3 * s : p.x + w;
        const std::string name = loc(tl->sections[k].second);
        dl->PushClipRect(ImVec2(x, p.y), ImVec2(xEnd, p.y + h), true);
        dl->AddRectFilled(ImVec2(x + 1, p.y), ImVec2(x + TextWidth(name.c_str()) + 4 * s, p.y + ImGui::GetFontSize() + 1),
                          IM_COL32(24, 26, 30, 220));  // keeps the note markers from running through the text
        dl->AddText(ImVec2(x + 2 * s, p.y + 1), P.accent2, name.c_str());
        dl->PopClipRect();
    }
    const float px = X(pa.position());
    dl->AddRectFilled(ImVec2(px - 1.5f * s, p.y - 2 * s), ImVec2(px + 1.5f * s, p.y + h + 2 * s), P.accent);
    // Interaction: click to jump; Shift+drag sets the loop
    ImGuiIO& io = ImGui::GetIO();
    auto T = [&](float x) { return std::clamp((double)((x - p.x) / w) * tl->length, 0.0, tl->length); };
    if (ImGui::IsItemActivated()) loopDragStart_ = io.KeyShift ? T(io.MousePos.x) : -1;
    if (ImGui::IsItemActive() && loopDragStart_ >= 0) {
        double a = loopDragStart_, b = T(io.MousePos.x);
        if (a > b) std::swap(a, b);
        // Snap to bar lines
        auto snap = [&](double t) {
            double best = t, bd = 1e9;
            for (double bt : tl->barTimes)
                if (std::fabs(bt - t) < bd) {
                    bd = std::fabs(bt - t);
                    best = bt;
                }
            if (std::fabs(tl->length - t) < bd) best = tl->length;
            return best;
        };
        pa.loopA.store(snap(a));
        pa.loopB.store(std::max(snap(b), snap(a) + 0.2));
        pa.loopOn = true;
    } else if (ImGui::IsItemDeactivated() && loopDragStart_ < 0 && ImGui::IsItemHovered()) {
        double t = T(io.MousePos.x);
        int b = tl->barAt(t);
        practice_.seek(tl->barTimes.empty() ? t : tl->barTimes[b]);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(tr("Click to jump to that bar; Shift+drag to select a loop range (bar %d)"), tl->barAt(T(io.MousePos.x)) + 1);
}

// ------------------------------------------------------------------ tab highway

void App::drawHighway(ImVec2 size) {
    const float s = S();
    auto tl = practice_.timeline();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##highway", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    dl->AddRectFilled(p, q, IM_COL32(20, 22, 26, 255), 6 * s);
    dl->PushClipRect(p, q, true);
    const float topM = 30 * s, botM = 20 * s;
    const float laneH = (size.y - topM - botM) / 6.f;
    auto laneY = [&](int st) { return p.y + topM + (st + 0.5f) * laneH; };
    const float nowX = p.x + size.x * 0.22f;
    const float pps = 210.f * s * settings_.zoom;  // pixels per second of song time
    PracticeAudio& pa = practice_.audio();
    const double pos = pa.position();
    const double t0 = pos - (nowX - p.x) / pps, t1 = pos + (q.x - nowX) / pps;
    auto X = [&](double t) { return nowX + (float)((t - pos) * pps); };
    // Strings
    static const char* names[6] = {"e", "B", "G", "D", "A", "E"};
    for (int st = 0; st < 6; ++st) {
        float y = laneY(st);
        if (st % 2 == 0) dl->AddRectFilled(ImVec2(p.x, y - laneH * 0.5f), ImVec2(q.x, y + laneH * 0.5f), IM_COL32(255, 255, 255, 6));
        dl->AddLine(ImVec2(p.x, y), ImVec2(q.x, y), withAlpha(stringColor(st), 0.38f), 1.f + st * 0.4f * s);
    }
    if (!tl) {
        dl->PopClipRect();
        const char* t = tr("Pick or import a song to start practicing");
        dl->AddText(ImVec2(p.x + size.x * 0.5f - TextWidth(t) * 0.5f, p.y + size.y * 0.5f), P.textDim, t);
        return;
    }
    // Beat and bar lines
    for (size_t b = 0; b < tl->beatTimes.size(); ++b) {
        double t = tl->beatTimes[b];
        if (t < t0 - 1 || t > t1 + 1) continue;
        float x = X(t);
        bool bar = tl->beatAccent[b] != 0;
        dl->AddLine(ImVec2(x, p.y + topM - 4 * s), ImVec2(x, q.y - botM + 4 * s), bar ? IM_COL32(110, 116, 128, 200) : IM_COL32(70, 75, 85, 110),
                    bar ? 2.f : 1.f);
    }
    for (size_t b = 0; b < tl->barTimes.size(); ++b) {
        double t = tl->barTimes[b];
        if (t < t0 - 1 || t > t1 + 1) continue;
        char nb[24];
        std::snprintf(nb, sizeof nb, "%zu", b + 1);
        dl->AddText(ImVec2(X(t) + 3 * s, q.y - botM + 2 * s), P.textDim, nb);
    }
    for (size_t k = 0; k < tl->sections.size(); ++k) {
        const auto& sec = tl->sections[k];
        if (sec.first < t0 - 4 || sec.first > t1) continue;
        const float xEnd = k + 1 < tl->sections.size() ? X(tl->sections[k + 1].first) - 3 * s : q.x;
        dl->PushClipRect(ImVec2(X(sec.first), p.y), ImVec2(xEnd, q.y), true);
        dl->AddText(boldFont(), ImGui::GetFontSize(), ImVec2(X(sec.first) + 3 * s, p.y + 4 * s), P.accent2, loc(sec.second).c_str());
        dl->PopClipRect();
    }
    // Hit zone
    const float win = (float)(0.2 * settings_.tolerance * pps * settings_.speed);  // judging window (real time -> song time)
    dl->AddRectFilled(ImVec2(nowX - win, p.y + topM - 6 * s), ImVec2(nowX + win, q.y - botM + 6 * s), IM_COL32(255, 255, 255, 10));
    // Notes
    const int nextEv = nextEventIndex();
    const bool holding = pa.holding();
    const float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 7.f);
    int iStart = std::max(0, tl->eventAt(t0 - 8.0));
    for (int i = iStart; i < (int)tl->events.size(); ++i) {
        const TimelineEvent& e = tl->events[i];
        if (e.time > t1 + 0.5) break;
        const float x = X(e.time);
        const EventResult* r = i < (int)results_.size() ? &results_[i] : nullptr;
        const Judge j = r ? r->judge : Judge::Pending;
        int topString = 6;
        for (int k = 0; k < e.noteCount; ++k) {
            const TimelineNote& n = tl->notes[e.firstNote + k];
            const float y = laneY(n.string);
            const ImU32 sc = stringColor(n.string);
            // Sustain tail
            float tail = (float)(n.dur * pps);
            if (tail > 14 * s)
                dl->AddRectFilled(ImVec2(x, y - laneH * 0.12f), ImVec2(x + tail, y + laneH * 0.12f),
                                  withAlpha(j == Judge::Pending ? sc : judgeColor(j), 0.28f), 3 * s);
            if (n.flags & NF_TieFromPrev) continue;
            char lab[8];
            if (n.flags & NF_Dead) std::snprintf(lab, sizeof lab, "x");
            else std::snprintf(lab, sizeof lab, "%d", n.fret);
            const float hw = std::max(13.f * s, TextWidth(lab) * 0.5f + 7 * s), hh = std::min(laneH * 0.40f, 15.f * s);
            ImU32 fill = sc;
            if (j != Judge::Pending) fill = judgeColor(j);
            if (j == Judge::Miss) fill = IM_COL32(85, 88, 96, 255);
            if (x < nowX - win && j == Judge::Pending) fill = withAlpha(sc, 0.55f);
            const ImVec2 a(x - hw, y - hh), b(x + hw, y + hh);
            if (i == nextEv && holding) {
                const float g = 6 * s + 4 * s * pulse;
                dl->AddRectFilled(ImVec2(a.x - g, a.y - g), ImVec2(b.x + g, b.y + g), withAlpha(P.purple, 0.35f + 0.3f * pulse), 10 * s);
            }
            dl->AddRectFilled(a, b, fill, 6 * s);
            if (i == nextEv) dl->AddRect(ImVec2(a.x - 2 * s, a.y - 2 * s), ImVec2(b.x + 2 * s, b.y + 2 * s), IM_COL32(255, 255, 255, 230), 7 * s, 0, 2 * s);
            ImGui::PushFont(boldFont(), 0.f);
            dl->AddText(ImVec2(x - TextWidth(lab) * 0.5f, y - ImGui::GetFontSize() * 0.5f), IM_COL32(18, 18, 20, 255), lab);
            ImGui::PopFont();
            // Technique marks
            const char* tech = (n.flags & NF_Hammer) ? "h" : (n.flags & NF_Pull) ? "p" : (n.flags & NF_Slide) ? "/" : (n.flags & NF_Bend) ? "b" : nullptr;
            if (tech) dl->AddText(ImVec2(a.x - 9 * s, a.y - 6 * s), P.text, tech);
            topString = std::min(topString, n.string);
        }
        // Chord name / strum direction
        if (!e.chordName.empty() || e.strum != Strum::None) {
            std::string cn = e.chordName;
            if (e.strum == Strum::Down) cn += " \xe2\x86\x93";  // down arrow
            if (e.strum == Strum::Up) cn += " \xe2\x86\x91";    // up arrow
            dl->AddText(boldFont(), ImGui::GetFontSize() * 1.05f, ImVec2(x - 6 * s, p.y + 8 * s), P.text, cn.c_str());
        }
        if (!e.lyric.empty()) dl->AddText(ImVec2(x - 6 * s, q.y - botM - ImGui::GetFontSize() - 2 * s), P.accent2, loc(e.lyric).c_str());
        // Judgement label
        if (r && j != Judge::Pending && topString < 6) {
            char b[48];
            if (j == Judge::Miss) std::snprintf(b, sizeof b, "%s", tr("miss"));
            else if (j == Judge::WrongPitch)
                std::snprintf(b, sizeof b, "%s",
                              r->detectedMidi > 0 ? ("\xe2\x86\x92" + midiToName((int)std::lround(r->detectedMidi))).c_str() : tr("wrong"));
            else if (r->onsetFrame && std::fabs(r->timingMs) >= 15.f) std::snprintf(b, sizeof b, "%+d", (int)std::lround(r->timingMs));
            else std::snprintf(b, sizeof b, "%s", j == Judge::Perfect ? "\xe2\x9c\x93" : "");  // check mark
            if (b[0]) {
                float y = laneY(topString) - std::min(laneH * 0.40f, 15.f * s) - ImGui::GetFontSize() - 1 * s;
                dl->AddText(ImVec2(x - TextWidth(b) * 0.5f, y), judgeColor(j), b);
            }
        }
    }
    // Now line
    dl->AddRectFilled(ImVec2(nowX - 1.5f * s, p.y + topM - 10 * s), ImVec2(nowX + 1.5f * s, q.y - botM + 10 * s), P.accent);
    for (int st = 0; st < 6; ++st) {
        dl->AddText(ImVec2(p.x + 6 * s, laneY(st) - ImGui::GetFontSize() * 0.5f), stringColor(st), names[st]);
    }
    // Count-in / waiting hint
    if (pa.state() == TransportState::CountIn) {
        char b[16];
        std::snprintf(b, sizeof b, "%d", std::max(1, (int)std::ceil(pa.countInLeft())));
        ImGui::PushFont(boldFont(), ImGui::GetStyle().FontSizeBase * 4.f);
        ImVec2 ts = ImGui::CalcTextSize(b);
        dl->AddText(ImVec2(p.x + size.x * 0.5f - ts.x * 0.5f, p.y + size.y * 0.5f - ts.y * 0.5f), withAlpha(P.yellow, 0.85f), b);
        ImGui::PopFont();
    }
    if (holding && nextEv >= 0) {
        const TimelineEvent& e = tl->events[nextEv];
        std::string hint = tr("Play: ");
        if (e.noteCount >= 3) {
            if (e.chordName.empty()) {
                hint += tr("this chord");
            } else {
                char b[64];
                std::snprintf(b, sizeof b, tr("%s chord"), e.chordName.c_str());
                hint += b;
            }
        } else {
            for (int k = 0; k < e.noteCount; ++k) {
                const TimelineNote& n = tl->notes[e.firstNote + k];
                hint += stringName(n.string) + " " + fretName(n.fret) + " (" + midiToName(n.midi) + ")  ";
            }
        }
        LiveFeedback lf = practice_.live();
        if (lf.wrongMidi > 0 && practice_.audio().frameClock() - lf.wrongFrame < (uint64_t)(engine_.sampleRate() * 1.5)) {
            char b[96];
            std::snprintf(b, sizeof b, tr("   You played %s, try again"), midiToName(lf.wrongMidi).c_str());
            hint += b;
        }
        dl->AddText(boldFont(), ImGui::GetFontSize() * 1.1f, ImVec2(nowX + 24 * s, p.y + 6 * s), P.purple, hint.c_str());
    }
    dl->PopClipRect();
    // Mouse wheel zoom
    if (hovered) {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        float wv = ImGui::GetIO().MouseWheel;
        if (wv != 0) settings_.zoom = std::clamp(settings_.zoom * (wv > 0 ? 1.1f : 1.f / 1.1f), 0.5f, 2.5f);
    }
}

// ------------------------------------------------------------------ pitch trace

void App::drawPitchRoll(ImVec2 size) {
    const float s = S();
    auto tl = practice_.timeline();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##pitchroll", size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    dl->AddRectFilled(p, q, IM_COL32(20, 22, 26, 255), 6 * s);
    dl->PushClipRect(p, q, true);
    dl->AddText(ImVec2(p.x + 8 * s, p.y + 4 * s), P.textDim, tr("Blocks = notes to play, line = your pitch"));
    // Pitch range
    int lo = 52, hi = 72;
    if (tl && !tl->notes.empty()) {
        lo = 200;
        hi = 0;
        for (auto& n : tl->notes) {
            if (n.flags & NF_Dead) continue;
            lo = std::min(lo, n.midi);
            hi = std::max(hi, n.midi);
        }
        lo -= 3;
        hi += 3;
        if (hi - lo < 14) {
            int c = (hi + lo) / 2;
            lo = c - 7;
            hi = c + 7;
        }
    }
    const float top = p.y + 24 * s, bot = q.y - 6 * s, left = p.x + 40 * s;
    auto Y = [&](float m) { return bot - (m - lo + 0.5f) / (float)(hi - lo + 1) * (bot - top); };
    for (int m = lo; m <= hi; ++m) {
        float y = Y((float)m);
        int pc = ((m % 12) + 12) % 12;
        bool natural = pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11;
        dl->AddLine(ImVec2(left, y), ImVec2(q.x, y), natural ? IM_COL32(52, 56, 64, 255) : IM_COL32(38, 41, 47, 255));
        if (pc == 0 || pc == 4 || pc == 7 || pc == 9 || pc == 2 || pc == 11 || pc == 5)
            if (Y((float)m) - Y((float)m + 1) > ImGui::GetFontSize() * 0.75f || pc == 0 || pc == 4)
                dl->AddText(ImVec2(p.x + 6 * s, y - ImGui::GetFontSize() * 0.5f), pc == 0 ? P.text : P.textDim, midiToName(m).c_str());
    }
    const double fs = engine_.sampleRate();
    PracticeAudio& pa = practice_.audio();
    const float speed = std::max(0.1f, settings_.speed);
    const float pps = 210.f * s * settings_.zoom * speed;  // pixels per second of real time (same speed as the highway)
    const float nowX = p.x + size.x * 0.22f;
    const uint64_t nowFrame = pa.frameClock();
    const int lat = engine_.outputLatencyFrames();
    auto XF = [&](double frame) { return nowX + (float)((frame - (double)nowFrame) / fs * pps); };
    // Expected notes
    if (tl) {
        const double pos = pa.position();
        const double visBack = (nowX - left) / pps * speed, visFwd = (q.x - nowX) / pps * speed;
        for (int i = std::max(0, tl->eventAt(pos - visBack - 8.0)); i < (int)tl->events.size(); ++i) {
            const TimelineEvent& e = tl->events[i];
            if (e.time > pos + visFwd + 0.5) break;
            const EventResult* r = i < (int)results_.size() ? &results_[i] : nullptr;
            double f0;
            if (r && r->reachedFrame && (double)r->reachedFrame + fs * 30 > (double)nowFrame && e.time <= pos + 1e-3)
                f0 = (double)r->reachedFrame + lat;
            else if (e.time >= pos - 1e-3)
                f0 = (double)nowFrame + (e.time - pos) / speed * fs + lat;
            else
                continue;
            const Judge j = r ? r->judge : Judge::Pending;
            for (int k = 0; k < e.noteCount; ++k) {
                const TimelineNote& n = tl->notes[e.firstNote + k];
                if (!n.playable()) continue;
                float x0 = XF(f0), x1 = XF(f0 + n.dur / speed * fs);
                float y = Y((float)n.midi), hh = std::max(3.f * s, (Y(0) - Y(1)) * 0.42f);
                ImU32 c = j == Judge::Pending ? stringColor(n.string) : judgeColor(j);
                dl->AddRectFilled(ImVec2(x0, y - hh), ImVec2(std::max(x1, x0 + 6 * s), y + hh), withAlpha(c, j == Judge::Pending ? 0.45f : 0.6f), 3 * s);
            }
        }
    }
    // The pitch you play
    analyzer_.copyHistory(nowFrame > (uint64_t)(fs * 12) ? nowFrame - (uint64_t)(fs * 12) : 0, pitchHist_);
    ImVec2 prev(-1, -1);
    float prevM = -100;
    for (const auto& pt : pitchHist_) {
        float x = XF((double)pt.frame);
        if (x < left - 4) continue;
        if (pt.onset) dl->AddLine(ImVec2(x, bot - 10 * s), ImVec2(x, bot), IM_COL32(255, 255, 255, 120), 1.5f);
        if (pt.midi < 0 || pt.clarity < 0.75f || pt.midi < lo - 1 || pt.midi > hi + 1) {
            prev = ImVec2(-1, -1);
            continue;
        }
        ImVec2 cur(x, Y(pt.midi));
        ImU32 c = IM_COL32(255, 255, 255, 230);
        if (prev.x >= 0 && std::fabs(pt.midi - prevM) < 0.8f) dl->AddLine(prev, cur, c, 2.5f * s);
        else dl->AddCircleFilled(cur, 2.f * s, c);
        prev = cur;
        prevM = pt.midi;
    }
    dl->AddLine(ImVec2(nowX, top), ImVec2(nowX, bot), withAlpha(P.accent, 0.8f), 2 * s);
    dl->PopClipRect();
}

// ------------------------------------------------------------------ info panel (right)

void App::drawInfoPanel(ImVec2 size) {
    const float s = S();
    auto tl = practice_.timeline();
    ImGui::BeginChild("##info", size, ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const int ev = nextEventIndex();
    std::vector<int> expected;
    // Next note / chord
    if (tl && ev >= 0) {
        const TimelineEvent& e = tl->events[ev];
        expected = tl->expectedMidis(ev);
        ImGui::TextDisabled("%s", practice_.audio().holding() ? tr("Play now:") : tr("Next:"));
        const float diagH = std::min(150.f * s, size.y * 0.42f);
        if (e.noteCount >= 3) {
            int shape[6];
            eventShape(*tl, e, shape);
            ImVec2 dp = ImGui::GetCursorScreenPos();
            ChordDiagram(dl, ImVec2(dp.x + 4 * s, dp.y), std::min(150.f * s, w * 0.45f), diagH, shape,
                         e.chordName.empty() ? tr("Chord") : e.chordName.c_str(), P.text);
            ImGui::SetCursorScreenPos(ImVec2(dp.x + std::min(150.f * s, w * 0.45f) + 14 * s, dp.y + 4 * s));
            ImGui::BeginGroup();
            for (int k = e.noteCount - 1; k >= 0; --k) {
                const TimelineNote& n = tl->notes[e.firstNote + k];
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(stringColor(n.string)), "%s %s  %s", stringName(n.string).c_str(),
                                   fretName(n.fret).c_str(), midiToName(n.midi).c_str());
            }
            ImGui::EndGroup();
            ImGui::SetCursorScreenPos(ImVec2(dp.x, dp.y + diagH + 4 * s));
        } else {
            std::vector<FretMark> marks;
            int maxF = 0;
            std::string big;
            for (int k = 0; k < e.noteCount; ++k) {
                const TimelineNote& n = tl->notes[e.firstNote + k];
                static char lab[6][4];
                std::snprintf(lab[k % 6], 4, "%d", n.fret);
                marks.push_back({n.string, n.fret, stringColor(n.string), lab[k % 6]});
                maxF = std::max(maxF, n.fret);
                big += midiToName(n.midi) + " ";
            }
            ImGui::PushFont(boldFont(), ImGui::GetStyle().FontSizeBase * 2.0f);
            ImGui::TextUnformatted(big.c_str());
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::BeginGroup();
            for (int k = 0; k < e.noteCount; ++k) {
                const TimelineNote& n = tl->notes[e.firstNote + k];
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(stringColor(n.string)), "%s %s", stringName(n.string).c_str(),
                                   fretName(n.fret).c_str());
            }
            ImGui::EndGroup();
            int first = maxF <= 7 ? 1 : maxF - 4, last = first + 7;
            ImVec2 fp = ImGui::GetCursorScreenPos();
            Fretboard(dl, fp, ImVec2(w, std::min(110.f * s, size.y * 0.3f)), first, last, marks.data(), (int)marks.size());
            ImGui::Dummy(ImVec2(w, std::min(110.f * s, size.y * 0.3f)));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Click to hear this note"));
            if (ImGui::IsItemClicked() && !expected.empty()) practice_.audio().previewNote(expected[0], e.noteCount ? tl->notes[e.firstNote].string : -1);
        }
    } else {
        ImGui::TextDisabled("%s", tl ? tr("Song finished") : tr("No song yet"));
    }
    ImGui::Separator();
    // The note you are playing
    {
        float m = analyzer_.currentMidi();
        ImGui::TextDisabled("%s", tr("Your note:"));
        ImGui::SameLine();
        if (m > 0) {
            int n = (int)std::lround(m);
            float cents = (m - n) * 100.f;
            bool match = std::find(expected.begin(), expected.end(), n) != expected.end();
            ImGui::PushFont(boldFont(), ImGui::GetStyle().FontSizeBase * 1.3f);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(match ? P.green : P.text), "%s", midiToName(n).c_str());
            ImGui::PopFont();
            if (match) {
                ImGui::SameLine();
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.green), "%s", tr("\xe2\x9c\x93 correct"));
            }
            CentsGauge(cents, true, ImVec2(w, 14 * s));
        } else {
            ImGui::TextDisabled("%s", tr("(nothing detected)"));
            CentsGauge(0, false, ImVec2(w, 14 * s));
        }
    }
    ImGui::Separator();
    // Stats
    {
        const PracticeStats& st = stats_;
        ImGui::PushFont(boldFont(), ImGui::GetStyle().FontSizeBase * 2.2f);
        float sc = st.score() * 100.f;
        ImU32 sc_col = st.judged == 0 ? P.textDim : (sc >= 90 ? P.green : sc >= 70 ? P.yellow : P.accent);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(sc_col), "%.0f%%", st.judged ? sc : 0.f);
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextDisabled(tr("Score  (judged %d / %d)"), st.judged, st.total);
        ImGui::Text(tr("Combo %d   best %d"), st.combo, st.maxCombo);
        ImGui::EndGroup();
        // Judgement counts; wraps to the next line when the panel is too narrow
        bool firstChip = true;
        const float rightX = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        auto chip = [&](Judge j, int v) {
            char b[64];
            std::snprintf(b, sizeof b, "%s %d", tr(judgeName(j)), v);
            if (!firstChip && ImGui::GetItemRectMax().x + 12 * s + TextWidth(b) <= rightX) ImGui::SameLine(0, 12 * s);
            firstChip = false;
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(judgeColor(j)), "%s", b);
        };
        chip(Judge::Perfect, st.perfect);
        chip(Judge::Good, st.good);
        chip(Judge::Ok, st.ok);
        chip(Judge::WrongPitch, st.wrong);
        chip(Judge::Miss, st.miss);
        if (st.timingN >= 3) {
            float mt = st.meanTiming();
            const char* tend = std::fabs(mt) < 15 ? tr("right on time") : (mt > 0 ? tr("generally late") : tr("generally early"));
            ImGui::PushTextWrapPos(0.f);
            ImGui::Text(tr("Timing: %s (mean %+.0f ms, mean error %.0f ms)"), tend, mt, st.meanAbsTiming());
            ImGui::PopTextWrapPos();
        }
        // Score per pass
        auto passes = practice_.passes();
        if (!passes.empty()) {
            ImGui::TextDisabled("%s", tr("Recent passes:"));
            const int n = std::min(12, (int)passes.size());
            ImVec2 bp = ImGui::GetCursorScreenPos();
            const float bh = 40 * s, bw = std::min(22.f * s, (w - 8 * s) / n - 4 * s);
            for (int i = 0; i < n; ++i) {
                const auto& ps = passes[passes.size() - n + i];
                float v = ps.stats.score();
                ImU32 c = v >= 0.9f ? P.green : v >= 0.7f ? P.yellow : P.accent;
                float x = bp.x + i * (bw + 4 * s);
                dl->AddRectFilled(ImVec2(x, bp.y + bh * (1 - v)), ImVec2(x + bw, bp.y + bh), c, 2 * s);
            }
            ImGui::Dummy(ImVec2(w, bh + 2 * s));
        }
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------------ summary

void App::drawSummaryPopup() {
    const float s = S();
    const std::string title = std::string(tr("Practice summary")) + "###summary";
    if (showSummary_) {
        ImGui::OpenPopup(title.c_str());
        showSummary_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(560 * s, 0));
    if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoResize)) {
        const PracticeStats& st = lastSummary_.stats;
        const float sc = st.score();
        int stars = sc >= 0.95f ? 3 : sc >= 0.8f ? 2 : sc >= 0.6f ? 1 : 0;
        ImGui::PushFont(boldFont(), ImGui::GetStyle().FontSizeBase * 2.6f);
        std::string star;
        for (int i = 0; i < 3; ++i) star += i < stars ? kStarOn : kStarOff;
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.yellow), "%s", star.c_str());
        ImGui::SameLine();
        ImGui::Text("%.0f%%", sc * 100.f);
        ImGui::PopFont();
        ImGui::Text(tr("Hit %d / %d    perfect %d  good %d  OK %d  wrong %d  missed %d    best combo %d"), st.hits(), st.judged,
                    st.perfect, st.good, st.ok, st.wrong, st.miss, st.maxCombo);
        ImGui::TextDisabled(tr("Speed %.0f%%%s"), lastSummary_.speed * 100.f, lastSummary_.wait ? tr("  · wait mode") : "");
        ImGui::Separator();
        SectionTitle(tr("Tips"));
        ImGui::PushTextWrapPos(0.f);
        int tips = 0;
        auto tip = [&](const char* t) {
            ImGui::Bullet();
            ImGui::TextUnformatted(t);
            ++tips;
        };
        const float wr = st.judged ? (float)st.wrong / st.judged : 0, mr = st.judged ? (float)st.miss / st.judged : 0;
        char b[320];
        if (st.timingN >= 4 && st.meanTiming() > 35) {
            std::snprintf(b, sizeof b, tr("You tend to play late (mean %+.0f ms). Follow the metronome and pick right on the \"click\"."),
                          st.meanTiming());
            tip(b);
        }
        if (st.timingN >= 4 && st.meanTiming() < -35) {
            std::snprintf(b, sizeof b, tr("You tend to play early (mean %+.0f ms). Take your time and wait for the beat."), st.meanTiming());
            tip(b);
        }
        if (st.timingN >= 4 && st.meanAbsTiming() > 60)
            tip(tr("Your timing is not steady yet. Slow down to about 70% and practice a few times with the metronome."));
        if (wr > 0.2f) tip(tr("Quite a few notes had the wrong pitch. Turn on \"Wait mode\" so the song only continues when you get each note right."));
        if (mr > 0.2f)
            tip(tr("Many notes were missed. Slow down, or check the input level (the \"Input\" meter at the top should jump when you play)."));
        if (sc >= 0.9f) {
            std::snprintf(b, sizeof b, tr("Great job! Try raising the speed to %.0f%%, or move on to the next lesson."),
                          std::min(150.f, lastSummary_.speed * 100.f + 10.f));
            tip(b);
        }
        if (tips == 0) tip(tr("Keep it up! Practice a few more times and go for three stars."));
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        const float bw = 120 * s;
        if (ImGui::Button(tr("Again"), ImVec2(bw, 0))) {
            practice_.stop();
            practice_.play();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Slower, again"), ImVec2(bw, 0))) {
            settings_.speed = std::max(0.25f, settings_.speed - 0.1f);
            practice_.stop();
            practice_.play();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        int next = library_.nextOf(songPath_);
        ImGui::BeginDisabled(next < 0);
        if (ImGui::Button(tr("Next lesson"), ImVec2(bw, 0))) {
            openSong(library_.entries()[next].path, false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Close"), ImVec2(90 * s, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ------------------------------------------------------------------ layout

void App::drawPractice() {
    const float s = S();
    practice_.snapshot(results_, stats_);
    if (settings_.showLibrary) {
        const float libW = 300 * s;
        ImGui::BeginChild("##lib", ImVec2(libW, 0), ImGuiChildFlags_Borders);
        drawLibrary(libW - 2 * ImGui::GetStyle().WindowPadding.x);
        ImGui::EndChild();
        ImGui::SameLine();
    }
    ImGui::BeginChild("##pmain", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawSongHeader();
    drawTransport();
    drawTimeline(24 * s);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float hwH = std::max(200.f * s, avail.y * 0.52f);
    drawHighway(ImVec2(avail.x, hwH));
    avail = ImGui::GetContentRegionAvail();
    const float infoW = std::clamp(avail.x * 0.34f, 300.f * s, 440.f * s);
    drawPitchRoll(ImVec2(avail.x - infoW - 8 * s, avail.y));
    ImGui::SameLine(0, 8 * s);
    drawInfoPanel(ImVec2(infoW, avail.y));
    ImGui::EndChild();
}

}  // namespace gs
