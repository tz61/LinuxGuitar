// Guitar Studio: low-latency guitar effects + practice (Linux, direct ALSA).
#include <SDL.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "audio/PulseBridge.h"
#include "common/I18n.h"
#include "common/Png.h"
#include "ui/App.h"
#include "ui/Settings.h"
#include "ui/Theme.h"

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif

namespace gs {
// packaging/icons/hicolor/128x128/apps/guitarstudio.png, compiled in by CMake (gs_app_icon.cpp)
extern const unsigned char kAppIconPng[];
extern const unsigned kAppIconPngSize;
}  // namespace gs

namespace {

volatile sig_atomic_t g_quit = 0;

void onQuitSignal(int) { g_quit = 1; }  // Ctrl+C / kill: take the normal exit path (restores the sound card and system audio)

void onCrashSignal(int sig) {
    gs::PulseBridge::emergencyRestore();  // try to put the system audio back
    signal(sig, SIG_DFL);
    raise(sig);
}

// Only one instance may own the sound card. Returns true = got the lock (released automatically on exit)
bool acquireInstanceLock() {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    std::string path = (rt && *rt) ? std::string(rt) + "/guitarstudio.lock" : "/tmp/guitarstudio-" + std::to_string(getuid()) + ".lock";
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return true;  // no lock file possible: don't restrict
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return false;
    }
    return true;  // fd intentionally left open
}

void usage() {
    std::printf(
        "Usage: guitarstudio [options] [song file]\n"
        "  --simulate            no-sound-card mode (UI and practice work, no audio)\n"
        "  --demo                demo: a simulated player plays the current song (use with --simulate)\n"
        "  --device hw:X,Y       ALSA device to use (default hw:PCH,0)\n"
        "  --preset NAME         load a preset at startup\n"
        "  --tab 0..4            initial page (0 effects, 1 practice, 2 tuner, 3 looper, 4 settings)\n"
        "  --play                start playing right after opening\n"
        "  --screenshot FILE.png render, save a screenshot and exit (for testing)\n"
        "  --wait SECONDS        how long to run before the screenshot (default 3)\n"
        "  --size WxH            window size\n"
        "  --scale 1.5           UI scale\n"
        "  --lang en|zh          UI language (default: saved setting, English on first run)\n"
        "  --strict              abort on ImGui usage errors (for development)\n");
}

// Window icon for the title bar and the task switcher (the menu and desktop use the installed icon files)
bool setWindowIcon(SDL_Window* window) {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
    if (!gs::decodePng(gs::kAppIconPng, gs::kAppIconPngSize, w, h, rgba)) return false;
    SDL_Surface* icon = SDL_CreateRGBSurfaceWithFormatFrom(rgba.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
    if (!icon) return false;
    SDL_SetWindowIcon(window, icon);
    SDL_FreeSurface(icon);
    return true;
}

// Offscreen framebuffer (the window is hidden in screenshot mode)
struct Fbo {
    typedef void (*GenFB)(GLsizei, GLuint*);
    typedef void (*BindFB)(GLenum, GLuint);
    typedef void (*FBTex)(GLenum, GLenum, GLenum, GLuint, GLint);
    GenFB genFramebuffers = nullptr;
    BindFB bindFramebuffer = nullptr;
    FBTex framebufferTexture2D = nullptr;
    GLuint fbo = 0, tex = 0;
    int w = 0, h = 0;
    bool init(int W, int H) {
        genFramebuffers = (GenFB)SDL_GL_GetProcAddress("glGenFramebuffers");
        bindFramebuffer = (BindFB)SDL_GL_GetProcAddress("glBindFramebuffer");
        framebufferTexture2D = (FBTex)SDL_GL_GetProcAddress("glFramebufferTexture2D");
        if (!genFramebuffers || !bindFramebuffer || !framebufferTexture2D) return false;
        w = W;
        h = H;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        genFramebuffers(1, &fbo);
        bindFramebuffer(GL_FRAMEBUFFER, fbo);
        framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        return true;
    }
};

}  // namespace

