#include "audio/AlsaDuplex.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

#include "audio/Format.h"
#include "common/I18n.h"

namespace gs {

namespace {
std::string alsaErr(const char* what, int r) { return std::string(what) + ": " + snd_strerror(r); }
inline float clamp1(float x) { return x > 1.f ? 1.f : (x < -1.f ? -1.f : x); }
}  // namespace

bool AlsaDuplex::setupHw(snd_pcm_t* pcm, bool playback, unsigned& rate, unsigned& period, unsigned bufFrames,
                         unsigned& channels, int& format, unsigned& actualBuf, std::string& err) {
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    int r = snd_pcm_hw_params_any(pcm, hw);
    if (r < 0) return err = alsaErr("hw_params_any", r), false;
    snd_pcm_hw_params_set_rate_resample(pcm, hw, 0);
    if ((r = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
        return err = alsaErr("set_access", r), false;
    const snd_pcm_format_t fmts[] = {SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S24_3LE, SND_PCM_FORMAT_S16_LE};
    format = SND_PCM_FORMAT_UNKNOWN;
    for (auto f : fmts)
        if (snd_pcm_hw_params_set_format(pcm, hw, f) == 0) {
            format = f;
            break;
        }
    if (format == SND_PCM_FORMAT_UNKNOWN) return err = tr("The sound card supports none of S32/S24/S16"), false;
    unsigned ch = 2;
    if ((r = snd_pcm_hw_params_set_channels_near(pcm, hw, &ch)) < 0) return err = alsaErr("set_channels", r), false;
    channels = ch;
    if ((r = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr)) < 0) return err = alsaErr("set_rate", r), false;
    snd_pcm_uframes_t ps = period;
    int dir = 0;
    if ((r = snd_pcm_hw_params_set_period_size_near(pcm, hw, &ps, &dir)) < 0)
        return err = alsaErr("set_period_size", r), false;
    period = (unsigned)ps;
    snd_pcm_uframes_t bs = std::max<snd_pcm_uframes_t>(bufFrames, ps * 2);
    if ((r = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bs)) < 0)
        return err = alsaErr("set_buffer_size", r), false;
    if ((r = snd_pcm_hw_params(pcm, hw)) < 0)
        return err = alsaErr(playback ? tr("playback parameters") : tr("capture parameters"), r), false;
    snd_pcm_hw_params_get_buffer_size(hw, &bs);
    actualBuf = (unsigned)bs;
    return true;
}

bool AlsaDuplex::setupSw(snd_pcm_t* pcm, unsigned period) {
    snd_pcm_sw_params_t* sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_uframes_t boundary = 0;
    snd_pcm_sw_params_get_boundary(sw, &boundary);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, boundary);  // started manually (linked)
    snd_pcm_sw_params_set_avail_min(pcm, sw, period);
    return snd_pcm_sw_params(pcm, sw) == 0;
}

