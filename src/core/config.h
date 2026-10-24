// Launcher configuration: every game, player, path, color and visible string
// lives in launcher.json and is loaded into these structs. Defaults here are
// the documented defaults in CONFIG.md; keep both in sync.
#pragma once
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vars.h"

namespace fl {

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    unsigned long ref() const { return r | (g << 8) | (b << 16); }  // COLORREF
};
bool parseColor(const std::wstring& text, Color& out);

struct ThemeColors {
    Color background, surface, surfaceHover, sidebar, sidebarText, sidebarTextMuted;
    Color primary, primaryHover, onPrimary, text, textMuted, border, focus;
    Color success, warning, error, online;
};
ThemeColors darkPalette();
ThemeColors lightPalette();
// Name/value pairs in declaration order, for docs and --dump-defaults.
std::vector<std::pair<const char*, Color*>> colorFields(ThemeColors& c);

struct WindowSpec {
    int width = 1180, height = 720, minWidth = 720, minHeight = 520;
};

struct Branding {
    std::wstring title = L"Multiplayer Everything";
    std::wstring subtitle = L"Flash Launcher";
    std::wstring windowTitle = L"${TITLE} ${SUBTITLE}";
    std::wstring theme = L"dark";  // dark | light
    ThemeColors colors;            // palette for `theme` with overrides applied
    std::wstring font = L"Segoe UI";
    float fontSize = 10.0f;  // points
    WindowSpec window;
};

struct Paths {
    std::wstring games = L"${LAUNCHER_DIR}\\games";
    std::wstring players = L"${LAUNCHER_DIR}\\players";
    std::wstring data = L"${env:LOCALAPPDATA}\\MultiplayerEverything\\FlashLauncher";
    // First folder that contains FPSoftware becomes ${FLASHPOINT}.
    std::vector<std::wstring> flashpoint = {L"C:\\Flashpoint", L"D:\\Flashpoint", L"${folder:Profile}\\Flashpoint"};
};

struct Behavior {
    bool minimizeOnLaunch = false;
    bool stopRelayWithGame = true;
    int quickExitSeconds = 4;  // a player that exits faster than this with an error is reported
};

struct PlayerSpec {
    std::string id;
    std::wstring name, website;
    // Candidates, first match wins: absolute paths (a '*' is allowed in the file
    // name), or a bare file name looked up on PATH.
    std::vector<std::wstring> paths;
    std::vector<std::wstring> args;  // ${SWF} is appended when no arg mentions it
};

struct PlayerOverride {
    std::vector<std::wstring> swf;
    std::vector<std::wstring> args;
    bool hasSwf = false, hasArgs = false;
};

struct RelaySpec {
    bool enabled = false;
    int port = 7777;
};

struct ActionSpec {
    std::string id;
    std::wstring label, description, hint;
    std::vector<std::wstring> swf;      // candidates; http(s):// URLs are passed as-is
    std::vector<std::wstring> args;     // added after the player's own args
    std::vector<std::string> players;   // allowed players in order of preference; empty = all
    std::map<std::string, PlayerOverride> overrides;  // per player id
    RelaySpec relay;                    // "host" actions start the built-in relay
};

struct GameSpec {
    std::string id;
    std::wstring name, description, playerCount, controls, image;
    std::vector<std::string> categories;
    Color color{0x4F, 0x8B, 0xFF, 255}, colorEnd{0x9C, 0x6B, 0xFF, 255};
    std::vector<ActionSpec> actions;
};

struct Category {
    std::string id;
    std::wstring name;
};

struct Config {
    int schemaVersion = 1;
    Branding branding;
    Paths paths;
    Behavior behavior;
    std::vector<PlayerSpec> players;
    std::vector<Category> categories;
    std::vector<GameSpec> games;
    std::map<std::string, std::wstring> text;  // UI strings, defaults + overrides
    std::wstring configDir;                     // folder of launcher.json

    // UI text with ${VAR} substitution from `vars` (pairs of name, value).
    std::wstring t(const std::string& key,
                   std::initializer_list<std::pair<std::wstring_view, std::wstring_view>> vars = {}) const;
    const GameSpec* findGame(std::string_view id) const;
    const PlayerSpec* findPlayer(std::string_view id) const;
    const Category* findCategory(std::string_view id) const;
};

const std::vector<std::pair<std::string, std::wstring>>& defaultTexts();

struct ConfigIssue {
    enum class Severity { Warning, Error } severity;
    std::wstring where, message;
};

struct LoadResult {
    Config config;
    std::vector<ConfigIssue> issues;
    bool ok() const;
};

LoadResult loadConfig(std::string_view jsonText, const std::wstring& configDir);
LoadResult loadConfigFile(const std::wstring& path);
// Cross-references: unique ids, known players and categories, variables.
void validateConfig(const Config& cfg, std::vector<ConfigIssue>& issues);
std::wstring formatIssues(const std::vector<ConfigIssue>& issues);

}  // namespace fl
