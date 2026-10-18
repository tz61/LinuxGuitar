#include "audio/CardMixer.h"

#include <alsa/asoundlib.h>

#include <cmath>

#include "audio/Format.h"
#include "common/I18n.h"

namespace gs {

bool CardMixer::open(int cardIndex, std::string& msg) {
    close();
    std::string hw = "hw:" + std::to_string(cardIndex);
    if (snd_mixer_open(&mixer_, 0) < 0) {
        mixer_ = nullptr;
        msg = tr("Cannot open the mixer");
        return false;
    }
    if (snd_mixer_attach(mixer_, hw.c_str()) < 0 || snd_mixer_selem_register(mixer_, nullptr, nullptr) < 0 ||
        snd_mixer_load(mixer_) < 0) {
        snd_mixer_close(mixer_);
        mixer_ = nullptr;
        msg = strf(tr("Cannot load the mixer of %s"), hw.c_str());
        return false;
    }
    if (snd_ctl_open(&ctl_, hw.c_str(), SND_CTL_NONBLOCK) < 0) ctl_ = nullptr;
    msg = tr("Mixer opened");
    return true;
}

void CardMixer::close() {
    if (mixer_) snd_mixer_close(mixer_);
    if (ctl_) snd_ctl_close(ctl_);
    mixer_ = nullptr;
    ctl_ = nullptr;
}

void CardMixer::refresh() {
    if (mixer_) snd_mixer_handle_events(mixer_);
}

snd_mixer_elem_t* CardMixer::find(const char* name, int index) {
    if (!mixer_) return nullptr;
    snd_mixer_selem_id_t* sid;
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, index);
    snd_mixer_selem_id_set_name(sid, name);
    return snd_mixer_find_selem(mixer_, sid);
}

std::vector<std::string> CardMixer::inputSources() {
    std::vector<std::string> out;
    snd_mixer_elem_t* e = find("Input Source");
    if (!e || !snd_mixer_selem_is_enumerated(e)) return out;
    int n = snd_mixer_selem_get_enum_items(e);
    for (int i = 0; i < n; ++i) {
        char buf[64] = {0};
        snd_mixer_selem_get_enum_item_name(e, i, sizeof buf - 1, buf);
        out.emplace_back(buf);
    }
    return out;
}

int CardMixer::inputSource() {
    snd_mixer_elem_t* e = find("Input Source");
    if (!e || !snd_mixer_selem_is_enumerated(e)) return -1;
    unsigned int item = 0;
    if (snd_mixer_selem_get_enum_item(e, SND_MIXER_SCHN_FRONT_LEFT, &item) < 0) return -1;
    return (int)item;
}

bool CardMixer::setInputSource(int idx) {
    snd_mixer_elem_t* e = find("Input Source");
    if (!e || !snd_mixer_selem_is_enumerated(e)) return false;
    return snd_mixer_selem_set_enum_item(e, SND_MIXER_SCHN_FRONT_LEFT, (unsigned)idx) >= 0;
}

bool CardMixer::hasJackDetect(const std::string& source) {
    if (!ctl_) return false;
    snd_ctl_elem_id_t* id;
    snd_ctl_elem_info_t* info;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_CARD);
    snd_ctl_elem_id_set_name(id, (source + " Jack").c_str());
    snd_ctl_elem_info_set_id(info, id);
    return snd_ctl_elem_info(ctl_, info) == 0;
}

bool CardMixer::jackPlugged(const std::string& source) {
    if (!ctl_) return false;
    snd_ctl_elem_id_t* id;
    snd_ctl_elem_value_t* v;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_value_alloca(&v);
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_CARD);
    snd_ctl_elem_id_set_name(id, (source + " Jack").c_str());
    snd_ctl_elem_value_set_id(v, id);
    if (snd_ctl_elem_read(ctl_, v) < 0) return false;
    return snd_ctl_elem_value_get_boolean(v, 0) != 0;
}

static bool isCaptureVol(snd_mixer_elem_t* e) { return snd_mixer_selem_has_capture_volume(e); }

int CardMixer::boostMax(const std::string& source) {
    snd_mixer_elem_t* e = find((source + " Boost").c_str());
    if (!e) return 0;
    long lo = 0, hi = 0;
    if (isCaptureVol(e)) snd_mixer_selem_get_capture_volume_range(e, &lo, &hi);
    else snd_mixer_selem_get_playback_volume_range(e, &lo, &hi);
    return (int)(hi - lo);
}

int CardMixer::boost(const std::string& source) {
    snd_mixer_elem_t* e = find((source + " Boost").c_str());
    if (!e) return 0;
    long lo = 0, hi = 0, v = 0;
    if (isCaptureVol(e)) {
        snd_mixer_selem_get_capture_volume_range(e, &lo, &hi);
        snd_mixer_selem_get_capture_volume(e, SND_MIXER_SCHN_FRONT_LEFT, &v);
    } else {
        snd_mixer_selem_get_playback_volume_range(e, &lo, &hi);
        snd_mixer_selem_get_playback_volume(e, SND_MIXER_SCHN_FRONT_LEFT, &v);
    }
    return (int)(v - lo);
}

