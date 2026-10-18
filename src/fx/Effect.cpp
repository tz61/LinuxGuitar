#include "fx/Effect.h"

#include <algorithm>
#include <cmath>

#include "common/I18n.h"

namespace gs {

Effect::Effect(std::vector<ParamInfo> params) : info_(std::move(params)) {
    values_.reset(new std::atomic<float>[info_.size() > 0 ? info_.size() : 1]);
    resetParamsToDefault();
}

bool Effect::loadFile(const std::string& path, std::string& err) {
    (void)path;
    err = tr("Not supported");
    return false;
}

int Effect::paramIndex(const std::string& id) const {
    for (size_t i = 0; i < info_.size(); ++i)
        if (info_[i].id == id) return (int)i;
    return -1;
}

void Effect::setParam(int i, float v) {
    if (i < 0 || i >= (int)info_.size() || !std::isfinite(v)) return;
    const ParamInfo& p = info_[i];
    v = std::clamp(v, p.minV, p.maxV);
    if (p.scale == ParamScale::Int || p.scale == ParamScale::Toggle || p.scale == ParamScale::Choice)
        v = std::round(v);
    values_[i].store(v, std::memory_order_relaxed);
}

bool Effect::setParam(const std::string& id, float v) {
    int i = paramIndex(id);
    if (i < 0) return false;
    setParam(i, v);
    return true;
}

void Effect::resetParamsToDefault() {
    for (size_t i = 0; i < info_.size(); ++i) values_[i].store(info_[i].defV, std::memory_order_relaxed);
}

std::atomic<float>& globalBpm() {
    static std::atomic<float> bpm{100.f};
    return bpm;
}

}  // namespace gs
