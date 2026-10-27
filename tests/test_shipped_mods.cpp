// The PC mods the launcher ships, installed from the layouts of their real GitHub
// release zips (file names only), into fake game folders: files land where the
// mod needs them, and an uninstall leaves the game folder exactly as it was.
#include <filesystem>

#include "core/config.h"
#include "core/mod/manager.h"
#include "core/mod/package.h"
#include "core/win.h"
#include "test.h"

using namespace fl;
using namespace fl::mod;

namespace {

Config loadShipped() {
    std::wstring path = fs::join(fs::parent(fs::parent(fs::parent(fs::selfExePath()))), L"launcher\\launcher.json");
    if (!fs::isFile(path)) path = L"launcher\\launcher.json";
    LoadResult r = loadConfigFile(path);
    if (!r.ok()) throw std::runtime_error(narrow(formatIssues(r.issues)));
    return r.config;
}

const GameSpec* shippedMod(const Config& c, const char* id) {
    const GameSpec* g = c.findGame(id);
    return g && g->mod ? g : nullptr;
}

// What is in the game folder, as "relative path size" lines.
std::string folderSnapshot(const std::wstring& dir) {
    std::string out;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(fs::longPath(dir), ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec))
            out += narrow(it->path().wstring().substr(dir.size())) + " " + std::to_string(it->file_size(ec)) + "\n";
    return out;
}

Outcome installShipped(const Config& cfg, const GameSpec& g, const std::wstring& gameDir, const std::wstring& payload,
                       const char* version, Operation op = Operation::Install) {
    Spec spec = *g.mod;
    spec.mod.version = widen(version);
    spec.text = &cfg.text;
    Vars base;
    CancelToken cancel;
    return install(spec, base, op, gameDir, payload, [](const Progress&) {}, cancel);
}

Outcome uninstallShipped(const Config& cfg, const GameSpec& g, const std::wstring& gameDir) {
    Spec spec = *g.mod;
    spec.text = &cfg.text;
    Vars base;
    CancelToken cancel;
    return uninstall(spec, base, gameDir, [](const Progress&) {}, cancel);
}

}  // namespace

TEST(shipped_spooky_installs_from_its_release_layout_and_uninstalls_cleanly) {
    Config cfg = loadShipped();
    const GameSpec* g = shippedMod(cfg, "spooky-together");
    REQUIRE(g != nullptr);
    CHECK(g->mod->package.github == L"multiplayereverything/spooky-together");
    REQUIRE(g->mod->package.extras.size() == 1);
    CHECK(g->mod->package.extras[0].github == L"multiplayereverything/spooky-together");
    CHECK(g->mod->package.extras[0].tag == L"bepinex-5.4.23.5");
    TempDir root("spooky-ship");
    std::wstring game = root / L"Game", payload = root / L"Pkg";
    writeText(game + L"\\SpookyUnity.exe", "game");
    writeText(game + L"\\SpookyUnity_Data\\data", "d");
    writeText(game + L"\\UnityPlayer.dll", "u");
    // SpookyTogether-BepInEx-0.2.9.zip is the game-folder layout (its own copy of the loader is ignored);
    // the loader comes from BepInEx_win_x64_5.4.23.5.zip, unpacked as "loader".
    for (const char* f : {"winhttp.dll", "changelog.txt", "README.md", "BepInEx\\core\\BepInEx.dll",
                          "BepInEx\\plugins\\SpookyTogether\\SpookyTogether.BepInEx.dll"})
        writeText(payload + L"\\" + widen(f), std::string("zip ") + f);
    for (const char* f : {"winhttp.dll", "doorstop_config.ini", ".doorstop_version", "changelog.txt",
                          "BepInEx\\core\\BepInEx.dll", "BepInEx\\core\\0Harmony.dll"})
        writeText(payload + L"\\loader\\" + widen(f), std::string("loader ") + f);
    std::string before = folderSnapshot(game);
    Outcome o = installShipped(cfg, *g, game, payload, "0.2.9");
    if (o.result != Outcome::Result::Success) fprintf(stderr, "    %ls\n", o.message.c_str());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(exists(game + L"\\winhttp.dll") && exists(game + L"\\doorstop_config.ini") && exists(game + L"\\.doorstop_version"));
    CHECK(exists(game + L"\\BepInEx\\core\\BepInEx.dll") && exists(game + L"\\BepInEx\\core\\0Harmony.dll"));
    CHECK(exists(game + L"\\BepInEx\\plugins\\SpookyTogether\\SpookyTogether.BepInEx.dll"));
    CHECK(!exists(game + L"\\README.md") && !exists(game + L"\\changelog.txt"));  // the zip's own notes stay out of the game
    Outcome again = installShipped(cfg, *g, game, payload, "0.2.9", Operation::Repair);
    CHECK(again.result == Outcome::Result::Success);
    REQUIRE(uninstallShipped(cfg, *g, game).result == Outcome::Result::Success);
    CHECK(folderSnapshot(game) == before);
}

