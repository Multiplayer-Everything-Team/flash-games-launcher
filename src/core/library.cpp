#include "library.h"

#include <algorithm>

#include "fs.h"
#include "log.h"
#include "strings.h"
#include "win.h"

namespace fl {

Library::Library(const Config& cfg, Vars base) : cfg_(cfg), vars_(std::move(base)) {
    vars_.set(L"TITLE", cfg.branding.title);
    vars_.set(L"SUBTITLE", cfg.branding.subtitle);
    refresh();
}

bool Library::isUrl(const std::wstring& s) { return startsWithI(s, L"http://") || startsWithI(s, L"https://"); }

namespace {

// Expands one candidate; returns empty when it references a variable that is
// unknown or empty (e.g. ${FLASHPOINT} on a PC without Flashpoint).
std::wstring expandCandidate(const std::wstring& raw, const Vars& vars) {
    std::vector<std::wstring> unknown;
    std::wstring s = vars.expand(raw, &unknown);
    if (!unknown.empty()) return {};
    size_t i = 0;
    while ((i = raw.find(L"${", i)) != std::wstring::npos) {
        size_t end = raw.find(L'}', i);
        if (end == std::wstring::npos) break;
        std::wstring name = raw.substr(i + 2, end - i - 2);
        if (!startsWithI(name, L"env:") && !startsWithI(name, L"folder:") && vars.get(name).empty()) return {};
        i = end + 1;
    }
    return s;
}

bool naturalLess(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (iswdigit(a[i]) && iswdigit(b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && iswdigit(a[i2])) ++i2;
            while (j2 < b.size() && iswdigit(b[j2])) ++j2;
            unsigned long long x = wcstoull(a.substr(i, i2 - i).c_str(), nullptr, 10);
            unsigned long long y = wcstoull(b.substr(j, j2 - j).c_str(), nullptr, 10);
            if (x != y) return x < y;
            i = i2;
            j = j2;
            continue;
        }
        wchar_t x = towlower(a[i]), y = towlower(b[j]);
        if (x != y) return x < y;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

std::wstring globFile(const std::wstring& pattern) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(fs::longPath(pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::vector<std::wstring> names;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (names.empty()) return {};
    // Highest name in natural order wins, so "flashplayer_32_sa.exe" beats "flashplayer_9_sa.exe".
    std::sort(names.begin(), names.end(), naturalLess);
    return fs::join(fs::parent(pattern), names.back());
}

std::wstring searchPath(const std::wstring& name) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = SearchPathW(nullptr, name.c_str(), nullptr, static_cast<DWORD>(std::size(buf)), buf, nullptr);
    return n && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

}  // namespace

std::wstring Library::findFirst(const std::vector<std::wstring>& candidates, const Vars& vars) const {
    for (const auto& raw : candidates) {
        std::wstring s = expandCandidate(raw, vars);
        if (s.empty()) continue;
        if (isUrl(s)) return s;
        if (s.find(L'\\') == std::wstring::npos && s.find(L'/') == std::wstring::npos) {
            std::wstring found = searchPath(s);
            if (!found.empty()) return fs::normalize(found);
            continue;
        }
        s = fs::normalize(s);
        if (fs::fileName(s).find_first_of(L"*?") != std::wstring::npos) {
            std::wstring found = globFile(s);
            if (!found.empty()) return found;
        } else if (fs::isFile(s)) {
            return s;
        }
    }
    return {};
}

void Library::refresh() {
    vars_.set(L"GAMES_DIR", fs::normalize(vars_.expand(cfg_.paths.games)));
    vars_.set(L"PLAYERS_DIR", fs::normalize(vars_.expand(cfg_.paths.players)));
    vars_.set(L"DATA_DIR", fs::normalize(vars_.expand(cfg_.paths.data)));
    std::wstring flashpoint;
    for (const auto& raw : cfg_.paths.flashpoint) {
        std::wstring p = fs::normalize(vars_.expand(raw));
        if (!p.empty() && fs::isDir(fs::join(p, L"FPSoftware"))) {
            flashpoint = p;
            break;
        }
    }
    vars_.set(L"FLASHPOINT", flashpoint);

    players_.clear();
    for (const auto& p : cfg_.players) {
        PlayerStatus st;
        st.spec = &p;
        st.exe = findFirst(p.paths, vars_);
        logger().info(p.name + L": " + (st.found() ? st.exe : L"not found"));
        players_.push_back(std::move(st));
    }
}

const PlayerStatus* Library::player(const std::string& id) const {
    for (const auto& p : players_)
        if (p.spec->id == id) return &p;
    return nullptr;
}

std::vector<const PlayerSpec*> Library::allowedPlayers(const ActionSpec& a, const std::string& preferred) const {
    std::vector<const PlayerSpec*> out;
    if (a.players.empty()) {
        for (const auto& p : cfg_.players) out.push_back(&p);
    } else {
        for (const auto& id : a.players)
            if (auto p = cfg_.findPlayer(id)) out.push_back(p);
    }
    auto it = std::find_if(out.begin(), out.end(), [&](const PlayerSpec* p) { return p->id == preferred; });
    if (it != out.end()) std::rotate(out.begin(), it, it + 1);  // the user's choice goes first
    return out;
}

ResolvedAction Library::resolve(const GameSpec& g, const ActionSpec& a, const UserData& user) const {
    ResolvedAction r;
    r.game = &g;
    r.action = &a;
    const GameStats* stats = user.find(g.id);
    Vars v = vars_;
    v.set(L"GAME_ID", widen(g.id));
    v.set(L"GAME_NAME", g.name);
    v.set(L"PORT", std::to_wstring(a.relay.port));

    std::wstring userSwf;
    if (stats) {
        auto it = stats->swf.find(a.id);
        if (it != stats->swf.end() && fs::isFile(it->second)) userSwf = it->second;
    }

    bool anyPlayer = false, anySwf = false;
    ResolvedAction fallback;  // best partial match, reported when nothing is ready
    for (const PlayerSpec* p : allowedPlayers(a, stats ? stats->player : std::string())) {
        const PlayerStatus* st = player(p->id);
        auto o = a.overrides.find(p->id);
        const PlayerOverride* ov = o == a.overrides.end() ? nullptr : &o->second;
        std::wstring swf = userSwf;
        if (swf.empty()) swf = findFirst(ov && ov->hasSwf ? ov->swf : a.swf, v);
        bool hasExe = st && st->found();
        anyPlayer = anyPlayer || hasExe;
        anySwf = anySwf || !swf.empty();

        ResolvedAction c = r;
        c.player = p;
        c.playerExe = hasExe ? st->exe : std::wstring();
        c.swf = swf;
        c.swfIsUrl = isUrl(swf);
        c.userSwf = !userSwf.empty();
        if (hasExe && !swf.empty()) {
            Vars av = v;
            av.set(L"SWF", swf);
            av.set(L"SWF_DIR", c.swfIsUrl ? fs::parent(c.playerExe) : fs::parent(swf));
            bool mentionsSwf = false;
            auto add = [&](const std::vector<std::wstring>& list) {
                for (const auto& arg : list) {
                    if (arg.find(L"${SWF}") != std::wstring::npos) mentionsSwf = true;
                    c.args.push_back(av.expand(arg));
                }
            };
            add(p->args);
            add(ov && ov->hasArgs ? ov->args : a.args);
            if (!mentionsSwf) c.args.push_back(swf);
            // Flash loads a game's extra files relative to the working folder.
            c.workDir = av.get(L"SWF_DIR");
            c.state = ActionState::Ready;
            return c;
        }
        // Prefer an installed player; among those (or with none installed), one that has the file.
        if (!fallback.player || (hasExe && fallback.playerExe.empty()) ||
            (!swf.empty() && fallback.swf.empty() && hasExe == !fallback.playerExe.empty()))
            fallback = c;
    }
    // An installed player without its file is a file problem (download or locate
    // it), even when another player's copy "exists", such as a web address for a
    // player that isn't installed. Only with no player at all is it a player problem.
    fallback.state = !anyPlayer && anySwf ? ActionState::NoPlayer : ActionState::NoSwf;
    return fallback;
}

ActionState Library::gameState(const GameSpec& g, const UserData& user) const {
    ActionState best = ActionState::NoSwf;
    for (const auto& a : g.actions) {
        ActionState s = resolve(g, a, user).state;
        if (s == ActionState::Ready) return s;
        if (s == ActionState::NoPlayer) best = s;
    }
    return best;
}

}  // namespace fl
