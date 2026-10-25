// The launcher window: where every control and panel goes.
#include "launcher.h"

#include <algorithm>
#include <cwctype>
#include <regex>

#include "core/embedded.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/motion.h"
#include "core/strings.h"
#include "launcher_internal.h"
// win.h (via theme.h) first: these need the full Windows types.
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace fl::ui {

using namespace Gdiplus;
using namespace detail;

// ---- layout ---------------------------------------------------------------

bool Launcher::isNarrow() const {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    return cr.right < theme_.s(kNarrowBelow);
}

void Launcher::relayout() {
    endIntro();
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
    int brandH = t.s(86), setH = t.s(36);
    int setTop = sidebar_.bottom - t.s(12) - setH;
    MoveWindow(nav_, t.s(12), brandH, sw - t.s(24), setTop - t.s(8) - brandH, TRUE);

    int x = top_.left + t.s(kPad), boxH = t.s(36), y = (height(top_) - boxH) / 2;
    int searchRight = top_.right - t.s(kPad);
    if (narrowMode) {  // no sidebar: Settings moves to the end of the top bar
        int bw = t.s(96);
        MoveWindow(settingsBtn_, searchRight - bw, y, bw, boxH, TRUE);
        searchRight -= bw + t.s(10);
    } else {
        MoveWindow(settingsBtn_, t.s(12), setTop, sw - t.s(24), setH, TRUE);
    }
    if (narrowMode) {
        int cw = t.s(170);
        MoveWindow(navCombo_, x, y + (boxH - t.s(28)) / 2, cw, t.s(300), TRUE);
        x += cw + t.s(10);
    }
    searchBox_ = {x, y, std::max<LONG>(x + t.s(120), static_cast<LONG>(searchRight)), y + boxH};
    // The edit sits inside a painted rounded box; center its single text line.
    HDC dc = GetDC(hwnd_);
    int lineH = textHeight(dc, t.body, L"Ag", 100, DT_SINGLELINE);
    ReleaseDC(hwnd_, dc);
    MoveWindow(search_, searchBox_.left + t.s(30), searchBox_.top + (boxH - lineH) / 2, width(searchBox_) - t.s(40),
               lineH, TRUE);
    MoveWindow(grid_, gridRc_.left + t.s(kPad) - t.s(kGap) / 2, gridRc_.top,
               width(gridRc_) - t.s(kPad) * 2 + t.s(kGap), height(gridRc_), TRUE);
    layoutGrid();
    layoutRoomsPage();
    layoutDetail();
    layoutToast();
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
    int th = static_cast<int>(cardW * 0.5625) + theme_.s(94) + theme_.s(kGap);
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
    if (intro_) {
        intro_->layoutLater = true;
        return;
    }
    dl_ = {};
    tabRows_.clear();
    std::vector<HWND> all = {playerCombo_, favBtn_,      locateBtn_,   folderBtn_,  getPlayerBtn_,
                             stopHostBtn_, copyBtn_,     downloadBtn_, tabPlayBtn_, tabRoomsBtn_,
                             moreBtn_,     quickBtn_,    tabCodeEdit_, tabCodeBtn_};
    all.insert(all.end(), roomBtns_.begin(), roomBtns_.end());
    all.insert(all.end(), actionBtns_.begin(), actionBtns_.end());
    if (selected_ < 0 || width(detail_) <= 0) {
        for (HWND h : all) ShowWindow(h, SW_HIDE);
        return;
    }
    // Controls placed below are collected here and shown together at the end;
    // hiding everything first would throw keyboard focus out of the panel.
    std::set<HWND> shown;
    const Theme& t = theme_;
    const GameSpec& g = cfg_.games[selected_];
    HDC dc = GetDC(hwnd_);
    int x = detail_.left + t.s(kPad), w = width(detail_) - t.s(kPad) * 2, y = t.s(kPad);
    // The header (art, title, meta, status) is placed after the controls so the
    // art can shrink on short windows instead of running into the buttons.
    int titleH = textHeight(dc, t.title, g.name, w);
    std::wstring meta = categoryText(g);
    int metaH = meta.empty() ? 0 : textHeight(dc, t.caption, meta, w);
    bool hostingThis = relay_.running() && relayGame_ == selected_;
    int slotsH = g.maxPlayers > 0 ? t.s(hostingThis ? 56 : 40) : 0;
    // Games with online rooms get two tabs: Play (the actions) and Rooms.
    bool tabs = !g.onlineId.empty() && onlineOn_ && !g.comingSoon;
    if (!tabs) detailTab_ = 0;
    bool roomsTab = detailTab_ == 1;
    int tabsH = tabs ? t.s(36) : 0;

    // Everything below the description is laid out bottom-up from the panel's
    // end, so the buttons never move when the description is long.
    int bottom = detail_.bottom - t.s(kPad);
    int rowH = t.s(34), actionH = t.s(54), gap = t.s(8), roomRowH = t.s(40);
    const GameStats* st = user_.find(g.id);
    // Originals show only when Settings asks; "more" actions (direct IP, LAN)
    // wait behind "Other ways to connect".
    std::vector<size_t> mainActs, moreActs;
    for (size_t i = 0; i < g.actions.size(); ++i) {
        if (!actionVisible(selected_, g.actions[i])) continue;
        (g.actions[i].more ? moreActs : mainActs).push_back(i);
    }
    std::vector<size_t> listed = mainActs;
    if (showMore_) listed.insert(listed.end(), moreActs.begin(), moreActs.end());
    bool quick = tabs && roomsKnown_ && !g.joinAction.empty() && !notOnServer(g);
    const ActionSpec* missingSwf = nullptr;
    const PlayerSpec* missingPlayer = nullptr;
    const ServerSpec* missingRuntime = nullptr;
    // A PC mod whose game can't be found asks for its folder.
    const ModView* mv = g.mod ? modView(selected_) : nullptr;
    bool needFolder = mv && !mv->status.game.found();
    for (size_t i : listed) {
        const ActionSpec& a = g.actions[i];
        if (a.isMod()) continue;
        ResolvedAction r = lib_.resolve(g, a, user_);
        if (r.state == ActionState::NoSwf && !missingSwf) missingSwf = &a;
        if (r.state == ActionState::NoPlayer && !missingPlayer && r.player) missingPlayer = r.player;
        if (r.state == ActionState::NoRuntime && !missingRuntime) missingRuntime = &a.server;
    }
    std::vector<HostLine> hostLinesView = hostView();
    bool hosting = !hostLinesView.empty();
    int hostLines = static_cast<int>(hostLinesView.size());
    int hostLineH = textHeight(dc, t.caption, L"Ag", 100, DT_SINGLELINE) + t.s(4);
    int hostBoxH = t.s(14) + hostLines * hostLineH + t.s(8) + rowH + t.s(14);
    int comboH = t.s(30);
    bool picker = SendMessageW(playerCombo_, CB_GETCOUNT, 0, 0) > 2;  // "Automatic" + 2 players
    std::vector<CatalogFile> downloadable = missingSwf ? missingGameFiles(g) : std::vector<CatalogFile>{};
    bool isUpdate = downloadable.empty();
    if (isUpdate) downloadable = outdatedFor(g);
    std::vector<RoomInfo> roomList;
    if (tabs)
        if (auto it = rooms_.find(g.id); it != rooms_.end()) roomList = it->second;

    auto place = [&](HWND b, bool show) {
        if (!show) return;
        MoveWindow(b, x, bottom - rowH, w, rowH, TRUE);
        shown.insert(b);
        bottom -= rowH + gap;
    };
    int headerBottom = bottom;
    if (!roomsTab) {
        // Short windows: thinner action buttons (no second line) and no player
        // picker, so the game's name and state stay visible above the buttons.
        int fixed = rowH + gap + (missingPlayer || missingRuntime ? rowH + gap : 0) + (missingSwf || needFolder ? rowH + gap : 0) +
                    (downloadable.empty() ? 0 : rowH + gap) + (hosting ? hostBoxH + t.s(12) : 0) +
                    (moreActs.empty() ? 0 : rowH + gap) +
                    (showMore_ ? static_cast<int>(moreActs.size()) * (actionH + gap) : 0);
        int headerMin = titleH + t.s(4 + 24 + 16) + (slotsH ? slotsH + t.s(12) : 0) + (tabs ? tabsH + t.s(12) : 0);
        int actionsH = static_cast<int>(mainActs.size() + (quick ? 1 : 0)) * (actionH + gap) +
                       (picker ? comboH + t.s(16) : 0);
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
        shown.insert(favBtn_);
        shown.insert(folderBtn_);
        bottom -= rowH + gap;

        // Fix-it buttons for whatever stops the game from starting.
        if (missingPlayer)
            SetWindowTextW(getPlayerBtn_,
                           cfg_.t(missingPlayer->install.enabled() ? "button.installPlayer" : "button.getPlayer",
                                  {{L"PLAYER", missingPlayer->name}})
                               .c_str());
        if (!missingPlayer && missingRuntime)
            SetWindowTextW(getPlayerBtn_, cfg_.t("button.getRuntime", {{L"RUNTIME", missingRuntime->name}}).c_str());
        EnableWindow(getPlayerBtn_, !installing_);
        btnInfo_[getPlayerBtn_].busy = installing_ && !jobIsGame_;
        place(getPlayerBtn_, missingPlayer != nullptr || missingRuntime != nullptr);
        if (missingSwf)
            SetWindowTextW(locateBtn_, (listed.size() > 1 ? cfg_.t("button.locateFor", {{L"ACTION", missingSwf->label}})
                                                          : cfg_.t("button.locate"))
                                           .c_str());
        if (needFolder) {
            mod::Spec ms = modSpec(selected_);
            SetWindowTextW(locateBtn_, ms.t("mod.chooseFolder", mod::withSpec(lib_.vars(), ms)).c_str());
        }
        place(locateBtn_, missingSwf != nullptr || needFolder);
        if (!downloadable.empty()) {
            uint64_t bytes = 0;
            for (const auto& f : downloadable) bytes += f.size;
            SetWindowTextW(downloadBtn_, cfg_.t(isUpdate ? "button.updateGame" : "button.downloadGame",
                                                {{L"SIZE", formatBytes(bytes)}})
                                             .c_str());
            // Missing files block play, so that fix leads; an update is optional.
            btnInfo_[downloadBtn_].style = isUpdate ? BtnStyle::Secondary : BtnStyle::Primary;
        }
        EnableWindow(downloadBtn_, !installing_);
        btnInfo_[downloadBtn_].busy = installing_ && jobIsGame_;
        place(downloadBtn_, !downloadable.empty());

        // Hosting box: relay status, peers, this PC's addresses.
        if (hosting) {
            dl_.host = {x, bottom - hostBoxH, x + w, bottom};
            int by = dl_.host.top + t.s(14);
            for (int i = 0; i < hostLines; ++i, by += hostLineH)
                dl_.hostLines.push_back({x + t.s(14), by, x + w - t.s(14), by + hostLineH});
            int bw = (w - t.s(28) - gap) / 2;
            MoveWindow(copyBtn_, x + t.s(14), dl_.host.bottom - t.s(14) - rowH, bw, rowH, TRUE);
            MoveWindow(stopHostBtn_, x + t.s(14) + bw + gap, dl_.host.bottom - t.s(14) - rowH,
                       w - t.s(28) - bw - gap, rowH, TRUE);
            SetWindowTextW(copyBtn_, cfg_.t("button.copyAddress").c_str());
            SetWindowTextW(stopHostBtn_, cfg_.t("button.stopHost").c_str());
            shown.insert(copyBtn_);
            shown.insert(stopHostBtn_);
            bottom = dl_.host.top - t.s(12);
        }

        // Action buttons, bottom-up: the "more" ones under their toggle, then the
        // rest, Quick play right under the leading action.
        auto placeAction = [&](size_t i) {
            HWND b = actionBtns_[i];
            const ActionSpec& a = g.actions[i];
            bool starting = std::any_of(sessions_.begin(), sessions_.end(), [&](const Session& s) {
                return s.game == selected_ && s.action == a.id && s.server && !s.pid;
            });
            bool modWorking = a.isMod() && modJobGame_ == selected_ && a.modOp == modJobOp_;
            btnInfo_[b].busy = starting || modWorking;
            btnInfo_[b].progress = modWorking;
            MoveWindow(b, x, bottom - actionH, w, actionH, TRUE);
            shown.insert(b);
            bottom -= actionH + gap;
        };
        if (showMore_)
            for (size_t k = moreActs.size(); k-- > 0;) placeAction(moreActs[k]);
        if (!moreActs.empty()) {
            SetWindowTextW(moreBtn_, cfg_.t(showMore_ ? "button.less" : "button.more").c_str());
            btnInfo_[moreBtn_].icon = showMore_ ? kIconLess : kIconMore;
            place(moreBtn_, true);
        }
        for (size_t k = mainActs.size(); k-- > 1;) placeAction(mainActs[k]);
        if (quick) {
            const RoomInfo* busiest = nullptr;
            for (const auto& r : roomList)
                if ((r.max <= 0 || r.players < r.max) && (!busiest || r.players > busiest->players)) busiest = &r;
            SetWindowTextW(quickBtn_, cfg_.t("button.quickPlay").c_str());
            btnInfo_[quickBtn_].sub = cfg_.t(busiest ? "button.quickJoin" : "button.quickHost");
            MoveWindow(quickBtn_, x, bottom - actionH, w, actionH, TRUE);
            shown.insert(quickBtn_);
            bottom -= actionH + gap;
        }
        if (!mainActs.empty()) placeAction(mainActs[0]);

        // Player picker, only worth showing when there is a choice.
        if (picker) {
            bottom -= t.s(4);
            int labelW = textWidth(dc, t.strong, cfg_.t("detail.player")) + t.s(12);
            dl_.playerLabel = {x, bottom - comboH, x + labelW, bottom};
            MoveWindow(playerCombo_, x + labelW, bottom - comboH, w - labelW, t.s(240), TRUE);
            shown.insert(playerCombo_);
            bottom -= comboH + t.s(12);
        }
        headerBottom = bottom;
    } else {
        // Rooms tab: join by code at the bottom, the room rows above it.
        int jbw = textWidth(dc, t.button, cfg_.t("roomsPage.joinCode")) + t.s(32);
        int lineH = textHeight(dc, t.button, L"Ag", 100, DT_SINGLELINE);
        dl_.codeBox = {x, bottom - rowH, x + w - jbw - gap, bottom};
        MoveWindow(tabCodeEdit_, dl_.codeBox.left + t.s(12), dl_.codeBox.top + (rowH - lineH) / 2,
                   width(dl_.codeBox) - t.s(20), lineH, TRUE);
        MoveWindow(tabCodeBtn_, x + w - jbw, bottom - rowH, jbw, rowH, TRUE);
        shown.insert(tabCodeEdit_);
        shown.insert(tabCodeBtn_);
        bottom -= rowH + t.s(14);
        headerBottom = bottom - roomRowH * 2;  // the header gives way to at least two rows
    }

    // The art gives way before the description does: keep room for two lines of it.
    int descMin = roomsTab || g.description.empty() ? 0 : textHeight(dc, t.body, L"Ag", w, DT_SINGLELINE) * 2 + t.s(10);
    int headerH = titleH + t.s(4) + metaH + t.s(10) + (slotsH ? slotsH + t.s(12) : 0) + t.s(24) + t.s(12) +
                  (tabs ? tabsH + t.s(12) : 0) + descMin;
    int artH = std::min(static_cast<int>(w * 0.5625), t.s(200));
    int room = headerBottom - t.s(8) - y - headerH - t.s(16);
    if (room < artH) artH = room >= t.s(72) ? room : 0;
    if (artH > 0) {
        dl_.art = {x, y, x + w, y + artH};
        y += artH + t.s(16);
    }
    // Each header line is shown only while it still fits above the controls.
    int limit = headerBottom - t.s(8);
    if (y + titleH <= limit) {
        dl_.title = {x, y, x + w, y + titleH};
        y += titleH + t.s(4);
    }
    if (metaH && y + metaH + t.s(10 + 24) <= limit) {
        dl_.meta = {x, y, x + w, y + metaH};
        y += metaH + t.s(10);
    }
    if (slotsH && y + slotsH + t.s(12) <= limit) {
        dl_.slots = {x, y, x + w, y + slotsH};
        y += slotsH + t.s(12);
    }
    if (y + t.s(24) <= limit) {
        dl_.status = {x, y, x + w, y + t.s(24)};
        y += t.s(24) + t.s(12);
    }
    if (tabs) {
        std::wstring playLabel = cfg_.t("tab.play");
        std::wstring roomsLabel = roomList.empty() ? cfg_.t("tab.rooms")
                                                   : cfg_.t("tab.roomsCount", {{L"N", std::to_wstring(roomList.size())}});
        int tw1 = textWidth(dc, t.button, playLabel) + t.s(28), tw2 = textWidth(dc, t.button, roomsLabel) + t.s(28);
        dl_.tabs = {x, y, x + w, y + tabsH};
        SetWindowTextW(tabPlayBtn_, playLabel.c_str());
        SetWindowTextW(tabRoomsBtn_, roomsLabel.c_str());
        btnInfo_[tabPlayBtn_].selected = !roomsTab;
        btnInfo_[tabRoomsBtn_].selected = roomsTab;
        MoveWindow(tabPlayBtn_, x, y, tw1, tabsH, TRUE);
        MoveWindow(tabRoomsBtn_, x + tw1, y, tw2, tabsH, TRUE);
        shown.insert(tabPlayBtn_);
        shown.insert(tabRoomsBtn_);
        y += tabsH + t.s(12);
    }

    if (roomsTab) {
        dl_.rooms = {x, y, x + w, bottom};
        size_t fit = static_cast<size_t>(std::max(0, (bottom - y) / roomRowH));
        size_t n = std::min(roomList.size(), fit);
        while (roomBtns_.size() < n) {
            HWND b = makeButton(kIdRoom0 + static_cast<int>(roomBtns_.size()), BtnStyle::Secondary);
            btnInfo_[b].label = cfg_.t("rooms.join");
            roomBtns_.push_back(b);
            all.push_back(b);
        }
        for (size_t i = 0; i < n; ++i) {
            RECT row{x, y + static_cast<int>(i) * roomRowH, x + w, y + static_cast<int>(i + 1) * roomRowH};
            tabRows_.push_back({selected_, roomList[i], row});
            int bw = t.s(64);
            MoveWindow(roomBtns_[i], row.right - bw, row.top + t.s(4), bw, roomRowH - t.s(8), TRUE);
            // The accessible name says which room; the button itself just reads "Join".
            SetWindowTextW(roomBtns_[i], cfg_.t("rooms.joinName", {{L"CODE", roomList[i].code}}).c_str());
            EnableWindow(roomBtns_[i], roomList[i].max <= 0 || roomList[i].players < roomList[i].max);
            shown.insert(roomBtns_[i]);
        }
    } else {
        // Text between the tabs and the controls, dropped when it doesn't fit.
        int textBottom = bottom - t.s(8);
        std::wstring stats = statsText(st);
        int statsH = textHeight(dc, t.caption, stats, w);
        if (textBottom - statsH >= y) {
            dl_.stats = {x, textBottom - statsH, x + w, textBottom};
            textBottom = dl_.stats.top - t.s(10);
        }
        dl_.desc = {x, y, x + w, std::max(y, textBottom)};
        descText_ = fitText(dc, t.body, g.description, w, height(dl_.desc));
    }
    ReleaseDC(hwnd_, dc);
    for (HWND h : all) ShowWindow(h, shown.count(h) ? SW_SHOW : SW_HIDE);
    InvalidateRect(hwnd_, &detail_, FALSE);
    for (HWND hb : all) InvalidateRect(hb, nullptr, FALSE);
    wake();
}

}  // namespace fl::ui
