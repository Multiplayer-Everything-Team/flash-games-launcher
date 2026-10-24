// The launcher window: category sidebar, search, a grid of game cards and a
// details panel with the game's actions (Play / Host / Join...). Games run in
// their own processes; the window only starts them, watches them exit and
// runs the built-in relay while someone hosts.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <atomic>
#include <mutex>
#include <thread>

#include "core/config.h"
#include "core/install.h"
#include "core/library.h"
#include "core/process.h"
#include "core/relay.h"
#include "core/settings.h"
#include "theme.h"

namespace fl::ui {

struct LauncherOptions {
    std::wstring snapshot;   // render the window to this PNG and exit
    std::string select;      // game id to select at start
    int width = 0, height = 0;
    bool demoHost = false;   // snapshot: start the relay for the selected game
};

class Launcher {
public:
    Launcher(const Config& cfg, Library& lib, UserData& user, std::wstring userPath);
    ~Launcher();
    int run(const LauncherOptions& opt);

    // Starts an action without the window (command line --launch). Blocks until
    // the game exits when it hosts, so the relay stays up for the session.
    static int launchHeadless(const Config& cfg, Library& lib, UserData& user, const std::wstring& userPath,
                              const GameSpec& game, const ActionSpec& action, std::wstring& message);

private:
    struct NavItem {
        std::string key;  // all | fav | ready | cat:<id>
        std::wstring label;
        int count = 0;
    };
    struct Session {
        DWORD pid = 0;
        int game = -1;
        std::string action;
        std::wstring player;
        ULONGLONG started = 0;
        bool host = false;
    };
    enum class BtnStyle { Primary, Secondary, Ghost, Danger };
    struct BtnInfo {
        BtnStyle style = BtnStyle::Secondary;
        std::wstring sub;  // second, smaller line
        bool ready = true;
    };
    struct DetailLayout {
        RECT art{}, title{}, meta{}, slots{}, status{}, desc{}, controlsHead{}, controls{}, stats{}, playerLabel{}, host{};
        std::vector<RECT> hostLines;
    };

    static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK gridProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR self);
    static LRESULT CALLBACK searchProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR self);
    static LRESULT CALLBACK buttonProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR self);
    LRESULT handle(UINT m, WPARAM w, LPARAM l);

    void create(const LauncherOptions& opt);
    void createControls();
    void applyFonts();
    HWND makeButton(int id, BtnStyle style);
    void relayout();
    void layoutGrid();
    void layoutDetail();

    void buildNav();
    void rebuildGrid();
    bool matches(int gameIndex) const;
    void select(int gameIndex);
    int selectedAction(int preferred) const;
    void refreshAll();

    void paint(HDC dc);
    void paintSidebar(Gdiplus::Graphics& g, HDC dc);
    void paintDetail(Gdiplus::Graphics& g, HDC dc);
    void paintStatus(Gdiplus::Graphics& g, HDC dc);
    void drawCard(HDC dc, int gameIndex, RECT r, bool selected, bool focused);
    void drawButton(HWND button, HDC dc, RECT r, UINT state);
    void drawNavItem(const DRAWITEMSTRUCT& d);
    void paintSlots(Gdiplus::Graphics& g, const GameSpec& game);
    std::wstring categoryText(const GameSpec& game) const;
    std::wstring statsText(const GameStats* st) const;
    Gdiplus::Bitmap* art(int gameIndex, int w, int h, bool detail);

    void runAction(int gameIndex, size_t actionIndex);
    void onProcessExit(DWORD pid, DWORD code);
    void stopRelay(bool announce);
    void locateFile();
    void openFolder();
    void getPlayer();
    // Asks, then downloads and unpacks `player` on a worker thread.
    void offerInstall(const PlayerSpec& player, bool firstRun);
    void onInstalled();
    std::wstring installText() const;
    void copyAddress();
    void toggleFavorite();
    void onPlayerChoice();
    void notice(const std::wstring& text, const Color& color);
    void save();
    std::wstring stateText(int gameIndex, Color& color) const;
    std::wstring formatDuration(int64_t seconds) const;
    std::wstring formatAgo(int64_t unixTime) const;
    bool isPlaying(int gameIndex) const;
    bool isNarrow() const;

    const Config& cfg_;
    Library& lib_;
    UserData& user_;
    std::wstring userPath_;
    Theme theme_;
    HWND hwnd_ = nullptr, nav_ = nullptr, navCombo_ = nullptr, search_ = nullptr, grid_ = nullptr,
         playerCombo_ = nullptr;
    HWND favBtn_ = nullptr, locateBtn_ = nullptr, folderBtn_ = nullptr, getPlayerBtn_ = nullptr,
         stopHostBtn_ = nullptr, copyBtn_ = nullptr;
    std::vector<HWND> actionBtns_;
    std::map<HWND, BtnInfo> btnInfo_;
    HWND hoverBtn_ = nullptr;
    int hoverItem_ = -1;

    RECT sidebar_{}, top_{}, searchBox_{}, gridRc_{}, detail_{}, status_{};
    DetailLayout dl_;
    std::wstring descText_, controlsText_;  // shortened to fit their rectangles
    std::vector<NavItem> navItems_;
    size_t navIndex_ = 0;
    std::vector<int> visible_;
    int selected_ = -1;
    bool suppressSelect_ = false;
    int tileW_ = 0, tileH_ = 0;

    std::map<std::pair<int, bool>, std::unique_ptr<Gdiplus::Bitmap>> artCache_;
    std::map<int, std::unique_ptr<Gdiplus::Bitmap>> images_;
    std::map<int, bool> imageTried_;

    std::vector<Session> sessions_;
    std::unique_ptr<sys::ProcessWatcher> watcher_;
    Relay relay_;
    int relayGame_ = -1;
    std::vector<sys::NetAddress> addresses_;

    std::wstring notice_;
    Color noticeColor_;
    ULONGLONG noticeUntil_ = 0;
    bool snapshot_ = false;

    std::thread installThread_;
    std::atomic<bool> installing_{false}, installCancel_{false};
    const PlayerSpec* installPlayer_ = nullptr;
    mutable std::mutex installMu_;
    InstallStatus installStatus_;
    InstallResult installResult_;
};

}  // namespace fl::ui
