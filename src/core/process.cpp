#include "process.h"

#include <algorithm>

#include "fs.h"
#include "log.h"
#include "strings.h"
// win.h first: these need the full Windows types.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>

namespace fl::sys {

std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    out += L"\"";
    return out;
}

std::wstring buildCommandLine(const std::wstring& exe, const std::vector<std::wstring>& args) {
    // argv[0] is always quoted: CreateProcess otherwise tries "C:\Program.exe" first.
    std::wstring cmd = L"\"" + exe + L"\"";
    for (const auto& a : args) cmd += L" " + quoteArg(a);
    return cmd;
}

bool startProcess(const std::wstring& exe, const std::vector<std::wstring>& args, const std::wstring& workDir,
                  Started& out, std::wstring& error) {
    std::wstring cmd = buildCommandLine(exe, args);
    logger().info(L"Starting: " + cmd + (workDir.empty() ? L"" : L"  (in " + workDir + L")"));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
        error = win32ErrorMessage(GetLastError());
        logger().error(L"CreateProcess failed: " + error);
        return false;
    }
    CloseHandle(pi.hThread);
    out.process = pi.hProcess;
    out.pid = pi.dwProcessId;
    return true;
}

bool openPath(const std::wstring& p) {
    auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return r > 32;
}

bool revealInExplorer(const std::wstring& file) {
    if (fs::isDir(file)) return openPath(file);
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(file.c_str());
    if (!pidl) return openPath(fs::parent(file));
    HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
    return SUCCEEDED(hr);
}

std::vector<NetAddress> localAddresses() {
    std::vector<NetAddress> out, virtualOnes;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            if (sin->sin_family != AF_INET) continue;
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&sin->sin_addr);
            if (b[0] == 169 && b[1] == 254) continue;  // link-local: nobody can reach it
            wchar_t text[32];
            InetNtopW(AF_INET, &sin->sin_addr, text, 32);
            NetAddress n{text, a->FriendlyName ? a->FriendlyName : L""};
            // Hyper-V/WSL switches are noise; VPN adapters (ZeroTier, Radmin, Tailscale) are
            // exactly what friends over the internet need, so keep those.
            std::wstring lower = toLower(n.adapter);
            if (lower.find(L"vethernet") != std::wstring::npos || lower.find(L"virtualbox") != std::wstring::npos ||
                lower.find(L"vmware") != std::wstring::npos)
                continue;
            (a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211 ? out : virtualOnes).push_back(n);
        }
    }
    out.insert(out.end(), virtualOnes.begin(), virtualOnes.end());
    return out;
}

ProcessWatcher::ProcessWatcher(ExitFn onExit) : onExit_(std::move(onExit)) {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread([this] { loop(); });
}

ProcessWatcher::~ProcessWatcher() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
    for (auto& i : items_) CloseHandle(i.h);  // the games keep running; we only stop watching
    CloseHandle(wake_);
}

void ProcessWatcher::watch(HANDLE process, DWORD pid) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        items_.push_back({process, pid});
    }
    SetEvent(wake_);
}

size_t ProcessWatcher::count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return items_.size();
}

void ProcessWatcher::loop() {
    for (;;) {
        std::vector<HANDLE> handles{wake_};
        std::vector<Item> snapshot;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return;
            // WaitForMultipleObjects is limited to 64 handles; nobody runs 63 games at once.
            for (size_t i = 0; i < items_.size() && handles.size() < MAXIMUM_WAIT_OBJECTS; ++i) {
                handles.push_back(items_[i].h);
                snapshot.push_back(items_[i]);
            }
        }
        DWORD r = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) {
            if (r == WAIT_FAILED) Sleep(50);
            continue;
        }
        size_t idx = r - WAIT_OBJECT_0 - 1;
        if (idx >= snapshot.size()) continue;
        Item done = snapshot[idx];
        DWORD code = 0;
        GetExitCodeProcess(done.h, &code);
        {
            std::lock_guard<std::mutex> lock(mu_);
            items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const Item& i) { return i.h == done.h; }),
                         items_.end());
        }
        CloseHandle(done.h);
        if (onExit_) onExit_(done.pid, code);
    }
}

}  // namespace fl::sys
