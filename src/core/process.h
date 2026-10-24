// Starting players, watching them exit, and other OS integration that has no
// UI: opening folders/URLs and listing this PC's network addresses.
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "win.h"

namespace fl::sys {

std::wstring quoteArg(const std::wstring& arg);
std::wstring buildCommandLine(const std::wstring& exe, const std::vector<std::wstring>& args);

struct Started {
    HANDLE process = nullptr;  // owned by the caller (or handed to ProcessWatcher)
    DWORD pid = 0;
};
bool startProcess(const std::wstring& exe, const std::vector<std::wstring>& args, const std::wstring& workDir,
                  Started& out, std::wstring& error);

bool openPath(const std::wstring& pathOrUrl);
// Opens Explorer with `file` selected (or the folder itself).
bool revealInExplorer(const std::wstring& file);

struct NetAddress {
    std::wstring address, adapter;  // "192.168.1.20", "Wi-Fi" / "ZeroTier One"
};
// Up, non-loopback IPv4 addresses, physical adapters first.
std::vector<NetAddress> localAddresses();

// Waits for any number of processes on one thread and calls `onExit` there
// (pid, exit code). Closes the handles it was given.
class ProcessWatcher {
public:
    using ExitFn = std::function<void(DWORD pid, DWORD exitCode)>;
    explicit ProcessWatcher(ExitFn onExit);
    ~ProcessWatcher();
    ProcessWatcher(const ProcessWatcher&) = delete;
    ProcessWatcher& operator=(const ProcessWatcher&) = delete;

    void watch(HANDLE process, DWORD pid);
    size_t count() const;

private:
    void loop();
    struct Item {
        HANDLE h;
        DWORD pid;
    };
    ExitFn onExit_;
    mutable std::mutex mu_;
    std::vector<Item> items_;
    HANDLE wake_ = nullptr;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace fl::sys
