#include "launcher.h"

#include <algorithm>

#include "core/fs.h"
#include "core/log.h"
#include "core/strings.h"
// win.h (via theme.h) first: these need the full Windows types.
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace fl::ui {

using namespace Gdiplus;

namespace {

constexpr wchar_t kClass[] = L"MultiplayerFlashLauncher";
constexpr UINT WM_APP_EXIT = WM_APP + 1;   // wParam pid, lParam exit code
constexpr UINT WM_APP_RELAY = WM_APP + 2;  // relay peers changed
constexpr UINT_PTR kTimer = 1;

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
    kIdAction0 = 200,
};

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

RectF rf(const RECT& r) {
    return RectF(static_cast<REAL>(r.left), static_cast<REAL>(r.top), static_cast<REAL>(r.right - r.left),
                 static_cast<REAL>(r.bottom - r.top));
}

int width(const RECT& r) { return r.right - r.left; }
int height(const RECT& r) { return r.bottom - r.top; }

bool pngEncoder(CLSID& out) {
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

std::wstring windowText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    if (n) GetWindowTextW(h, s.data(), n + 1);
    return s;
}

std::wstring stateReason(const Config& cfg, const ResolvedAction& r) {
    if (r.state == ActionState::NoSwf) return cfg.t("status.noSwf");
    if (r.state == ActionState::NoPlayer) return cfg.t("status.noPlayer");
    return cfg.t("status.ready");
}

}  // namespace

Launcher::Launcher(const Config& cfg, Library& lib, UserData& user, std::wstring userPath)
    : cfg_(cfg), lib_(lib), user_(user), userPath_(std::move(userPath)) {}

Launcher::~Launcher() {
    relay_.stop();
    watcher_.reset();
}

// ---- window setup --------------------------------------------------------

int Launcher::run(const LauncherOptions& opt) {
    snapshot_ = !opt.snapshot.empty();
    create(opt);
    if (!hwnd_) return 1;
    if (!opt.select.empty())
        for (size_t i = 0; i < cfg_.games.size(); ++i)
            if (cfg_.games[i].id == opt.select) select(static_cast<int>(i));
    if (opt.demoHost && selected_ >= 0) {
        std::wstring err;
        for (const auto& a : cfg_.games[selected_].actions)
            if (a.relay.enabled && relay_.start(a.relay.port, err)) {
                relayGame_ = selected_;
                addresses_ = sys::localAddresses();
                break;
            }
        layoutDetail();
    }

    if (snapshot_) {
        auto pump = [] {
            for (int i = 0; i < 15; ++i) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                Sleep(20);
            }
        };
        pump();
        RECT cr;
        GetClientRect(hwnd_, &cr);
        HDC screen = GetDC(hwnd_);
        Buffer buf(screen, cr.right, cr.bottom);
        ReleaseDC(hwnd_, screen);
        PrintWindow(hwnd_, buf.dc, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
        SelectObject(buf.dc, buf.old);  // GDI+ must not read a bitmap still selected into a DC
        CLSID png;
        int code = 1;
        if (pngEncoder(png)) {
            Bitmap bmp(buf.bmp, nullptr);
            fs::ensureDir(fs::parent(opt.snapshot), nullptr, nullptr);
            code = bmp.Save(opt.snapshot.c_str(), &png, nullptr) == Ok ? 0 : 1;
        }
        buf.old = SelectObject(buf.dc, buf.bmp);
        relay_.stop();
        DestroyWindow(hwnd_);
        return code;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN) {
            bool ctrl = GetKeyState(VK_CONTROL) < 0;
            if (ctrl && msg.wParam == 'F') {
                SetFocus(search_);
                SendMessageW(search_, EM_SETSEL, 0, -1);
                continue;
            }
            if (msg.wParam == VK_F5) {
                refreshAll();
                continue;
            }
            if (msg.wParam == VK_ESCAPE && msg.hwnd == search_ && GetWindowTextLengthW(search_)) {
                SetWindowTextW(search_, L"");
                continue;
            }
            if (msg.wParam == VK_RETURN && msg.hwnd == search_) {
                if (!visible_.empty()) {
                    SetFocus(grid_);
                    ListView_SetItemState(grid_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
                continue;
            }
        }
        if (IsDialogMessageW(hwnd_, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

void Launcher::create(const LauncherOptions& opt) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    theme_.init(cfg_);
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = theme_.iconBig;
    wc.hIconSm = theme_.iconSmall;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    watcher_ = std::make_unique<sys::ProcessWatcher>([this](DWORD pid, DWORD code) {
        if (hwnd_) PostMessageW(hwnd_, WM_APP_EXIT, pid, static_cast<LPARAM>(code));
    });
    relay_.setOnChange([this] {
        if (hwnd_) PostMessageW(hwnd_, WM_APP_RELAY, 0, 0);
    });

    // Create at 96 DPI units, then resize once the real monitor DPI is known.
    UINT dpi = GetDpiForSystem();
    int w = opt.width ? opt.width : MulDiv(cfg_.branding.window.width, dpi, 96);
    int h = opt.height ? opt.height : MulDiv(cfg_.branding.window.height, dpi, 96);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (!snapshot_ && user_.width > 0 && user_.height > 0) {
        RECT want{user_.x, user_.y, user_.x + user_.width, user_.y + user_.height};
        if (MonitorFromRect(&want, MONITOR_DEFAULTTONULL)) {  // still on a connected screen
            x = user_.x;
            y = user_.y;
            w = user_.width;
            h = user_.height;
        }
    }
    Vars v = lib_.vars();
    std::wstring title = v.expand(cfg_.branding.windowTitle);
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    hwnd_ = CreateWindowExW(0, kClass, title.c_str(), style, x, y, w, h, nullptr, nullptr, inst, this);
    if (!hwnd_) return;
    theme_.build(GetDpiForWindow(hwnd_));
    BOOL darkTitle = theme_.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkTitle, sizeof darkTitle);
    createControls();
    buildNav();
    rebuildGrid();
    int start = -1;
    for (size_t i = 0; i < cfg_.games.size(); ++i)
        if (cfg_.games[i].id == user_.selectedGame) start = static_cast<int>(i);
    if (start < 0 && !visible_.empty()) start = visible_.front();
    select(start);
    relayout();
    if (snapshot_) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(hwnd_, user_.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
        SetTimer(hwnd_, kTimer, 1000, nullptr);
    }
    SetFocus(grid_);
}

void Launcher::createControls() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    auto child = [&](const wchar_t* cls, DWORD style, DWORD ex, int id) {
        return CreateWindowExW(ex, cls, L"", WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    };
    nav_ = child(L"LISTBOX", WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT, 0,
                 kIdNav);
    navCombo_ = child(WC_COMBOBOXW, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, kIdNavCombo);
    search_ = child(L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL, 0, kIdSearch);
    SetWindowSubclass(search_, searchProc, 3, reinterpret_cast<DWORD_PTR>(this));
    grid_ = child(WC_LISTVIEWW, WS_TABSTOP | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_AUTOARRANGE | LVS_NOCOLUMNHEADER, 0,
                  kIdGrid);
    ListView_SetExtendedListViewStyle(grid_, LVS_EX_DOUBLEBUFFER);
    ListView_SetView(grid_, LV_VIEW_TILE);
    LVCOLUMNW col{};
    col.mask = LVCF_WIDTH;
    col.cx = 100;
    ListView_InsertColumn(grid_, 0, &col);
    SetWindowSubclass(grid_, gridProc, 1, reinterpret_cast<DWORD_PTR>(this));
    playerCombo_ = child(WC_COMBOBOXW, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, kIdPlayer);
    favBtn_ = makeButton(kIdFav, BtnStyle::Ghost);
    folderBtn_ = makeButton(kIdFolder, BtnStyle::Ghost);
    locateBtn_ = makeButton(kIdLocate, BtnStyle::Secondary);
    getPlayerBtn_ = makeButton(kIdGetPlayer, BtnStyle::Secondary);
    copyBtn_ = makeButton(kIdCopy, BtnStyle::Secondary);
    stopHostBtn_ = makeButton(kIdStopHost, BtnStyle::Danger);
    if (theme_.dark && !theme_.highContrast) {
        for (HWND h : {nav_, grid_}) SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
        for (HWND h : {navCombo_, playerCombo_}) SetWindowTheme(h, L"DarkMode_CFD", nullptr);
    }
    applyFonts();
}

HWND Launcher::makeButton(int id, BtnStyle style) {
    // A real push button painted in NM_CUSTOMDRAW (not BS_OWNERDRAW) keeps the
    // Button role and Invoke action that screen readers and UI Automation use.
    HWND b = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SetWindowSubclass(b, buttonProc, 2, reinterpret_cast<DWORD_PTR>(this));
    btnInfo_[b].style = style;
    SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.strong), FALSE);
    return b;
}

void Launcher::applyFonts() {
    for (HWND h : {nav_, navCombo_, search_, grid_, playerCombo_})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.body), TRUE);
    for (auto& [b, info] : btnInfo_) SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.strong), FALSE);
    SendMessageW(nav_, LB_SETITEMHEIGHT, 0, theme_.s(38));
    SendMessageW(search_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(theme_.s(4), theme_.s(4)));
    ListView_SetBkColor(grid_, theme_.c.background.ref());
}