bool AlsaDuplex::open(const AlsaConfig& cfg, std::string& err) {
    close();
    int r = snd_pcm_open(&cap_, cfg.device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (r < 0) {
        cap_ = nullptr;
        err = alsaErr(strf(tr("Cannot open capture device %s"), cfg.device.c_str()).c_str(), r);
        return false;
    }
    r = snd_pcm_open(&play_, cfg.device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (r < 0) {
        play_ = nullptr;
        close();
        err = alsaErr(strf(tr("Cannot open playback device %s"), cfg.device.c_str()).c_str(), r);
        return false;
    }
    unsigned rateC = cfg.rate, rateP = cfg.rate, perC = std::max(8u, cfg.period), perP = perC;
    // The playback buffer must be large (calibration uses a big prefill); its size doesn't affect latency,
    // only the prefill does
    if (!setupHw(play_, true, rateP, perP, std::max(1024u, perP * 8), playCh_, playFmt_, playBufFrames_, err) ||
        !setupHw(cap_, false, rateC, perC, std::max(1024u, perC * 16), capCh_, capFmt_, capBufFrames_, err)) {
        close();
        return false;
    }
    if (rateC != rateP || perC != perP) {
        close();
        err = tr("Could not set capture and playback to the same sample rate/period");
        return false;
    }
    rate_ = rateP;
    period_ = perP;
    if (!setupSw(cap_, period_) || !setupSw(play_, period_)) {
        close();
        err = tr("Failed to set sw_params");
        return false;
    }
    linked_ = snd_pcm_link(cap_, play_) == 0;
    const unsigned bps = (unsigned)snd_pcm_format_physical_width((snd_pcm_format_t)playFmt_) / 8;
    capBuf_.assign((size_t)period_ * capCh_ * 4, 0);
    playBuf_.assign((size_t)period_ * playCh_ * 4, 0);
    silence_.assign((size_t)playBufFrames_ * playCh_ * bps, 0);
    prefill_ = std::min(maxPrefill(), period_ * 4);
    return true;
}

void AlsaDuplex::close() {
    if (cap_ && play_ && linked_) snd_pcm_unlink(cap_);
    if (cap_) snd_pcm_close(cap_);
    if (play_) snd_pcm_close(play_);
    cap_ = play_ = nullptr;
    linked_ = false;
}

std::string AlsaDuplex::formatDesc() const {
    if (!isOpen()) return "-";
    return strf(tr("%s/%s, %s, playback buffer %u frames"), snd_pcm_format_name((snd_pcm_format_t)capFmt_),
                snd_pcm_format_name((snd_pcm_format_t)playFmt_), linked_ ? tr("linked") : tr("not linked"),
                playBufFrames_);
}

bool AlsaDuplex::writeSilence(unsigned frames) {
    const unsigned frameBytes = (unsigned)snd_pcm_format_physical_width((snd_pcm_format_t)playFmt_) / 8 * playCh_;
    unsigned done = 0;
    while (done < frames) {
        unsigned chunk = std::min<unsigned>(frames - done, (unsigned)(silence_.size() / frameBytes));
        snd_pcm_sframes_t r = snd_pcm_writei(play_, silence_.data(), chunk);
        if (r == -EINTR || r == -EAGAIN) continue;
        if (r < 0) return false;
        done += (unsigned)r;
    }
    return true;
}

bool AlsaDuplex::start(unsigned prefillFrames) {
    if (!isOpen()) return false;
    prefill_ = std::clamp(prefillFrames, period_, maxPrefill());
    snd_pcm_drop(cap_);
    snd_pcm_drop(play_);
    snd_pcm_prepare(cap_);
    if (snd_pcm_state(play_) != SND_PCM_STATE_PREPARED) snd_pcm_prepare(play_);
    if (!writeSilence(prefill_)) return false;
    if (linked_) return snd_pcm_start(cap_) == 0;
    return snd_pcm_start(play_) == 0 && snd_pcm_start(cap_) == 0;
}

void AlsaDuplex::stop() {
    if (cap_) snd_pcm_drop(cap_);
    if (play_) snd_pcm_drop(play_);
}

AlsaCalibration AlsaDuplex::calibrate(double seconds) {
    AlsaCalibration c;
    const unsigned X = std::min(maxPrefill(), 768u);
    if (!start(X)) return c;
    snd_pcm_sframes_t d = 0;
    if (snd_pcm_delay(play_, &d) == 0 && d >= 0 && (unsigned)d < X) c.fifoFrames = X - (unsigned)d;
    long minDelay = X;
    const int n = std::max(20, (int)(seconds * rate_ / period_));
    const unsigned frameBytes = (unsigned)snd_pcm_format_physical_width((snd_pcm_format_t)capFmt_) / 8 * capCh_;
    for (int k = 0; k < n; ++k) {
        unsigned got = 0;
        while (got < period_) {
            snd_pcm_sframes_t r = snd_pcm_readi(cap_, capBuf_.data() + (size_t)got * frameBytes, period_ - got);
            if (r == -EINTR || r == -EAGAIN) continue;
            if (r < 0) {
                stop();
                return c;
            }
            got += (unsigned)r;
        }
        snd_pcm_sframes_t pd = 0;
        if (snd_pcm_delay(play_, &pd) == 0) minDelay = std::min<long>(minDelay, pd);
        if (!writeSilence(period_)) {
            stop();
            return c;
        }
    }
    stop();
    c.usedFrames = (unsigned)std::max<long>((long)period_, (long)X - minDelay);
    c.ok = true;
    return c;
}

int AlsaDuplex::readPeriod(float* L, float* R) {
    const unsigned frameBytes = (unsigned)snd_pcm_format_physical_width((snd_pcm_format_t)capFmt_) / 8 * capCh_;
    unsigned got = 0;
    while (got < period_) {
        snd_pcm_sframes_t r = snd_pcm_readi(cap_, capBuf_.data() + (size_t)got * frameBytes, period_ - got);
        if (r == -EINTR || r == -EAGAIN) continue;
        if (r < 0) return (int)r;
        got += (unsigned)r;
    }
    const unsigned ch = capCh_;
    if (capFmt_ == SND_PCM_FORMAT_S32_LE) {
        const int32_t* s = reinterpret_cast<const int32_t*>(capBuf_.data());
        const float k = 1.f / 2147483648.f;
        for (unsigned i = 0; i < period_; ++i) {
            L[i] = (float)s[i * ch] * k;
            R[i] = ch > 1 ? (float)s[i * ch + 1] * k : L[i];
        }
    } else if (capFmt_ == SND_PCM_FORMAT_S16_LE) {
        const int16_t* s = reinterpret_cast<const int16_t*>(capBuf_.data());
        const float k = 1.f / 32768.f;
        for (unsigned i = 0; i < period_; ++i) {
            L[i] = (float)s[i * ch] * k;
            R[i] = ch > 1 ? (float)s[i * ch + 1] * k : L[i];
        }
    } else {  // S24_3LE
        const uint8_t* s = reinterpret_cast<const uint8_t*>(capBuf_.data());
        const float k = 1.f / 8388608.f;
        auto rd = [&](unsigned idx) {
            const uint8_t* p = s + idx * 3;
            int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
            return (float)v * k;
        };
        for (unsigned i = 0; i < period_; ++i) {
            L[i] = rd(i * ch);
            R[i] = ch > 1 ? rd(i * ch + 1) : L[i];
        }
    }
    return (int)period_;
}

int AlsaDuplex::writePeriod(const float* L, const float* R) {
    const unsigned ch = playCh_;
    if (playFmt_ == SND_PCM_FORMAT_S32_LE) {
        int32_t* d = reinterpret_cast<int32_t*>(playBuf_.data());
        for (unsigned i = 0; i < period_; ++i) {
            d[i * ch] = (int32_t)std::lrint(clamp1(L[i]) * 2147483392.f);
            if (ch > 1) d[i * ch + 1] = (int32_t)std::lrint(clamp1(R[i]) * 2147483392.f);
            for (unsigned c = 2; c < ch; ++c) d[i * ch + c] = 0;
        }
    } else if (playFmt_ == SND_PCM_FORMAT_S16_LE) {
        int16_t* d = reinterpret_cast<int16_t*>(playBuf_.data());
        for (unsigned i = 0; i < period_; ++i) {
            d[i * ch] = (int16_t)std::lrint(clamp1(L[i]) * 32767.f);
            if (ch > 1) d[i * ch + 1] = (int16_t)std::lrint(clamp1(R[i]) * 32767.f);
            for (unsigned c = 2; c < ch; ++c) d[i * ch + c] = 0;
        }
    } else {
        uint8_t* d = reinterpret_cast<uint8_t*>(playBuf_.data());
        auto wr = [&](unsigned idx, float x) {
            int32_t v = (int32_t)std::lrint(clamp1(x) * 8388607.f);
            uint8_t* p = d + idx * 3;
            p[0] = (uint8_t)(v & 0xff);
            p[1] = (uint8_t)((v >> 8) & 0xff);
            p[2] = (uint8_t)((v >> 16) & 0xff);
        };
        for (unsigned i = 0; i < period_; ++i) {
            wr(i * ch, L[i]);
            if (ch > 1) wr(i * ch + 1, R[i]);
            for (unsigned c = 2; c < ch; ++c) wr(i * ch + c, 0.f);
        }
    }
    const unsigned frameBytes = (unsigned)snd_pcm_format_physical_width((snd_pcm_format_t)playFmt_) / 8 * ch;
    unsigned done = 0;
    while (done < period_) {
        snd_pcm_sframes_t r = snd_pcm_writei(play_, playBuf_.data() + (size_t)done * frameBytes, period_ - done);
        if (r == -EINTR || r == -EAGAIN) continue;
        if (r < 0) return (int)r;
        done += (unsigned)r;
    }
    return (int)period_;
}

std::vector<AlsaDeviceInfo> AlsaDuplex::listDevices() {
    std::vector<AlsaDeviceInfo> out;
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char hwname[32];
        snprintf(hwname, sizeof hwname, "hw:%d", card);
        snd_ctl_t* ctl = nullptr;
        if (snd_ctl_open(&ctl, hwname, 0) < 0) continue;
        snd_ctl_card_info_t* ci;
        snd_ctl_card_info_alloca(&ci);
        std::string cardId, cardName;
        if (snd_ctl_card_info(ctl, ci) == 0) {
            cardId = snd_ctl_card_info_get_id(ci);
            cardName = snd_ctl_card_info_get_name(ci);
        }
        int dev = -1;
        while (snd_ctl_pcm_next_device(ctl, &dev) == 0 && dev >= 0) {
            AlsaDeviceInfo d;
            d.card = card;
            d.device = dev;
            d.id = "hw:" + (cardId.empty() ? std::to_string(card) : cardId) + "," + std::to_string(dev);
            snd_pcm_info_t* pi;
            snd_pcm_info_alloca(&pi);
            for (int st = 0; st < 2; ++st) {
                snd_pcm_info_set_device(pi, (unsigned)dev);
                snd_pcm_info_set_subdevice(pi, 0);
                snd_pcm_info_set_stream(pi, st == 0 ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE);
                if (snd_ctl_pcm_info(ctl, pi) == 0) {
                    if (st == 0) d.canPlayback = true;
                    else d.canCapture = true;
                    d.name = cardName + ": " + snd_pcm_info_get_name(pi);
                }
            }
            if (d.canCapture || d.canPlayback) out.push_back(d);
        }
        snd_ctl_close(ctl);
    }
    return out;
}

int AlsaDuplex::cardIndexOf(const std::string& device) {
    auto p = device.find(':');
    if (p == std::string::npos) return -1;
    std::string id = device.substr(p + 1);
    auto c = id.find(',');
    if (c != std::string::npos) id = id.substr(0, c);
    return snd_card_get_index(id.c_str());
}

}  // namespace gs