TEST(shipped_baldi_installs_the_plugin_and_the_loader_extra) {
    Config cfg = loadShipped();
    const GameSpec* g = shippedMod(cfg, "baldi-together");
    REQUIRE(g != nullptr);
    CHECK(g->mod->package.github == L"multiplayereverything/baldi-plus-coop");
    REQUIRE(g->mod->package.extras.size() == 1);
    CHECK(g->mod->package.extras[0].dir == L"loader" && g->mod->package.extras[0].tag == L"bepinex-5.4.23.5");
    CHECK(g->mod->package.extras[0].github == L"multiplayereverything/baldi-plus-coop");
    TempDir root("baldi-ship");
    std::wstring game = root / L"Game", payload = root / L"Pkg";
    writeText(game + L"\\BALDI.exe", "game");
    writeText(game + L"\\BALDI_Data\\data", "d");
    writeText(game + L"\\UnityPlayer.dll", "u");
    writeText(game + L"\\UserData\\BaldiTogether.ini", "keep me");
    // BaldiTogether-BepInEx-0.9.0.zip: the plugin and notes; BepInEx_win_x64_5.4.23.5.zip unpacked as "loader".
    writeText(payload + L"\\BepInEx\\plugins\\BaldiTogether\\BaldiTogether.BepInEx.dll", "plugin");
    writeText(payload + L"\\README.md", "notes");
    for (const char* f : {"winhttp.dll", "doorstop_config.ini", ".doorstop_version", "changelog.txt", "BepInEx\\core\\BepInEx.dll"})
        writeText(payload + L"\\loader\\" + widen(f), std::string("loader ") + f);
    std::string before = folderSnapshot(game);
    Outcome o = installShipped(cfg, *g, game, payload, "0.9.0");
    if (o.result != Outcome::Result::Success) fprintf(stderr, "    %ls\n", o.message.c_str());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(exists(game + L"\\winhttp.dll") && exists(game + L"\\BepInEx\\core\\BepInEx.dll"));
    CHECK(exists(game + L"\\BepInEx\\plugins\\BaldiTogether\\BaldiTogether.BepInEx.dll"));
    CHECK(!exists(game + L"\\changelog.txt") && !exists(game + L"\\README.md"));
    REQUIRE(uninstallShipped(cfg, *g, game).result == Outcome::Result::Success);
    CHECK(folderSnapshot(game) == before);  // including the player's own UserData settings
}

