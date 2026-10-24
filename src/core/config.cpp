#include "config.h"

#include <algorithm>
#include <set>

#include "fs.h"
#include "json.h"
#include "strings.h"

namespace fl {

bool parseColor(const std::wstring& text, Color& out) {
    std::wstring s = trim(text);
    if (s.empty() || s[0] != L'#') return false;
    s = s.substr(1);
    for (wchar_t c : s)
        if (!iswxdigit(c)) return false;
    auto hex = [&](size_t pos, size_t len) {
        return static_cast<uint8_t>(wcstoul(s.substr(pos, len).c_str(), nullptr, 16) * (len == 1 ? 17 : 1));
    };
    if (s.size() == 3) {
        out = {hex(0, 1), hex(1, 1), hex(2, 1), 255};
    } else if (s.size() == 6 || s.size() == 8) {
        out = {hex(0, 2), hex(2, 2), hex(4, 2), s.size() == 8 ? hex(6, 2) : uint8_t(255)};
    } else {
        return false;
    }
    return true;
}

static Color rgb(uint32_t v) { return {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v), 255}; }

ThemeColors darkPalette() {
    ThemeColors c;
    c.background = rgb(0x15171C);
    c.surface = rgb(0x1E2128);
    c.surfaceHover = rgb(0x272B34);
    c.sidebar = rgb(0x0E1014);
    c.sidebarText = rgb(0xF2F4F7);
    c.sidebarTextMuted = rgb(0x8B93A1);
    c.primary = rgb(0xC8313A);  // the arcade start button; white on it is 5.3:1
    c.primaryHover = rgb(0xB42A33);
    c.onPrimary = rgb(0xFFFFFF);
    c.text = rgb(0xE8EAED);
    c.textMuted = rgb(0x9AA1AC);
    c.border = rgb(0x323743);
    c.focus = rgb(0x8FB0FF);
    c.success = rgb(0x3CCB7F);
    c.warning = rgb(0xF2B544);
    c.error = rgb(0xFF6464);
    c.online = rgb(0x8FB0FF);
    return c;
}

ThemeColors lightPalette() {
    ThemeColors c;
    c.background = rgb(0xFFFFFF);
    c.surface = rgb(0xF4F6F9);
    c.surfaceHover = rgb(0xE9EDF3);
    c.sidebar = rgb(0x1B2030);
    c.sidebarText = rgb(0xF2F4F7);
    c.sidebarTextMuted = rgb(0xA3ABBA);
    c.primary = rgb(0xB42A33);
    c.primaryHover = rgb(0x9C232B);
    c.onPrimary = rgb(0xFFFFFF);
    c.text = rgb(0x1C2128);
    c.textMuted = rgb(0x5B6472);
    c.border = rgb(0xD5DAE1);
    c.focus = rgb(0x2F6FEB);
    c.success = rgb(0x1F9D57);
    c.warning = rgb(0xB7791F);
    c.error = rgb(0xD93636);
    c.online = rgb(0x0E9C90);
    return c;
}

std::vector<std::pair<const char*, Color*>> colorFields(ThemeColors& c) {
    return {{"background", &c.background},
            {"surface", &c.surface},
            {"surfaceHover", &c.surfaceHover},
            {"sidebar", &c.sidebar},
            {"sidebarText", &c.sidebarText},
            {"sidebarTextMuted", &c.sidebarTextMuted},
            {"primary", &c.primary},
            {"primaryHover", &c.primaryHover},
            {"onPrimary", &c.onPrimary},
            {"text", &c.text},
            {"textMuted", &c.textMuted},
            {"border", &c.border},
            {"focus", &c.focus},
            {"success", &c.success},
            {"warning", &c.warning},
            {"error", &c.error},
            {"online", &c.online}};
}

