#pragma once
// Asynchronous native file dialog (kdialog on KDE, zenity elsewhere) that does not block the UI.
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace gs {

class FileDialog {
public:
    enum Kind { None = 0, Song, IR, Folder };
    ~FileDialog();
    bool busy() const { return st_->busy.load(); }
    void open(Kind kind, const std::string& title, const std::string& patterns, const std::string& startDir);
    // Poll for the result (every frame). Returns true when the dialog closed (empty path = cancelled)
    bool poll(Kind& kind, std::string& path);
    static bool available();

private:
    struct State {  // shared with the worker thread (which may outlive this object)
        std::atomic<bool> busy{false};
        std::mutex mu;
        bool done = false;
        Kind kind = None;
        std::string result;
    };
    std::shared_ptr<State> st_ = std::make_shared<State>();
    std::thread th_;
};

}  // namespace gs