TEST(shipped_miside_installs_bepinex_6_and_the_plugin_and_uninstalls_cleanly) {
    Config cfg = loadShipped();
    const GameSpec* g = shippedMod(cfg, "miside-together");
    REQUIRE(g != nullptr);
    REQUIRE(g->mod->package.extras.size() == 1);
    CHECK(g->mod->package.extras[0].github == L"multiplayereverything/miside-coop");
    CHECK(g->mod->package.extras[0].tag == L"bepinex-6.0.0-be.790");
    TempDir root("miside-ship");
    std::wstring game = root / L"Game", payload = root / L"Pkg";
    writeText(game + L"\\MiSideFull.exe", "game");
    writeText(game + L"\\MiSideFull_Data\\data", "d");
    writeText(game + L"\\GameAssembly.dll", "g");
    // MiSideTogether-0.3.1.zip is the plugin only; the BepInEx-Unity.IL2CPP-win-x64 be.790 zip is unpacked as "loader".
    writeText(payload + L"\\BepInEx\\plugins\\MiSideTogether\\MiSideTogether.dll", "plugin");
    for (const char* f : {"winhttp.dll", "doorstop_config.ini", ".doorstop_version", "changelog.txt",
                          "BepInEx\\core\\BepInEx.Unity.IL2CPP.dll", "BepInEx\\core\\BepInEx.Core.dll",
                          "dotnet\\System.dll", "dotnet\\clrjit.dll"})
        writeText(payload + L"\\loader\\" + widen(f), std::string("loader ") + f);
    std::string before = folderSnapshot(game);
    Outcome o = installShipped(cfg, *g, game, payload, "0.3.1");
    if (o.result != Outcome::Result::Success) fprintf(stderr, "    %ls\n", o.message.c_str());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(exists(game + L"\\BepInEx\\core\\BepInEx.Unity.IL2CPP.dll") && exists(game + L"\\dotnet\\clrjit.dll"));
    CHECK(exists(game + L"\\winhttp.dll") && exists(game + L"\\doorstop_config.ini") && exists(game + L"\\.doorstop_version"));
    CHECK(exists(game + L"\\BepInEx\\plugins\\MiSideTogether\\MiSideTogether.dll"));
    CHECK(!exists(game + L"\\changelog.txt"));
    REQUIRE(uninstallShipped(cfg, *g, game).result == Outcome::Result::Success);
    CHECK(folderSnapshot(game) == before);
}

TEST(shipped_gta_keeps_the_games_eax_dll_next_to_the_wrapper_and_puts_it_back) {
    Config cfg = loadShipped();
    const GameSpec* g = shippedMod(cfg, "gta-coop");
    REQUIRE(g != nullptr);
    CHECK(g->mod->package.github == L"multiplayereverything/gta-coop" && g->siteId == "gta");
    TempDir root("gta-ship");
    std::wstring game = root / L"Game", payload = root / L"Pkg";
    writeText(game + L"\\gta_sa.exe", "game");
    writeText(game + L"\\eax.dll", "THE GAMES EAX");
    writeText(game + L"\\models\\gta3.img", "img");
    // GTACoop-1.0.0.zip: game\ (the files that go next to gta_sa.exe) and server\.
    writeText(payload + L"\\game\\coop_eax.dll", "COOP WRAPPER");
    writeText(payload + L"\\game\\CoopAndreasSA.dll", "plugin");
    writeText(payload + L"\\game\\LaunchCoopAndreas.exe", "launcher");
    writeText(payload + L"\\game\\LaunchCoopAndreas.exe.manifest", "manifest");
    writeText(payload + L"\\game\\install.bat", "not run by the launcher");
    writeText(payload + L"\\game\\CoopAndreas\\main.scm", "scm");
    writeText(payload + L"\\game\\CoopAndreas\\script.img", "img");
    writeText(payload + L"\\server\\server.exe", "server");
    writeText(payload + L"\\server\\server-config.ini", "port = 6767");
    writeText(payload + L"\\source\\0001.patch", "source");
    std::string before = folderSnapshot(game);
    Outcome o = installShipped(cfg, *g, game, payload, "1.0.0");
    if (o.result != Outcome::Result::Success) fprintf(stderr, "    %ls\n", o.message.c_str());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(readText(game + L"\\eax.dll") == "COOP WRAPPER");    // what install.bat does: coop_eax.dll becomes eax.dll
    CHECK(readText(game + L"\\eax_orig.dll") == "THE GAMES EAX");  // and the game's own is kept beside it
    CHECK(exists(game + L"\\CoopAndreasSA.dll") && exists(game + L"\\LaunchCoopAndreas.exe"));
    CHECK(exists(game + L"\\CoopAndreas\\main.scm") && exists(game + L"\\CoopAndreas\\server\\server.exe"));
    CHECK(!exists(game + L"\\install.bat") && !exists(game + L"\\coop_eax.dll") && !exists(game + L"\\CoopAndreas\\source"));
    // The host edits the server's settings; an update keeps them, and the copy of the original eax.dll stays the
    // original (the wrapper is not copied over it).
    writeText(game + L"\\CoopAndreas\\server\\server-config.ini", "port = 7000");
    Outcome again = installShipped(cfg, *g, game, payload, "1.0.0", Operation::Update);
    REQUIRE(again.result == Outcome::Result::Success);
    CHECK(readText(game + L"\\eax_orig.dll") == "THE GAMES EAX" && readText(game + L"\\eax.dll") == "COOP WRAPPER");
    CHECK(readText(game + L"\\CoopAndreas\\server\\server-config.ini") == "port = 7000");
    REQUIRE(uninstallShipped(cfg, *g, game).result == Outcome::Result::Success);
    CHECK(readText(game + L"\\eax.dll") == "THE GAMES EAX");
    // Only the host's edited settings may be left behind (the engine never deletes a file the player changed).
    std::string left = folderSnapshot(game);
    CHECK(left.find("eax_orig.dll") == std::string::npos && left.find("CoopAndreasSA.dll") == std::string::npos);
    (void)before;
}