const std::vector<std::pair<std::string, std::wstring>>& defaultTexts() {
    static const std::vector<std::pair<std::string, std::wstring>> texts = {
        {"nav.all", L"All games"},
        {"nav.favorites", L"Favorites"},
        {"nav.ready", L"Ready to play"},
        {"search.cue", L"Search games  (Ctrl+F)"},
        {"status.ready", L"Ready"},
        {"status.noPlayer", L"No Flash player found"},
        {"status.noSwf", L"Game file missing"},
        {"status.playing", L"Playing"},
        {"badge.online", L"Online"},
        {"slot.label", L"P${N}"},
        {"slot.open", L"Open"},
        {"slot.host", L"${NAME} (host)"},
        {"detail.playerAuto", L"Automatic (${PLAYER})"},
        {"detail.playerAutoNone", L"Automatic"},
        {"detail.player", L"Player"},
        {"detail.players", L"Players"},
        {"detail.controls", L"Controls"},
        {"detail.never", L"Not played yet"},
        {"detail.stats", L"Played ${COUNT} times for ${TIME} in total, last ${WHEN}."},
        {"detail.statsOnce", L"Played once for ${TIME}, ${WHEN}."},
        {"detail.empty", L"Pick a game on the left."},
        {"button.favorite", L"\u2606  Favorite"},
        {"button.unfavorite", L"\u2605  Favorite"},
        {"button.locate", L"Locate game file\u2026"},
        {"button.locateFor", L"Locate file for \u201C${ACTION}\u201D\u2026"},
        {"button.folder", L"Open folder"},
        {"button.getPlayer", L"Get ${PLAYER}"},
        {"button.installPlayer", L"Install ${PLAYER}"},
        {"button.downloadGame", L"Download game files (${SIZE})"},
        {"download.title", L"Download ${GAME}?"},
        {"download.body", L"The launcher downloads ${COUNT} file(s), ${SIZE} in total, from ${SOURCE}, checks each one against its published checksum, and saves them in ${DIR}."},
        {"download.yes", L"Download"},
        {"download.downloading", L"Downloading ${GAME}: ${DONE} of ${TOTAL}"},
        {"download.done", L"${GAME} is downloaded and ready."},
        {"download.failed", L"Couldn't download ${GAME}: ${ERROR}"},
        {"install.title", L"Install ${PLAYER}?"},
        {"install.firstRun", L"No Flash player was found on this PC, so games can't start yet."},
        {"install.body", L"The launcher downloads the latest ${PLAYER} release from ${SOURCE}, checks it, and puts it in ${DIR}. ${PLAYER} is free and open source."},
        {"install.yes", L"Install ${PLAYER}"},
        {"install.no", L"Not now"},
        {"install.finding", L"Looking up the latest ${PLAYER} release\u2026"},
        {"install.downloading", L"Downloading ${PLAYER}: ${DONE} of ${TOTAL}"},
        {"install.unpacking", L"Unpacking ${PLAYER}\u2026"},
        {"install.done", L"${PLAYER} is installed. Games can start now."},
        {"install.failed", L"Couldn't install ${PLAYER}: ${ERROR}"},
        {"button.stopHost", L"Stop hosting"},
        {"button.copyAddress", L"Copy address"},
        {"host.running", L"Hosting on port ${PORT}"},
        {"host.waiting", L"Waiting for friends\u2026"},
        {"host.connected", L"${COUNT} connected"},
        {"host.you", L"(host)"},
        {"host.addresses", L"Friends join with:"},
        {"host.noAddress", L"No network found. Friends on this PC use 127.0.0.1:${PORT}"},
        {"host.portInUse", L"Port ${PORT} is already in use; using the relay that is already running."},
        {"host.failed", L"Could not start the relay on port ${PORT}: ${ERROR}"},
        {"notice.launched", L"Started ${GAME} in ${PLAYER}."},
        {"notice.quitFast", L"${PLAYER} closed right away (exit code ${CODE}). Details are in the log."},
        {"notice.launchFailed", L"Could not start ${PLAYER}: ${ERROR}"},
        {"notice.notReady", L"${GAME} can't start: ${REASON}"},
        {"notice.copied", L"Copied ${ADDRESS} to the clipboard."},
        {"notice.located", L"${GAME} will use ${FILE}."},
        {"notice.relayStopped", L"Stopped hosting."},
        {"bar.found", L"${PLAYER} \u2713"},
        {"bar.missing", L"${PLAYER} \u2717"},
        {"bar.flashpoint", L"Flashpoint: ${PATH}"},
        {"bar.playing", L"Playing ${GAME} for ${TIME}"},
        {"empty.title", L"No games here"},
        {"empty.body", L"Try another category or clear the search."},
        {"time.never", L"never"},
        {"time.justNow", L"just now"},
        {"time.minutesAgo", L"${N} min ago"},
        {"time.hoursAgo", L"${N} h ago"},
        {"time.daysAgo", L"${N} days ago"},
        {"dialog.locateTitle", L"Find the game file for ${GAME}"},
        {"dialog.swfFilter", L"Flash games (*.swf)"},
    };
    return texts;
}

