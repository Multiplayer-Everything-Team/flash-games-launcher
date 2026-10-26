// The website's games.json: parsing, matching a launcher game to its entry, and the on-disk copy
// that keeps the launcher working offline.
#include "core/config.h"
#include "core/settings.h"
#include "core/site.h"
#include "test.h"

using namespace fl;

namespace {

const char* kSite = R"({
  "games": [
    { "id": "gunmayhem2", "kind": "flash", "modId": "gm2", "name": "Gun Mayhem 2", "status": "available", "quality": "B",
      "version": "1", "updated": "2026-10-24", "connect": "Hosted relay", "maintainer": "Multiplayer Everything",
      "synced": { "see": "synced", "story": "unknown" } },
    { "id": "baldi", "kind": "pc", "modId": "baldi", "name": "Baldi Together", "status": "testing", "quality": "",
      "version": "0.9.1", "synced": { "see": "synced", "story": "partly" },
      "progress": { "lab": "done", "release": "none" }, "somethingNew": [1, 2] },
    { "name": "no id, skipped" },
    "not an object"
  ]
})";

}  // namespace

TEST(site_json_is_parsed_and_unknown_fields_are_ignored) {
    std::vector<SiteGame> list;
    REQUIRE(parseSite(kSite, list));
    REQUIRE(list.size() == 2);  // the entry without an id and the non-object are skipped
    CHECK(list[0].id == "gunmayhem2" && list[0].modId == "gm2" && list[0].kind == "flash");
    CHECK(list[0].status == "available" && list[0].quality == "B" && list[0].version == L"1");
    CHECK(list[0].connect == L"Hosted relay" && list[0].updated == L"2026-10-24");
    CHECK(list[0].synced.at("see") == "synced" && list[0].synced.at("story") == "unknown");
    CHECK(list[1].quality.empty() && list[1].synced.at("story") == "partly");
    CHECK(list[1].progress.at("lab") == "done");
    CHECK(!parseSite("not json", list) && list.empty());
    CHECK(!parseSite("{ \"other\": [] }", list));
}

TEST(a_game_finds_its_site_entry_by_site_id_then_id_then_mod_id) {
    std::vector<SiteGame> list;
    REQUIRE(parseSite(kSite, list));
    const SiteGame* byId = findSite(list, "", "gunmayhem2", "gm2");
    REQUIRE(byId != nullptr);
    CHECK(byId->id == "gunmayhem2");
    const SiteGame* byMod = findSite(list, "", "something-else", "gm2");
    REQUIRE(byMod != nullptr);
    CHECK(byMod->id == "gunmayhem2");
    const SiteGame* bySite = findSite(list, "baldi", "baldi-together", "");
    REQUIRE(bySite != nullptr);
    CHECK(bySite->id == "baldi");
    CHECK(findSite(list, "", "baldi-together", "") == nullptr);  // no guessing from similar names
    CHECK(findSite(list, "", "", "") == nullptr);
}

TEST(the_site_copy_on_disk_round_trips_and_goes_stale_after_an_hour) {
    TempDir dir("site");
    std::wstring file = dir / L"site.json";
    SiteCache none;
    CHECK(!loadSiteCache(file, none));
    SiteCache c;
    c.fetchedAt = 1000000;
    c.raw = kSite;
    REQUIRE(saveSiteCache(file, c));
    SiteCache back;
    REQUIRE(loadSiteCache(file, back));
    CHECK(back.fetchedAt == 1000000 && back.raw == c.raw);
    CHECK(back.fresh(1000000 + 3599) && !back.fresh(1000000 + 3600) && !back.fresh(999999));
    CHECK(!SiteCache{}.fresh(5));
    writeText(file, "{ broken");
    CHECK(!loadSiteCache(file, none));
}

TEST(a_fresh_copy_is_used_without_asking_and_an_old_one_when_the_site_is_unreachable) {
    TempDir dir("site2");
    std::wstring file = dir / L"site.json";
    SiteCache c;
    c.fetchedAt = unixNow();
    c.raw = kSite;
    REQUIRE(saveSiteCache(file, c));
    std::vector<SiteGame> list;
    std::wstring err;
    // Fresh: no request is made (the address would not answer anyway).
    CHECK(loadSite(L"https://invalid.invalid/games.json", file, list, err) && list.size() == 2);
    // An hour old and the site is unreachable: the old copy still serves.
    c.fetchedAt = unixNow() - 7200;
    REQUIRE(saveSiteCache(file, c));
    CHECK(loadSite(L"http://127.0.0.1:1/games.json", file, list, err) && list.size() == 2);
    // No copy and no site: nothing, and a reason.
    TempDir empty("site3");
    CHECK(!loadSite(L"http://127.0.0.1:1/games.json", empty / L"site.json", list, err) && list.empty() && !err.empty());
    // No address at all: only the copy.
    CHECK(loadSite(L"", file, list, err) && list.size() == 2);
}

TEST(the_site_address_and_a_games_site_id_come_from_launcher_json) {
    LoadResult r = loadConfig(R"({ "paths": {"games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
        "data": "${LAUNCHER_DIR}\\data", "flashpoint": []}, "players": [ {"id": "r", "paths": ["r.exe"]} ],
        "downloads": { "site": "https://example.com/games.json" },
        "games": [ { "id": "g", "name": "G", "site": "gee", "actions": [ { "id": "p", "swf": "a.swf" } ] } ] })",
                              L"C:\\x");
    REQUIRE(r.ok());
    CHECK(r.config.downloads.site == L"https://example.com/games.json" && r.config.games[0].siteId == "gee");
    LoadResult def = loadConfig(R"({ "paths": {"games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
        "data": "${LAUNCHER_DIR}\\data", "flashpoint": []}, "players": [ {"id": "r", "paths": ["r.exe"]} ],
        "games": [ { "id": "g", "name": "G", "actions": [ { "id": "p", "swf": "a.swf" } ] } ] })",
                                L"C:\\x");
    REQUIRE(def.ok());
    CHECK(def.config.downloads.site == L"https://www.multiplayereverything.com/games.json");
    LoadResult bad = loadConfig(R"({ "paths": {"games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
        "data": "${LAUNCHER_DIR}\\data", "flashpoint": []}, "players": [ {"id": "r", "paths": ["r.exe"]} ],
        "downloads": { "site": "http://example.com/games.json" }, "games": [] })",
                                L"C:\\x");
    CHECK(!bad.ok());
}
