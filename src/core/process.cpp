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
                  Started& out, std::wstring& error, bool hidden) {
    std::wstring cmd = buildCommandLine(exe, args);
    logger().info(L"Starting: " + cmd + (workDir.empty() ? L"" : L"  (in " + workDir + L")"));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, hidden ? CREATE_NO_WINDOW : 0, nullptr,
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

BackgroundProcess::~BackgroundProcess() { stop(); }

bool BackgroundProcess::start(const std::wstring& exe, const std::vector<std::wstring>& args,
                              const std::wstring& workDir, LineFn onLine, std::wstring& error) {
    stop();
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE write = nullptr;
    if (!CreatePipe(&read_, &write, &sa, 0)) {
        error = win32ErrorMessage(GetLastError());
        return false;
    }
    SetHandleInformation(read_, HANDLE_FLAG_INHERIT, 0);  // only the child's end is inherited
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job_) SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof limits);

    std::wstring cmd = buildCommandLine(exe, args);
    logger().info(L"Starting server: " + cmd + L"  (in " + workDir + L")");
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = write;
    si.hStdError = write;
    PROCESS_INFORMATION pi{};
    // Suspended until it is in the job, so nothing it starts can escape the job.
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    DWORD code = GetLastError();
    CloseHandle(write);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        error = win32ErrorMessage(code);
        stop();
        return false;
    }
    if (job_) AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    HANDLE read = read_;
    reader_ = std::thread([read, onLine] {
        std::string pending;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read, buf, sizeof buf, &n, nullptr) && n) {
            pending.append(buf, n);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (onLine) onLine(widen(line));
            }
        }
        if (!pending.empty() && onLine) onLine(widen(pending));
    });
    return true;
}

void BackgroundProcess::stop() {
    if (job_) {
        TerminateJobObject(job_, 1);
        CloseHandle(job_);
        job_ = nullptr;
    } else if (process_) {
        TerminateProcess(process_, 1);
    }
    // The pipe closes once every process holding it is gone; the reader ends then.
    if (reader_.joinable()) reader_.join();
    if (read_) CloseHandle(read_);
    if (process_) CloseHandle(process_);
    read_ = process_ = nullptr;
    pid_ = 0;
}

bool BackgroundProcess::running() const {
    return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

bool waitForServer(const std::wstring& url, int timeoutMs, const std::function<bool()>& alive) {
    std::wstring rest = url.substr(url.find(L"://") == std::wstring::npos ? 0 : url.find(L"://") + 3);
    std::wstring hostPort = rest.substr(0, rest.find(L'/'));
    std::wstring host = hostPort, port = startsWithI(url, L"https://") ? L"443" : L"80";
    if (size_t colon = hostPort.rfind(L':'); colon != std::wstring::npos) {
        host = hostPort.substr(0, colon);
        port = hostPort.substr(colon + 1);
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    bool up = false;
    ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    while (!up && GetTickCount64() < until && (!alive || alive())) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(narrow(host).c_str(), narrow(port).c_str(), &hints, &res) == 0 && res) {
            SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            up = connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0;
            closesocket(s);
            freeaddrinfo(res);
        }
        if (!up) Sleep(250);
    }
    WSACleanup();
    return up;
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
