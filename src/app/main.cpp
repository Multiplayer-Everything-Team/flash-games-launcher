// Entry point: loads launcher.json (--config or next to the exe), then opens
// the launcher window or runs one of the command-line modes (--list,
// --launch, --relay, --validate, --snapshot).
#include "app/cli.h"
#include "core/config.h"
#include "core/embedded.h"
#include "core/fs.h"
#include "core/install.h"
#include "core/library.h"
#include "core/log.h"
#include "core/relay.h"
#include "core/rooms.h"
#include "core/settings.h"
#include "core/strings.h"
#include "ui/launcher.h"

#include <commctrl.h>
#include <shellapi.h>

using namespace fl;

namespace {

bool g_console = false;
HANDLE g_stop = nullptr;

void setupConsole() {
    g_console = true;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return;  // redirected
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
        SetStdHandle(STD_OUTPUT_HANDLE, con);
        SetStdHandle(STD_ERROR_HANDLE, con);
    }
}

void write(DWORD which, const std::wstring& s) {
    HANDLE h = GetStdHandle(which);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0, n = 0;
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, s.c_str(), static_cast<DWORD>(s.size()), &n, nullptr);
    } else {
        std::string u = narrow(replaceAll(s, L"\n", L"\r\n"));
        WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &n, nullptr);
    }
}

void out(const std::wstring& s) { write(STD_OUTPUT_HANDLE, s); }

void fail(const std::wstring& msg) {
    logger().error(msg);
    if (g_console) write(STD_ERROR_HANDLE, msg + L"\n");
    else MessageBoxW(nullptr, msg.c_str(), L"Flash Launcher", MB_OK | MB_ICONERROR);
}

std::wstring fullPath(const std::wstring& p) {
    if (p.empty()) return p;
    wchar_t buf[32768];
    DWORD n = GetFullPathNameW(fs::normalize(p).c_str(), 32768, buf, nullptr);
    return n ? fs::normalize(buf) : fs::normalize(p);
}

BOOL WINAPI ctrlHandler(DWORD type) {
    if ((type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) && g_stop) {
        SetEvent(g_stop);
        return TRUE;
    }
    return FALSE;
}

int runRelay(int port) {
    Relay relay;
    std::wstring err;
    if (!relay.start(port, err)) {
        fail(L"Cannot start the relay on port " + std::to_wstring(port) + L": " + err);
        return 1;
    }
    out(L"Relay listening on port " + std::to_wstring(port) + L". Press Ctrl+C to stop.\n");
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    size_t lastPeers = static_cast<size_t>(-1);
    while (WaitForSingleObject(g_stop, 500) == WAIT_TIMEOUT) {
        RelaySnapshot s = relay.snapshot();
        if (s.peers.size() == lastPeers) continue;
        lastPeers = s.peers.size();
        std::wstring line = std::to_wstring(s.peers.size()) + L" connected";
        for (const auto& p : s.peers) line += L"  [" + p.name + (p.host ? L" (host)" : L"") + L" " + p.address + L"]";
        out(line + L"\n");
    }
    relay.stop();
    return 0;
}

