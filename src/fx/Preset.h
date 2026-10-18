#pragma once
// Effect chain presets: data structures + text format + built-in presets.
//
// Text format (UTF-8, one item per line, '#' starts a comment):
//   name=Classic Rock
//   desc=Medium gain for chords and riffs
//   fx=noisegate bypass=0 threshold=-58 release=80
//   fx=amp bypass=0 channel=1 gain=6 bass=5.5 mid=6 treble=6
//   file=/home/me/ir/4x12.wav        <- applies to the preceding fx line (path may contain spaces; runs to end of line)
// Parameters that are not listed take their defaults; unknown parameters/types are skipped with a warning.
#include <string>
#include <utility>
#include <vector>

namespace gs {

struct FxSlotState {
    std::string typeId;
    bool bypass = false;
    std::vector<std::pair<std::string, float>> params;  // parameter id -> value
    std::string file;                                   // optional external file (e.g. an IR)
};

struct PresetData {
    std::string name;         // English; the UI translates built-in names with tr()
    std::string description;
    std::vector<FxSlotState> slots;
};

std::string presetToText(const PresetData& p);
bool presetFromText(const std::string& text, PresetData& out, std::string& err);
const std::vector<PresetData>& builtinPresets();  // the first one is the default preset

}  // namespace gs
