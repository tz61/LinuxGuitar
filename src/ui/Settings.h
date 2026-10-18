#pragma once
// User settings (~/.config/guitarstudio/settings.ini) plus common path/file helpers.
#include <string>
#include <vector>

namespace gs {

struct Settings {
    // Audio device
    std::string device = "hw:PCH,0";
    int rate = 48000;
    int period = 16;
    float safetyMs = 1.0f;
    bool reserveDevice = true;
    bool systemAudio = true;     // system audio passthrough
    float systemAudioDb = -6.f;
    // Input
    std::string inputSource = "auto";  // "auto" or a mixer input source name (Rear Mic / Front Mic / Line)
    int boost = 2;                      // hardware boost step (Realtek: 0/10/20/30 dB)
    float captureDb = 10.f;             // capture gain (dB)
    bool applyHwGain = true;            // let this app manage the hardware gain
    int inputChannel = 0;               // 0 auto, 1 left, 2 right, 3 mix
    float inputGainDb = 0.f;            // digital gain
    // Output
    int outputTarget = 0;  // 0 speakers (rear green line out), 1 front headphones, 2 both
    float masterDb = -6.f;
    float guitarDb = 0.f;
    // Practice
    float speed = 1.f;
    bool waitMode = false;
    bool metronome = true;
    float metronomeVol = 0.45f;
    bool reference = true;
    float referenceVol = 0.45f;
    bool countIn = true;
    int countInBeats = 2;               // count-in clicks before a song starts
    int endBeats = 2;                   // beats after the last note before the run ends
    float tolerance = 1.f;
    float latencyOffsetMs = 0.f;
    bool autoStart = true;
    float zoom = 1.f;
    float freeBpm = 80.f;
    int beatsPerBar = 4;
    // Tuner
    float a4 = 440.f;
    bool tunerMute = true;
    int tuning = 0;  // 0 standard, 1 half step down, 2 drop D
    // Interface
    static constexpr float kDefaultUiScale = 1.5f;
    int language = 0;                   // 0 English, 1 Chinese
    float uiScale = kDefaultUiScale;    // 150% by default
    int winW = 0, winH = 0;             // 0 = size from the screen
    bool winMax = false;
    int tab = 1;
    std::string lastSong;
    std::string lastPreset;
    bool showLibrary = true;

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

std::string homeDir();
std::string configDir();      // ~/.config/guitarstudio
std::string dataDir();        // ~/.local/share/guitarstudio
std::string userSongDir();    // ~/.local/share/guitarstudio/songs
std::string builtinSongDir(); // built-in lessons: <prefix>/share/guitarstudio/lessons once installed, else assets/songs
std::string userPresetDir();  // ~/.config/guitarstudio/presets
bool ensureDir(const std::string& path);
bool fileExists(const std::string& path);
std::string readFile(const std::string& path, bool* ok = nullptr);
bool writeFile(const std::string& path, const std::string& data);
std::vector<std::string> listDir(const std::string& dir);  // full paths, sorted by file name
std::string baseName(const std::string& path);
std::string extLower(const std::string& path);              // ".gtab"
std::string sanitizeFileName(const std::string& s);

}  // namespace gs