TEST(a_package_that_lost_a_file_is_reported_as_damaged_and_can_be_discarded) {
    Config cfg = loadShipped();
    const GameSpec* g = shippedMod(cfg, "gta-coop");
    REQUIRE(g != nullptr);
    TempDir root("av");
    std::wstring game = root / L"Game", payload = root / L"cache\\gta-coop\\1.0.0\\payload";
    writeText(game + L"\\gta_sa.exe", "game");
    writeText(game + L"\\eax.dll", "THE GAMES EAX");
    writeText(game + L"\\models\\gta3.img", "img");
    // An antivirus program took LaunchCoopAndreas.exe out of the unpacked package.
    writeText(payload + L"\\game\\coop_eax.dll", "wrapper");
    writeText(payload + L"\\game\\CoopAndreasSA.dll", "plugin");
    writeText(payload + L"\\game\\CoopAndreas\\main.scm", "scm");
    writeText(payload + L"\\server\\server.exe", "server");
    std::string before = folderSnapshot(game);
    Outcome o = installShipped(cfg, *g, game, payload, "1.0.0");
    CHECK(o.result == Outcome::Result::Failed && o.packageDamaged);
    CHECK(o.message.find(L"LaunchCoopAndreas.exe") != std::wstring::npos && o.message.find(L"ntivirus") != std::wstring::npos);
    CHECK(folderSnapshot(game) == before);  // nothing was changed in the game folder
    std::wstring cache = root / L"cache";
    REQUIRE(exists(payload + L"\\game\\coop_eax.dll"));
    discardPackage(cache, L"gta-coop", L"1.0.0");
    CHECK(!exists(payload + L"\\game\\coop_eax.dll"));
}

TEST(a_required_file_blocks_the_install_until_it_is_there) {
    LoadResult r = loadConfig(R"({ "paths": {"games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
                       "data": "${LAUNCHER_DIR}\\data", "flashpoint": []}, "players": [ {"id": "r", "paths": ["r.exe"]} ],
        "games": [ { "id": "g", "name": "G", "mod": { "game": {"name": "G", "executable": "G.exe",
        "detection": [{"type": "path", "path": "C:\\x"}]}, "components": [{"id": "m", "source": "a"}],
        "package": {"url": "https://example.invalid/p.zip"},
        "install": {"requires": [{"path": "Loader/core/loader.dll", "message": "Install the loader first."}]} } } ] })",
                              L"C:\\x");
    REQUIRE(r.ok());
    Spec spec = *r.config.games[0].mod;
    spec.text = &r.config.text;
    TempDir root("requires");
    Vars base;
    CHECK(missingRequirement(spec, base, root / L"Game") == L"Install the loader first.");
    writeText(root / L"Game\\Loader\\core\\loader.dll", "x");
    CHECK(missingRequirement(spec, base, root / L"Game").empty());
}