std::wstring Config::t(const std::string& key,
                       std::initializer_list<std::pair<std::wstring_view, std::wstring_view>> vars) const {
    auto it = text.find(key);
    std::wstring s = it == text.end() ? widen(key) : it->second;
    for (const auto& [name, value] : vars) s = replaceAll(s, L"${" + std::wstring(name) + L"}", value);
    return s;
}

const GameSpec* Config::findGame(std::string_view id) const {
    for (const auto& g : games)
        if (g.id == id) return &g;
    return nullptr;
}

const PlayerSpec* Config::findPlayer(std::string_view id) const {
    for (const auto& p : players)
        if (p.id == id) return &p;
    return nullptr;
}

const Category* Config::findCategory(std::string_view id) const {
    for (const auto& c : categories)
        if (c.id == id) return &c;
    return nullptr;
}

bool LoadResult::ok() const {
    for (const auto& i : issues)
        if (i.severity == ConfigIssue::Severity::Error) return false;
    return true;
}

namespace {

using json::Type;
using json::Value;

const wchar_t* typeName(Type t) {
    switch (t) {
        case Type::Null: return L"null";
        case Type::Bool: return L"true/false";
        case Type::Number: return L"a number";
        case Type::String: return L"a string";
        case Type::Array: return L"a list";
        case Type::Object: return L"an object";
    }
    return L"?";
}

// Reads one JSON object into struct fields and reports type errors and
// unknown keys (usually typos) with their path.
class Section {
public:
    Section(const Value* v, std::wstring path, std::vector<ConfigIssue>& issues)
        : v_(v && v->isObject() ? v : nullptr), path_(std::move(path)), issues_(issues) {
        if (v && !v->isObject() && !v->isNull()) error(L"", L"must be an object");
    }
    ~Section() { reportUnknown(); }