int main(int argc, char** argv) {
    // Translation catalogs are registered once, before any other thread starts
    gs::registerUiTranslations();
    gs::registerFxTranslations();
    gs::registerSongTranslations();
    gs::registerAudioTranslations();
    gs::registerPracticeTranslations();

    gs::AppOptions opt;
    std::string shot;
    double shotWait = 3.0;
    int winW = 0, winH = 0;
    float scaleOverride = 0.f;
    int langOverride = -1;
    bool strict = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--simulate") opt.simulate = true;
        else if (a == "--demo") opt.demo = true;
        else if (a == "--device") opt.device = next();
        else if (a == "--preset") opt.preset = next();
        else if (a == "--tab") opt.tab = std::atoi(next().c_str());
        else if (a == "--play") opt.autoplay = true;
        else if (a == "--screenshot") shot = next();
        else if (a == "--wait") shotWait = std::atof(next().c_str());
        else if (a == "--size") std::sscanf(next().c_str(), "%dx%d", &winW, &winH);
        else if (a == "--scale") scaleOverride = (float)std::atof(next().c_str());
        else if (a == "--lang") {
            std::string l = next();
            langOverride = (l == "zh" || l == "cn" || l == "zh_CN") ? 1 : 0;
        } else if (a == "--strict") strict = true;
        else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (!a.empty() && a[0] != '-') opt.song = a;
        else {
            std::fprintf(stderr, "Unknown option: %s\n", a.c_str());
            usage();
            return 1;
        }
    }

    // The saved settings decide the language (needed before the first message) and the window geometry
    gs::Settings saved;
    saved.load(gs::configDir() + "/settings.ini");
    if (langOverride >= 0) saved.language = langOverride;
    gs::setLanguage((gs::Lang)saved.language);

    // Normal mode allows only one instance (it owns the sound card); no-sound-card (test) mode is not
    // restricted and never touches PulseAudio
    if (!opt.simulate && !acquireInstanceLock()) {
        std::string msg = gs::tr("Guitar Studio is already running.\nIt needs exclusive access to the sound card, so only one copy can be open at a time.");
        if (!opt.song.empty())  // e.g. "Open with" in the file manager
            msg += std::string("\n\n") + gs::tr("To open this song, drag the file into the open Guitar Studio window.");
        std::fprintf(stderr, "%s\n", msg.c_str());
        if (shot.empty() && SDL_Init(SDL_INIT_VIDEO) == 0) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Guitar Studio", msg.c_str(), nullptr);
            SDL_Quit();
        }
        return 0;
    }
    struct sigaction qa{};
    qa.sa_handler = onQuitSignal;
    sigemptyset(&qa.sa_mask);
    sigaction(SIGINT, &qa, nullptr);
    sigaction(SIGTERM, &qa, nullptr);
    sigaction(SIGHUP, &qa, nullptr);
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) signal(sig, onCrashSignal);

    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {  // no SDL audio: we drive the sound card ourselves
        std::fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    // Window size: the last size; on first run 85% of the screen
    SDL_Rect usable{0, 0, 1920, 1080};
    SDL_GetDisplayUsableBounds(0, &usable);
    if (winW <= 0 || winH <= 0) {
        winW = saved.winW > 400 ? saved.winW : std::max(1280, (int)(usable.w * 0.85f));
        winH = saved.winH > 300 ? saved.winH : std::max(800, (int)(usable.h * 0.85f));
        winW = std::min(winW, usable.w);
        winH = std::min(winH, usable.h);
    }
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (!shot.empty()) flags |= SDL_WINDOW_HIDDEN;
    else if (saved.winMax) flags |= SDL_WINDOW_MAXIMIZED;
    const char* kTitle = "Guitar Studio \xe2\x80\x94 Guitar Effects & Practice";
    SDL_Window* window = SDL_CreateWindow(gs::tr(kTitle), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW, winH, flags);
    if (!window) {
        std::fprintf(stderr, "Cannot create window: %s\n", SDL_GetError());
        return 1;
    }
    if (!setWindowIcon(window)) std::fprintf(stderr, "Cannot set the window icon\n");
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // ImGui usage errors in UI code: by default only report them instead of crashing (--strict aborts, for debugging)
    io.ConfigErrorRecovery = true;
    io.ConfigErrorRecoveryEnableAssert = strict;
    static std::string iniPath = gs::configDir() + "/imgui.ini";
    gs::ensureDir(gs::configDir());
    io.IniFilename = iniPath.c_str();
    ImGui_ImplSDL2_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init("#version 330");

    opt.dpiScale = ImGui_ImplSDL2_GetContentScaleForWindow(window);
    gs::App app(opt);
    app.init();
    if (scaleOverride > 0.f) app.setUiScale(scaleOverride);
    if (langOverride >= 0) app.setLanguage(langOverride);
    gs::Lang titleLang = gs::language();
    SDL_SetWindowTitle(window, gs::tr(kTitle));

    Fbo fbo;
    if (!shot.empty() && !fbo.init(winW, winH)) {
        std::fprintf(stderr, "Cannot create the offscreen framebuffer\n");
        shot.clear();
    }
    const auto t0 = std::chrono::steady_clock::now();
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE && ev.window.windowID == SDL_GetWindowID(window))
                running = false;
            if (ev.type == SDL_DROPFILE && ev.drop.file) {
                app.onDropFile(ev.drop.file);
                SDL_free(ev.drop.file);
            }
        }
        if (app.takeScaleChange()) gs::ui::applyTheme(app.uiScale());
        if (gs::language() != titleLang) {
            titleLang = gs::language();
            SDL_SetWindowTitle(window, gs::tr(kTitle));
        }
        // Lower the frame rate while minimized to save power (audio is unaffected)
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(50);
            continue;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        if (!shot.empty()) io.DisplaySize = ImVec2((float)winW, (float)winH);
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        if (!shot.empty()) fbo.bindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);
        glViewport(0, 0, shot.empty() ? (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x) : winW,
                   shot.empty() ? (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y) : winH);
        glClearColor(0.118f, 0.129f, 0.149f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (!shot.empty()) {
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (el >= shotWait) {
                std::vector<unsigned char> px((size_t)winW * winH * 4);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, winW, winH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                bool ok = gs::writePng(shot, winW, winH, px.data(), true);
                std::printf(ok ? "Screenshot saved: %s\n" : "Failed to save screenshot: %s\n", shot.c_str());
                running = false;
            }
            SDL_Delay(8);
        } else {
            SDL_GL_SwapWindow(window);
        }
        if (app.quitRequested() || g_quit) running = false;
    }

    if (shot.empty()) {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        app.setWindowState(w, h, (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0);
    }
    app.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