TEST(shipped_cupheads_installs_the_plugin_and_bepinex_5_and_uninstalls_cleanly) {
    Config cfg = loadShipped();
    const GameSpec* g = shippedMod(cfg, "cupheads");
    REQUIRE(g != nullptr);
    CHECK(g->mod->package.github == L"multiplayereverything/CupHeads");
    REQUIRE(g->mod->package.extras.size() == 1);
    CHECK(g->mod->package.extras[0].tag == L"bepinex-5.4.23.5");
    TempDir root("cup-ship");
    std::wstring game = root / L"Game", payload = root / L"Pkg";
    writeText(game + L"\\Cuphead.exe", "game");
    writeText(game + L"\\Cuphead_Data\\data", "d");
    // CupHeads-BepInEx-1.7.0.zip is the plugin only; BepInEx_win_x64_5.4.23.5.zip is unpacked as "loader".
    writeText(payload + L"\\BepInEx\\plugins\\CupheadOnline\\CupheadOnline.dll", "plugin");
    for (const char* f : {"winhttp.dll", "doorstop_config.ini", ".doorstop_version", "changelog.txt", "BepInEx\\core\\BepInEx.dll"})
        writeText(payload + L"\\loader\\" + widen(f), std::string("loader ") + f);
    std::string before = folderSnapshot(game);
    Outcome o = installShipped(cfg, *g, game, payload, "1.7.0");
    if (o.result != Outcome::Result::Success) fprintf(stderr, "    %ls\n", o.message.c_str());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(exists(game + L"\\winhttp.dll") && exists(game + L"\\BepInEx\\core\\BepInEx.dll"));
    CHECK(exists(game + L"\\BepInEx\\plugins\\CupheadOnline\\CupheadOnline.dll"));
    REQUIRE(uninstallShipped(cfg, *g, game).result == Outcome::Result::Success);
    CHECK(folderSnapshot(game) == before);
}

TEST(shipped_rick_and_morty_installs_its_own_bepinex_cfg) {
    Config cfg = loadShipped();
    const GameSpec* g = shippedMod(cfg, "rick-and-morty-together");
    REQUIRE(g != nullptr);
    CHECK(g->mod->package.github == L"multiplayereverything/rick-and-morty-coop");
    TempDir root("rick-ship");
    std::wstring game = root / L"Game", payload = root / L"Pkg";
    writeText(game + L"\\RickAndMortyVR.exe", "game");
    writeText(game + L"\\RickAndMortyVR_Data\\data", "d");
    // RickAndMortyTogether-BepInEx-0.1.0.zip: the plugin, the settings this game needs, and a readme.
    writeText(payload + L"\\BepInEx\\plugins\\RickAndMortyTogether\\RickAndMortyTogether.dll", "plugin");
    writeText(payload + L"\\BepInEx\\config\\BepInEx.cfg", "[Preloader.Entrypoint]");
    writeText(payload + L"\\RickAndMortyTogether-README.md", "notes");
    for (const char* f : {"winhttp.dll", "doorstop_config.ini", ".doorstop_version", "BepInEx\\core\\BepInEx.dll"})
        writeText(payload + L"\\loader\\" + widen(f), std::string("loader ") + f);
    std::string before = folderSnapshot(game);
    Outcome o = installShipped(cfg, *g, game, payload, "0.1.0");
    if (o.result != Outcome::Result::Success) fprintf(stderr, "    %ls\n", o.message.c_str());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(readText(game + L"\\BepInEx\\config\\BepInEx.cfg") == "[Preloader.Entrypoint]");  // the mod's settings, not a default
    CHECK(!exists(game + L"\\RickAndMortyTogether-README.md"));
    REQUIRE(uninstallShipped(cfg, *g, game).result == Outcome::Result::Success);
    CHECK(folderSnapshot(game) == before);
}

