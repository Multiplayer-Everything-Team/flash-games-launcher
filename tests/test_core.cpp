// Config parsing/validation, file and player lookup, command lines, settings.
#include "core/config.h"
#include "core/install.h"
#include "core/library.h"
#include "core/process.h"
#include "core/settings.h"
#include "test.h"

using namespace fl;

namespace {

std::string minimalConfig(const std::string& games, const std::string& players = "") {
    std::string p = players.empty() ? R"([{ "id": "ruffle", "paths": ["${PLAYERS_DIR}\\ruffle.exe"] },
                                          { "id": "flash", "paths": ["${PLAYERS_DIR}\\flash*.exe"], "args": ["-x"] }])"
                                    : players;
    return R"({ "paths": { "games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
                           "data": "${LAUNCHER_DIR}\\data", "flashpoint": ["${LAUNCHER_DIR}\\fp"] },
               "categories": [{ "id": "online" }],
               "players": )" +
           p + R"(, "games": )" + games + "}";
}

bool hasIssue(const LoadResult& r, const std::wstring& where, ConfigIssue::Severity sev) {
    for (const auto& i : r.issues)
        if (i.severity == sev && i.where.find(where) != std::wstring::npos) return true;
    return false;
}

Vars baseVars(const TempDir& dir) {
    Vars v;
    v.set(L"LAUNCHER_DIR", dir.path());
    return v;
}

}  // namespace

TEST(config_loads_shipped_launcher_json) {
    // The real config must stay valid: it is what users run.
    std::wstring path = fs::join(fs::parent(fs::parent(fs::parent(fs::selfExePath()))), L"launcher\\launcher.json");
    if (!fs::isFile(path)) path = L"launcher\\launcher.json";
    LoadResult r = loadConfigFile(path);
    if (!r.ok()) fprintf(stderr, "%ls\n", formatIssues(r.issues).c_str());
    REQUIRE(r.ok());
    CHECK(r.issues.empty());
    CHECK(r.config.findGame("gunmayhem2") != nullptr);
    CHECK(r.config.findGame("stealthhunter2") != nullptr);
    const GameSpec* gm2 = r.config.findGame("gunmayhem2");
    bool hostHasRelay = false;
    for (const auto& a : gm2->actions)
        if (a.id == "host") hostHasRelay = a.relay.enabled && a.relay.port == 7777;
    CHECK(hostHasRelay);
}

TEST(config_reports_typos_and_bad_references) {
    LoadResult r = loadConfig(minimalConfig(R"([
        { "id": "a", "nmae": "typo", "categories": ["nope"],
          "actions": [{ "id": "play", "swf": "x.swf", "players": ["ghost"] }] },
        { "id": "a", "actions": [] }
    ])"),
                              L"C:\\x");
    CHECK(!r.ok());
    CHECK(hasIssue(r, L"games[0].nmae", ConfigIssue::Severity::Warning));
    CHECK(hasIssue(r, L"games[0].categories", ConfigIssue::Severity::Error));
    CHECK(hasIssue(r, L"games[0].actions[0].players", ConfigIssue::Severity::Error));
    CHECK(hasIssue(r, L"games[1].id", ConfigIssue::Severity::Error));       // duplicate
    CHECK(hasIssue(r, L"games[1].actions", ConfigIssue::Severity::Error));  // no actions
}

TEST(config_rejects_bad_json_with_position) {
    LoadResult r = loadConfig("{ \"games\": [ }", L"C:\\x");
    CHECK(!r.ok());
    REQUIRE(!r.issues.empty());
    CHECK(r.issues[0].message.find(L"line") != std::wstring::npos);
}

TEST(config_text_substitution) {
    LoadResult r = loadConfig(minimalConfig(R"([{ "id": "g", "actions": [{ "id": "p", "swf": "a.swf" }] }])"), L"C:\\x");
    REQUIRE(r.ok());
    CHECK(r.config.t("bar.found", {{L"PLAYER", L"Ruffle"}}).find(L"Ruffle") == 0);
}

TEST(library_resolves_player_swf_and_args) {
    TempDir dir("lib-resolve");
    writeText(dir / L"players\\ruffle.exe", "x");
    writeText(dir / L"games\\game.swf", "x");
    LoadResult r = loadConfig(minimalConfig(R"([{ "id": "g", "name": "Game",
        "actions": [{ "id": "host", "swf": ["${GAMES_DIR}\\missing.swf", "${GAMES_DIR}\\game.swf"],
                      "overrides": { "ruffle": { "args": ["--tcp-connections", "allow"] } },
                      "relay": { "port": 7777 } }] }])"),
                              dir.path());
    REQUIRE(r.ok());
    Library lib(r.config, baseVars(dir));
    UserData user;
    ResolvedAction a = lib.resolve(r.config.games[0], r.config.games[0].actions[0], user);
    REQUIRE(a.state == ActionState::Ready);
    CHECK(a.player->id == "ruffle");
    CHECK(fs::fileName(a.swf) == L"game.swf");
    REQUIRE(a.args.size() == 3);  // override args, then the SWF appended
    CHECK(a.args[0] == L"--tcp-connections");
    CHECK(a.args[2] == a.swf);
    CHECK(fs::normalize(a.workDir) == fs::normalize(dir / L"games"));
}

