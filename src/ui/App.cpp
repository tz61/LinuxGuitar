#include "ui/App.h"

#include <limits.h>
#include <stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

#include "common/I18n.h"
#include "fx/Preset.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace gs {

using namespace ui;

static const char* kSongPatterns =
    "*.gtab *.gp *.gp3 *.gp4 *.gp5 *.gpx *.xml *.musicxml *.mxl *.mid *.midi *.txt *.tab";

App::App(const AppOptions& opt) : opt_(opt) {}

App::~App() { shutdown(); }

bool App::init() {
    ensureDir(configDir());
    ensureDir(dataDir());
    ensureDir(userSongDir());
    ensureDir(userPresetDir());
    const bool firstRun = !settings_.load(configDir() + "/settings.ini");
    if (firstRun)  // 150% by default, more on high-DPI screens
        settings_.uiScale = std::clamp(std::max(Settings::kDefaultUiScale, std::round(opt_.dpiScale * 20.f) / 20.f), 1.f, 2.5f);
    if (!opt_.device.empty()) settings_.device = opt_.device;
    gs::setLanguage((Lang)settings_.language);
    applyTheme(settings_.uiScale);
    if (!loadFonts()) toast(tr("No CJK font (Noto Sans CJK) found; Chinese text may show as boxes"), true);

    library_.loadScores(dataDir() + "/scores.tsv");
    library_.scan(builtinSongDir(), userSongDir());
    if (!opt_.simulate) PulseBridge::cleanupStale();  // test instances (no sound card) never touch PulseAudio
    analyzer_.setFrameCallback([this](const AnalysisFrame& f) { practice_.onAnalysisFrame(f); });

    scanPresets();
    bool restored = !opt_.preset.empty() && loadPresetNamed(opt_.preset);
    if (!restored) {
        bool ok = false;
        std::string txt = readFile(configDir() + "/last_chain.preset", &ok);
        PresetData pd;
        std::string err;
        if (ok && presetFromText(txt, pd, err)) {
            engine_.loadPreset(pd);
            curPreset_ = pd.name;
            restored = true;
        }
    }
    if (!restored && !builtinPresets().empty()) {
        engine_.loadPreset(builtinPresets()[0]);
        curPreset_ = builtinPresets()[0].name;
    }

    startAudio();
    tab_ = (Tab)std::clamp(opt_.tab >= 0 ? opt_.tab : settings_.tab, 0, (int)Tab::Count - 1);
    // A built-in lesson remembered from another location (source tree vs. installed copy) opens from this one
    std::string last = settings_.lastSong;
    if (!last.empty() && library_.find(last) < 0)
        for (auto& e : library_.entries())
            if (e.builtin && baseName(e.path) == baseName(last)) {
                last = e.path;
                break;
            }
    if (!opt_.song.empty()) importSong(opt_.song);
    else if (!last.empty() && fileExists(last)) openSong(last, false);
    else if (!library_.entries().empty()) openSong(library_.entries()[0].path, false);
    if (opt_.autoplay) autoPlayAt_ = ImGui::GetTime() + 0.8;
    welcomeShown_ = !firstRun || opt_.simulate;
    inited_ = true;
    return true;
}

void App::shutdown() {
    if (!inited_) return;
    inited_ = false;
    settings_.tab = (int)tab_;
    settings_.save(configDir() + "/settings.ini");
    writeFile(configDir() + "/last_chain.preset",
              presetToText(engine_.currentPreset(curPreset_.empty() ? "Last settings" : curPreset_)));  // English; shown with tr()
    library_.saveScores(dataDir() + "/scores.tsv");
    const EngineStatus st = engine_.status();
    std::printf("[Guitar Studio] %s: round trip %.2f ms (prefill %d frames), %d xruns, CPU peak %.0f%%, %s\n",
                st.simulated ? "no sound card" : st.device.c_str(), engine_.roundTripMs(), engine_.prefillFrames(),
                engine_.xruns(), engine_.cpuPeak() * 100.f, st.rtMsg.c_str());
    if (engine_.xruns() > 0) {
        std::printf("[Guitar Studio] xrun times (s):");
        for (double t : engine_.xrunTimes()) std::printf(" %.2f", t);
        std::printf("\n");
    }
    practice_.stop();
    engine_.stop();          // release the sound card -> PulseAudio resumes
    engine_.pulse().stop();  // restore system audio routing
}

void App::setLanguage(int lang) {
    settings_.language = std::clamp(lang, 0, 1);
    gs::setLanguage((Lang)settings_.language);
}

// ------------------------------------------------------------------ audio

