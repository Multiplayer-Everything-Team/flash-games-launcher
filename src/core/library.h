// Turns the configuration into something playable: finds each player's exe,
// each action's .swf, and builds the exact command line. Pure lookups, no UI;
// call refresh() again after files change (F5, "Locate", a player install).
#pragma once
#include <string>
#include <vector>

#include "config.h"
#include "settings.h"
#include "vars.h"

namespace fl {

struct PlayerStatus {
    const PlayerSpec* spec = nullptr;
    std::wstring exe;  // empty when not found
    bool found() const { return !exe.empty(); }
};

enum class ActionState { Ready, NoPlayer, NoSwf };

struct ResolvedAction {
    const GameSpec* game = nullptr;
    const ActionSpec* action = nullptr;
    ActionState state = ActionState::NoSwf;
    const PlayerSpec* player = nullptr;  // chosen player (or the first allowed one when none is found)
    std::wstring playerExe, swf, workDir;
    std::vector<std::wstring> args;  // final argv after the exe
    bool swfIsUrl = false;
    bool userSwf = false;  // the file came from "Locate", not the config
};

class Library {
public:
    // `base` must hold LAUNCHER_DIR; the library adds GAMES_DIR, PLAYERS_DIR,
    // DATA_DIR and FLASHPOINT.
    Library(const Config& cfg, Vars base);

    void refresh();
    const Vars& vars() const { return vars_; }
    const std::vector<PlayerStatus>& players() const { return players_; }
    const PlayerStatus* player(const std::string& id) const;
    std::wstring flashpointDir() const { return vars_.get(L"FLASHPOINT"); }
    std::wstring dataDir() const { return vars_.get(L"DATA_DIR"); }

    // Players this action may use, in the order they would be tried.
    std::vector<const PlayerSpec*> allowedPlayers(const ActionSpec& a, const std::string& preferred) const;
    ResolvedAction resolve(const GameSpec& g, const ActionSpec& a, const UserData& user) const;
    // The game's overall state: Ready when any action can start.
    ActionState gameState(const GameSpec& g, const UserData& user) const;

    // First existing path among candidates (variables expanded; '*' allowed in the
    // file name; bare names searched on PATH). Empty when none exists.
    std::wstring findFirst(const std::vector<std::wstring>& candidates, const Vars& vars) const;
    static bool isUrl(const std::wstring& s);

private:
    const Config& cfg_;
    Vars vars_;
    std::vector<PlayerStatus> players_;
};

}  // namespace fl
