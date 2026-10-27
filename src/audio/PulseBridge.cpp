#include "audio/PulseBridge.h"

#include <fcntl.h>
#include <poll.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "common/I18n.h"

extern char** environ;

namespace gs {

namespace {
const char* kSinkName = "guitarstudio_sys";
char g_moduleId[24] = {0};  // module id of the loaded virtual sink (for the crash handler)

bool safeName(const std::string& s) {
    if (s.empty() || s.size() > 200) return false;
    for (char c : s)
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == ':' || c == '@')) return false;
    return true;
}

bool allDigits(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit); }

std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string l;
    while (std::getline(is, l)) out.push_back(l);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string defaultSink() {
    std::string out;
    if (runProgram({"pactl", "get-default-sink"}, &out) == 0) return trim(out);
    if (runProgram({"pactl", "info"}, &out) == 0)
        for (auto& l : lines(out))
            if (l.rfind("Default Sink:", 0) == 0) return trim(l.substr(13));
    return {};
}

// sink-input ids (only those playing to onSinkIndex when it is non-empty)
std::vector<std::string> sinkInputs(const std::string& onSinkIndex = {}) {
    std::vector<std::string> ids;
    std::string out;
    if (runProgram({"pactl", "list", "short", "sink-inputs"}, &out) != 0) return ids;
    for (auto& l : lines(out)) {
        std::istringstream is(l);
        std::string id, sink;
        is >> id >> sink;
        if (!allDigits(id)) continue;
        if (!onSinkIndex.empty() && sink != onSinkIndex) continue;
        ids.push_back(id);
    }
    return ids;
}

std::string sinkIndexOf(const std::string& name) {
    std::string out;
    if (runProgram({"pactl", "list", "short", "sinks"}, &out) != 0) return {};
    for (auto& l : lines(out)) {
        std::istringstream is(l);
        std::string idx, nm;
        is >> idx >> nm;
        if (nm == name) return idx;
    }
    return {};
}
}  // namespace

int runProgram(const std::vector<std::string>& argv, std::string* out, int timeoutMs) {
    if (argv.empty()) return -1;
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        return -1;
    }
    std::string buf;
    auto t0 = std::chrono::steady_clock::now();
    char tmp[4096];
    while (true) {
        int remain = timeoutMs - (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t0).count();
        if (remain <= 0) {
            kill(pid, SIGKILL);
            break;
        }
        struct pollfd pfd{pipefd[0], POLLIN, 0};
        if (poll(&pfd, 1, remain) <= 0) continue;
        ssize_t n = read(pipefd[0], tmp, sizeof tmp);
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
    }
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (out) *out = buf;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

PulseBridge::PulseBridge() {
    ring_.init(1u << 19);   // 1.3 s even for 192 kHz stereo
    tmpL_.assign(4096, 0.f);
    tmpR_.assign(4096, 0.f);
}

bool PulseBridge::pactlAvailable() {
    std::string out;
    return runProgram({"pactl", "info"}, &out, 2000) == 0;
}

void PulseBridge::cleanupStale() {
    std::string out;
    if (runProgram({"pactl", "list", "short", "modules"}, &out, 2000) != 0) return;
    for (auto& l : lines(out)) {
        if (l.find(std::string("sink_name=") + kSinkName) == std::string::npos) continue;
        std::istringstream is(l);
        std::string id;
        is >> id;
        if (allDigits(id)) runProgram({"pactl", "unload-module", id});
    }
}

