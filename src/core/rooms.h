// Public rooms on the Multiplayer Everything central server (global-mods-server
// relay.py): "RELAY|1|LIST|<game>" answers "LIST|<code>,<players>,<max>,<name>;..."
// as NUL-terminated XMLSocket text. Also resolves which server to ask, the same
// way the mods do: mods-info servers.json first, then a built-in address.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct RoomInfo {
    std::wstring code, name;
    int players = 0, max = 0;
};

// Parses the LIST reply. Room names may contain commas (they're the last field).
bool parseRoomList(std::string_view reply, std::vector<RoomInfo>& out);

// One short request on its own connection. `server` is "host:port".
// `rejected` (optional) is set when the server answered but doesn't run this
// game yet ("E|Unknown game ..."): the mod's mods-info file isn't merged.
bool listRooms(const std::wstring& server, const std::string& gameId, std::vector<RoomInfo>& out, std::wstring& error,
               int timeoutMs = 4000, bool* rejected = nullptr);

// First "online" server in mods-info servers.json at `serversUrl`; `fallback`
// when that can't be read. Returns "host:port" or empty.
std::wstring resolveRoomServer(const std::wstring& serversUrl, const std::wstring& fallback, std::wstring* error);

}  // namespace fl
