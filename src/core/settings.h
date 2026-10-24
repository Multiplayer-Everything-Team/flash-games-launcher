// Per-user launcher data: favorites, play statistics, chosen player, located
// game files and window placement. Stored as UTF-8 JSON in paths.data and
// written atomically so a crash never leaves a half-written file.
#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace fl {

struct GameStats {
    bool favorite = false;
    int plays = 0;
    int64_t seconds = 0;     // total play time
    int64_t lastPlayed = 0;  // unix time, 0 = never
    std::string player;      // preferred player id, empty = config order
    std::map<std::string, std::wstring> swf;  // action id -> file chosen with "Locate"
};

struct UserData {
    std::map<std::string, GameStats> games;
    std::string selectedGame, category;
    int x = 0, y = 0, width = 0, height = 0;  // 0 = default placement
    bool maximized = false;

    GameStats& stats(const std::string& gameId) { return games[gameId]; }
    const GameStats* find(const std::string& gameId) const;
};

UserData loadUserData(const std::wstring& path);
bool saveUserData(const UserData& data, const std::wstring& path, std::wstring* error = nullptr);

int64_t unixNow();

}  // namespace fl
