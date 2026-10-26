#include "site.h"

#include "fs.h"
#include "install.h"
#include "json.h"
#include "log.h"
#include "settings.h"
#include "strings.h"

namespace fl {

namespace {

std::string text(const json::Value& o, const char* key) {
    const json::Value* v = o.find(key);
    return v && v->isString() ? v->str() : std::string();
}

std::map<std::string, std::string> stringMap(const json::Value& o, const char* key) {
    std::map<std::string, std::string> out;
    const json::Value* v = o.find(key);
    if (!v || !v->isObject()) return out;
    for (size_t i = 0; i < v->size(); ++i)
        if (v->at(i).isString()) out[v->keyAt(i)] = v->at(i).str();
    return out;
}

}  // namespace

bool parseSite(std::string_view jsonText, std::vector<SiteGame>& out) {
    out.clear();
    std::string err;
    json::Value root = json::parse(jsonText, &err);
    const json::Value* games = root.isObject() ? root.find("games") : nullptr;
    if (!games || !games->isArray()) return false;
    for (size_t i = 0; i < games->size(); ++i) {
        const json::Value& g = games->at(i);
        if (!g.isObject()) continue;
        SiteGame s;
        s.id = text(g, "id");
        if (s.id.empty()) continue;
        s.modId = text(g, "modId");
        s.kind = text(g, "kind");
        s.name = widen(text(g, "name"));
        s.status = text(g, "status");
        s.quality = text(g, "quality");
        s.version = widen(text(g, "version"));
        s.updated = widen(text(g, "updated"));
        s.connect = widen(text(g, "connect"));
        s.maintainer = widen(text(g, "maintainer"));
        s.engine = widen(text(g, "engine"));
        s.synced = stringMap(g, "synced");
        s.progress = stringMap(g, "progress");
        out.push_back(std::move(s));
    }
    return true;
}

const SiteGame* findSite(const std::vector<SiteGame>& list, const std::string& siteId, const std::string& gameId,
                         const std::string& modId) {
    auto by = [&](const std::string& id, bool mod) -> const SiteGame* {
        if (id.empty()) return nullptr;
        for (const auto& s : list)
            if ((mod ? s.modId : s.id) == id) return &s;
        return nullptr;
    };
    if (const SiteGame* s = by(siteId, false)) return s;
    if (const SiteGame* s = by(gameId, false)) return s;
    return by(modId, true);
}

bool loadSiteCache(const std::wstring& path, SiteCache& out) {
    std::string data, err;
    if (!fs::readText(path, data)) return false;
    json::Value root = json::parse(data, &err);
    if (!root.isObject()) return false;
    const json::Value* at = root.find("at");
    const json::Value* raw = root.find("games");
    if (!at || !at->isNumber() || !raw || !raw->isString()) return false;
    out.fetchedAt = static_cast<int64_t>(at->number());
    out.raw = raw->str();
    return true;
}

bool saveSiteCache(const std::wstring& path, const SiteCache& cache) {
    json::Value root = json::Value::object();
    root.set("at", cache.fetchedAt);
    root.set("games", cache.raw);
    std::string data = json::serialize(root, true);
    fs::ensureDir(fs::parent(path), nullptr, nullptr);
    return fs::writeFile(path, data.data(), data.size(), nullptr);
}

bool loadSite(const std::wstring& url, const std::wstring& cachePath, std::vector<SiteGame>& list, std::wstring& error,
              bool forceFetch) {
    list.clear();
    SiteCache cache;
    bool haveCache = loadSiteCache(cachePath, cache) && parseSite(cache.raw, list);
    int64_t now = unixNow();
    if (haveCache && !forceFetch && cache.fresh(now)) return true;
    if (url.empty() || (!startsWithI(url, L"https://") && !startsWithI(url, L"http://127.0.0.1"))) {
        if (!haveCache) error = L"No address for the website's data";
        return haveCache;
    }
    std::string body;
    std::vector<SiteGame> fetched;
    if (httpGetText(url, body, error) && parseSite(body, fetched)) {
        list = std::move(fetched);
        SiteCache fresh;
        fresh.fetchedAt = now;
        fresh.raw = std::move(body);
        saveSiteCache(cachePath, fresh);
        error.clear();
        return true;
    }
    if (error.empty()) error = L"The website's data isn't in the expected shape";
    if (haveCache) logger().warn(L"Website: " + error + L"; using the copy from " + std::to_wstring((now - cache.fetchedAt) / 60) + L" minutes ago");
    else list.clear();
    return haveCache;
}

}  // namespace fl
