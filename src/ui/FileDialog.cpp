#include "ui/FileDialog.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include "audio/PulseBridge.h"  // runProgram
#include "common/I18n.h"

namespace gs {

static bool haveProgram(const char* name) {
    std::string out;
    return runProgram({"sh", "-c", std::string("command -v ") + name}, &out, 2000) == 0 && !out.empty();
}

bool FileDialog::available() { return haveProgram("kdialog") || haveProgram("zenity"); }

FileDialog::~FileDialog() {
    if (th_.joinable()) th_.detach();  // the dialog may still be open: don't wait (the thread only touches shared state)
}

void FileDialog::open(Kind kind, const std::string& title, const std::string& patterns, const std::string& startDir) {
    if (st_->busy.exchange(true)) return;
    if (th_.joinable()) th_.join();
    {
        std::lock_guard<std::mutex> lk(st_->mu);
        st_->done = false;
        st_->kind = kind;
        st_->result.clear();
    }
    std::shared_ptr<State> st = st_;
    const std::string allFiles = tr("All files");
    th_ = std::thread([st, kind, title, patterns, startDir, allFiles] {
        const char* de = getenv("XDG_CURRENT_DESKTOP");
        bool kde = de && strstr(de, "KDE");
        bool useKde = (kde && haveProgram("kdialog")) || !haveProgram("zenity");
        std::vector<std::string> argv;
        if (useKde) {
            argv = {"kdialog", "--title", title};
            if (kind == Folder) {
                argv.push_back("--getexistingdirectory");
                argv.push_back(startDir);
            } else {
                argv.push_back("--getopenfilename");
                argv.push_back(startDir);
                argv.push_back(patterns);
            }
        } else {
            argv = {"zenity", "--file-selection", "--title=" + title};
            if (kind == Folder) argv.push_back("--directory");
            else {
                argv.push_back("--file-filter=" + title + " | " + patterns);
                argv.push_back("--file-filter=" + allFiles + " | *");
            }
            if (!startDir.empty()) argv.push_back("--filename=" + startDir + "/");
        }
        std::string out;
        int rc = runProgram(argv, &out, 30 * 60 * 1000);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        std::lock_guard<std::mutex> lk(st->mu);
        st->result = rc == 0 ? out : std::string();
        st->done = true;
        st->busy.store(false);
    });
}

bool FileDialog::poll(Kind& kind, std::string& path) {
    std::lock_guard<std::mutex> lk(st_->mu);
    if (!st_->done) return false;
    st_->done = false;
    kind = st_->kind;
    path = st_->result;
    return true;
}

}  // namespace gs