bool PulseBridge::start(unsigned rate, std::string& msg) {
    stop();
    cleanupStale();
    if (!pactlAvailable()) {
        msg = tr("PulseAudio (pactl) not found; system audio passthrough unavailable");
        return false;
    }
    origSink_ = defaultSink();
    if (origSink_ == kSinkName) origSink_.clear();
    std::string out;
    if (runProgram({"pactl", "load-module", "module-null-sink", std::string("sink_name=") + kSinkName,
                    "rate=" + std::to_string(rate), "channels=2",
                    "sink_properties=device.description=GuitarStudio-SystemAudio"},
                   &out) != 0) {
        msg = tr("Failed to create the virtual output");
        return false;
    }
    module_ = std::atoi(trim(out).c_str());
    std::snprintf(g_moduleId, sizeof g_moduleId, "%d", module_);
    runProgram({"pactl", "set-default-sink", kSinkName});
    for (auto& id : sinkInputs()) runProgram({"pactl", "move-sink-input", id, kSinkName});

    rate_.store(rate);
    target_.store(rate * 30 / 1000);  // target buffer 30 ms
    gen_.fetch_add(1);
    run_.store(true);
    th_ = std::thread(&PulseBridge::captureMain, this, rate);
    msg = tr("System audio passthrough on: other programs' audio plays through Guitar Studio");
    return true;
}

void PulseBridge::captureMain(unsigned rate) {
    pa_sample_spec ss{};
    ss.format = PA_SAMPLE_FLOAT32LE;
    ss.rate = rate;
    ss.channels = 2;
    pa_buffer_attr ba{};
    ba.maxlength = (uint32_t)-1;
    ba.tlength = ba.prebuf = ba.minreq = (uint32_t)-1;
    ba.fragsize = (uint32_t)(rate / 100 * 2 * sizeof(float));  // 10 ms
    int err = 0;
    std::string dev = std::string(kSinkName) + ".monitor";
    pa_simple* s = pa_simple_new(nullptr, "Guitar Studio", PA_STREAM_RECORD, dev.c_str(), "System audio passthrough", &ss, nullptr,
                                 &ba, &err);
    if (!s) {
        run_.store(false);
        return;
    }
    std::vector<float> buf(rate / 200 * 2);  // 5 ms
    while (run_.load()) {
        if (pa_simple_read(s, buf.data(), buf.size() * sizeof(float), &err) < 0) break;
        ring_.write(buf.data(), buf.size());
    }
    pa_simple_free(s);
}

void PulseBridge::stop() {
    run_.store(false);
    if (th_.joinable()) th_.join();
    if (module_ >= 0) {
        std::string ours = sinkIndexOf(kSinkName);
        if (safeName(origSink_)) {
            runProgram({"pactl", "set-default-sink", origSink_});
            if (!ours.empty())
                for (auto& id : sinkInputs(ours)) runProgram({"pactl", "move-sink-input", id, origSink_});
        }
        runProgram({"pactl", "unload-module", std::to_string(module_)});
        module_ = -1;
        g_moduleId[0] = 0;
    }
}

void PulseBridge::emergencyRestore() {
    if (!g_moduleId[0]) return;
    pid_t pid = fork();
    if (pid == 0) {
        execlp("pactl", "pactl", "unload-module", g_moduleId, (char*)nullptr);
        _exit(127);
    }
    if (pid > 0)
        for (int i = 0; i < 150; ++i) {  // wait at most 1.5 s
            if (waitpid(pid, nullptr, WNOHANG) == pid) break;
            struct timespec ts{0, 10 * 1000 * 1000};
            nanosleep(&ts, nullptr);
        }
    g_moduleId[0] = 0;
}

void PulseBridge::mixInto(float* L, float* R, int n, float gain) {
    uint32_t g0 = gen_.load(std::memory_order_acquire);
    if (g0 != seenGen_) {  // restarted: drop old data, reset state
        seenGen_ = g0;
        reader_.reset(rate_.load(), target_.load());
    }
    if (!run_.load(std::memory_order_relaxed)) return;
    n = std::min(n, (int)tmpL_.size());
    reader_.read(tmpL_.data(), tmpR_.data(), n);  // silence while (re)buffering
    float g = gainSm_;
    for (int i = 0; i < n; ++i) {
        g += 0.002f * (gain - g);
        L[i] += g * tmpL_[i];
        R[i] += g * tmpR_[i];
    }
    gainSm_ = g;
}

}  // namespace gs
