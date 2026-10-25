#include <cstring>

#include "core/log.h"
#include "core/strings.h"
#include "core/win.h"
#include "test.h"
// win.h first.
#include <shellapi.h>
#include <winsock2.h>

int g_checkFailures = 0;

std::vector<TestCase>& testRegistry() {
    static std::vector<TestCase> r;
    return r;
}

TempDir::TempDir(const std::string& name) {
    static int counter = 0;
    path_ = fl::fs::join(fl::fs::tempDir(), L"fl-tests\\" + fl::widen(name) + L"-" +
                                                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++counter));
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

void writeText(const std::wstring& path, const std::string& text) {
    fl::fs::ensureDir(fl::fs::parent(path), nullptr, nullptr);
    fl::fs::writeFile(path, text.data(), text.size(), nullptr);
}

std::string readText(const std::wstring& path) {
    std::string s;
    fl::fs::readText(path, s);
    return s;
}

bool exists(const std::wstring& path) { return fl::fs::exists(path); }

// Stand-in for a Flash player in tests\e2e.ps1: appends its arguments (one per
// line) to %FL_FAKE_OUT%, stays open for %FL_FAKE_MS% milliseconds, exits 0.
static int fakePlayer() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::string lines;
    for (int i = 2; i < argc; ++i) lines += fl::narrow(argv[i]) + "\n";
    LocalFree(argv);
    wchar_t out[1024] = L"", ms[32] = L"";
    GetEnvironmentVariableW(L"FL_FAKE_OUT", out, 1024);
    GetEnvironmentVariableW(L"FL_FAKE_MS", ms, 32);
    if (*out) {
        FILE* f = _wfopen(out, L"ab");
        if (f) {
            fwrite(lines.data(), 1, lines.size(), f);
            fclose(f);
        }
    }
    Sleep(static_cast<DWORD>(_wtoi(ms)));
    return 0;
}

// Stand-in for an HTML5 game's server (tests\e2e.ps1): prints a tunnel-style
// link the way the PB2 co-op server does, then answers every HTTP request with
// "ok" until it is stopped.
static int fakeServer(int port) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));
    if (bind(l, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(l, 16) != 0) return 1;
    printf("Fake server running.\n  Internet:  https://fake-test-link.trycloudflare.com   <- send this\n");
    printf("  Same Wi-Fi/LAN:   http://10.1.2.3:%d/\n", port);
    fflush(stdout);
    for (;;) {
        SOCKET c = accept(l, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        char buf[2048];
        recv(c, buf, sizeof buf, 0);
        const char reply[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        send(c, reply, sizeof reply - 1, 0);
        closesocket(c);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--fake-player") == 0) return fakePlayer();
    if (argc > 2 && strcmp(argv[1], "--fake-server") == 0) return fakeServer(atoi(argv[2]));
    const char* filter = argc > 1 ? argv[1] : nullptr;
    TempDir logs("logs");
    fl::logger().open(logs / L"tests.log");
    fl::logger().setLevel(fl::LogLevel::Debug);
    int failed = 0, ran = 0;
    for (const auto& t : testRegistry()) {
        if (filter && !strstr(t.name, filter)) continue;
        ++ran;
        int before = g_checkFailures;
        bool threw = false;
        try {
            t.fn();
        } catch (const std::exception& e) {
            threw = true;
            fprintf(stderr, "    exception: %s\n", e.what());
        }
        bool ok = !threw && g_checkFailures == before;
        printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", t.name);
        fflush(stdout);
        if (!ok) ++failed;
    }
    printf("\n%d of %d tests passed\n", ran - failed, ran);
    if (failed) {
        fl::logger().close();
        printf("log: %ls\n", (logs / L"tests.log").c_str());
    }
    return failed ? 1 : 0;
}