void App::startAudio() {
    EngineConfig cfg;
    cfg.alsa.device = settings_.device;
    cfg.alsa.rate = (unsigned)settings_.rate;
    cfg.alsa.period = (unsigned)settings_.period;
    cfg.alsa.safetyMs = settings_.safetyMs;
    cfg.reserveDevice = settings_.reserveDevice;
    cfg.simulate = opt_.simulate;
    cfg.fallbackToSim = true;
    engine_.attachAnalyzer(&analyzer_);
    engine_.attachPractice(&practice_.audio());
    engine_.onPrepare = [this](double fs, int, int outLat) { practice_.prepare(fs, outLat); };
    engine_.simInput = [this](float* L, float* R, int n, uint64_t) {
        practice_.audio().renderDemoInput(L, n);
        std::memcpy(R, L, sizeof(float) * (size_t)n);
    };
    syncPractice();
    engine_.start(cfg);
    const EngineStatus st = engine_.status();
    if (st.simulated && !opt_.simulate) {
        char b[512];
        std::snprintf(b, sizeof b,
                      tr("Cannot open sound card %s: %s (running without a sound card; you can pick another device in Settings)"),
                      settings_.device.c_str(), st.error.c_str());
        toast(b, true);
    }
    practice_.audio().demoPlayer = opt_.demo && st.simulated;
    applyInputSettings(true);
    applyOutputSettings(true);
    sysAudioMsg_.clear();
    if (settings_.systemAudio && !st.simulated) engine_.pulse().start(st.rate, sysAudioMsg_);
    else engine_.pulse().stop();
    lastXruns_ = 0;
}

void App::restartAudio() {
    practice_.stop();
    engine_.stop();
    startAudio();
    toast(tr("Audio restarted"));
}

void App::applyInputSettings(bool force) {
    CardMixer& mx = engine_.mixer();
    if (!mx.isOpen()) return;
    mx.refresh();
    inputSources_ = mx.inputSources();
    std::vector<bool> jacks(inputSources_.size(), false);
    for (size_t i = 0; i < inputSources_.size(); ++i) jacks[i] = mx.jackPlugged(inputSources_[i]);
    int want = -1;
    if (settings_.inputSource == "auto") {
        // Priority: line in (high impedance, no bias voltage: best for guitar) > rear mic > front mic
        static const char* prio[] = {"Line", "Rear Mic", "Front Mic", "Mic", "Headset Mic", "Internal Mic"};
        for (const char* pn : prio) {
            for (size_t i = 0; i < inputSources_.size() && want < 0; ++i)
                if (inputSources_[i] == pn && jacks[i]) want = (int)i;
            if (want >= 0) break;
        }
    } else {
        for (size_t i = 0; i < inputSources_.size(); ++i)
            if (inputSources_[i] == settings_.inputSource) want = (int)i;
    }
    int cur = mx.inputSource();
    if (want >= 0 && (want != cur || force)) mx.setInputSource(want);
    cur = mx.inputSource();
    std::string prev = activeSource_;
    activeSource_ = (cur >= 0 && cur < (int)inputSources_.size()) ? inputSources_[cur] : "";
    if (!prev.empty() && prev != activeSource_ && !force)
        toast(std::string(tr("Input switched to: ")) + tr(CardMixer::sourceLabel(activeSource_)));
    // Tell the user about newly plugged jacks
    if (!jackState_.empty() && jackState_.size() == jacks.size())
        for (size_t i = 0; i < jacks.size(); ++i)
            if (jacks[i] && !jackState_[i]) toast(std::string(tr("Plugged in: ")) + tr(CardMixer::sourceLabel(inputSources_[i])));
    jackState_ = jacks;
    if (settings_.applyHwGain && !activeSource_.empty()) {
        if (mx.boostMax(activeSource_) > 0 && mx.boost(activeSource_) != settings_.boost)
            mx.setBoost(activeSource_, settings_.boost);
        float c = mx.captureDb();
        if (std::isnan(c) || std::fabs(c - settings_.captureDb) > 0.4f) mx.setCaptureDb(settings_.captureDb);
    }
}

void App::setPulsePort(const std::string& port) {
    // Run pactl on a background thread so the UI never stalls; the lambda only captures the string, not the App
    std::thread([port] {
        std::string out;
        if (runProgram({"pactl", "list", "short", "sinks"}, &out, 2000) != 0) return;
        std::istringstream is(out);
        std::string line;
        while (std::getline(is, line)) {
            std::istringstream ls(line);
            std::string idx, name;
            ls >> idx >> name;
            if (name.rfind("alsa_output", 0) != 0 || name.find("analog") == std::string::npos) continue;
            if (runProgram({"pactl", "set-sink-port", name, port}, nullptr, 2000) == 0)
                runProgram({"pactl", "set-sink-mute", name, "0"}, nullptr, 2000);
        }
    }).detach();
}

