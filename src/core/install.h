// Installs a Flash player the launcher can't find: the newest release asset
// from the player's GitHub repository (or a fixed URL), checked against its
// SHA-256, unpacked into the player's folder. Runs on a worker thread; no UI.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "config.h"
#include "vars.h"

namespace fl {

struct InstallStatus {
    enum class Stage { Finding, Downloading, Verifying, Unpacking, Done, Failed } stage = Stage::Finding;
    uint64_t done = 0, total = 0;  // bytes, while downloading
};

struct InstallResult {
    bool ok = false;
    std::wstring exe;      // the installed player
    std::wstring version;  // release tag, when known
    std::wstring error;
};

using InstallProgress = std::function<void(const InstallStatus&)>;

// `vars` must hold PLAYERS_DIR and DATA_DIR. `cancel` may be null.
InstallResult installPlayer(const PlayerSpec& player, const Vars& vars, const InstallProgress& progress,
                            const std::atomic<bool>* cancel);

// One downloadable game file from the game catalog (catalog.json in the
// flash-games repository): `name` is the file name in the games folder and
// `game` the launcher's game id.
struct CatalogFile {
    std::string game;
    std::wstring name, url, sha256, description;
    uint64_t size = 0;
};

// `baseUrl` is the catalog's own address; file paths are relative to its folder.
bool parseCatalog(std::string_view json, const std::wstring& baseUrl, std::vector<CatalogFile>& out,
                  std::wstring& error);
// Downloads and parses the catalog. `raw` receives the JSON for caching.
bool fetchCatalog(const std::wstring& url, std::vector<CatalogFile>& out, std::string& raw, std::wstring& error,
                  const std::atomic<bool>* cancel = nullptr);
// Downloads `files` into `dir`, each checked against its SHA-256 and written
// under a temporary name first. Files already there with the right checksum are kept.
InstallResult downloadGameFiles(const std::vector<CatalogFile>& files, const std::wstring& dir,
                                const InstallProgress& progress, const std::atomic<bool>* cancel);
// Catalog files that exist in `dir` with a different SHA-256: newer builds are
// published. Reads every such file, so call it off the UI thread.
std::vector<CatalogFile> outdatedGameFiles(const std::vector<CatalogFile>& catalog, const std::wstring& dir);

// Small text download (JSON); false with `error` set on failure.
bool httpGetText(const std::wstring& url, std::string& out, std::wstring& error);

}  // namespace fl