// ---- data -> controls ----------------------------------------------------

void Launcher::buildNav() {
    navItems_.clear();
    navItems_.push_back({"all", cfg_.t("nav.all")});
    navItems_.push_back({"fav", cfg_.t("nav.favorites")});
    navItems_.push_back({"ready", cfg_.t("nav.ready")});
    for (const auto& c : cfg_.categories) navItems_.push_back({"cat:" + c.id, c.name});
    for (auto& n : navItems_) {
        n.count = 0;
        for (size_t i = 0; i < cfg_.games.size(); ++i) {
            const auto& g = cfg_.games[i];
            const GameStats* st = user_.find(g.id);
            bool in = n.key == "all" || (n.key == "fav" && st && st->favorite) ||
                      (n.key == "ready" && lib_.gameState(g, user_) == ActionState::Ready) ||
                      (n.key.rfind("cat:", 0) == 0 &&
                       std::find(g.categories.begin(), g.categories.end(), n.key.substr(4)) != g.categories.end());
            if (in) ++n.count;
        }
    }
    navIndex_ = 0;
    for (size_t i = 0; i < navItems_.size(); ++i)
        if (navItems_[i].key == user_.category) navIndex_ = i;
    SendMessageW(nav_, LB_RESETCONTENT, 0, 0);
    SendMessageW(navCombo_, CB_RESETCONTENT, 0, 0);
    for (const auto& n : navItems_) {
        // The visible count is drawn separately; screen readers get it in the text.
        std::wstring text = n.label + L" (" + std::to_wstring(n.count) + L")";
        SendMessageW(nav_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        SendMessageW(navCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(nav_, LB_SETCURSEL, navIndex_, 0);
    SendMessageW(navCombo_, CB_SETCURSEL, navIndex_, 0);
}

bool Launcher::matches(int gi) const {
    const GameSpec& g = cfg_.games[gi];
    const std::string& key = navItems_.empty() ? std::string("all") : navItems_[navIndex_].key;
    const GameStats* st = user_.find(g.id);
    if (key == "fav" && !(st && st->favorite)) return false;
    if (key == "ready" && lib_.gameState(g, user_) != ActionState::Ready) return false;
    if (key.rfind("cat:", 0) == 0 &&
        std::find(g.categories.begin(), g.categories.end(), key.substr(4)) == g.categories.end())
        return false;
    std::wstring hay = toLower(g.name + L" " + g.description + L" " + g.playerCount);
    for (const auto& c : g.categories)
        if (auto cat = cfg_.findCategory(c)) hay += L" " + toLower(cat->name);
    for (const auto& word : split(toLower(trim(windowText(search_))), L' '))
        if (!word.empty() && hay.find(word) == std::wstring::npos) return false;
    return true;
}

void Launcher::rebuildGrid() {
    visible_.clear();
    for (size_t i = 0; i < cfg_.games.size(); ++i)
        if (matches(static_cast<int>(i))) visible_.push_back(static_cast<int>(i));
    suppressSelect_ = true;
    SendMessageW(grid_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(grid_);
    int selIndex = -1;
    for (size_t i = 0; i < visible_.size(); ++i) {
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = static_cast<int>(i);
        std::wstring name = cfg_.games[visible_[i]].name;
        it.pszText = name.data();
        it.lParam = visible_[i];
        ListView_InsertItem(grid_, &it);
        if (visible_[i] == selected_) selIndex = static_cast<int>(i);
    }
    if (selIndex >= 0) {
        ListView_SetItemState(grid_, selIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(grid_, selIndex, FALSE);
    }
    SendMessageW(grid_, WM_SETREDRAW, TRUE, 0);
    suppressSelect_ = false;
    ShowWindow(grid_, visible_.empty() ? SW_HIDE : SW_SHOW);
    hoverItem_ = -1;
    InvalidateRect(grid_, nullptr, TRUE);
    InvalidateRect(hwnd_, &gridRc_, FALSE);
}

void Launcher::select(int gi) {
    selected_ = gi;
    if (gi >= 0) user_.selectedGame = cfg_.games[gi].id;
    // Player choice: "Automatic" plus every player any action of this game allows.
    SendMessageW(playerCombo_, CB_RESETCONTENT, 0, 0);
    if (gi >= 0) {
        const GameSpec& g = cfg_.games[gi];
        std::wstring autoText = L"Automatic";
        SendMessageW(playerCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(autoText.c_str()));
        const GameStats* st = user_.find(g.id);
        int sel = 0, idx = 1;
        for (const auto& p : cfg_.players) {
            bool allowed = false;
            for (const auto& a : g.actions)
                allowed = allowed || a.players.empty() ||
                          std::find(a.players.begin(), a.players.end(), p.id) != a.players.end();
            if (!allowed) continue;
            const PlayerStatus* ps = lib_.player(p.id);
            std::wstring text = p.name + (ps && ps->found() ? L"" : L"  (not found)");
            int at = static_cast<int>(SendMessageW(playerCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())));
            SendMessageW(playerCombo_, CB_SETITEMDATA, at, reinterpret_cast<LPARAM>(&p));
            if (st && st->player == p.id) sel = idx;
            ++idx;
        }
        SendMessageW(playerCombo_, CB_SETCURSEL, sel, 0);
    }
    // One button per action, created on demand and reused.
    size_t count = gi >= 0 ? cfg_.games[gi].actions.size() : 0;
    while (actionBtns_.size() < count)
        actionBtns_.push_back(makeButton(kIdAction0 + static_cast<int>(actionBtns_.size()), BtnStyle::Secondary));
    for (size_t i = 0; i < actionBtns_.size(); ++i) {
        HWND b = actionBtns_[i];
        if (i >= count) {
            ShowWindow(b, SW_HIDE);
            continue;
        }
        const GameSpec& g = cfg_.games[gi];
        const ActionSpec& a = g.actions[i];
        ResolvedAction r = lib_.resolve(g, a, user_);
        BtnInfo& info = btnInfo_[b];
        info.style = i == 0 ? BtnStyle::Primary : BtnStyle::Secondary;
        info.ready = r.state == ActionState::Ready;
        std::wstring sub = a.description;
        if (r.state == ActionState::Ready && r.player) sub = sub.empty() ? r.player->name : sub + L" \u00B7 " + r.player->name;
        if (r.state != ActionState::Ready) sub = stateReason(cfg_, r);
        info.sub = sub;
        SetWindowTextW(b, a.label.c_str());
    }
    layoutDetail();
    InvalidateRect(hwnd_, &detail_, FALSE);
}

int Launcher::selectedAction(int preferred) const {
    if (selected_ < 0) return -1;
    const auto& actions = cfg_.games[selected_].actions;
    if (preferred >= 0 && preferred < static_cast<int>(actions.size())) return preferred;
    return actions.empty() ? -1 : 0;
}

void Launcher::refreshAll() {
    lib_.refresh();
    artCache_.clear();
    images_.clear();
    imageTried_.clear();
    std::string keep = navItems_.empty() ? std::string() : navItems_[navIndex_].key;
    user_.category = keep;
    buildNav();
    rebuildGrid();
    select(selected_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---- layout ---------------------------------------------------------------

bool Launcher::isNarrow() const {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    return cr.right < theme_.s(kNarrowBelow);
}

void Launcher::relayout() {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) return;
    const Theme& t = theme_;
    bool narrowMode = isNarrow();
    int sw = narrowMode ? 0 : t.s(kSidebarW);
    int dw = std::min<int>(t.s(narrowMode ? kDetailNarrowW : kDetailW), cr.right / 2);
    sidebar_ = {0, 0, sw, cr.bottom - t.s(kStatusH)};
    status_ = {0, cr.bottom - t.s(kStatusH), cr.right, cr.bottom};
    detail_ = {cr.right - dw, 0, cr.right, status_.top};
    top_ = {sw, 0, detail_.left, t.s(kTopH)};
    gridRc_ = {sw, top_.bottom, detail_.left, status_.top};

    ShowWindow(nav_, narrowMode ? SW_HIDE : SW_SHOW);
    ShowWindow(navCombo_, narrowMode ? SW_SHOW : SW_HIDE);
    int brandH = t.s(86);
    MoveWindow(nav_, t.s(12), brandH, sw - t.s(24), sidebar_.bottom - brandH - t.s(12), TRUE);

    int x = top_.left + t.s(kPad), boxH = t.s(36), y = (height(top_) - boxH) / 2;
    if (narrowMode) {
        int cw = t.s(170);
        MoveWindow(navCombo_, x, y + (boxH - t.s(28)) / 2, cw, t.s(300), TRUE);
        x += cw + t.s(10);
    }
    searchBox_ = {x, y, std::max<LONG>(x + t.s(120), static_cast<LONG>(top_.right - t.s(kPad))), y + boxH};
    // The edit sits inside a painted rounded box; center its single text line.
    HDC dc = GetDC(hwnd_);
    int lineH = textHeight(dc, t.body, L"Ag", 100, DT_SINGLELINE);
    ReleaseDC(hwnd_, dc);
    MoveWindow(search_, searchBox_.left + t.s(30), searchBox_.top + (boxH - lineH) / 2, width(searchBox_) - t.s(40),
               lineH, TRUE);
    MoveWindow(grid_, gridRc_.left + t.s(kPad) - t.s(kGap) / 2, gridRc_.top,
               width(gridRc_) - t.s(kPad) * 2 + t.s(kGap), height(gridRc_), TRUE);
    layoutGrid();
    layoutDetail();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Launcher::layoutGrid() {
    RECT r;
    GetClientRect(grid_, &r);
    int avail = r.right - GetSystemMetrics(SM_CXVSCROLL);
    int minTile = theme_.s(kCardMinW + kGap);
    int cols = std::max(1, avail / std::max(minTile, 1));
    int tw = std::max(minTile, avail / cols);
    int cardW = tw - theme_.s(kGap);
    int th = static_cast<int>(cardW * 0.5625) + theme_.s(84) + theme_.s(kGap);
    if (tw == tileW_ && th == tileH_) return;
    tileW_ = tw;
    tileH_ = th;
    LVTILEVIEWINFO tvi{sizeof tvi};
    tvi.dwMask = LVTVIM_TILESIZE | LVTVIM_COLUMNS;
    tvi.dwFlags = LVTVIF_FIXEDSIZE;
    tvi.sizeTile = {tw, th};
    tvi.cLines = 0;
    ListView_SetTileViewInfo(grid_, &tvi);
    for (auto it = artCache_.begin(); it != artCache_.end();) {
        if (!it->first.second) it = artCache_.erase(it);  // card art depends on the tile size
        else ++it;
    }
    ListView_Arrange(grid_, LVA_DEFAULT);
}

void Launcher::layoutDetail() {
    dl_ = {};
    std::vector<HWND> all = {playerCombo_, favBtn_, locateBtn_, folderBtn_, getPlayerBtn_, stopHostBtn_, copyBtn_};
    all.insert(all.end(), actionBtns_.begin(), actionBtns_.end());
    if (selected_ < 0 || width(detail_) <= 0) {
        for (HWND h : all) ShowWindow(h, SW_HIDE);
        return;
    }
    const Theme& t = theme_;
    const GameSpec& g = cfg_.games[selected_];
    HDC dc = GetDC(hwnd_);
    int x = detail_.left + t.s(kPad), w = width(detail_) - t.s(kPad) * 2, y = t.s(kPad);
    // The header (art, title, meta, status) is placed after the controls so the
    // art can shrink on short windows instead of running into the buttons.
    int titleH = textHeight(dc, t.title, g.name, w);
    std::wstring meta = g.playerCount;
    for (const auto& c : g.categories)
        if (auto cat = cfg_.findCategory(c)) meta += (meta.empty() ? L"" : L" \u00B7 ") + cat->name;
    int metaH = textHeight(dc, t.caption, meta, w);

    // Everything below the description is laid out bottom-up from the panel's
    // end, so the buttons never move when the description is long.
    int bottom = detail_.bottom - t.s(kPad);
    int rowH = t.s(34), actionH = t.s(54), gap = t.s(8);
    const GameStats* st = user_.find(g.id);
    const ActionSpec* missingSwf = nullptr;
    const PlayerSpec* missingPlayer = nullptr;
    for (const auto& a : g.actions) {
        ResolvedAction r = lib_.resolve(g, a, user_);
        if (r.state == ActionState::NoSwf && !missingSwf) missingSwf = &a;
        if (r.state == ActionState::NoPlayer && !missingPlayer && r.player) missingPlayer = r.player;
    }
    bool hosting = relay_.running() && relayGame_ == selected_;
    RelaySnapshot snap = relay_.snapshot();
    int hostLines = 2 + static_cast<int>(std::max<size_t>(snap.peers.size(), 1)) +
                    static_cast<int>(std::min<size_t>(std::max<size_t>(addresses_.size(), 1), 3));
    int hostLineH = textHeight(dc, t.caption, L"Ag", 100, DT_SINGLELINE) + t.s(4);
    int hostBoxH = t.s(14) + hostLines * hostLineH + t.s(8) + rowH + t.s(14);
    int comboH = t.s(30);
    bool picker = SendMessageW(playerCombo_, CB_GETCOUNT, 0, 0) > 2;  // "Automatic" + 2 players

    // Short windows: thinner action buttons (no second line) and no player picker,
    // so the game's name and state stay visible above the buttons.
    int fixed = rowH + gap + (missingPlayer ? rowH + gap : 0) + (missingSwf ? rowH + gap : 0) +
                (hosting ? hostBoxH + t.s(12) : 0);
    int headerMin = titleH + t.s(4 + 24 + 16);
    int actionsH = static_cast<int>(g.actions.size()) * (actionH + gap) + (picker ? comboH + t.s(16) : 0);
    if (bottom - fixed - actionsH < y + headerMin) {
        actionH = t.s(40);
        picker = false;
    }

    // Row: favorite + folder (ghost buttons).
    SetWindowTextW(favBtn_, cfg_.t(st && st->favorite ? "button.unfavorite" : "button.favorite").c_str());
    SetWindowTextW(folderBtn_, cfg_.t("button.folder").c_str());
    int half = (w - gap) / 2;
    MoveWindow(favBtn_, x, bottom - rowH, half, rowH, TRUE);
    MoveWindow(folderBtn_, x + half + gap, bottom - rowH, w - half - gap, rowH, TRUE);
    ShowWindow(favBtn_, SW_SHOW);
    ShowWindow(folderBtn_, SW_SHOW);
    bottom -= rowH + gap;

    // Fix-it buttons for whatever stops the game from starting.
    auto place = [&](HWND b, bool show) {
        if (!show) {
            ShowWindow(b, SW_HIDE);
            return;
        }
        MoveWindow(b, x, bottom - rowH, w, rowH, TRUE);
        ShowWindow(b, SW_SHOW);
        bottom -= rowH + gap;
    };
    if (missingPlayer)
        SetWindowTextW(getPlayerBtn_, cfg_.t("button.getPlayer", {{L"PLAYER", missingPlayer->name}}).c_str());
    place(getPlayerBtn_, missingPlayer != nullptr);
    if (missingSwf)
        SetWindowTextW(locateBtn_, (g.actions.size() > 1 ? cfg_.t("button.locateFor", {{L"ACTION", missingSwf->label}})
                                                          : cfg_.t("button.locate"))
                                       .c_str());
    place(locateBtn_, missingSwf != nullptr);

    // Hosting box: relay status, peers, this PC's addresses.
    ShowWindow(stopHostBtn_, hosting ? SW_SHOW : SW_HIDE);
    ShowWindow(copyBtn_, hosting ? SW_SHOW : SW_HIDE);
    if (hosting) {
        dl_.host = {x, bottom - hostBoxH, x + w, bottom};
        int by = dl_.host.top + t.s(14);
        for (int i = 0; i < hostLines; ++i, by += hostLineH)
            dl_.hostLines.push_back({x + t.s(14), by, x + w - t.s(14), by + hostLineH});
        int bw = (w - t.s(28) - gap) / 2;
        MoveWindow(copyBtn_, x + t.s(14), dl_.host.bottom - t.s(14) - rowH, bw, rowH, TRUE);
        MoveWindow(stopHostBtn_, x + t.s(14) + bw + gap, dl_.host.bottom - t.s(14) - rowH, w - t.s(28) - bw - gap, rowH,
                   TRUE);
        SetWindowTextW(copyBtn_, cfg_.t("button.copyAddress").c_str());
        SetWindowTextW(stopHostBtn_, cfg_.t("button.stopHost").c_str());
        bottom = dl_.host.top - t.s(12);
    }

    // Action buttons, last one at the bottom.
    for (size_t i = g.actions.size(); i-- > 0;) {
        HWND b = actionBtns_[i];
        MoveWindow(b, x, bottom - actionH, w, actionH, TRUE);
        ShowWindow(b, SW_SHOW);
        bottom -= actionH + gap;
    }
    for (size_t i = g.actions.size(); i < actionBtns_.size(); ++i) ShowWindow(actionBtns_[i], SW_HIDE);

    // Player picker, only worth showing when there is a choice.
    if (picker) {
        bottom -= t.s(4);
        int labelW = textWidth(dc, t.strong, cfg_.t("detail.player")) + t.s(12);
        dl_.playerLabel = {x, bottom - comboH, x + labelW, bottom};
        MoveWindow(playerCombo_, x + labelW, bottom - comboH, w - labelW, t.s(240), TRUE);
        ShowWindow(playerCombo_, SW_SHOW);
        bottom -= comboH + t.s(12);
    } else {
        ShowWindow(playerCombo_, SW_HIDE);
    }

    int headerH = titleH + t.s(4) + metaH + t.s(10) + t.s(24) + t.s(12);
    int artH = std::min(static_cast<int>(w * 0.5625), t.s(200));
    int room = bottom - t.s(8) - y - headerH - t.s(16);
    if (room < artH) artH = room >= t.s(72) ? room : 0;
    if (artH > 0) {
        dl_.art = {x, y, x + w, y + artH};
        y += artH + t.s(16);
    }
    // Each header line is shown only while it still fits above the controls.
    int limit = bottom - t.s(8);
    if (y + titleH <= limit) {
        dl_.title = {x, y, x + w, y + titleH};
        y += titleH + t.s(4);
    }
    if (y + metaH + t.s(10 + 24) <= limit) {
        dl_.meta = {x, y, x + w, y + metaH};
        y += metaH + t.s(10);
    }
    if (y + t.s(24) <= limit) {
        dl_.status = {x, y, x + w, y + t.s(24)};
        y += t.s(24) + t.s(12);
    }

    // Text between the status pill and the controls, dropped when it doesn't fit.
    int textBottom = bottom - t.s(8);
    std::wstring stats;
    if (st && st->plays > 0)
        stats = cfg_.t("detail.stats", {{L"COUNT", std::to_wstring(st->plays)},
                                        {L"TIME", formatDuration(st->seconds)},
                                        {L"WHEN", formatAgo(st->lastPlayed)}});
    else
        stats = cfg_.t("detail.never");
    int statsH = textHeight(dc, t.caption, stats, w);
    if (textBottom - statsH >= y) {
        dl_.stats = {x, textBottom - statsH, x + w, textBottom};
        textBottom = dl_.stats.top - t.s(10);
    }
    if (!g.controls.empty()) {
        int headH = textHeight(dc, t.strong, cfg_.t("detail.controls"), w);
        int ch = textHeight(dc, t.caption, g.controls, w);
        int avail = textBottom - y - headH - t.s(4);
        if (avail > t.s(16)) {
            ch = std::min(ch, avail);
            dl_.controls = {x, textBottom - ch, x + w, textBottom};
            dl_.controlsHead = {x, dl_.controls.top - t.s(4) - headH, x + w, dl_.controls.top - t.s(4)};
            textBottom = dl_.controlsHead.top - t.s(12);
        }
    }
    dl_.desc = {x, y, x + w, std::max(y, textBottom)};
    descText_ = fitText(dc, t.body, g.description, w, height(dl_.desc));
    controlsText_ = fitText(dc, t.caption, g.controls, w, height(dl_.controls));
    ReleaseDC(hwnd_, dc);
    InvalidateRect(hwnd_, &detail_, FALSE);
    for (HWND hb : all) InvalidateRect(hb, nullptr, FALSE);
}

// ---- painting -------------------------------------------------------------

std::wstring Launcher::stateText(int gi, Color& color) const {
    if (isPlaying(gi)) {
        color = theme_.c.online;
        return cfg_.t("status.playing");
    }
    switch (lib_.gameState(cfg_.games[gi], user_)) {
        case ActionState::Ready: color = theme_.c.success; return cfg_.t("status.ready");
        case ActionState::NoPlayer: color = theme_.c.error; return cfg_.t("status.noPlayer");
        case ActionState::NoSwf: break;
    }
    color = theme_.c.warning;
    return cfg_.t("status.noSwf");
}

Bitmap* Launcher::art(int gi, int w, int h, bool detail) {
    auto key = std::make_pair(gi, detail);
    auto it = artCache_.find(key);
    if (it != artCache_.end() && it->second && static_cast<int>(it->second->GetWidth()) == w &&
        static_cast<int>(it->second->GetHeight()) == h)
        return it->second.get();
    const GameSpec& g = cfg_.games[gi];
    if (!imageTried_[gi]) {
        imageTried_[gi] = true;
        if (!g.image.empty()) {
            std::wstring path = lib_.vars().expand(g.image);
            if (!fs::isAbsolute(path)) path = fs::join(cfg_.configDir, path);
            images_[gi] = loadImageFile(path);
        }
    }
    std::unique_ptr<Bitmap> bmp;
    if (images_[gi]) {
        // Pre-scale once so painting is a 1:1 blit (smooth scrolling).
        bmp = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
        Graphics gg(bmp.get());
        gg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        drawCover(gg, *images_[gi], RectF(0, 0, static_cast<REAL>(w), static_cast<REAL>(h)), 0, true);
    } else {
        bmp = coverArt(g.name, g.color, g.colorEnd, w, h);
    }
    Bitmap* raw = bmp.get();
    artCache_[key] = std::move(bmp);
    return raw;
}

void Launcher::paint(HDC dc) {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    Buffer buf(dc, cr.right, cr.bottom);
    {
        Graphics g(buf.dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        const Theme& t = theme_;
        SolidBrush bg(gc(t.c.background));
        g.FillRectangle(&bg, 0, 0, cr.right, cr.bottom);
        paintSidebar(g, buf.dc);

        // Search box.
        fillRound(g, rf(searchBox_), t.sf(10), t.c.surface);
        strokeRound(g, rf(searchBox_), t.sf(10), GetFocus() == search_ ? t.c.primary : t.c.border, t.sf(1));
        RECT icon{searchBox_.left + t.s(10), searchBox_.top, searchBox_.left + t.s(30), searchBox_.bottom};
        drawText(g, L"\u2315", icon, t.body, t.c.textMuted, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

        if (visible_.empty()) {
            RECT r = gridRc_;
            r.top += t.s(80);
            drawText(g, cfg_.t("empty.title"), r, t.heading, t.c.text, DT_CENTER | DT_SINGLELINE);
            r.top += t.s(30);
            drawText(g, cfg_.t("empty.body"), r, t.body, t.c.textMuted, DT_CENTER | DT_SINGLELINE);
        }
        paintDetail(g, buf.dc);
        paintStatus(g, buf.dc);
    }
    BitBlt(dc, 0, 0, cr.right, cr.bottom, buf.dc, 0, 0, SRCCOPY);
}

void Launcher::paintSidebar(Graphics& g, HDC) {
    if (width(sidebar_) <= 0) return;
    const Theme& t = theme_;
    SolidBrush sb(gc(t.c.sidebar));
    g.FillRectangle(&sb, rf(sidebar_));
    // Text-only brand: no logo has been supplied, so none is invented.
    RECT r1{t.s(24), t.s(22), sidebar_.right - t.s(12), t.s(48)};
    drawText(g, cfg_.branding.title, r1, t.brand, t.c.sidebarText, DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT r2{t.s(24), t.s(48), sidebar_.right - t.s(12), t.s(70)};
    drawText(g, cfg_.branding.subtitle, r2, t.caption, t.c.sidebarTextMuted, DT_SINGLELINE | DT_END_ELLIPSIS);
}

void Launcher::paintDetail(Graphics& g, HDC) {
    const Theme& t = theme_;
    SolidBrush panel(gc(t.c.surface));
    g.FillRectangle(&panel, rf(detail_));
    Pen edge(gc(t.c.border), 1);
    g.DrawLine(&edge, static_cast<REAL>(detail_.left), 0.0f, static_cast<REAL>(detail_.left),
               static_cast<REAL>(detail_.bottom));
    if (selected_ < 0) {
        RECT r = detail_;
        r.top += t.s(80);
        drawText(g, cfg_.t("detail.empty"), r, t.body, t.c.textMuted, DT_CENTER | DT_SINGLELINE);
        return;
    }
    const GameSpec& game = cfg_.games[selected_];
    if (height(dl_.art) > 0)
        if (Bitmap* a = art(selected_, width(dl_.art), height(dl_.art), true))
            drawCover(g, *a, rf(dl_.art), t.sf(12), true);
    if (height(dl_.title) > 0) drawText(g, game.name, dl_.title, t.title, t.c.text, DT_WORDBREAK);
    std::wstring meta = game.playerCount;
    for (const auto& c : game.categories)
        if (auto cat = cfg_.findCategory(c)) meta += (meta.empty() ? L"" : L" \u00B7 ") + cat->name;
    if (height(dl_.meta) > 0) drawText(g, meta, dl_.meta, t.caption, t.c.textMuted, DT_WORDBREAK);

    // Status pill.
    if (height(dl_.status) > 0) {
        Color sc;
        std::wstring status = stateText(selected_, sc);
        HDC mdc = g.GetHDC();
        int tw = textWidth(mdc, t.caption, status);
        g.ReleaseHDC(mdc);
        RECT pill{dl_.status.left, dl_.status.top, dl_.status.left + tw + t.s(30), dl_.status.bottom};
        fillRound(g, rf(pill), t.sf(12), mix(t.c.surface, sc, 0.12f));
        SolidBrush dot(gc(sc));
        g.FillEllipse(&dot, static_cast<REAL>(pill.left + t.s(10)),
                      static_cast<REAL>(pill.top + height(pill) / 2 - t.s(4)), t.sf(8), t.sf(8));
        RECT pt{pill.left + t.s(22), pill.top, pill.right, pill.bottom};
        drawText(g, status, pt, t.caption, sc, DT_SINGLELINE | DT_VCENTER);
    }

    if (height(dl_.desc) > 0) drawText(g, descText_, dl_.desc, t.body, t.c.text, DT_WORDBREAK);
    if (height(dl_.controls) > 0) {
        drawText(g, cfg_.t("detail.controls"), dl_.controlsHead, t.strong, t.c.text, DT_SINGLELINE);
        drawText(g, controlsText_, dl_.controls, t.caption, t.c.textMuted, DT_WORDBREAK);
    }
    const GameStats* st = user_.find(game.id);
    std::wstring stats = st && st->plays > 0 ? cfg_.t("detail.stats", {{L"COUNT", std::to_wstring(st->plays)},
                                                                        {L"TIME", formatDuration(st->seconds)},
                                                                        {L"WHEN", formatAgo(st->lastPlayed)}})
                                             : cfg_.t("detail.never");
    if (height(dl_.stats) > 0) drawText(g, stats, dl_.stats, t.caption, t.c.textMuted, DT_WORDBREAK);
    if (width(dl_.playerLabel) > 0)
        drawText(g, cfg_.t("detail.player"), dl_.playerLabel, t.strong, t.c.text, DT_SINGLELINE | DT_VCENTER);

    if (height(dl_.host) > 0 && !dl_.hostLines.empty()) {
        fillRound(g, rf(dl_.host), t.sf(10), mix(t.c.surface, t.c.online, 0.10f));
        strokeRound(g, rf(dl_.host), t.sf(10), mix(t.c.surface, t.c.online, 0.55f), t.sf(1));
        RelaySnapshot snap = relay_.snapshot();
        size_t line = 0;
        auto put = [&](const std::wstring& s, HFONT f, const Color& c) {
            if (line < dl_.hostLines.size()) drawText(g, s, dl_.hostLines[line++], f, c, DT_SINGLELINE | DT_END_ELLIPSIS);
        };
        std::wstring port = std::to_wstring(relay_.port());
        put(L"\u25CF  " + cfg_.t("host.running", {{L"PORT", port}}), t.strong, t.c.online);
        if (snap.peers.empty()) {
            put(cfg_.t("host.waiting"), t.caption, t.c.textMuted);
        } else {
            for (const auto& p : snap.peers)
                put(L"\u2022 " + p.name + (p.host ? L" " + cfg_.t("host.you") : L"") + L"   " + p.address, t.caption,
                    t.c.text);
        }
        put(cfg_.t("host.addresses"), t.caption, t.c.textMuted);
        if (addresses_.empty()) {
            put(cfg_.t("host.noAddress", {{L"PORT", port}}), t.caption, t.c.text);
        } else {
            for (size_t i = 0; i < addresses_.size() && i < 3; ++i)
                put(addresses_[i].address + L":" + port + L"   " + addresses_[i].adapter, t.caption, t.c.text);
        }
    }
}

void Launcher::paintStatus(Graphics& g, HDC) {
    const Theme& t = theme_;
    SolidBrush bar(gc(t.c.sidebar));
    g.FillRectangle(&bar, rf(status_));
    RECT r{status_.left + t.s(16), status_.top, status_.right - t.s(16), status_.bottom};
    // Right: which players were found.
    std::wstring right;
    for (const auto& p : lib_.players())
        right += (right.empty() ? L"" : L"   ") +
                 cfg_.t(p.found() ? "bar.found" : "bar.missing", {{L"PLAYER", p.spec->name}});
    if (!lib_.flashpointDir().empty()) right += L"   " + cfg_.t("bar.flashpoint", {{L"PATH", lib_.flashpointDir()}});
    HDC mdc = g.GetHDC();
    int rw = std::min<int>(textWidth(mdc, t.caption, right), width(r) / 2);
    g.ReleaseHDC(mdc);
    RECT rr{r.right - rw, r.top, r.right, r.bottom};
    drawText(g, right, rr, t.caption, t.c.sidebarTextMuted, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS);

    RECT left{r.left, r.top, rr.left - t.s(16), r.bottom};
    std::wstring text;
    Color color = t.c.sidebarText;
    if (!notice_.empty() && GetTickCount64() < noticeUntil_) {
        text = notice_;
        color = noticeColor_;
    } else if (!sessions_.empty()) {
        const Session& s = sessions_.back();
        ULONGLONG secs = (GetTickCount64() - s.started) / 1000;
        wchar_t clock[32];
        swprintf(clock, 32, L"%llu:%02llu", secs / 60, secs % 60);
        text = cfg_.t("bar.playing", {{L"GAME", cfg_.games[s.game].name + L" (" + s.player + L")"}, {L"TIME", clock}});
        color = t.c.online;
    }
    if (!text.empty()) {
        SolidBrush dot(gc(color));
        g.FillEllipse(&dot, static_cast<REAL>(left.left), static_cast<REAL>((left.top + left.bottom) / 2 - t.s(4)), t.sf(8),
                      t.sf(8));
        left.left += t.s(16);
        drawText(g, text, left, t.caption, color, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
}

void Launcher::drawCard(HDC dc, int gi, RECT tile, bool selected, bool focused) {
    const Theme& t = theme_;
    const GameSpec& game = cfg_.games[gi];
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    SolidBrush bg(gc(t.c.background));
    g.FillRectangle(&bg, rf(tile));
    int half = t.s(kGap) / 2;
    RECT c{tile.left + half, tile.top + half, tile.right - half, tile.bottom - half};
    bool hover = hoverItem_ >= 0 && hoverItem_ < static_cast<int>(visible_.size()) && visible_[hoverItem_] == gi;
    float radius = t.sf(12);
    fillRound(g, rf(c), radius, hover || selected ? t.c.surfaceHover : t.c.surface);
    int artH = static_cast<int>(width(c) * 0.5625);
    RECT artR{c.left, c.top, c.right, c.top + artH};
    if (Bitmap* a = art(gi, width(artR), height(artR), false)) drawCover(g, *a, rf(artR), radius, false);

    // Badges on the art.
    auto badge = [&](const std::wstring& text, const Color& color, bool right) {
        HDC mdc = g.GetHDC();
        int w = textWidth(mdc, t.tiny, text) + t.s(14);
        g.ReleaseHDC(mdc);
        int bh = t.s(20);
        RECT b = right ? RECT{artR.right - t.s(10) - w, artR.top + t.s(10), artR.right - t.s(10), artR.top + t.s(10) + bh}
                       : RECT{artR.left + t.s(10), artR.top + t.s(10), artR.left + t.s(10) + w, artR.top + t.s(10) + bh};
        fillRound(g, rf(b), t.sf(10), color);
        drawText(g, text, b, t.tiny, Color{255, 255, 255, 255}, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    };
    bool online = false;
    for (const auto& a : game.actions) online = online || a.relay.enabled;
    // Badges are darkened until white text on them reaches 4.5:1.
    if (online) badge(cfg_.t("badge.online"), mix(t.c.online, Color{0, 0, 0, 255}, 0.45f), false);
    const GameStats* st = user_.find(game.id);
    if (st && st->favorite) badge(L"\u2605", mix(t.c.warning, Color{0, 0, 0, 255}, 0.45f), true);

    int x = c.left + t.s(14), w = width(c) - t.s(28), y = artR.bottom + t.s(12);
    RECT name{x, y, x + w, y + t.s(22)};
    drawText(g, game.name, name, t.strong, t.c.text, DT_SINGLELINE | DT_END_ELLIPSIS);
    y += t.s(22);
    std::wstring meta = game.playerCount;
    if (!game.categories.empty())
        if (auto cat = cfg_.findCategory(game.categories.front())) meta += (meta.empty() ? L"" : L" \u00B7 ") + cat->name;
    RECT mr{x, y, x + w, y + t.s(20)};
    drawText(g, meta, mr, t.caption, t.c.textMuted, DT_SINGLELINE | DT_END_ELLIPSIS);
    y += t.s(24);
    Color sc;
    std::wstring status = stateText(gi, sc);
    SolidBrush dot(gc(sc));
    g.FillEllipse(&dot, static_cast<REAL>(x), static_cast<REAL>(y + t.s(6)), t.sf(8), t.sf(8));
    RECT sr{x + t.s(14), y, x + w, y + t.s(20)};
    drawText(g, status, sr, t.caption, sc, DT_SINGLELINE | DT_END_ELLIPSIS);

    if (selected) strokeRound(g, rf(c), radius, t.c.primary, t.sf(2));
    if (focused && !(SendMessageW(hwnd_, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        RECT f{c.left - t.s(3), c.top - t.s(3), c.right + t.s(3), c.bottom + t.s(3)};
        strokeRound(g, rf(f), radius + t.sf(3), t.c.focus, t.sf(1.5));
    }
}

void Launcher::drawButton(HWND button, HDC hdc, RECT r, UINT state) {
    const Theme& t = theme_;
    const BtnInfo& info = btnInfo_[button];
    Buffer buf(hdc, width(r), height(r));
    {
        Graphics g(buf.dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        // Buttons live on the detail panel (or the hosting box inside it).
        RECT onScreen;
        GetWindowRect(button, &onScreen);
        MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&onScreen), 2);
        POINT mid{(onScreen.left + onScreen.right) / 2, (onScreen.top + onScreen.bottom) / 2};
        Color under = PtInRect(&dl_.host, mid) ? mix(t.c.surface, t.c.online, 0.10f) : t.c.surface;
        SolidBrush ub(gc(under));
        g.FillRectangle(&ub, 0, 0, width(r), height(r));

        bool hover = hoverBtn_ == button, pressed = (state & CDIS_SELECTED) != 0;
        Color fill = t.c.surfaceHover, text = t.c.text, border = t.c.border;
        switch (info.style) {
            case BtnStyle::Primary:
                // A primary action that can't start looks like a secondary one, so its
                // muted label keeps AA contrast.
                fill = info.ready ? (hover ? t.c.primaryHover : t.c.primary)
                                  : (hover ? mix(t.c.surfaceHover, t.c.text, 0.06f) : t.c.surfaceHover);
                text = info.ready ? t.c.onPrimary : t.c.textMuted;
                border = info.ready ? fill : t.c.border;
                break;
            case BtnStyle::Secondary:
                fill = hover ? mix(t.c.surfaceHover, t.c.text, 0.06f) : t.c.surfaceHover;
                text = info.ready ? t.c.text : t.c.textMuted;
                break;
            case BtnStyle::Ghost:
                fill = hover ? t.c.surfaceHover : under;
                border = hover ? t.c.border : under;
                break;
            case BtnStyle::Danger:
                fill = hover ? mix(t.c.surfaceHover, t.c.error, 0.22f) : mix(t.c.surfaceHover, t.c.error, 0.15f);
                text = mix(t.c.error, Color{255, 255, 255, 255}, 0.35f);  // plain error red is under 4.5:1 on the tint
                border = mix(t.c.surfaceHover, t.c.error, 0.4f);
                break;
        }
        if (pressed) fill = mix(fill, Color{0, 0, 0, 255}, 0.15f);
        RectF box(0.5f, 0.5f, static_cast<REAL>(width(r)) - 1, static_cast<REAL>(height(r)) - 1);
        fillRound(g, box, t.sf(9), fill);
        strokeRound(g, box, t.sf(9), border, t.sf(1));

        std::wstring label = windowText(button);
        RECT tr{t.s(14), 0, width(r) - t.s(14), height(r)};
        if (!info.sub.empty() && height(r) >= t.s(48)) {
            RECT top{tr.left, t.s(8), tr.right, height(r) / 2 + t.s(2)};
            RECT bottom{tr.left, height(r) / 2 + t.s(1), tr.right, height(r) - t.s(6)};
            drawText(g, label, top, t.heading, text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            Color subColor = info.style == BtnStyle::Primary && info.ready ? t.c.onPrimary
                             : info.ready                                   ? t.c.textMuted
                                                                            : t.c.warning;
            drawText(g, info.sub, bottom, t.caption, subColor, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            if (info.style == BtnStyle::Primary && info.ready) {
                RECT arrow{tr.right - t.s(20), 0, tr.right, height(r)};
                drawText(g, L"\u25B6", arrow, t.body, text, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
            }
        } else {
            drawText(g, label, tr, t.strong, text, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
        }
        if ((state & CDIS_FOCUS) && !(SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            RectF f(t.sf(2), t.sf(2), static_cast<REAL>(width(r)) - t.sf(4), static_cast<REAL>(height(r)) - t.sf(4));
            strokeRound(g, f, t.sf(7), t.c.focus, t.sf(1.5));
        }
    }
    BitBlt(hdc, r.left, r.top, width(r), height(r), buf.dc, 0, 0, SRCCOPY);
}

void Launcher::drawNavItem(const DRAWITEMSTRUCT& d) {
    if (d.itemID == static_cast<UINT>(-1) || d.itemID >= navItems_.size()) return;
    const Theme& t = theme_;
    const NavItem& n = navItems_[d.itemID];
    RECT r = d.rcItem;
    Buffer buf(d.hDC, width(r), height(r));
    {
        Graphics g(buf.dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        SolidBrush bg(gc(t.c.sidebar));
        g.FillRectangle(&bg, 0, 0, width(r), height(r));
        bool sel = (d.itemState & ODS_SELECTED) != 0;
        RectF box(0, t.sf(2), static_cast<REAL>(width(r)), static_cast<REAL>(height(r)) - t.sf(4));
        if (sel) {
            fillRound(g, box, t.sf(8), mix(t.c.sidebar, t.c.primary, 0.22f));
            RectF accent(0, box.Y + t.sf(8), t.sf(3), box.Height - t.sf(16));
            fillRound(g, accent, t.sf(1.5), t.c.primary);
        }
        RECT tr{t.s(14), 0, width(r) - t.s(44), height(r)};
        drawText(g, n.label, tr, sel ? t.strong : t.body, sel ? t.c.sidebarText : mix(t.c.sidebarText, t.c.sidebarTextMuted, 0.4f),
                 DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT cr{width(r) - t.s(44), 0, width(r) - t.s(12), height(r)};
        drawText(g, std::to_wstring(n.count), cr, t.caption, t.c.sidebarTextMuted, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        if ((d.itemState & ODS_FOCUS) && GetFocus() == nav_ &&
            !(SendMessageW(hwnd_, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
            strokeRound(g, box, t.sf(8), t.c.focus, t.sf(1.5));
    }
    BitBlt(d.hDC, r.left, r.top, width(r), height(r), buf.dc, 0, 0, SRCCOPY);
}

// ---- actions --------------------------------------------------------------

void Launcher::notice(const std::wstring& text, const Color& color) {
    notice_ = text;
    noticeColor_ = color;
    noticeUntil_ = GetTickCount64() + 10000;
    logger().info(L"Notice: " + text);
    InvalidateRect(hwnd_, &status_, FALSE);
}

bool Launcher::isPlaying(int gi) const {
    for (const auto& s : sessions_)
        if (s.game == gi) return true;
    return false;
}

void Launcher::runAction(int gi, size_t ai) {
    if (gi < 0 || ai >= cfg_.games[gi].actions.size()) return;
    const GameSpec& g = cfg_.games[gi];
    const ActionSpec& a = g.actions[ai];
    ResolvedAction r = lib_.resolve(g, a, user_);
    if (r.state != ActionState::Ready) {
        notice(cfg_.t("notice.notReady", {{L"GAME", g.name}, {L"REASON", stateReason(cfg_, r)}}), theme_.c.warning);
        if (r.state == ActionState::NoSwf) SetFocus(locateBtn_);
        else if (r.state == ActionState::NoPlayer) SetFocus(getPlayerBtn_);
        return;
    }
    bool startedRelay = false;
    std::wstring warning;
    if (a.relay.enabled && !(relay_.running() && relay_.port() == a.relay.port)) {
        std::wstring err;
        bool inUse = false;
        if (relay_.start(a.relay.port, err, &inUse)) {
            startedRelay = true;
            relayGame_ = gi;
            addresses_ = sys::localAddresses();
        } else if (inUse) {
            // Most likely the mod's own relay script is already running; the game works with it.
            warning = cfg_.t("host.portInUse", {{L"PORT", std::to_wstring(a.relay.port)}});
        } else {
            notice(cfg_.t("host.failed", {{L"PORT", std::to_wstring(a.relay.port)}, {L"ERROR", err}}), theme_.c.error);
            return;
        }
    } else if (a.relay.enabled) {
        relayGame_ = gi;
    }

    sys::Started p;
    std::wstring err;
    if (!sys::startProcess(r.playerExe, r.args, r.workDir, p, err)) {
        if (startedRelay) stopRelay(false);
        notice(cfg_.t("notice.launchFailed", {{L"PLAYER", r.player->name}, {L"ERROR", err}}), theme_.c.error);
        return;
    }
    Session s;
    s.pid = p.pid;
    s.game = gi;
    s.action = a.id;
    s.player = r.player->name;
    s.started = GetTickCount64();
    s.host = a.relay.enabled;
    sessions_.push_back(s);
    watcher_->watch(p.process, p.pid);

    GameStats& st = user_.stats(g.id);
    ++st.plays;
    st.lastPlayed = unixNow();
    save();
    std::wstring msg = warning.empty() ? (a.hint.empty() ? cfg_.t("notice.launched", {{L"GAME", g.name}, {L"PLAYER", r.player->name}}) : a.hint)
                                       : warning;
    notice(msg, warning.empty() ? theme_.c.success : theme_.c.warning);
    buildNav();
    select(selected_);
    InvalidateRect(grid_, nullptr, FALSE);
    if (cfg_.behavior.minimizeOnLaunch) ShowWindow(hwnd_, SW_MINIMIZE);
}

void Launcher::onProcessExit(DWORD pid, DWORD code) {
    auto it = std::find_if(sessions_.begin(), sessions_.end(), [&](const Session& s) { return s.pid == pid; });
    if (it == sessions_.end()) return;
    Session s = *it;
    sessions_.erase(it);
    int64_t secs = static_cast<int64_t>((GetTickCount64() - s.started) / 1000);
    const GameSpec& g = cfg_.games[s.game];
    user_.stats(g.id).seconds += secs;
    save();
    logger().info(g.name + L" (" + s.player + L") exited with code " + std::to_wstring(code) + L" after " +
                  std::to_wstring(secs) + L" s");
    if (code != 0 && secs < cfg_.behavior.quickExitSeconds)
        notice(cfg_.t("notice.quitFast", {{L"PLAYER", s.player}, {L"CODE", std::to_wstring(code)}}), theme_.c.error);
    if (s.host && cfg_.behavior.stopRelayWithGame && relay_.running()) {
        bool otherHost = std::any_of(sessions_.begin(), sessions_.end(), [](const Session& x) { return x.host; });
        if (!otherHost) stopRelay(true);
    }
    if (sessions_.empty() && IsIconic(hwnd_) && cfg_.behavior.minimizeOnLaunch) ShowWindow(hwnd_, SW_RESTORE);
    select(selected_);
    InvalidateRect(grid_, nullptr, FALSE);
    InvalidateRect(hwnd_, &status_, FALSE);
}

void Launcher::stopRelay(bool announce) {
    relay_.stop();
    relayGame_ = -1;
    if (announce) notice(cfg_.t("notice.relayStopped"), theme_.c.textMuted);
    layoutDetail();
}

void Launcher::locateFile() {
    if (selected_ < 0) return;
    const GameSpec& g = cfg_.games[selected_];
    const ActionSpec* target = nullptr;
    for (const auto& a : g.actions)
        if (!target && lib_.resolve(g, a, user_).state == ActionState::NoSwf) target = &a;
    if (!target && !g.actions.empty()) target = &g.actions.front();
    if (!target) return;
    wchar_t file[MAX_PATH * 4] = L"";
    std::wstring filter = cfg_.t("dialog.swfFilter");
    filter.push_back(L'\0');
    filter += L"*.swf";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    std::wstring title = cfg_.t("dialog.locateTitle", {{L"GAME", g.name}});
    std::wstring initial = lib_.flashpointDir().empty() ? lib_.vars().get(L"GAMES_DIR") : lib_.flashpointDir();
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrTitle = title.c_str();
    ofn.lpstrInitialDir = initial.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    user_.stats(g.id).swf[target->id] = fs::normalize(file);
    save();
    notice(cfg_.t("notice.located", {{L"GAME", g.name}, {L"FILE", fs::fileName(file)}}), theme_.c.success);
    buildNav();
    rebuildGrid();
    select(selected_);
}

void Launcher::openFolder() {
    if (selected_ < 0) return;
    const GameSpec& g = cfg_.games[selected_];
    for (const auto& a : g.actions) {
        ResolvedAction r = lib_.resolve(g, a, user_);
        if (!r.swf.empty() && !r.swfIsUrl) {
            sys::revealInExplorer(r.swf);
            return;
        }
    }
    std::wstring dir = lib_.vars().get(L"GAMES_DIR");
    fs::ensureDir(dir, nullptr, nullptr);
    sys::openPath(dir);
}

void Launcher::getPlayer() {
    if (selected_ < 0) return;
    const GameSpec& g = cfg_.games[selected_];
    for (const auto& a : g.actions) {
        ResolvedAction r = lib_.resolve(g, a, user_);
        if (r.state == ActionState::NoPlayer && r.player && !r.player->website.empty()) {
            sys::openPath(r.player->website);
            return;
        }
    }
    sys::openPath(lib_.vars().get(L"PLAYERS_DIR"));
}

void Launcher::copyAddress() {
    std::wstring addr = (addresses_.empty() ? std::wstring(L"127.0.0.1") : addresses_.front().address) + L":" +
                        std::to_wstring(relay_.port());
    if (!OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    size_t bytes = (addr.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(mem), addr.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
    notice(cfg_.t("notice.copied", {{L"ADDRESS", addr}}), theme_.c.success);
}

void Launcher::toggleFavorite() {
    if (selected_ < 0) return;
    GameStats& st = user_.stats(cfg_.games[selected_].id);
    st.favorite = !st.favorite;
    save();
    buildNav();
    if (navItems_[navIndex_].key == "fav") rebuildGrid();
    select(selected_);
    InvalidateRect(grid_, nullptr, FALSE);
    InvalidateRect(nav_, nullptr, FALSE);
}

void Launcher::onPlayerChoice() {
    if (selected_ < 0) return;
    int sel = static_cast<int>(SendMessageW(playerCombo_, CB_GETCURSEL, 0, 0));
    auto* p = sel > 0 ? reinterpret_cast<const PlayerSpec*>(SendMessageW(playerCombo_, CB_GETITEMDATA, sel, 0)) : nullptr;
    user_.stats(cfg_.games[selected_].id).player = p ? p->id : std::string();
    save();
    select(selected_);
    InvalidateRect(grid_, nullptr, FALSE);
}

void Launcher::save() {
    if (snapshot_) return;
    std::wstring err;
    if (!saveUserData(user_, userPath_, &err)) logger().warn(L"Could not save settings: " + err);
}

std::wstring Launcher::formatDuration(int64_t s) const {
    if (s < 60) return std::to_wstring(s) + L" s";
    if (s < 3600) return std::to_wstring(s / 60) + L" min";
    return std::to_wstring(s / 3600) + L" h " + std::to_wstring((s % 3600) / 60) + L" min";
}

std::wstring Launcher::formatAgo(int64_t t) const {
    if (t <= 0) return cfg_.t("time.never");
    int64_t d = unixNow() - t;
    if (d < 60) return cfg_.t("time.justNow");
    if (d < 3600) return cfg_.t("time.minutesAgo", {{L"N", std::to_wstring(d / 60)}});
    if (d < 86400) return cfg_.t("time.hoursAgo", {{L"N", std::to_wstring(d / 3600)}});
    return cfg_.t("time.daysAgo", {{L"N", std::to_wstring(d / 86400)}});
}

// ---- message handling -----------------------------------------------------

LRESULT CALLBACK Launcher::wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Launcher* self = reinterpret_cast<Launcher*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<Launcher*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;
    }
    if (!self) return DefWindowProcW(h, m, w, l);
    return self->handle(m, w, l);
}

LRESULT CALLBACK Launcher::gridProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<Launcher*>(ref);
    if (m == WM_MOUSEMOVE || m == WM_MOUSELEAVE) {
        int item = -1;
        if (m == WM_MOUSEMOVE) {
            LVHITTESTINFO hit{};
            hit.pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            item = ListView_HitTest(h, &hit);
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
        }
        if (item != self->hoverItem_) {
            RECT r;
            if (self->hoverItem_ >= 0 && ListView_GetItemRect(h, self->hoverItem_, &r, LVIR_BOUNDS))
                InvalidateRect(h, &r, FALSE);
            self->hoverItem_ = item;
            if (item >= 0 && ListView_GetItemRect(h, item, &r, LVIR_BOUNDS)) InvalidateRect(h, &r, FALSE);
        }
    } else if (m == WM_SETFOCUS || m == WM_KILLFOCUS) {
        InvalidateRect(h, nullptr, FALSE);  // focus ring on the focused card
    } else if (m == WM_SIZE) {
        LRESULT r = DefSubclassProc(h, m, w, l);
        self->layoutGrid();
        return r;
    }
    return DefSubclassProc(h, m, w, l);
}

LRESULT CALLBACK Launcher::searchProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    // The hint is drawn here instead of with EM_SETCUEBANNER: Windows draws that in
    // the system gray, which is only 3.1:1 on the dark search box.
    LRESULT r = DefSubclassProc(h, m, w, l);
    if (m == WM_PAINT && GetWindowTextLengthW(h) == 0) {
        auto* self = reinterpret_cast<Launcher*>(ref);
        HDC dc = GetDC(h);
        RECT rc;
        GetClientRect(h, &rc);
        rc.left += self->theme_.s(4);
        HGDIOBJ old = SelectObject(dc, self->theme_.body);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, self->theme_.c.textMuted.ref());
        std::wstring cue = self->cfg_.t("search.cue");
        DrawTextW(dc, cue.c_str(), static_cast<int>(cue.size()), &rc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(h, dc);
    }
    return r;
}

LRESULT CALLBACK Launcher::buttonProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<Launcher*>(ref);
    if (m == WM_MOUSEMOVE && self->hoverBtn_ != h) {
        HWND old = self->hoverBtn_;
        self->hoverBtn_ = h;
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        if (old) InvalidateRect(old, nullptr, FALSE);
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_MOUSELEAVE && self->hoverBtn_ == h) {
        self->hoverBtn_ = nullptr;
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_ERASEBKGND) {
        return 1;  // drawn completely in NM_CUSTOMDRAW
    }
    return DefSubclassProc(h, m, w, l);
}

LRESULT Launcher::handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_SIZE:
            if (w != SIZE_MINIMIZED) relayout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(l);
            UINT dpi = GetDpiForWindow(hwnd_);
            mm->ptMinTrackSize = {MulDiv(cfg_.branding.window.minWidth, dpi, 96),
                                  MulDiv(cfg_.branding.window.minHeight, dpi, 96)};
            return 0;
        }
        case WM_DPICHANGED: {
            theme_.build(HIWORD(w));
            applyFonts();
            artCache_.clear();
            tileW_ = tileH_ = 0;
            auto* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, width(*r), height(*r), SWP_NOZORDER | SWP_NOACTIVATE);
            relayout();
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_SYSCOLORCHANGE:
            theme_.build(theme_.dpi);
            applyFonts();
            artCache_.clear();
            relayout();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            paint(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(w), theme_.c.text.ref());
            SetBkColor(reinterpret_cast<HDC>(w), theme_.c.surface.ref());
            return reinterpret_cast<LRESULT>(theme_.brSurface);
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(w);
            bool sidebar = reinterpret_cast<HWND>(l) == nav_;
            SetTextColor(dc, (sidebar ? theme_.c.sidebarText : theme_.c.text).ref());
            SetBkColor(dc, (sidebar ? theme_.c.sidebar : theme_.c.surface).ref());
            return reinterpret_cast<LRESULT>(sidebar ? theme_.brSidebar : theme_.brSurface);
        }
        case WM_MEASUREITEM: {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(l);
            if (mi->CtlID == kIdNav) mi->itemHeight = theme_.s(38);
            return TRUE;
        }
        case WM_DRAWITEM: {
            auto* d = reinterpret_cast<DRAWITEMSTRUCT*>(l);
            if (d->CtlID == kIdNav) drawNavItem(*d);
            return TRUE;
        }
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(l);
            if (hdr->code == NM_CUSTOMDRAW && btnInfo_.count(hdr->hwndFrom)) {
                auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(l);
                if (cd->dwDrawStage == CDDS_PREPAINT) {
                    drawButton(hdr->hwndFrom, cd->hdc, cd->rc, cd->uItemState);
                    return CDRF_SKIPDEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            if (hdr->hwndFrom != grid_) break;
            if (hdr->code == NM_CUSTOMDRAW) {
                auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(l);
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    int idx = static_cast<int>(cd->nmcd.dwItemSpec);
                    RECT r;
                    if (idx >= 0 && idx < static_cast<int>(visible_.size()) &&
                        ListView_GetItemRect(grid_, idx, &r, LVIR_BOUNDS)) {
                        UINT state = ListView_GetItemState(grid_, idx, LVIS_SELECTED | LVIS_FOCUSED);
                        drawCard(cd->nmcd.hdc, visible_[idx], r, (state & LVIS_SELECTED) != 0,
                                 (state & LVIS_FOCUSED) && GetFocus() == grid_);
                    }
                    return CDRF_SKIPDEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            if (hdr->code == LVN_ITEMCHANGED && !suppressSelect_) {
                auto* nm = reinterpret_cast<NMLISTVIEW*>(l);
                if ((nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED))
                    select(static_cast<int>(nm->lParam));
            } else if (hdr->code == LVN_ITEMACTIVATE) {
                auto* nm = reinterpret_cast<NMITEMACTIVATE*>(l);
                if (nm->iItem >= 0 && nm->iItem < static_cast<int>(visible_.size())) runAction(visible_[nm->iItem], 0);
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(w), code = HIWORD(w);
            if (id == kIdNav && code == LBN_SELCHANGE) {
                navIndex_ = static_cast<size_t>(SendMessageW(nav_, LB_GETCURSEL, 0, 0));
                SendMessageW(navCombo_, CB_SETCURSEL, navIndex_, 0);
                user_.category = navItems_[navIndex_].key;
                rebuildGrid();
            } else if (id == kIdNavCombo && code == CBN_SELCHANGE) {
                navIndex_ = static_cast<size_t>(SendMessageW(navCombo_, CB_GETCURSEL, 0, 0));
                SendMessageW(nav_, LB_SETCURSEL, navIndex_, 0);
                user_.category = navItems_[navIndex_].key;
                rebuildGrid();
            } else if (id == kIdSearch && code == EN_CHANGE) {
                rebuildGrid();
            } else if (id == kIdSearch && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
                InvalidateRect(hwnd_, &searchBox_, FALSE);
            } else if (id == kIdPlayer && code == CBN_SELCHANGE) {
                onPlayerChoice();
            } else if (code == BN_CLICKED) {
                if (id >= kIdAction0) runAction(selected_, static_cast<size_t>(id - kIdAction0));
                else if (id == kIdFav) toggleFavorite();
                else if (id == kIdLocate) locateFile();
                else if (id == kIdFolder) openFolder();
                else if (id == kIdGetPlayer) getPlayer();
                else if (id == kIdStopHost) stopRelay(true);
                else if (id == kIdCopy) copyAddress();
                else if (id == IDOK && selected_ >= 0) runAction(selected_, 0);  // Enter outside the grid
            }
            return 0;
        }
        case WM_TIMER:
            if (!sessions_.empty() || (noticeUntil_ && GetTickCount64() >= noticeUntil_)) {
                if (GetTickCount64() >= noticeUntil_) noticeUntil_ = 0;
                InvalidateRect(hwnd_, &status_, FALSE);
            }
            return 0;
        case WM_APP_EXIT:
            onProcessExit(static_cast<DWORD>(w), static_cast<DWORD>(l));
            return 0;
        case WM_APP_RELAY:
            layoutDetail();
            return 0;
        case WM_CLOSE: {
            WINDOWPLACEMENT wp{sizeof wp};
            if (GetWindowPlacement(hwnd_, &wp)) {
                user_.x = wp.rcNormalPosition.left;
                user_.y = wp.rcNormalPosition.top;
                user_.width = width(wp.rcNormalPosition);
                user_.height = height(wp.rcNormalPosition);
                user_.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
            }
            save();
            DestroyWindow(hwnd_);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd_, kTimer);
            hwnd_ = nullptr;
            if (!snapshot_) PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

// ---- headless launch -----------------------------------------------------

int Launcher::launchHeadless(const Config& cfg, Library& lib, UserData& user, const std::wstring& userPath,
                             const GameSpec& game, const ActionSpec& action, std::wstring& message) {
    ResolvedAction r = lib.resolve(game, action, user);
    if (r.state != ActionState::Ready) {
        message = cfg.t("notice.notReady", {{L"GAME", game.name}, {L"REASON", stateReason(cfg, r)}});
        return 4;
    }
    Relay relay;
    if (action.relay.enabled) {
        std::wstring err;
        bool inUse = false;
        if (!relay.start(action.relay.port, err, &inUse) && !inUse) {
            message = cfg.t("host.failed", {{L"PORT", std::to_wstring(action.relay.port)}, {L"ERROR", err}});
            return 1;
        }
    }
    sys::Started p;
    std::wstring err;
    if (!sys::startProcess(r.playerExe, r.args, r.workDir, p, err)) {
        message = cfg.t("notice.launchFailed", {{L"PLAYER", r.player->name}, {L"ERROR", err}});
        return 1;
    }
    GameStats& st = user.stats(game.id);
    ++st.plays;
    st.lastPlayed = unixNow();
    saveUserData(user, userPath);
    message = action.hint.empty() ? cfg.t("notice.launched", {{L"GAME", game.name}, {L"PLAYER", r.player->name}})
                                  : action.hint;
    if (relay.running()) {
        ULONGLONG start = GetTickCount64();
        WaitForSingleObject(p.process, INFINITE);
        st.seconds += static_cast<int64_t>((GetTickCount64() - start) / 1000);
        saveUserData(user, userPath);
    }
    CloseHandle(p.process);
    return 0;
}

}  // namespace fl::ui