void App::applyOutputSettings(bool force) {
    CardMixer& mx = engine_.mixer();
    if (!mx.isOpen()) return;
    const bool hp = mx.jackPlugged("Front Headphone");
    const bool frontMic = mx.jackPlugged("Front Mic");
    const bool hpChanged = outputInit_ && hp != hpPlugged_;
    const int t = std::clamp(settings_.outputTarget, 0, 2);
    if (hp && t != 1 && (hpChanged || !outputInit_))
        toast(frontMic ? tr("Something is plugged into the front panel jack (probably a headphone/mic combo jack): a guitar "
                            "plugged in there touches the headphone output, so the computer cannot record it. Please plug the "
                            "guitar into the blue \"line in\" jack on the back (next to the green speaker jack).")
                       : tr("Something is plugged into the front headphone jack, which is an output. If it is the guitar, plug "
                            "it into the blue line-in jack on the back instead (or the pink mic jack)."),
              true);
    // PulseAudio prefers headphones by default and mutes the speakers as soon as anything is plugged in front.
    // Keep its port in line with our choice.
    if (force || hpChanged || t != lastOutputTarget_) {
        lastOutputTarget_ = t;
        setPulsePort(t == 1 ? "analog-output-headphones" : "analog-output-lineout");
    }
    hpPlugged_ = hp;
    outputInit_ = true;
    // Hardware switches (PulseAudio may change them; checked once per second)
    if (t != 1 && mx.autoMute() == 1) mx.setAutoMute(false);
    if (mx.playbackSwitch("Master") == 0) mx.setPlaybackSwitch("Master", true);
    const int wantFront = t != 1, wantHp = t != 0;
    if (mx.playbackSwitch("Front") >= 0 && mx.playbackSwitch("Front") != wantFront) {
        mx.setPlaybackSwitch("Front", wantFront);
        if (wantFront) mx.setPlaybackVolumeMax("Front");
    }
    if (mx.playbackSwitch("Headphone") >= 0 && mx.playbackSwitch("Headphone") != wantHp) {
        mx.setPlaybackSwitch("Headphone", wantHp);
        if (wantHp) mx.setPlaybackVolumeMax("Headphone");
    }
}

void App::pollDevices() {
    applyInputSettings(false);
    applyOutputSettings(false);
    const int x = engine_.xruns();
    if (x > lastXruns_ + 2) xrunWarnUntil_ = ImGui::GetTime() + 8.0;
    lastXruns_ = x;
}

void App::syncPractice() {
    PracticeAudio& pa = practice_.audio();
    pa.speed = settings_.speed;
    pa.waitMode = settings_.waitMode;
    pa.metronome = settings_.metronome;
    pa.metronomeVol = settings_.metronomeVol;
    pa.reference = settings_.reference;
    pa.referenceVol = settings_.referenceVol;
    pa.countIn = settings_.countIn;
    pa.countInBeats = settings_.countInBeats;
    practice_.setEndBeats(settings_.endBeats);
    pa.freeBpm = settings_.freeBpm;
    pa.freeBeatsPerBar = settings_.beatsPerBar;
    practice_.tolerance = settings_.tolerance;
    practice_.latencyOffsetMs = settings_.latencyOffsetMs;
    const int lat = engine_.outputLatencyFrames();
    practice_.setOutputLatency(lat);
    pa.demoLatencyFrames = lat;
    engine_.inputChannel = settings_.inputChannel;
    engine_.inputGainDb = settings_.inputGainDb;
    engine_.masterDb = settings_.masterDb;
    engine_.guitarDb = settings_.guitarDb;
    engine_.systemAudioDb = settings_.systemAudioDb;
    engine_.guitarMute = tab_ == Tab::Tuner && settings_.tunerMute;
    globalBpm().store(song_ && pa.state() != TransportState::Stopped ? (float)(songBpm() * settings_.speed) : settings_.freeBpm);
}

float App::songBpm() const {
    if (!song_ || song_->tempo.empty()) return 120.f;
    return (float)song_->bpmAt(practice_.audio().position() > 0 ? song_->secToBeat(practice_.audio().position()) : 0);
}

// ------------------------------------------------------------------ songs / presets

bool App::openSong(const std::string& path, bool autoStart, int track) {
    auto song = std::make_shared<Song>();
    std::string err;
    std::vector<TrackInfo> tracks;
    if (!loadSongFile(path, *song, err, track, &tracks)) {
        toast(std::string(tr("Failed to open song: ")) + err, true);
        return false;
    }
    if (!err.empty()) toast(std::string(tr("Note: ")) + err.substr(0, 200));
    practice_.stop();
    practice_.audio().loopOn = false;
    practice_.loadSong(song);
    song_ = song;
    songPath_ = path;
    tracks_ = tracks;
    track_ = track;
    settings_.lastSong = path;
    showSummary_ = false;
    if (autoStart) {
        tab_ = Tab::Practice;
        autoPlayAt_ = ImGui::GetTime() + 1.5;  // a moment to get ready, then start with the count-in
    }
    return true;
}

void App::importSong(const std::string& path) {
    // Songs that are already in the library (e.g. built-in lessons) are opened directly, not copied
    char real[4096];
    std::string rp = realpath(path.c_str(), real) ? std::string(real) : path;
    for (auto& e : library_.entries()) {
        char r2[4096];
        if (realpath(e.path.c_str(), r2) && rp == r2) {
            if (openSong(e.path, settings_.autoStart)) tab_ = Tab::Practice;
            return;
        }
    }
    std::string err;
    std::string dst = library_.importFile(path, err);
    if (dst.empty()) {
        toast(std::string(tr("Import failed: ")) + err, true);
        return;
    }
    if (openSong(dst, settings_.autoStart)) {
        tab_ = Tab::Practice;
        toast(std::string(tr("Imported: ")) + (song_->title.empty() ? baseName(dst) : loc(song_->title)) +
              (settings_.autoStart ? tr("  - starting practice now (Space to pause)") : ""));
    }
}