TEST(shipped_dave_the_diver_installs_bepinex_6_and_the_plugin) {
    Config cfg = loadShipped();
    const GameSpec* g = shippedMod(cfg, "dave-the-diver-multiplayer");
    REQUIRE(g != nullptr);
    CHECK(g->mod->package.github == L"multiplayereverything/Dave-The-Divers-Coop");
    REQUIRE(g->mod->package.extras.size() == 1);
    CHECK(g->mod->package.extras[0].tag == L"bepinex-6.0.0-be.790");
    TempDir root("dave-ship");
    std::wstring game = root / L"Game", payload = root / L"Pkg";
    writeText(game + L"\\DaveTheDiver.exe", "game");
    writeText(game + L"\\DaveTheDiver_Data\\data", "d");
    writeText(game + L"\\GameAssembly.dll", "g");
    // DaveTheDiverMP-0.19.0.zip: the plugin and its license; the be.790 IL2CPP zip is unpacked as "loader".
    writeText(payload + L"\\BepInEx\\plugins\\DaveTheDiverMP\\DaveTheDiverMP.dll", "plugin");
    writeText(payload + L"\\BepInEx\\plugins\\DaveTheDiverMP\\LICENSE.txt", "MIT");
    for (const char* f : {"winhttp.dll", "doorstop_config.ini", ".doorstop_version", "BepInEx\\core\\BepInEx.Unity.IL2CPP.dll",
                          "dotnet\\clrjit.dll"})
        writeText(payload + L"\\loader\\" + widen(f), std::string("loader ") + f);
    std::string before = folderSnapshot(game);
    Outcome o = installShipped(cfg, *g, game, payload, "0.19.0");
    if (o.result != Outcome::Result::Success) fprintf(stderr, "    %ls\n", o.message.c_str());
    REQUIRE(o.result == Outcome::Result::Success);
    CHECK(exists(game + L"\\BepInEx\\core\\BepInEx.Unity.IL2CPP.dll") && exists(game + L"\\dotnet\\clrjit.dll"));
    CHECK(exists(game + L"\\BepInEx\\plugins\\DaveTheDiverMP\\DaveTheDiverMP.dll"));
    REQUIRE(uninstallShipped(cfg, *g, game).result == Outcome::Result::Success);
    CHECK(folderSnapshot(game) == before);
}

TEST(mod_config_github_extras_and_requires_are_validated) {
    auto load = [](const std::string& mod) {
        return loadConfig(R"({ "paths": {"games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
                       "data": "${LAUNCHER_DIR}\\data", "flashpoint": []}, "players": [ {"id": "r", "paths": ["r.exe"]} ],
                       "games": [ { "id": "g", "name": "G", "mod": { "game": {"name": "G", "executable": "G.exe",
                       "detection": [{"type": "path", "path": "C:\\x"}]}, )" +
                              mod + "} } ] }",
                          L"C:\\x");
    };
    LoadResult ok = load(R"("components": [{"id": "m", "source": "a"}],
        "package": {"github": "o/r", "asset": "*.zip", "extras": [{"dir": "loader", "github": "o/l", "tag": "v1", "asset": "l.zip"}]},
        "install": {"requires": [{"path": "BepInEx/core/x.dll", "message": "install it"}]})");
    if (!ok.ok()) fprintf(stderr, "%ls\n", formatIssues(ok.issues).c_str());
    REQUIRE(ok.ok());
    const Spec& s = *ok.config.games[0].mod;
    CHECK(s.package.github == L"o/r" && s.package.extras.size() == 1 && s.package.extras[0].tag == L"v1");
    CHECK(s.install.requires_.size() == 1 && s.install.requires_[0].message == L"install it");
    CHECK(!load(R"("components": [{"id": "m", "source": "a"}], "package": {"github": "justarepo"})").ok());
    CHECK(!load(R"("components": [{"id": "m", "source": "a"}],
        "package": {"github": "o/r", "extras": [{"dir": "a/b", "github": "o/l", "asset": "l.zip"}]})")
               .ok());
    CHECK(!load(R"("components": [{"id": "m", "source": "a"}],
        "package": {"github": "o/r", "extras": [{"dir": "x"}]})")
               .ok());
    CHECK(!load(R"("components": [{"id": "m", "source": "a"}], "package": {"github": "o/r"},
        "install": {"requires": [{"message": "no path"}]})")
               .ok());
}
