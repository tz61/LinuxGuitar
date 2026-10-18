#pragma once
// Sound card hardware mixer: input source selection (rear mic / front mic / line in), hardware gain, jack
// detection, hardware master volume. Targets Realtek HDA; features whose controls don't exist are skipped.
#include <string>
#include <vector>

typedef struct _snd_mixer snd_mixer_t;
typedef struct _snd_mixer_elem snd_mixer_elem_t;
typedef struct _snd_ctl snd_ctl_t;

namespace gs {

class CardMixer {
public:
    ~CardMixer() { close(); }
    bool open(int cardIndex, std::string& msg);
    void close();
    bool isOpen() const { return mixer_ != nullptr; }
    void refresh();  // pick up changes made by other programs

    std::vector<std::string> inputSources();  // e.g. {"Rear Mic", "Front Mic", "Line"}
    int inputSource();                        // -1 = no such control
    bool setInputSource(int idx);
    bool jackPlugged(const std::string& source);  // reads the "<source> Jack" jack-sense control
    bool hasJackDetect(const std::string& source);

    int boostMax(const std::string& source);  // 0 = no boost control; Realtek usually 3 (+10 dB per step)
    int boost(const std::string& source);
    bool setBoost(const std::string& source, int step);

    bool captureRangeDb(float& lo, float& hi);
    float captureDb();
    bool setCaptureDb(float db);

    bool masterRangeDb(float& lo, float& hi);
    float masterDb();
    bool setMasterDb(float db);

    bool setLoopbackMixing(bool enabled);  // analog loopback (hardware passthrough); must be off or the dry signal is heard

    // Output switches: "Front" = rear green line out (speakers), "Headphone" = front panel. -1 = no such control
    int playbackSwitch(const char* name);
    bool setPlaybackSwitch(const char* name, bool on);
    bool setPlaybackVolumeMax(const char* name);
    int autoMute();                  // -1 no control, 0 off, 1 on (mutes speakers when headphones are plugged)
    bool setAutoMute(bool enabled);

    static std::string sourceLabel(const std::string& source);  // English display label (translate with tr() for the UI)

private:
    snd_mixer_elem_t* find(const char* name, int index = 0);
    snd_mixer_t* mixer_ = nullptr;
    snd_ctl_t* ctl_ = nullptr;
};

}  // namespace gs