void App::pasteSong() {
    const char* clip = ImGui::GetClipboardText();
    if (!clip || !*clip) {
        toast(tr("The clipboard is empty. Copy some guitar tab text from a web page first, then click here"), true);
        return;
    }
    std::string err;
    std::string dst = library_.importText(clip, err);
    if (dst.empty()) {
        toast(std::string(tr("Could not read a song from the clipboard: ")) + err, true);
        return;
    }
    if (openSong(dst, settings_.autoStart)) toast(tr("Song imported from the clipboard"));
}

void App::onDropFile(const std::string& path) {
    std::string e = extLower(path);
    if (e == ".wav") {
        for (auto& s : engine_.chain())
            if (s->fx->acceptsFile()) {
                std::string err;
                if (s->fx->loadFile(path, err)) toast(std::string(tr("Cabinet IR loaded: ")) + baseName(path));
                else toast(std::string(tr("IR load failed: ")) + err, true);
                return;
            }
        toast(tr("That is a WAV file: add a \"Cabinet\" effect to the chain first, then drop the IR file"), true);
        return;
    }
    if (e == ".preset") {
        PresetData pd;
        std::string err;
        bool ok = false;
        if (presetFromText(readFile(path, &ok), pd, err) && ok) {
            engine_.loadPreset(pd);
            curPreset_ = pd.name;
            toast(std::string(tr("Preset loaded: ")) + tr(pd.name));
        } else {
            toast(std::string(tr("Invalid preset file: ")) + err, true);
        }
        return;
    }
    importSong(path);
}

void App::scanPresets() {
    presets_.clear();
    for (const auto& p : builtinPresets()) presets_.push_back({p, true, ""});
    for (auto& f : listDir(userPresetDir())) {
        if (extLower(f) != ".preset") continue;
        PresetData pd;
        std::string err;
        bool ok = false;
        std::string txt = readFile(f, &ok);
        if (ok && presetFromText(txt, pd, err)) {
            if (pd.name.empty()) pd.name = baseName(f);
            presets_.push_back({pd, false, f});
        }
    }
}

bool App::loadPresetNamed(const std::string& name) {
    for (auto& p : presets_)
        if (p.data.name == name) {
            std::string warn;
            engine_.loadPreset(p.data, &warn);
            curPreset_ = name;
            if (!warn.empty()) toast(warn, true);
            return true;
        }
    return false;
}

void App::saveCurrentPreset(const std::string& name) {
    if (name.empty()) return;
    PresetData pd = engine_.currentPreset(name);
    std::string path = userPresetDir() + "/" + sanitizeFileName(name) + ".preset";
    if (writeFile(path, presetToText(pd))) {
        curPreset_ = name;
        scanPresets();
        toast(std::string(tr("Preset saved: ")) + name);
    } else {
        toast(tr("Failed to save the preset"), true);
    }
}

// ------------------------------------------------------------------ per frame

void App::toast(const std::string& msg, bool error) {
    double now = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    toasts_.push_back({msg, error, now + (error ? 7.0 : 4.0)});
    if (toasts_.size() > 6) toasts_.pop_front();
}

void App::autoGainStep() {
    if (agState_ != 1) return;
    const double t = ImGui::GetTime() - agStart_;
    const float raw = engine_.inputPeakDb() - settings_.inputGainDb;
    agPeak_ = std::max(agPeak_, raw);
    if (t < 6.0) return;
    agState_ = 2;
    agOk_ = false;
    CardMixer& mx = engine_.mixer();
    if (agPeak_ < -70.f) {
        agMsg_ = tr("No guitar signal detected. Check that the cable is in a sound card input (blue line in or pink mic) "
                    "and that the guitar's volume knob is turned up.");
        return;
    }
    if (agPeak_ > -1.0f) {  // clipping: turn down a lot (hardware boost first) and reset the digital gain
        settings_.inputGainDb = 0.f;
        if (settings_.boost > 0) settings_.boost -= 1;  // -10 dB
        else settings_.captureDb -= 12.f;
        if (mx.isOpen()) {
            float lo, hi;
            if (mx.captureRangeDb(lo, hi)) settings_.captureDb = std::clamp(settings_.captureDb, lo, hi);
        }
        applyInputSettings(true);
        agMsg_ = tr("The signal was clipping and has been turned down. Please click \"Auto-set gain\" again.");
        return;
    }
    const float target = -9.f;
    float delta = target - agPeak_;
    std::string how;
    if (mx.isOpen() && settings_.applyHwGain) {
        float lo = -17.f, hi = 30.f;
        mx.captureRangeDb(lo, hi);
        const int bmax = mx.boostMax(activeSource_);
        float cap = settings_.captureDb + delta;
        int boost = settings_.boost;
        while (cap > std::min(hi, 20.f) && boost < bmax) {  // prefer the analog preamp boost (better signal-to-noise)
            ++boost;
            cap -= 10.f;
        }
        while (cap < lo && boost > 0) {
            --boost;
            cap += 10.f;
        }
        float rest = 0.f;
        if (cap > hi) {
            rest = cap - hi;
            cap = hi;
        }
        if (cap < lo) {
            rest = cap - lo;
            cap = lo;
        }
        settings_.boost = boost;
        settings_.captureDb = std::round(cap * 4.f) / 4.f;
        settings_.inputGainDb = std::clamp(rest, -24.f, 24.f);  // digital gain only for what the hardware can't do
        applyInputSettings(true);
        char b[200];
        std::snprintf(b, sizeof b, tr("hardware boost +%d0 dB, capture gain %.1f dB, digital gain %.1f dB"), boost,
                      settings_.captureDb, settings_.inputGainDb);
        how = b;
    } else {
        settings_.inputGainDb = std::clamp(delta, -24.f, 30.f);  // agPeak_ is the level before the digital gain
        char b[96];
        std::snprintf(b, sizeof b, tr("digital gain set to %d dB"), (int)settings_.inputGainDb);
        how = b;
    }
    char b[320];
    std::snprintf(b, sizeof b, tr("Done! Measured peak %.1f dBFS, adjusted %+.1f dB. %s"), agPeak_, delta, how.c_str());
    agMsg_ = b;
    agOk_ = true;
}