    const Value* get(const char* key, Type type) {
        if (!v_) return nullptr;
        seen_.insert(key);
        const Value* f = v_->find(key);
        if (!f || f->isNull()) return nullptr;
        if (f->type() != type) {
            error(widen(key), std::wstring(L"must be ") + typeName(type) + L", found " + typeName(f->type()));
            return nullptr;
        }
        return f;
    }
    void str(const char* key, std::wstring& out) {
        if (auto f = get(key, Type::String)) out = widen(f->str());
    }
    void str(const char* key, std::string& out) {
        if (auto f = get(key, Type::String)) out = f->str();
    }
    void boolean(const char* key, bool& out) {
        if (auto f = get(key, Type::Bool)) out = f->boolean();
    }
    void integer(const char* key, int& out, int lo, int hi) {
        if (auto f = get(key, Type::Number)) {
            double d = f->number();
            if (d < lo || d > hi) error(widen(key), L"must be between " + std::to_wstring(lo) + L" and " + std::to_wstring(hi));
            else out = static_cast<int>(d);
        }
    }
    void real(const char* key, float& out, float lo, float hi) {
        if (auto f = get(key, Type::Number)) {
            double d = f->number();
            if (d < lo || d > hi) error(widen(key), L"is out of range");
            else out = static_cast<float>(d);
        }
    }
    void color(const char* key, Color& out) {
        std::wstring text;
        str(key, text);
        if (!text.empty() && !parseColor(text, out)) error(widen(key), L"must be a color like \"#4F8BFF\"");
    }
    // Accepts a single string as a one-item list (handy for "swf": "x.swf").
    void strList(const char* key, std::vector<std::wstring>& out, bool* present = nullptr) {
        if (!v_) return;
        seen_.insert(key);
        const Value* f = v_->find(key);
        if (!f || f->isNull()) return;
        if (present) *present = true;
        out.clear();
        if (f->isString()) {
            out.push_back(widen(f->str()));
            return;
        }
        if (!f->isArray()) {
            error(widen(key), L"must be a string or a list of strings");
            return;
        }
        for (size_t i = 0; i < f->size(); ++i) {
            if (f->at(i).isString()) out.push_back(widen(f->at(i).str()));
            else error(widen(key), L"must be a list of strings");
        }
    }
    void idList(const char* key, std::vector<std::string>& out) {
        std::vector<std::wstring> w;
        strList(key, w);
        for (const auto& s : w) out.push_back(narrow(s));
    }
    std::wstring child(const std::string& key) const { return path_.empty() ? widen(key) : path_ + L"." + widen(key); }
    void error(const std::wstring& key, const std::wstring& msg) {
        issues_.push_back({ConfigIssue::Severity::Error, key.empty() ? path_ : child(narrow(key)), msg});
    }

private:
    void reportUnknown() {
        if (!v_) return;
        for (size_t i = 0; i < v_->size(); ++i) {
            const std::string& k = v_->keyAt(i);
            if (k.empty() || k[0] == '_' || k[0] == '$') continue;  // "_comment" keys are allowed
            if (!seen_.count(k))
                issues_.push_back({ConfigIssue::Severity::Warning, child(k), L"unknown setting (typo?)"});
        }
    }
    const Value* v_;
    std::wstring path_;
    std::vector<ConfigIssue>& issues_;
    std::set<std::string> seen_;
};

std::wstring indexPath(const wchar_t* list, size_t i) { return std::wstring(list) + L"[" + std::to_wstring(i) + L"]"; }

ActionSpec readAction(const Value& v, const std::wstring& where, std::vector<ConfigIssue>& issues) {
    ActionSpec a;
    Section s(&v, where, issues);
    s.str("id", a.id);
    s.str("label", a.label);
    s.str("description", a.description);
    s.str("hint", a.hint);
    s.strList("swf", a.swf);
    s.strList("args", a.args);
    s.idList("players", a.players);
    if (auto o = s.get("overrides", Type::Object)) {
        for (size_t i = 0; i < o->size(); ++i) {
            PlayerOverride po;
            Section os(&o->at(i), s.child("overrides") + L"." + widen(o->keyAt(i)), issues);
            os.strList("swf", po.swf, &po.hasSwf);
            os.strList("args", po.args, &po.hasArgs);
            a.overrides[o->keyAt(i)] = std::move(po);
        }
    }
    if (auto r = s.get("relay", Type::Object)) {
        Section rs(r, s.child("relay"), issues);
        a.relay.enabled = true;
        rs.boolean("enabled", a.relay.enabled);
        rs.integer("port", a.relay.port, 1, 65535);
    }
    if (a.label.empty()) a.label = widen(a.id);
    return a;
}

}  // namespace

