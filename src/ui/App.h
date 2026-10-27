#pragma once
// Application: wires up the audio engine, analysis, practice and song library, and draws the whole UI.
#include <imgui.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "analysis/Analyzer.h"
#include "audio/Engine.h"
#include "practice/Practice.h"
#include "song/Song.h"
#include "ui/FileDialog.h"
#include "ui/Library.h"
#include "ui/Settings.h"

namespace gs {

struct AppOptions {
    bool simulate = false;  // no-sound-card mode
    bool demo = false;      // demo: a simulated player plays the current song (no-sound-card mode only)
    std::string song;       // song to import and open at startup
    std::string device;     // overrides the sound card from the settings
    std::string preset;     // preset name to load at startup
    int tab = -1;
    bool autoplay = false;
    float dpiScale = 1.f;  // UI scale suggested by the screen DPI
};

enum class Tab { Rack = 0, Practice, Tuner, Looper, Settings, Count };

// Registers the Chinese catalog for UI strings (ui/I18nZh.cpp)
void registerUiTranslations();

class App {
public:
    explicit App(const AppOptions& opt);
    ~App();
    bool init();
    void frame();  // called every frame after ImGui::NewFrame()
    void onDropFile(const std::string& path);
    void shutdown();
    bool quitRequested() const { return quit_; }
    bool takeScaleChange() {
        bool c = scaleDirty_;
        scaleDirty_ = false;
        return c;
    }
    float uiScale() const { return settings_.uiScale; }
    void setUiScale(float scale);
    void setLanguage(int lang);  // 0 English, 1 Chinese
    // main() reports the window state before exiting so it can be restored next time
    void setWindowState(int w, int h, bool maximized) {
        settings_.winW = w;
        settings_.winH = h;
        settings_.winMax = maximized;
    }

private:
    // ---- UI ----
    void drawTopBar(float h);
    void drawNav(float w);
    void drawRack();
    void drawFxCard(int idx, float cardW, float cardH);
    float fxCardHeight(const Effect& fx, float cardW) const;
    void drawAddFxPopup();
    void drawPractice();
    void drawLibrary(float w);
    void drawSongHeader();
    void drawTransport();
    void drawMetronomeOptions(float width);  // metronome volume, count-in and ending beats (practice popup + Settings)
    void drawTimeline(float h);
    void drawHighway(ImVec2 size);
    void drawPitchRoll(ImVec2 size);
    void drawInfoPanel(ImVec2 size);
    void drawSummaryPopup();
    void drawTuner();
    void drawLooper();
    void drawSettings();
    void drawToasts();
    void drawWelcome();

    // ---- logic ----
    void startAudio();
    void restartAudio();
    void applyInputSettings(bool force);
    void applyOutputSettings(bool force);
    static void setPulsePort(const std::string& port);
    void pollDevices();
    bool openSong(const std::string& path, bool autoStart, int track = -1);
    void importSong(const std::string& path);
    void pasteSong();
    void scanPresets();
    bool loadPresetNamed(const std::string& name);
    void saveCurrentPreset(const std::string& name);
    void handleShortcuts();
    void toast(const std::string& msg, bool error = false);
    void syncPractice();
    void autoGainStep();
    float songBpm() const;
    int nextEventIndex() const;
    // Song position the player hears right now: the transport runs ahead of the speakers/headphones by the output
    // latency (a few ms on the sound card, ~0.2 s over Bluetooth). Updated once per frame.
    void updateHeardPosition();
    std::string outputLabel(const std::string& sink) const;  // "Sound card" / the device's description

    AppOptions opt_;
    Settings settings_;
    Analyzer analyzer_;
    Practice practice_{analyzer_};
    Engine engine_;
    Library library_;
    FileDialog fileDlg_;

    bool quit_ = false, scaleDirty_ = false, inited_ = false;
    Tab tab_ = Tab::Practice;

    // Presets
    struct PresetItem {
        PresetData data;
        bool builtin;
        std::string path;
    };
    std::vector<PresetItem> presets_;
    std::string curPreset_;
    char presetName_[128] = {0};
    bool addFxOpen_ = false;
    int addFxInsertAt_ = -1;
    int irTargetSlot_ = -1;  // id of the effect waiting for the IR file dialog

    // Practice
    std::shared_ptr<Song> song_;
    std::string songPath_;
    std::vector<TrackInfo> tracks_;
    int track_ = -1;
    double autoPlayAt_ = -1;
    std::vector<EventResult> results_;
    PracticeStats stats_;
    std::vector<PitchPoint> pitchHist_;
    bool showSummary_ = false;
    PassSummary lastSummary_;
    float libFilterLevel_ = 0;
    char libFilter_[64] = {0};
    bool loopSetMode_ = false;
    double loopDragStart_ = -1;

    // Meters
    float inPeakHold_ = -120, outPeakHold_ = -120;

    // Sound card input
    std::vector<std::string> inputSources_;
    std::vector<bool> jackState_;
    std::string activeSource_;
    double lastPoll_ = 0;
    bool hpPlugged_ = false, outputInit_ = false;
    int lastOutputTarget_ = -1;
    double signalSeenAt_ = -100;
    int lastXruns_ = 0;
    double xrunWarnUntil_ = 0;

    // Auto-gain wizard
    int agState_ = 0;  // 0 off, 1 measuring, 2 done
    bool agOk_ = false;
    double agStart_ = 0;
    float agPeak_ = -120;
    std::string agMsg_;

    // Toasts
    struct Toast {
        std::string msg;
        bool error;
        double until;
    };
    std::deque<Toast> toasts_;
    std::string sysAudioMsg_;
    std::vector<PulseSink> sinks_;  // PulseAudio playback devices (refreshed while Settings is open)
    double sinksTime_ = -10;
    std::deque<std::pair<uint64_t, double>> posHist_;  // (engine frame, transport position) once per frame
    const SongTimeline* posHistTl_ = nullptr;
    double heardPos_ = 0;
    bool welcomeShown_ = false;
    bool limiterUi_ = true;
};

}  // namespace gs
