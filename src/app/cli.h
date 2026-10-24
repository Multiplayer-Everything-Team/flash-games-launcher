// Command-line parsing for the launcher executable.
#pragma once
#include <string>
#include <vector>

namespace fl {

struct Args {
    std::wstring config, log, snapshot, launch;  // launch: "game" or "game:action"
    std::string installPlayer;                   // player id for --install-player
    std::string downloadGame;                    // game id or "all" for --download-game
    std::string select;
    int width = 0, height = 0, relayPort = 0;
    bool list = false, validate = false, help = false, demoHost = false, version = false;
    std::wstring error;
};

Args parseArgs(int argc, wchar_t** argv);
std::wstring usageText();

}  // namespace fl