LoadResult loadConfig(std::string_view jsonText, const std::wstring& configDir) {
    LoadResult r;
    Config& c = r.config;
    auto& issues = r.issues;
    c.configDir = configDir;
    for (const auto& [k, v] : defaultTexts()) c.text[k] = v;

    std::string err;
    Value root = json::parse(jsonText, &err);
    if (!err.empty()) {
        issues.push_back({ConfigIssue::Severity::Error, L"launcher.json", widen(err)});
        return r;
    }
    if (!root.isObject()) {
        issues.push_back({ConfigIssue::Severity::Error, L"launcher.json", L"the top level must be an object"});
        return r;
    }

    Section top(&root, L"", issues);
    top.integer("schemaVersion", c.schemaVersion, 1, 1);
    {
        Section s(top.get("branding", Type::Object), L"branding", issues);
        Branding& b = c.branding;
        s.str("title", b.title);
        s.str("subtitle", b.subtitle);
        s.str("windowTitle", b.windowTitle);
        s.str("theme", b.theme);
        if (!iequals(b.theme, L"dark") && !iequals(b.theme, L"light")) s.error(L"theme", L"must be \"dark\" or \"light\"");
        b.colors = iequals(b.theme, L"light") ? lightPalette() : darkPalette();
        if (auto colors = s.get("colors", Type::Object)) {
            Section cs(colors, L"branding.colors", issues);
            for (auto& [name, field] : colorFields(b.colors)) cs.color(name, *field);
        }
        s.str("font", b.font);
        s.str("displayFont", b.displayFont);
        if (auto pc = s.get("playerColors", Type::Array)) {
            b.playerColors.clear();
            for (size_t i = 0; i < pc->size(); ++i) {
                Color col;
                if (!pc->at(i).isString() || !parseColor(widen(pc->at(i).str()), col))
                    s.error(L"playerColors", L"must be a list of colors like \"#E5484D\"");
                else
                    b.playerColors.push_back(col);
            }
            if (b.playerColors.empty()) s.error(L"playerColors", L"needs at least one color");
        }
        s.real("fontSize", b.fontSize, 7, 20);
        Section ws(s.get("window", Type::Object), L"branding.window", issues);
        ws.integer("width", b.window.width, 400, 8000);
        ws.integer("height", b.window.height, 300, 8000);
        ws.integer("minWidth", b.window.minWidth, 400, 8000);
        ws.integer("minHeight", b.window.minHeight, 300, 8000);
    }
    {
        Section s(top.get("paths", Type::Object), L"paths", issues);
        s.str("games", c.paths.games);
        s.str("players", c.paths.players);
        s.str("data", c.paths.data);
        s.strList("flashpoint", c.paths.flashpoint);
    }
    {
        Section s(top.get("behavior", Type::Object), L"behavior", issues);
        s.boolean("minimizeOnLaunch", c.behavior.minimizeOnLaunch);
        s.boolean("stopRelayWithGame", c.behavior.stopRelayWithGame);
        s.integer("quickExitSeconds", c.behavior.quickExitSeconds, 0, 60);
    }
    {
        Section s(top.get("downloads", Type::Object), L"downloads", issues);
        s.str("catalog", c.downloads.catalog);
        const std::wstring& u = c.downloads.catalog;
        if (!u.empty() && !startsWithI(u, L"https://") && !startsWithI(u, L"http://127.0.0.1"))
            s.error(L"catalog", L"must be an https:// address");
    }
    if (auto list = top.get("players", Type::Array)) {
        for (size_t i = 0; i < list->size(); ++i) {
            PlayerSpec p;
            Section s(&list->at(i), indexPath(L"players", i), issues);
            s.str("id", p.id);
            s.str("name", p.name);
            s.str("website", p.website);
            s.strList("paths", p.paths);
            s.strList("args", p.args);
            if (auto inst = s.get("install", Type::Object)) {
                Section is(inst, s.child("install"), issues);
                is.str("github", p.install.github);
                is.str("asset", p.install.asset);
                is.str("url", p.install.url);
                is.str("sha256", p.install.sha256);
                is.str("dir", p.install.dir);
                is.str("exe", p.install.exe);
            }
            if (p.name.empty()) p.name = widen(p.id);
            c.players.push_back(std::move(p));
        }
    }
    if (auto list = top.get("categories", Type::Array)) {
        for (size_t i = 0; i < list->size(); ++i) {
            Category cat;
            Section s(&list->at(i), indexPath(L"categories", i), issues);
            s.str("id", cat.id);
            s.str("name", cat.name);
            if (cat.name.empty()) cat.name = widen(cat.id);
            c.categories.push_back(std::move(cat));
        }
    }
    if (auto list = top.get("games", Type::Array)) {
        for (size_t i = 0; i < list->size(); ++i) {
            GameSpec g;
            std::wstring where = indexPath(L"games", i);
            Section s(&list->at(i), where, issues);
            s.str("id", g.id);
            s.str("name", g.name);
            s.str("description", g.description);
            s.str("players", g.playerCount);
            s.integer("maxPlayers", g.maxPlayers, 0, 16);
            if (!g.maxPlayers)
                for (size_t k = 0; k < g.playerCount.size(); ++k)  // "1-4 players" -> 4
                    if (iswdigit(g.playerCount[k]))
                        g.maxPlayers = std::max(g.maxPlayers, _wtoi(g.playerCount.c_str() + k));
            s.str("controls", g.controls);
            s.str("image", g.image);
            s.idList("categories", g.categories);
            s.color("color", g.color);
            s.color("colorEnd", g.colorEnd);
            if (auto actions = s.get("actions", Type::Array)) {
                for (size_t j = 0; j < actions->size(); ++j)
                    g.actions.push_back(readAction(actions->at(j), where + L"." + indexPath(L"actions", j), issues));
            }
            if (g.name.empty()) g.name = widen(g.id);
            c.games.push_back(std::move(g));
        }
    }
    if (auto texts = top.get("text", Type::Object)) {
        for (size_t i = 0; i < texts->size(); ++i) {
            const std::string& k = texts->keyAt(i);
            if (!k.empty() && k[0] == '_') continue;
            if (!texts->at(i).isString()) {
                issues.push_back({ConfigIssue::Severity::Error, L"text." + widen(k), L"must be a string"});
                continue;
            }
            if (!c.text.count(k))
                issues.push_back({ConfigIssue::Severity::Warning, L"text." + widen(k), L"unknown text key (typo?)"});
            c.text[k] = widen(texts->at(i).str());
        }
    }
    if (r.ok()) validateConfig(c, issues);
    return r;
}

