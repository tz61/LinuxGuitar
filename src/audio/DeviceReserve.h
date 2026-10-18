#pragma once
// Audio device reservation protocol (org.freedesktop.ReserveDevice1, also used by JACK).
// Once we own the name "AudioN", PulseAudio / PipeWire release and suspend that card, so we can open the hw
// device exclusively for the lowest latency. Releasing the name on exit lets PulseAudio resume it.
#include <string>

struct DBusConnection;

namespace gs {

class DeviceReservation {
public:
    DeviceReservation() = default;
    ~DeviceReservation() { release(); }
    DeviceReservation(const DeviceReservation&) = delete;
    DeviceReservation& operator=(const DeviceReservation&) = delete;

    // Reserve ALSA card cardIndex. Returns true on success; msg receives a description.
    bool acquire(int cardIndex, const std::string& deviceName, std::string& msg);
    void release();
    bool held() const { return conn_ != nullptr; }
    // Handle requests from other programs (e.g. asking us to release). Call periodically from the UI thread.
    void poll();

    const std::string& appDeviceName() const { return deviceName_; }

private:
    DBusConnection* conn_ = nullptr;
    std::string service_, path_, deviceName_;
};

}  // namespace gs
