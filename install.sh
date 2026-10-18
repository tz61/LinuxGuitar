#!/usr/bin/env bash
# Builds Guitar Studio and installs it, with launchers in the application menu and on the desktop.
#   ./install.sh                     install for the current user into ~/.local (no sudo needed)
#   ./install.sh --system            install for all users into /usr/local (asks for sudo)
#   ./install.sh --prefix DIR        install into DIR
#   ./install.sh --no-desktop-icon   don't put a launcher on the desktop
#   ./install.sh --uninstall         remove it again (give the same --system / --prefix as for installing);
#                                    settings, imported songs and scores are kept
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="${ROOT}/build"
PREFIX="${HOME}/.local"
DESKTOP_ICON=1
UNINSTALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --system) PREFIX=/usr/local ;;
        --prefix)
            PREFIX="${2:?--prefix needs a directory}"
            shift
            ;;
        --no-desktop-icon) DESKTOP_ICON=0 ;;
        --uninstall) UNINSTALL=1 ;;
        -h | --help)
            sed -n '2,8p' "$0" | cut -c3-
            exit 0
            ;;
        *)
            echo "Unknown option: $1 (see ./install.sh --help)" >&2
            exit 1
            ;;
    esac
    shift
done
if [ "$(id -u)" -eq 0 ]; then
    echo "Please run this without sudo: it builds as you and asks for sudo itself when needed (--system)." >&2
    exit 1
fi
PREFIX="$(realpath -m "${PREFIX}")"

# sudo only when the prefix is not writable for the current user (e.g. /usr/local)
probe="${PREFIX}/share/applications"
while [ ! -e "${probe}" ]; do probe="$(dirname "${probe}")"; done
SUDO=""
if [ ! -w "${probe}" ]; then SUDO="sudo"; fi

# What "cmake --install" puts into the prefix (keep in sync with the install() rules in CMakeLists.txt)
FILES=(bin/guitarstudio
    share/applications/guitarstudio.desktop
    share/mime/packages/guitarstudio.xml)
while IFS= read -r icon; do
    FILES+=("share/icons/hicolor/${icon}")
done < <(cd "${ROOT}/packaging/icons/hicolor" && find . -type f -printf '%P\n')
LESSONS=share/guitarstudio/lessons

DESKTOP_DIR="$(xdg-user-dir DESKTOP 2> /dev/null || true)"
SHORTCUT=""
if [ -n "${DESKTOP_DIR}" ] && [ "${DESKTOP_DIR}" != "${HOME}" ] && [ -d "${DESKTOP_DIR}" ]; then
    SHORTCUT="${DESKTOP_DIR}/guitarstudio.desktop"
fi

refresh_caches() {
    ${SUDO} update-desktop-database -q "${PREFIX}/share/applications" 2> /dev/null || true
    ${SUDO} update-mime-database "${PREFIX}/share/mime" > /dev/null 2>&1 || true
    if [ -f "${PREFIX}/share/icons/hicolor/icon-theme.cache" ]; then
        ${SUDO} gtk-update-icon-cache -q -t -f "${PREFIX}/share/icons/hicolor" 2> /dev/null || true
    fi
    # Running KDE programs (desktop, menu, task bar) only look into icon folders that existed when they started;
    # this makes them look again, as after changing the icon theme
    if command -v dbus-send > /dev/null; then
        dbus-send --session --type=signal /KIconLoader org.kde.KIconLoader.iconChanged int32:0 2> /dev/null || true
    fi
    # KDE notices menu changes by itself after a while; this makes it immediate
    if command -v kbuildsycoca6 > /dev/null; then
        kbuildsycoca6 > /dev/null 2>&1 || true
    elif command -v kbuildsycoca5 > /dev/null; then
        kbuildsycoca5 > /dev/null 2>&1 || true
    fi
}

install_app() {
    echo ">> Building Guitar Studio (Release) ..."
    [ -f "${ROOT}/third_party/imgui/imgui.h" ] || "${ROOT}/scripts/fetch_deps.sh"
    cmake -S "${ROOT}" -B "${BUILD}" -DCMAKE_BUILD_TYPE=Release > /dev/null
    cmake --build "${BUILD}" --target guitarstudio -j"$(nproc)"

    echo ">> Installing into ${PREFIX}${SUDO:+ (with sudo)} ..."
    # Staged as the current user first, so that sudo never writes into the build directory
    STAGE="$(mktemp -d)"
    trap 'rm -rf "${STAGE}"' EXIT
    DESTDIR="${STAGE}" cmake --install "${BUILD}" --prefix "${PREFIX}" > /dev/null
    ${SUDO} mkdir -p "${PREFIX}"
    ${SUDO} rm -rf "${PREFIX:?}/${LESSONS}"  # so that lessons removed from the source don't linger
    # --remove-destination: the program can be replaced even while it is running
    ${SUDO} cp -r --remove-destination "${STAGE}${PREFIX}/." "${PREFIX}/"
    refresh_caches

    local on_desktop=""
    if [ "${DESKTOP_ICON}" = 1 ] && [ -n "${SHORTCUT}" ]; then
        install -m 755 "${PREFIX}/share/applications/guitarstudio.desktop" "${SHORTCUT}"
        # GNOME's desktop also wants the launcher marked as trusted; KDE only needs it executable
        gio set "${SHORTCUT}" metadata::trusted true 2> /dev/null || true
        on_desktop=" or the icon on the desktop"
    fi

    echo ">> Done. Start \"Guitar Studio\" from the application menu (Multimedia)${on_desktop}."
    case ":${PATH}:" in
        *":${PREFIX}/bin:"*) echo "   In a terminal: guitarstudio [song file]" ;;
        *) echo "   In a terminal: ${PREFIX}/bin/guitarstudio [song file]" ;;
    esac
    echo "   Song files (.gtab, Guitar Pro, MusicXML, MIDI) can also be opened with it from the file manager."
    local lock="${XDG_RUNTIME_DIR:-/tmp}/guitarstudio.lock"
    [ -n "${XDG_RUNTIME_DIR:-}" ] || lock="/tmp/guitarstudio-$(id -u).lock"
    if [ -e "${lock}" ] && ! flock -n "${lock}" true; then
        echo "   Guitar Studio is running right now: restart it to use the new version."
    fi
    if [ "${PREFIX}" != "${HOME}/.local" ] && [ -f "${HOME}/.local/share/applications/guitarstudio.desktop" ]; then
        echo "   Note: an earlier per-user install in ~/.local hides this one in your menu; remove it with ./install.sh --uninstall"
    fi
}

uninstall_app() {
    echo ">> Removing Guitar Studio from ${PREFIX}${SUDO:+ (with sudo)} ..."
    for f in "${FILES[@]}"; do ${SUDO} rm -f "${PREFIX}/${f}"; done
    ${SUDO} rm -rf "${PREFIX:?}/${LESSONS}"
    # Only removed when empty: a per-user install shares this folder with your imported songs and scores
    ${SUDO} rmdir "${PREFIX}/share/guitarstudio" 2> /dev/null || true
    if [ -n "${SHORTCUT}" ] && [ -f "${SHORTCUT}" ] && grep -qF "Exec=${PREFIX}/bin/guitarstudio" "${SHORTCUT}"; then
        rm -f "${SHORTCUT}"
    fi
    refresh_caches
    echo ">> Done. Kept your settings (~/.config/guitarstudio) and your imported songs and scores (~/.local/share/guitarstudio)."
}

if [ "${UNINSTALL}" = 1 ]; then uninstall_app; else install_app; fi