int App::nextEventIndex() const {
    auto tl = practice_.timeline();
    if (!tl) return -1;
    const PracticeAudio& pa = practice_.audio();
    if (pa.holding()) return pa.waitingEvent();
    const double pos = pa.position();
    int i = tl->eventAt(pos - 0.02);
    while (i < (int)tl->events.size() && !tl->events[i].judged) ++i;
    return i < (int)tl->events.size() ? i : -1;
}

void App::setUiScale(float scale) {
    scale = std::clamp(std::round(scale * 20.f) / 20.f, 0.6f, 2.5f);
    if (std::fabs(scale - settings_.uiScale) < 1e-3f) return;
    settings_.uiScale = scale;
    scaleDirty_ = true;  // main() applies it (applyTheme) before the next frame
    char b[64];
    std::snprintf(b, sizeof b, tr("UI size %.0f%%"), scale * 100.f);
    toast(b);
}

void App::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    // UI scale: Ctrl +/-/0 and Ctrl+wheel (also while typing)
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) setUiScale(settings_.uiScale + 0.1f);
        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) setUiScale(settings_.uiScale - 0.1f);
        if (ImGui::IsKeyPressed(ImGuiKey_0, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad0, false)) setUiScale(Settings::kDefaultUiScale);
        if (io.MouseWheel != 0.f) {
            setUiScale(settings_.uiScale + (io.MouseWheel > 0 ? 0.05f : -0.05f));
            io.MouseWheel = 0.f;  // don't scroll anything else
        }
    }
    if (io.WantTextInput) return;
    PracticeAudio& pa = practice_.audio();
    auto tl = practice_.timeline();
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && tl) {
        auto s = pa.state();
        if (s == TransportState::Playing || s == TransportState::CountIn) practice_.pause();
        else practice_.play();
    }
    if ((ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Home, false)) && tl) practice_.stop();
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) settings_.waitMode = !settings_.waitMode;
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) settings_.metronome = !settings_.metronome;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) settings_.reference = !settings_.reference;
    if (ImGui::IsKeyPressed(ImGuiKey_L, false) && tl) {
        bool on = !pa.loopOn.load();
        if (on && pa.loopB.load() <= pa.loopA.load() + 0.1) {  // no loop set yet: loop 2 bars from the current bar
            int b = tl->barAt(pa.position());
            pa.loopA.store(tl->barTimes.empty() ? 0 : tl->barTimes[b]);
            pa.loopB.store(b + 2 < (int)tl->barTimes.size() ? tl->barTimes[b + 2] : tl->length);
        }
        pa.loopOn = on;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) settings_.speed = std::min(1.5f, std::round(settings_.speed * 20.f + 1.f) / 20.f);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) settings_.speed = std::max(0.25f, std::round(settings_.speed * 20.f - 1.f) / 20.f);
    if (tl && !tl->barTimes.empty() && (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow))) {
        int b = tl->barAt(pa.position() + 0.01);
        b += ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? 1 : -1;
        b = std::clamp(b, 0, (int)tl->barTimes.size() - 1);
        practice_.seek(tl->barTimes[b]);
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
        fileDlg_.open(FileDialog::Song, tr("Import song"), kSongPatterns, homeDir());
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) pasteSong();
    for (int k = 0; k < (int)Tab::Count; ++k)
        if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_F1 + k), false)) tab_ = (Tab)k;
}

