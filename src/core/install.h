// Installs a Flash player the launcher can't find: the newest release asset
// from the player's GitHub repository (or a fixed URL), checked against its
// SHA-256, unpacked into the player's folder. Runs on a worker thread; no UI.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

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

}  // namespace fl
