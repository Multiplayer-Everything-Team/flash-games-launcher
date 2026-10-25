// Private to the launcher window's source files (launcher*.cpp): the window
// messages, control ids and small helpers they share. Not a public interface.
#pragma once
#include <algorithm>
#include <string>
#include <vector>

#include "core/library.h"
#include "theme.h"

namespace fl::ui {

// How a PC mod install / update / repair / uninstall ended.
struct ModDone {
    int game = -1;
    std::string op;
    bool ok = false;
    std::wstring message, detail, version;
    bool rollbackComplete = true;
};

namespace detail {

using namespace Gdiplus;

constexpr wchar_t kClass[] = L"MultiplayerEverythingLauncher";
constexpr UINT WM_APP_EXIT = WM_APP + 1;   // wParam pid, lParam exit code
constexpr UINT WM_APP_RELAY = WM_APP + 2;  // relay peers changed
constexpr UINT WM_APP_INSTALL = WM_APP + 3;   // player install progress
constexpr UINT WM_APP_INSTALLED = WM_APP + 4; // player install finished
constexpr UINT WM_APP_FIRSTRUN = WM_APP + 5;  // offer to install a player
constexpr UINT WM_APP_CATALOG = WM_APP + 6;   // fresh game catalog downloaded
constexpr UINT WM_APP_ROOMS = WM_APP + 7;     // fresh room list from the central server
constexpr UINT WM_APP_SERVERLINE = WM_APP + 8;  // an HTML5 server printed a line (lParam: ServerLine*)
constexpr UINT WM_APP_SERVERREADY = WM_APP + 9; // wParam session id, lParam 1 = answering, 0 = gave up
constexpr UINT WM_APP_MODS = WM_APP + 11;        // mods-info answered for the PC mods
constexpr UINT WM_APP_MODPROGRESS = WM_APP + 12; // a mod install/update moved on
constexpr UINT WM_APP_MODDONE = WM_APP + 13;     // a mod job ended; lParam: ModDone*
constexpr UINT WM_APP_UPDATED = WM_APP + 10;    // a newer launcher is in place; lParam: std::wstring* version

struct ServerLine {
    int session;
    std::wstring text;
};

// A fixed port per game keeps HTML5 saves: browsers file storage by address.
inline int stablePort(const std::string& gameId) {
    unsigned h = 2166136261u;
    for (char ch : gameId) h = (h ^ static_cast<unsigned char>(ch)) * 16777619u;
    return 47000 + static_cast<int>(h % 2000);
}
constexpr UINT_PTR kTimer = 1;
constexpr UINT_PTR kFrameTimer = 2;
// About 30 frames a second: enough for a slow breath and a short decode, and
// half the repaints of 60.
constexpr UINT kFrameMs = 33;
constexpr ULONGLONG kTitleMs = 380, kCodeMs = 500, kJoinMs = 450, kIntroMs = 830;

// Milliseconds since `from`, or "long ago" when it never started or is done.
inline ULONGLONG since(ULONGLONG from) { return from ? GetTickCount64() - from : ~0ull; }

inline bool systemAnimations() {
    // FL_MOTION=1 or 0 overrides Windows' setting, for tests and screenshots.
    wchar_t forced[4] = L"";
    if (GetEnvironmentVariableW(L"FL_MOTION", forced, 4)) return forced[0] == L'1';
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return on != FALSE;
}

enum : int {
    kIdNav = 100,
    kIdNavCombo,
    kIdSearch,
    kIdGrid,
    kIdPlayer,
    kIdFav = 110,
    kIdLocate,
    kIdFolder,
    kIdGetPlayer,
    kIdStopHost,
    kIdCopy,
    kIdDownload,
    kIdSettings,
    kIdTabPlay,
    kIdTabRooms,
    kIdMore,
    kIdQuick,
    kIdTabCode,
    kIdTabCodeJoin,
    kIdPageGame,
    kIdPageHideFull,
    kIdPageCode,
    kIdPageCodeJoin,
    kIdAction0 = 200,
    kIdRoom0 = 300,      // details panel Rooms tab rows
    kIdPageRoom0 = 400,  // Rooms page rows
    kIdPageRoomEnd = 500,
};

// Icon-font glyphs (Segoe Fluent Icons / MDL2 Assets) for the window's own items.
constexpr wchar_t kIconAll[] = L"\uE8A9", kIconFav[] = L"\uE734", kIconReady[] = L"\uE768", kIconRooms[] = L"\uE902",
                  kIconSettings[] = L"\uE713", kIconMore[] = L"\uE70D", kIconLess[] = L"\uE70E", kIconQuick[] = L"\uE945";
constexpr wchar_t kIconInstall[] = L"\uE896", kIconUpdate[] = L"\uE777", kIconRepair[] = L"\uE90F",
                  kIconUninstall[] = L"\uE74D";
constexpr wchar_t kToastClass[] = L"MultiplayerEverythingLauncherToast";
constexpr ULONGLONG kToastMs = 4000;

// 96-DPI layout units.
constexpr int kSidebarW = 228, kDetailW = 360, kDetailNarrowW = 300, kTopH = 68, kStatusH = 32, kPad = 20;
constexpr int kCardMinW = 196, kGap = 16, kNarrowBelow = 900;

class Buffer {
public:
    Buffer(HDC ref, int w, int h) {
        dc = CreateCompatibleDC(ref);
        bmp = CreateCompatibleBitmap(ref, std::max(w, 1), std::max(h, 1));
        old = SelectObject(dc, bmp);
    }
    ~Buffer() {
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    HDC dc;
    HBITMAP bmp;
    HGDIOBJ old;
};

inline RectF rf(const RECT& r) {
    return RectF(static_cast<REAL>(r.left), static_cast<REAL>(r.top), static_cast<REAL>(r.right - r.left),
                 static_cast<REAL>(r.bottom - r.top));
}

inline int width(const RECT& r) { return r.right - r.left; }
inline int height(const RECT& r) { return r.bottom - r.top; }

inline bool pngEncoder(CLSID& out) {
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    if (!size) return false;
    std::vector<uint8_t> buf(size);
    auto* codecs = reinterpret_cast<ImageCodecInfo*>(buf.data());
    GetImageEncoders(n, size, codecs);
    for (UINT i = 0; i < n; ++i)
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            out = codecs[i].Clsid;
            return true;
        }
    return false;
}

inline std::wstring windowText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    if (n) GetWindowTextW(h, s.data(), n + 1);
    return s;
}

using FlashVars = std::vector<std::pair<std::wstring, std::wstring>>;

// The action's own values first (online mode, your name), then this launch's
// (a room code). Empty values are left out.
inline FlashVars gameValues(const ActionSpec& a, const std::wstring& playerName, const FlashVars& given) {
    FlashVars out;
    for (const auto& [key, value] : a.flashvars) {
        std::wstring v = replaceAll(value, L"${PLAYER_NAME}", playerName);
        if (!v.empty()) out.emplace_back(key, v);
    }
    out.insert(out.end(), given.begin(), given.end());
    return out;
}

// Values for the game go in before the game file, the way the player expects
// options; players without flashvarArg can't take them.
inline void addFlashVars(ResolvedAction& r, const FlashVars& vars) {
    if (vars.empty() || !r.player || r.player->flashvarArg.empty()) return;
    auto at = std::find(r.args.rbegin(), r.args.rend(), r.swf);
    auto pos = at == r.args.rend() ? r.args.end() : std::prev(at.base());
    std::vector<std::wstring> extra;
    for (const auto& [key, value] : vars)
        extra.push_back(replaceAll(replaceAll(r.player->flashvarArg, L"${KEY}", key), L"${VALUE}", value));
    r.args.insert(pos, extra.begin(), extra.end());
}

inline std::wstring stateReason(const Config& cfg, const ResolvedAction& r) {
    if (r.state == ActionState::NoRuntime) return cfg.t("status.noRuntime", {{L"RUNTIME", r.action->server.name}});
    if (r.state == ActionState::NoSwf) return cfg.t("status.noSwf");
    if (r.state == ActionState::NoPlayer) return cfg.t("status.noPlayer");
    return cfg.t("status.ready");
}

}  // namespace detail
}  // namespace fl::ui