void App::frame() {
    engine_.reservation().poll();
    engine_.collectGarbage();
    syncPractice();
    if (ImGui::GetTime() - lastPoll_ > 1.0) {
        pollDevices();
        lastPoll_ = ImGui::GetTime();
    }
    FileDialog::Kind kind;
    std::string path;
    if (fileDlg_.poll(kind, path) && !path.empty()) {
        if (kind == FileDialog::Song) {
            importSong(path);
        } else if (kind == FileDialog::IR) {
            for (auto& s : engine_.chain())
                if ((int)s->id == irTargetSlot_) {
                    std::string err;
                    if (s->fx->loadFile(path, err)) toast(std::string(tr("IR loaded: ")) + baseName(path));
                    else toast(std::string(tr("IR load failed: ")) + err, true);
                }
        }
    }
    if (autoPlayAt_ > 0 && ImGui::GetTime() >= autoPlayAt_) {
        autoPlayAt_ = -1;
        practice_.play();
    }
    if (practice_.takeFinished()) {
        auto ps = practice_.passes();
        if (!ps.empty()) {
            lastSummary_ = ps.back();
            showSummary_ = true;
            library_.recordScore(songPath_, lastSummary_.stats.score());
        }
    }
    autoGainStep();
    if (analyzer_.levelDb() > analyzer_.noiseDb() + 15.f && analyzer_.levelDb() > -70.f) signalSeenAt_ = ImGui::GetTime();
    handleShortcuts();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float s = S();
    const float topH = 66.f * s;
    drawTopBar(topH);
    const float navW = 124.f * s;
    ImGui::SetCursorPos(ImVec2(0, topH));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(IM_COL32(26, 29, 33, 255)));
    ImGui::BeginChild("##nav", ImVec2(navW, 0), 0, ImGuiWindowFlags_NoScrollbar);
    drawNav(navW);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(P.bg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.f * s, 10.f * s));
    ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      tab_ == Tab::Practice ? (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse) : 0);
    switch (tab_) {
        case Tab::Rack: drawRack(); break;
        case Tab::Practice: drawPractice(); break;
        case Tab::Tuner: drawTuner(); break;
        case Tab::Looper: drawLooper(); break;
        case Tab::Settings: drawSettings(); break;
        default: break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    drawSummaryPopup();
    drawWelcome();
    drawToasts();
    ImGui::End();
}

// ------------------------------------------------------------------ top bar / navigation