TEST(library_falls_back_and_honours_preferred_player) {
    TempDir dir("lib-fallback");
    writeText(dir / L"players\\flash_32_sa.exe", "x");
    writeText(dir / L"players\\flash_9_sa.exe", "x");
    writeText(dir / L"games\\game.swf", "x");
    LoadResult r = loadConfig(
        minimalConfig(R"([{ "id": "g", "actions": [{ "id": "p", "swf": "${GAMES_DIR}\\game.swf" }] }])"), dir.path());
    REQUIRE(r.ok());
    Library lib(r.config, baseVars(dir));
    UserData user;
    const auto& g = r.config.games[0];
    ResolvedAction a = lib.resolve(g, g.actions[0], user);
    REQUIRE(a.state == ActionState::Ready);
    CHECK(a.player->id == "flash");  // Ruffle missing -> next player
    CHECK(fs::fileName(a.playerExe) == L"flash_32_sa.exe");  // natural order: 32 > 9
    REQUIRE(a.args.size() == 2);
    CHECK(a.args[0] == L"-x");

    writeText(dir / L"players\\ruffle.exe", "x");
    lib.refresh();
    CHECK(lib.resolve(g, g.actions[0], user).player->id == "ruffle");
    user.stats("g").player = "flash";
    CHECK(lib.resolve(g, g.actions[0], user).player->id == "flash");
}

TEST(library_reports_missing_pieces) {
    TempDir dir("lib-missing");
    LoadResult r = loadConfig(
        minimalConfig(R"([{ "id": "g", "actions": [{ "id": "p", "swf": "${GAMES_DIR}\\game.swf" }] }])"), dir.path());
    REQUIRE(r.ok());
    Library lib(r.config, baseVars(dir));
    UserData user;
    const auto& g = r.config.games[0];
    CHECK(lib.resolve(g, g.actions[0], user).state == ActionState::NoSwf);
    writeText(dir / L"games\\game.swf", "x");
    CHECK(lib.resolve(g, g.actions[0], user).state == ActionState::NoPlayer);
    CHECK(lib.gameState(g, user) == ActionState::NoPlayer);
}

TEST(library_uses_located_file_and_skips_missing_flashpoint) {
    TempDir dir("lib-located");
    writeText(dir / L"players\\ruffle.exe", "x");
    writeText(dir / L"elsewhere\\my game.swf", "x");
    LoadResult r = loadConfig(minimalConfig(R"([{ "id": "g",
        "actions": [{ "id": "p", "swf": ["${FLASHPOINT}\\game.swf", "${GAMES_DIR}\\game.swf"] }] }])"),
                              dir.path());
    REQUIRE(r.ok());
    Library lib(r.config, baseVars(dir));
    CHECK(lib.flashpointDir().empty());
    UserData user;
    const auto& g = r.config.games[0];
    CHECK(lib.resolve(g, g.actions[0], user).state == ActionState::NoSwf);
    user.stats("g").swf["p"] = dir / L"elsewhere\\my game.swf";
    ResolvedAction a = lib.resolve(g, g.actions[0], user);
    CHECK(a.state == ActionState::Ready);
    CHECK(a.userSwf);

    // With a Flashpoint folder present, ${FLASHPOINT} candidates count.
    writeText(dir / L"fp\\FPSoftware\\.keep", "");
    writeText(dir / L"fp\\game.swf", "x");
    lib.refresh();
    CHECK(fs::normalize(lib.flashpointDir()) == fs::normalize(dir / L"fp"));
    user.stats("g").swf.clear();
    CHECK(fs::normalize(lib.resolve(g, g.actions[0], user).swf) == fs::normalize(dir / L"fp\\game.swf"));
}