LoadResult loadConfigFile(const std::wstring& path) {
    std::string text;
    if (!fs::readText(path, text)) {
        LoadResult r;
        r.issues.push_back({ConfigIssue::Severity::Error, path, L"cannot read the file"});
        return r;
    }
    return loadConfig(text, fs::parent(path));
}

namespace {

void checkVars(const std::wstring& text, const std::wstring& where, std::vector<ConfigIssue>& issues) {
    const auto& known = knownVariableNames();
    size_t i = 0;
    while ((i = text.find(L"${", i)) != std::wstring::npos) {
        size_t end = text.find(L'}', i);
        if (end == std::wstring::npos) break;
        std::wstring name = text.substr(i + 2, end - i - 2);
        bool ok = startsWithI(name, L"env:") || startsWithI(name, L"folder:");
        for (const auto& k : known) ok = ok || iequals(k, name);
        if (!ok) issues.push_back({ConfigIssue::Severity::Warning, where, L"unknown variable ${" + name + L"}"});
        i = end + 1;
    }
}

bool validId(const std::string& id) {
    if (id.empty()) return false;
    for (char ch : id)
        if (!(isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.')) return false;
    return true;
}

}  // namespace

void validateConfig(const Config& cfg, std::vector<ConfigIssue>& issues) {
    auto err = [&](const std::wstring& where, const std::wstring& msg) {
        issues.push_back({ConfigIssue::Severity::Error, where, msg});
    };
    if (cfg.players.empty()) err(L"players", L"at least one player (Ruffle, Flash Player...) is required");
    if (cfg.games.empty()) err(L"games", L"the launcher needs at least one game");

    std::set<std::string> ids;
    for (size_t i = 0; i < cfg.players.size(); ++i) {
        const auto& p = cfg.players[i];
        std::wstring where = indexPath(L"players", i);
        if (!validId(p.id)) err(where + L".id", L"must be letters, digits, '-', '_' or '.'");
        else if (!ids.insert(p.id).second) err(where + L".id", L"duplicate player id \"" + widen(p.id) + L"\"");
        if (p.paths.empty()) err(where + L".paths", L"list at least one place to look for the player");
        for (const auto& s : p.paths) checkVars(s, where + L".paths", issues);
        for (const auto& s : p.args) checkVars(s, where + L".args", issues);
        const InstallSpec& in = p.install;
        if (in.enabled() || !in.dir.empty()) {
            if (in.dir.empty()) err(where + L".install.dir", L"say which folder the player is unpacked into");
            if (!in.github.empty() && in.asset.empty())
                err(where + L".install.asset", L"give the release file name, e.g. \"*windows-x86_64.zip\"");
            if (!in.github.empty() && in.github.find('/') == std::string::npos)
                err(where + L".install.github", L"must look like \"owner/repo\"");
            if (!in.url.empty() && !startsWithI(in.url, L"https://") && !startsWithI(in.url, L"http://127.0.0.1"))
                err(where + L".install.url", L"must be an https:// address");
            if (!in.enabled()) err(where + L".install", L"needs \"github\" or \"url\"");
            checkVars(in.dir, where + L".install.dir", issues);
        }
    }
    ids.clear();
    for (size_t i = 0; i < cfg.categories.size(); ++i) {
        std::wstring where = indexPath(L"categories", i);
        if (!validId(cfg.categories[i].id)) err(where + L".id", L"must be letters, digits, '-', '_' or '.'");
        else if (!ids.insert(cfg.categories[i].id).second) err(where + L".id", L"duplicate category id");
    }
    ids.clear();
    for (size_t i = 0; i < cfg.games.size(); ++i) {
        const auto& g = cfg.games[i];
        std::wstring where = indexPath(L"games", i);
        if (!validId(g.id)) err(where + L".id", L"must be letters, digits, '-', '_' or '.'");
        else if (!ids.insert(g.id).second) err(where + L".id", L"duplicate game id \"" + widen(g.id) + L"\"");
        for (const auto& c : g.categories)
            if (!cfg.findCategory(c)) err(where + L".categories", L"unknown category \"" + widen(c) + L"\"");
        checkVars(g.image, where + L".image", issues);
        if (g.actions.empty()) err(where + L".actions", L"add at least one action (e.g. \"Play\")");
        std::set<std::string> actionIds;
        for (size_t j = 0; j < g.actions.size(); ++j) {
            const auto& a = g.actions[j];
            std::wstring aw = where + L"." + indexPath(L"actions", j);
            if (!validId(a.id)) err(aw + L".id", L"must be letters, digits, '-', '_' or '.'");
            else if (!actionIds.insert(a.id).second) err(aw + L".id", L"duplicate action id \"" + widen(a.id) + L"\"");
            if (a.swf.empty()) {
                bool all = !a.overrides.empty();
                for (const auto& p : cfg.players)
                    if (a.players.empty() || std::find(a.players.begin(), a.players.end(), p.id) != a.players.end()) {
                        auto o = a.overrides.find(p.id);
                        all = all && o != a.overrides.end() && o->second.hasSwf;
                    }
                if (!all) err(aw + L".swf", L"list where the game's .swf file is");
            }
            for (const auto& s : a.swf) checkVars(s, aw + L".swf", issues);
            for (const auto& s : a.args) checkVars(s, aw + L".args", issues);
            for (const auto& p : a.players)
                if (!cfg.findPlayer(p)) err(aw + L".players", L"unknown player \"" + widen(p) + L"\"");
            for (const auto& [pid, o] : a.overrides) {
                if (!cfg.findPlayer(pid)) err(aw + L".overrides", L"unknown player \"" + widen(pid) + L"\"");
                for (const auto& s : o.swf) checkVars(s, aw + L".overrides", issues);
                for (const auto& s : o.args) checkVars(s, aw + L".overrides", issues);
            }
        }
    }
    checkVars(cfg.paths.games, L"paths.games", issues);
    checkVars(cfg.paths.players, L"paths.players", issues);
    checkVars(cfg.paths.data, L"paths.data", issues);
}

std::wstring formatIssues(const std::vector<ConfigIssue>& issues) {
    std::wstring out;
    for (const auto& i : issues) {
        out += i.severity == ConfigIssue::Severity::Error ? L"error   " : L"warning ";
        out += i.where.empty() ? L"(top level)" : i.where;
        out += L": " + i.message + L"\n";
    }
    return out;
}

}  // namespace fl