void App::drawTopBar(float h) {
    const float s = S();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float W = ImGui::GetWindowWidth();
    dl->AddRectFilled(p, ImVec2(p.x + W, p.y + h), P.panel);
    dl->AddLine(ImVec2(p.x, p.y + h - 1), ImVec2(p.x + W, p.y + h - 1), withAlpha(P.accent, 0.6f), 2.f);

    // Logo
    ImGui::SetCursorScreenPos(ImVec2(p.x + 16 * s, p.y + 8 * s));
    ImGui::PushFont(boldFont(), ImGui::GetStyle().FontSizeBase * 1.35f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.accent), "Guitar Studio");
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x + 17 * s, p.y + 38 * s));
    ImGui::TextDisabled("%s", tr("Guitar effects & practice"));

    // Audio status
    float x = p.x + 200 * s;
    const EngineStatus st = engine_.status();
    {
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + 10 * s));
        const ImVec2 cp = ImGui::GetCursorScreenPos();
        const float cw = 150 * s, ch = h - 20 * s;
        ImGui::InvisibleButton("##status", ImVec2(cw, ch));
        if (ImGui::IsItemClicked()) tab_ = Tab::Settings;
        const bool warn = ImGui::GetTime() < xrunWarnUntil_;
        ImU32 col = st.simulated ? P.red : (warn ? P.yellow : P.green);
        dl->AddRectFilled(cp, ImVec2(cp.x + cw, cp.y + ch), IM_COL32(30, 33, 38, 255), 6 * s);
        dl->AddCircleFilled(ImVec2(cp.x + 14 * s, cp.y + ch * 0.5f), 5 * s, col);
        char b[96];
        if (st.simulated) std::snprintf(b, sizeof b, "%s", tr("No sound card"));
        else std::snprintf(b, sizeof b, tr("Latency %.1f ms"), engine_.roundTripMs());
        ImGui::PushFont(boldFont(), 0.f);
        dl->AddText(ImVec2(cp.x + 26 * s, cp.y + 3 * s), P.text, b);
        ImGui::PopFont();
        std::snprintf(b, sizeof b, tr("%u Hz · period %u · CPU %.0f%%"), st.rate, st.period, engine_.cpuLoad() * 100.f);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.78f);
        dl->AddText(ImVec2(cp.x + 26 * s, cp.y + ch - ImGui::GetFontSize() - 3 * s), P.textDim, b);
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text(tr("Device: %s  %s"), st.device.c_str(), st.simulated ? tr("(not open)") : "");
            if (!st.error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), tr("Error: %s"), st.error.c_str());
            ImGui::Text(tr("Real-time scheduling: %s"), st.rtMsg.c_str());
            ImGui::Text(tr("Device reservation: %s"), st.reserveMsg.c_str());
            if (st.calibrated)
                ImGui::Text(tr("Calibration: FIFO %u frames, needs %u frames, prefill %d frames"), st.fifoFrames, st.usedFrames,
                            engine_.prefillFrames());
            ImGui::Text(tr("Dropouts (xruns): %d    CPU peak: %.0f%%"), engine_.xruns(), engine_.cpuPeak() * 100.f);
            ImGui::TextDisabled("%s", tr("Click to open the audio settings"));
            ImGui::EndTooltip();
        }
        x += cw + 16 * s;
    }

    // Input / output levels. The label column fits the current language; each block keeps its total width
    const float labelW = std::max(38 * s, std::max(TextWidth(tr("Input")), TextWidth(tr("Output"))) + 8 * s);
    const float meterW = std::max(110 * s, 188 * s - labelW);
    auto meterBlock = [&](const char* label, float db, float* hold, bool clip, float* gain, const char* gid, float gmin,
                          float gmax, const char* tip) {
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + 9 * s));
        ImGui::TextDisabled("%s", label);
        ImGui::SetCursorScreenPos(ImVec2(x + labelW, p.y + 14 * s));
        Meter(db, ImVec2(meterW, 10 * s), hold, clip);
        ImGui::SetCursorScreenPos(ImVec2(x + labelW, p.y + 32 * s));
        ImGui::SetNextItemWidth(meterW);
        ImGui::SliderFloat(gid, gain, gmin, gmax, "%.1f dB");
        if (ImGui::IsItemHovered()) {
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
            if (ImGui::GetIO().MouseWheel != 0) *gain = std::clamp(*gain + ImGui::GetIO().MouseWheel, gmin, gmax);
            ImGui::SetTooltip("%s", tip);
        }
        x += 200 * s;
    };
    meterBlock(tr("Input"), engine_.inputPeakDb(), &inPeakHold_, engine_.inputClipped(), &settings_.inputGainDb, "##ing", -24.f,
               24.f, tr("Digital input gain (red frame = input clipping: lower the hardware gain; Settings has \"Auto-set gain\")"));
    meterBlock(tr("Output"), engine_.outputPeakDb(), &outPeakHold_, engine_.outputLimited(), &settings_.masterDb, "##outg", -40.f,
               6.f, tr("Master volume (red frame = the safety limiter is working)"));

    // Mini tuner readout
    {
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + 10 * s));
        float hz = analyzer_.tunerHz();
        ImGui::BeginGroup();
        if (hz > 0) {
            float m = Analyzer::hzToMidi(hz) + 12.f * std::log2(440.f / settings_.a4);
            int n = (int)std::lround(m);
            float cents = (m - n) * 100.f;
            ImGui::PushFont(boldFont(), ImGui::GetStyle().FontSizeBase * 1.25f);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(std::fabs(cents) < 5 ? P.green : P.text), "%s", midiToName(n).c_str());
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextDisabled("%+.0f\xc2\xa2", cents);  // cent sign
        } else {
            ImGui::PushFont(boldFont(), ImGui::GetStyle().FontSizeBase * 1.25f);
            ImGui::TextDisabled("--");
            ImGui::PopFont();
        }
        ImGui::TextDisabled("%s", tr("Pitch"));
        ImGui::EndGroup();
        if (ImGui::IsItemClicked()) tab_ = Tab::Tuner;
        x += 90 * s;
    }

    // Metronome
    {
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + 9 * s));
        PracticeAudio& pa = practice_.audio();
        bool fm = pa.freeMetronome.load();
        if (ToggleChip(tr("Metronome"), &fm, P.cyan,
                       tr("Standalone metronome (when no song is playing). Press M on the Practice page to toggle the song metronome")))
            pa.freeMetronome = fm;
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + 35 * s));
        ImGui::SetNextItemWidth(104 * s);
        ImGui::DragFloat("##bpm", &settings_.freeBpm, 0.5f, 30.f, 300.f, "\xe2\x99\xa9=%.0f");  // quarter note sign
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Metronome tempo (BPM): drag left/right or double-click to type"));
        if (fm) {
            int beat = pa.freeBeatIndex();
            for (int i = 0; i < std::min(settings_.beatsPerBar, 8); ++i) {
                ImVec2 c(x + 112 * s + (i % 4) * 13 * s, p.y + (i < 4 ? 42 : 54) * s);
                dl->AddCircleFilled(c, 4.5f * s, i == beat ? (i == 0 ? P.accent : P.cyan) : IM_COL32(70, 75, 85, 255));
            }
        }
        x += 180 * s;
    }

    // Right side: mute
    {
        const float bw = 86 * s;
        ImGui::SetCursorScreenPos(ImVec2(p.x + W - bw - 14 * s, p.y + 12 * s));
        bool m = engine_.mute.load();
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(m ? P.red : IM_COL32(58, 64, 73, 255)));
        if (ImGui::Button(m ? tr("Muted") : tr("Mute"), ImVec2(bw, h - 24 * s))) engine_.mute = !m;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Mute everything (emergency: feedback / too loud)"));
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
}