const wchar_t* stateWord(ActionState s) {
    switch (s) {
        case ActionState::Ready: return L"ready";
        case ActionState::NoPlayer: return L"no player";
        case ActionState::NoSwf: return L"game file missing";
        case ActionState::NoRuntime: return L"server program missing";
    }
    return L"?";
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    Args a = parseArgs(argc, argv);
    LocalFree(argv);
    bool cliMode = a.help || a.version || a.list || a.validate || !a.launch.empty() || a.relayPort || !a.installPlayer.empty() ||
                   !a.downloadGame.empty() || !a.rooms.empty() || !a.error.empty();
    if (cliMode) setupConsole();
    if (!a.error.empty()) {
        fail(a.error + L"\n\n" + usageText());
        return 3;
    }
    if (a.help) {
        out(usageText());
        return 0;
    }
    if (a.version) {
        out(std::wstring(L"Multiplayer Everything Flash Launcher ") + LAUNCHER_VERSION + L"\n");
        return 0;
    }

    std::wstring exeDir = fs::parent(fs::selfExePath());
    std::wstring configPath = a.config.empty() ? fs::join(exeDir, L"launcher.json") : fullPath(a.config);
    if (a.relayPort && !fs::isFile(configPath)) return runRelay(a.relayPort);  // a bare relay needs no config

    // No launcher.json next to the exe: use the one built into it.
    bool builtIn = false;
    std::string embeddedConfig;
    if (a.config.empty() && !fs::isFile(configPath) && readEmbedded(L"launcher.json", embeddedConfig)) builtIn = true;
    LoadResult lr = builtIn ? loadConfig(embeddedConfig, exeDir) : loadConfigFile(configPath);
    if (a.validate) {
        out(formatIssues(lr.issues));
        out(lr.ok() ? L"OK\n" : L"Configuration has errors.\n");
        return lr.ok() ? 0 : 3;
    }
    if (!lr.ok()) {
        fail(L"launcher.json has problems:\n\n" + formatIssues(lr.issues) + L"\n" + configPath);
        return 3;
    }
    const Config& cfg = lr.config;
    Vars base;
    base.set(L"LAUNCHER_DIR", cfg.configDir);
    std::wstring dataDir = fs::normalize(base.expand(cfg.paths.data));
    logger().open(a.log.empty() ? fs::join(dataDir, L"launcher.log") : fullPath(a.log));
    logger().setEcho(!a.launch.empty() || a.relayPort);  // --list and --validate print only their own output
    logger().info(L"Config: " + (builtIn ? std::wstring(L"built into the exe") : configPath));
    for (const auto& i : lr.issues) logger().warn(i.where + L": " + i.message);

    if (a.relayPort) return runRelay(a.relayPort);

    Library lib(cfg, base);
    std::wstring userPath = fs::join(lib.dataDir(), L"settings.json");
    UserData user = loadUserData(userPath);
    if (!user.gamesDir.empty()) {
        lib.setGamesDirOverride(user.gamesDir);
        lib.refresh();
    }

    if (!a.rooms.empty()) {
        const GameSpec* g = cfg.findGame(a.rooms);
        if (!g || g->onlineId.empty()) {
            fail(L"No online game with id \"" + widen(a.rooms) + L"\"");
            return 3;
        }
        std::wstring err;
        std::wstring server = resolveRoomServer(cfg.rooms.servers, cfg.rooms.server, &err);
        std::vector<RoomInfo> rooms;
        if (server.empty() || !listRooms(server, g->onlineId, rooms, err)) {
            fail(L"Cannot list rooms: " + err);
            return 1;
        }
        out(server + L": " + std::to_wstring(rooms.size()) + L" open\n");
        for (const auto& r : rooms)
            out(r.code + L"  " + std::to_wstring(r.players) + L"/" + std::to_wstring(r.max) + L"  " + r.name + L"\n");
        return 0;
    }

    if (!a.installPlayer.empty()) {
        const PlayerSpec* p = cfg.findPlayer(a.installPlayer);
        if (!p || !p->install.enabled()) {
            fail(L"No installable player with id \"" + widen(a.installPlayer) + L"\" (it needs an \"install\" entry)");
            return 3;
        }
        int lastPct = -1;
        InstallResult r = installPlayer(*p, lib.vars(), [&](const InstallStatus& s) {
            if (s.stage != InstallStatus::Stage::Downloading || !s.total) return;
            int pct = static_cast<int>(s.done * 100 / s.total);
            if (pct / 10 != lastPct / 10) out(std::to_wstring(pct) + L"%\n");
            lastPct = pct;
        }, nullptr);
        if (!r.ok) {
            fail(cfg.t("install.failed", {{L"PLAYER", p->name}, {L"ERROR", r.error}}));
            return 1;
        }
        out(cfg.t("install.done", {{L"PLAYER", r.version.empty() ? p->name : p->name + L" " + r.version}}) + L"\n" + r.exe + L"\n");
        return 0;
    }

    if (!a.downloadGame.empty()) {
        if (cfg.downloads.catalog.empty()) {
            fail(L"launcher.json has no downloads.catalog");
            return 3;
        }
        std::vector<CatalogFile> files, wanted;
        std::string raw;
        std::wstring err;
        if (!fetchCatalog(cfg.downloads.catalog, files, raw, err)) {
            fail(L"Cannot read the game catalog: " + err);
            return 1;
        }
        for (const auto& f : files)
            if (a.downloadGame == "all" || f.game == a.downloadGame) wanted.push_back(f);
        if (wanted.empty()) {
            fail(L"The catalog has no files for \"" + widen(a.downloadGame) + L"\"");
            return 3;
        }
        InstallResult r = downloadGameFiles(wanted, lib.vars().get(L"GAMES_DIR"), nullptr, nullptr);
        if (!r.ok) {
            fail(L"Download failed: " + r.error);
            return 1;
        }
        for (const auto& f : wanted) out(fs::join(lib.vars().get(L"GAMES_DIR"), f.name) + L"\n");
        return 0;
    }

    if (a.list) {
        for (const auto& p : lib.players())
            out(p.spec->name + L": " + (p.found() ? p.exe : std::wstring(L"not found")) + L"\n");
        out(L"Flashpoint: " + (lib.flashpointDir().empty() ? std::wstring(L"not found") : lib.flashpointDir()) + L"\n\n");
        for (const auto& g : cfg.games) {
            out(widen(g.id) + L"  " + g.name + L"\n");
            for (const auto& act : g.actions) {
                ResolvedAction r = lib.resolve(g, act, user);
                out(L"    " + widen(act.id) + L" (" + act.label + L"): " + stateWord(r.state) +
                    (r.player ? L", " + r.player->name : L"") + (r.swf.empty() ? L"" : L", " + r.swf) + L"\n");
            }
        }
        return 0;
    }

    if (!a.launch.empty()) {
        std::wstring gameId = a.launch, actionId;
        if (size_t colon = a.launch.find(L':'); colon != std::wstring::npos) {
            gameId = a.launch.substr(0, colon);
            actionId = a.launch.substr(colon + 1);
        }
        const GameSpec* g = cfg.findGame(narrow(gameId));
        if (!g) {
            fail(L"No game with id \"" + gameId + L"\" (see --list)");
            return 3;
        }
        const ActionSpec* act = g->actions.empty() ? nullptr : &g->actions.front();
        if (!actionId.empty()) {
            act = nullptr;
            for (const auto& x : g->actions)
                if (x.id == narrow(actionId)) act = &x;
        }
        if (!act) {
            fail(L"\"" + g->name + L"\" has no action \"" + actionId + L"\"");
            return 3;
        }
        std::wstring message;
        int code = ui::Launcher::launchHeadless(cfg, lib, user, userPath, *g, *act, message);
        if (code) fail(message);
        else out(message + L"\n");
        return code;
    }

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    Gdiplus::GdiplusStartupInput gin;
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartup(&gdiToken, &gin, nullptr);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // file dialog, Explorer reveal
    int code;
    {
        ui::Launcher launcher(cfg, lib, user, userPath);
        ui::LauncherOptions opt;
        opt.snapshot = a.snapshot.empty() ? std::wstring() : fullPath(a.snapshot);
        opt.select = a.select;
        opt.width = a.width;
        opt.height = a.height;
        opt.demoHost = a.demoHost;
        code = launcher.run(opt);
    }
    CoUninitialize();
    Gdiplus::GdiplusShutdown(gdiToken);
    logger().close();
    return code;
}