TEST(library_passes_urls_through) {
    TempDir dir("lib-url");
    writeText(dir / L"players\\ruffle.exe", "x");
    LoadResult r = loadConfig(
        minimalConfig(R"([{ "id": "g", "actions": [{ "id": "p", "swf": "http://localflash/x/game.swf" }] }])"),
        dir.path());
    REQUIRE(r.ok());
    Library lib(r.config, baseVars(dir));
    UserData user;
    ResolvedAction a = lib.resolve(r.config.games[0], r.config.games[0].actions[0], user);
    CHECK(a.state == ActionState::Ready);
    CHECK(a.swfIsUrl);
    CHECK(a.args.back() == L"http://localflash/x/game.swf");
}

TEST(command_line_quoting) {
    CHECK(sys::quoteArg(L"plain") == L"plain");
    CHECK(sys::quoteArg(L"has space") == L"\"has space\"");
    CHECK(sys::quoteArg(L"") == L"\"\"");
    CHECK(sys::quoteArg(L"C:\\dir with space\\") == L"\"C:\\dir with space\\\\\"");
    CHECK(sys::quoteArg(L"say \"hi\"") == L"\"say \\\"hi\\\"\"");
    CHECK(sys::buildCommandLine(L"C:\\Program Files\\r.exe", {L"--a", L"C:\\My Games\\g.swf"}) ==
          L"\"C:\\Program Files\\r.exe\" --a \"C:\\My Games\\g.swf\"");
}

TEST(settings_round_trip_and_damaged_file) {
    TempDir dir("settings");
    std::wstring path = dir / L"data\\settings.json";
    UserData d;
    d.selectedGame = "gunmayhem2";
    d.category = "cat:online";
    d.width = 1200;
    d.maximized = true;
    GameStats& s = d.stats("gunmayhem2");
    s.favorite = true;
    s.plays = 3;
    s.seconds = 4000;
    s.lastPlayed = 1700000000;
    s.player = "ruffle";
    s.swf["host"] = L"C:\\Games\\\u00E9t\u00E9.swf";
    REQUIRE(saveUserData(d, path));
    UserData back = loadUserData(path);
    CHECK(back.selectedGame == "gunmayhem2");
    CHECK(back.category == "cat:online");
    CHECK(back.width == 1200 && back.maximized);
    const GameStats* b = back.find("gunmayhem2");
    REQUIRE(b != nullptr);
    CHECK(b->favorite && b->plays == 3 && b->seconds == 4000 && b->lastPlayed == 1700000000);
    CHECK(b->player == "ruffle");
    CHECK(b->swf.at("host") == L"C:\\Games\\\u00E9t\u00E9.swf");

    writeText(path, "{ not json");
    UserData fresh = loadUserData(path);
    CHECK(fresh.games.empty());
    CHECK(exists(path + L".bad"));  // kept for inspection, not overwritten
}

TEST(catalog_parsing_and_unsafe_names) {
    std::string json = R"({ "schemaVersion": 1, "files": [
        { "name": "gm2.swf", "game": "gunmayhem2", "path": "games/gunmayhem2/gm2.swf", "size": 10, "sha256": "ABCDEF" },
        { "name": "../evil.swf", "game": "x", "path": "games/x/evil.swf", "size": 1, "sha256": "00" },
        { "name": "sub/evil.swf", "game": "x", "path": "games/x/evil.swf", "size": 1, "sha256": "00" },
        { "name": "abs.swf", "game": "y", "path": "https://example.org/abs.swf", "size": 2, "sha256": "11" }
    ] })";
    std::vector<CatalogFile> files;
    std::wstring err;
    REQUIRE(parseCatalog(json, L"https://raw.githubusercontent.com/o/r/main/catalog.json", files, err));
    REQUIRE(files.size() == 2);  // names that could escape the games folder are dropped
    CHECK(files[0].url == L"https://raw.githubusercontent.com/o/r/main/games/gunmayhem2/gm2.swf");
    CHECK(files[0].sha256 == L"abcdef");
    CHECK(files[0].size == 10);
    CHECK(files[1].url == L"https://example.org/abs.swf");
    CHECK(!parseCatalog("{ nope", L"https://x/catalog.json", files, err));
}

TEST(library_blames_the_missing_file_when_a_player_is_installed) {
    // Ruffle is installed but its file is missing; Flash Player's copy is a web
    // address. The fix is the file, so the state must say so.
    TempDir dir("lib-blame");
    writeText(dir / L"players\\ruffle.exe", "x");
    LoadResult r = loadConfig(minimalConfig(R"([{ "id": "g", "actions": [{ "id": "p", "swf": "${GAMES_DIR}\\g.swf",
        "overrides": { "flash": { "swf": ["http://localflash/g.swf"] } } }] }])"),
                              dir.path());
    REQUIRE(r.ok());
    Library lib(r.config, baseVars(dir));
    UserData user;
    ResolvedAction a = lib.resolve(r.config.games[0], r.config.games[0].actions[0], user);
    CHECK(a.state == ActionState::NoSwf);
    CHECK(a.player && a.player->id == "ruffle");
}