void App::drawNav(float w) {
    const float s = S();
    struct Item {
        Tab tab;
        const char* name;
    };
    static const Item items[] = {{Tab::Rack, "Effects"}, {Tab::Practice, "Practice"}, {Tab::Tuner, "Tuner"},
                                 {Tab::Looper, "Looper"}, {Tab::Settings, "Settings"}};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(0, 8 * s));
    for (const auto& it : items) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 sz(w, 58 * s);
        ImGui::PushID((int)it.tab);
        if (ImGui::InvisibleButton("##nav", sz)) tab_ = it.tab;
        const bool active = tab_ == it.tab, hov = ImGui::IsItemHovered();
        if (active || hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + sz.y), active ? IM_COL32(44, 49, 56, 255) : IM_COL32(36, 40, 46, 255));
        if (active) dl->AddRectFilled(p, ImVec2(p.x + 4 * s, p.y + sz.y), P.accent);
        ImGui::PushFont(boldFont(), ImGui::GetStyle().FontSizeBase * 1.08f);
        dl->AddText(ImVec2(p.x + 20 * s, p.y + 9 * s), active ? P.text : IM_COL32(190, 195, 202, 255), tr(it.name));
        ImGui::PopFont();
        // Subtitle: the shortcut key (plus the English name when the UI is in Chinese)
        char sub[48];
        if (language() == Lang::Zh) std::snprintf(sub, sizeof sub, "%s  F%d", it.name, (int)it.tab + 1);
        else std::snprintf(sub, sizeof sub, "F%d", (int)it.tab + 1);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.75f);
        dl->AddText(ImVec2(p.x + 20 * s, p.y + 34 * s), P.textDim, sub);
        ImGui::PopFont();
        ImGui::PopID();
    }
    // Bottom: language, UI size, looper state, current preset
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 205 * s);
    ImGui::Indent(10 * s);
    const float bw = 26 * s;
    const float rowW = ImGui::GetContentRegionAvail().x - 8 * s;
    ImGui::TextDisabled("%s", tr("Language"));
    {
        const float half = (rowW - 4 * s) * 0.5f;
        const bool zh = language() == Lang::Zh;
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(!zh ? P.accent : IM_COL32(58, 64, 73, 255)));
        if (ImGui::Button("EN", ImVec2(half, 0))) setLanguage(0);
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 4 * s);
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(zh ? P.accent : IM_COL32(58, 64, 73, 255)));
        if (ImGui::Button("\xe4\xb8\xad\xe6\x96\x87", ImVec2(half, 0))) setLanguage(1);  // "Chinese" in Chinese
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 2 * s));
    ImGui::TextDisabled("%s", tr("UI size"));
    {
        const float mid = std::max(40.f * s, rowW - 2 * bw - 8 * s);
        if (ImGui::Button("-", ImVec2(bw, 0))) setUiScale(settings_.uiScale - 0.1f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out (Ctrl -)"));
        ImGui::SameLine(0, 4 * s);
        char b[16];
        std::snprintf(b, sizeof b, "%.0f%%", settings_.uiScale * 100.f);
        if (ImGui::Button(b, ImVec2(mid, 0))) setUiScale(Settings::kDefaultUiScale);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Click to reset to the default 150% (Ctrl 0); hold Ctrl and scroll to zoom"));
        ImGui::SameLine(0, 4 * s);
        if (ImGui::Button("+", ImVec2(bw, 0))) setUiScale(settings_.uiScale + 0.1f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in (Ctrl +)"));
    }
    ImGui::Dummy(ImVec2(0, 4 * s));
    ImGui::PushTextWrapPos(w - 8 * s);
    ImGui::TextDisabled("%s", tr("Preset:"));
    ImGui::TextUnformatted(curPreset_.empty() ? tr("(custom)") : tr(curPreset_.c_str()));
    Looper::State ls = engine_.looper().state();
    if (ls != Looper::Empty) {
        static const char* names[] = {"", "Looper: recording", "Looper: playing", "Looper: overdubbing", "Looper: stopped"};
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ls == Looper::Recording || ls == Looper::Overdubbing ? P.red : P.green),
                           "%s", tr(names[ls]));
    }
    ImGui::PopTextWrapPos();
    ImGui::Unindent(10 * s);
}

void App::drawToasts() {
    const double now = ImGui::GetTime();
    while (!toasts_.empty() && toasts_.front().until < now) toasts_.pop_front();
    if (toasts_.empty()) return;
    const float s = S();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->WorkPos.y + vp->WorkSize.y - 16 * s;
    const float maxW = 560 * s;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        ImVec2 ts = ImGui::CalcTextSize(it->msg.c_str(), nullptr, false, maxW - 24 * s);
        float a = (float)std::min(1.0, (it->until - now) / 0.4);
        ImVec2 p1(vp->WorkPos.x + vp->WorkSize.x - 16 * s, y);
        ImVec2 p0(p1.x - ts.x - 24 * s, y - ts.y - 16 * s);
        dl->AddRectFilled(p0, p1, withAlpha(it->error ? IM_COL32(120, 40, 36, 255) : IM_COL32(44, 50, 58, 255), 0.95f * a), 8 * s);
        dl->AddRectFilled(p0, ImVec2(p0.x + 4 * s, p1.y), withAlpha(it->error ? P.red : P.accent, a), 2 * s);
        dl->AddText(nullptr, 0.f, ImVec2(p0.x + 14 * s, p0.y + 8 * s), withAlpha(P.text, a), it->msg.c_str(), nullptr, maxW - 24 * s);
        y = p0.y - 8 * s;
    }
}

}  // namespace gs