bool CardMixer::setBoost(const std::string& source, int step) {
    snd_mixer_elem_t* e = find((source + " Boost").c_str());
    if (!e) return false;
    long lo = 0, hi = 0;
    if (isCaptureVol(e)) {
        snd_mixer_selem_get_capture_volume_range(e, &lo, &hi);
        long v = std::max(lo, std::min(hi, lo + step));
        return snd_mixer_selem_set_capture_volume_all(e, v) >= 0;
    }
    snd_mixer_selem_get_playback_volume_range(e, &lo, &hi);
    long v = std::max(lo, std::min(hi, lo + step));
    return snd_mixer_selem_set_playback_volume_all(e, v) >= 0;
}

bool CardMixer::captureRangeDb(float& lo, float& hi) {
    snd_mixer_elem_t* e = find("Capture");
    long a = 0, b = 0;
    if (!e || !snd_mixer_selem_has_capture_volume(e) || snd_mixer_selem_get_capture_dB_range(e, &a, &b) < 0) return false;
    lo = a / 100.f;
    hi = b / 100.f;
    return true;
}

float CardMixer::captureDb() {
    snd_mixer_elem_t* e = find("Capture");
    long v = 0;
    if (!e || snd_mixer_selem_get_capture_dB(e, SND_MIXER_SCHN_FRONT_LEFT, &v) < 0) return NAN;
    return v / 100.f;
}

bool CardMixer::setCaptureDb(float db) {
    snd_mixer_elem_t* e = find("Capture");
    if (!e || !snd_mixer_selem_has_capture_volume(e)) return false;
    if (snd_mixer_selem_has_capture_switch(e)) snd_mixer_selem_set_capture_switch_all(e, 1);
    return snd_mixer_selem_set_capture_dB_all(e, (long)std::lround(db * 100.f), 0) >= 0;
}

bool CardMixer::masterRangeDb(float& lo, float& hi) {
    snd_mixer_elem_t* e = find("Master");
    long a = 0, b = 0;
    if (!e || !snd_mixer_selem_has_playback_volume(e) || snd_mixer_selem_get_playback_dB_range(e, &a, &b) < 0) return false;
    lo = a / 100.f;
    hi = b / 100.f;
    return true;
}

float CardMixer::masterDb() {
    snd_mixer_elem_t* e = find("Master");
    long v = 0;
    if (!e || snd_mixer_selem_get_playback_dB(e, SND_MIXER_SCHN_MONO, &v) < 0) return NAN;
    return v / 100.f;
}

bool CardMixer::setMasterDb(float db) {
    snd_mixer_elem_t* e = find("Master");
    if (!e || !snd_mixer_selem_has_playback_volume(e)) return false;
    if (snd_mixer_selem_has_playback_switch(e)) snd_mixer_selem_set_playback_switch_all(e, 1);
    return snd_mixer_selem_set_playback_dB_all(e, (long)std::lround(db * 100.f), 0) >= 0;
}

bool CardMixer::setLoopbackMixing(bool enabled) {
    snd_mixer_elem_t* e = find("Loopback Mixing");
    if (!e || !snd_mixer_selem_is_enumerated(e)) return false;
    return snd_mixer_selem_set_enum_item(e, SND_MIXER_SCHN_FRONT_LEFT, enabled ? 1 : 0) >= 0;
}

int CardMixer::playbackSwitch(const char* name) {
    snd_mixer_elem_t* e = find(name);
    if (!e || !snd_mixer_selem_has_playback_switch(e)) return -1;
    int v = 0;
    snd_mixer_selem_get_playback_switch(e, SND_MIXER_SCHN_FRONT_LEFT, &v);
    return v ? 1 : 0;
}

bool CardMixer::setPlaybackSwitch(const char* name, bool on) {
    snd_mixer_elem_t* e = find(name);
    if (!e || !snd_mixer_selem_has_playback_switch(e)) return false;
    return snd_mixer_selem_set_playback_switch_all(e, on ? 1 : 0) >= 0;
}

bool CardMixer::setPlaybackVolumeMax(const char* name) {
    snd_mixer_elem_t* e = find(name);
    if (!e || !snd_mixer_selem_has_playback_volume(e)) return false;
    long lo = 0, hi = 0;
    snd_mixer_selem_get_playback_volume_range(e, &lo, &hi);
    return snd_mixer_selem_set_playback_volume_all(e, hi) >= 0;
}

int CardMixer::autoMute() {
    snd_mixer_elem_t* e = find("Auto-Mute Mode");
    if (!e || !snd_mixer_selem_is_enumerated(e)) return -1;
    unsigned int item = 0;
    snd_mixer_selem_get_enum_item(e, SND_MIXER_SCHN_FRONT_LEFT, &item);
    return item ? 1 : 0;
}

bool CardMixer::setAutoMute(bool enabled) {
    snd_mixer_elem_t* e = find("Auto-Mute Mode");
    if (!e || !snd_mixer_selem_is_enumerated(e)) return false;
    return snd_mixer_selem_set_enum_item(e, SND_MIXER_SCHN_FRONT_LEFT, enabled ? 1 : 0) >= 0;
}

std::string CardMixer::sourceLabel(const std::string& s) {
    // English labels; the UI translates them with tr()
    if (s == "Rear Mic") return "Rear Mic (pink)";
    if (s == "Front Mic") return "Front Mic";
    if (s == "Line") return "Line In (blue)";
    if (s == "Mic") return "Mic";
    if (s == "Internal Mic") return "Internal Mic";
    if (s == "Headset Mic") return "Headset Mic";
    return s;
}

}  // namespace gs
