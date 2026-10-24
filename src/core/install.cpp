#include "install.h"

#include <filesystem>
#include <vector>

#include "fs.h"
#include "json.h"
#include "log.h"
#include "process.h"
#include "strings.h"
#include "win.h"
// win.h first: winhttp needs the full Windows types.
#include <winhttp.h>

namespace fl {

namespace {

class Handle {
public:
    explicit Handle(HINTERNET h = nullptr) : h_(h) {}
    ~Handle() {
        if (h_) WinHttpCloseHandle(h_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HINTERNET get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HINTERNET h_;
};

std::wstring lastError(const wchar_t* what) { return std::wstring(what) + L": " + win32ErrorMessage(GetLastError()); }

// GET `url`, following redirects (GitHub sends downloads to its CDN). The body
// goes to `sink`; returns false with `error` set on failure or cancel.
bool httpGet(const std::wstring& url, const std::function<bool(const char*, size_t)>& sink, uint64_t* contentLength,
             const std::atomic<bool>* cancel, std::wstring& error) {
    URL_COMPONENTS uc{sizeof uc};
    wchar_t host[256] = L"", path[4096] = L"";
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    wchar_t extra[2048] = L"";
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        error = L"Bad download address: " + url;
        return false;
    }
    Handle session(WinHttpOpen(L"MultiplayerEverything-FlashLauncher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = lastError(L"WinHttpOpen");
        return false;
    }
    Handle conn(WinHttpConnect(session.get(), host, uc.nPort, 0));
    std::wstring object = std::wstring(path) + extra;
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    Handle req(conn ? WinHttpOpenRequest(conn.get(), L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags)
                    : nullptr);
    if (!req) {
        error = lastError((L"Connecting to " + std::wstring(host)).c_str());
        return false;
    }
    // GitHub's API answers JSON only to clients that send an Accept header.
    const wchar_t* headers = L"Accept: application/vnd.github+json, */*\r\n";
    if (!WinHttpSendRequest(req.get(), headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.get(), nullptr)) {
        error = lastError((L"Downloading from " + std::wstring(host)).c_str());
        return false;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = std::wstring(host) + L" answered HTTP " + std::to_wstring(status);
        return false;
    }
    if (contentLength) {
        wchar_t len[32] = L"";
        DWORD lsize = sizeof len;
        *contentLength = WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len,
                                             &lsize, WINHTTP_NO_HEADER_INDEX)
                             ? _wcstoui64(len, nullptr, 10)
                             : 0;
    }
    std::vector<char> buf(256 * 1024);
    for (;;) {
        if (cancel && *cancel) {
            error = L"Cancelled";
            return false;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req.get(), buf.data(), static_cast<DWORD>(buf.size()), &got)) {
            error = lastError(L"Download interrupted");
            return false;
        }
        if (got == 0) return true;
        if (!sink(buf.data(), got)) {
            error = L"Could not write the download to disk";
            return false;
        }
    }
}

struct Asset {
    std::wstring url, name, version, sha256;
    uint64_t size = 0;
};

bool findAsset(const InstallSpec& spec, Asset& out, const std::atomic<bool>* cancel, std::wstring& error) {
    if (!spec.url.empty()) {
        out.url = spec.url;
        out.name = fs::fileName(replaceAll(spec.url, L"/", L"\\"));
        out.sha256 = toLower(spec.sha256);
        return true;
    }
    std::string body;
    std::wstring api = L"https://api.github.com/repos/" + widen(spec.github) + L"/releases/latest";
    if (!httpGet(api, [&](const char* d, size_t n) { body.append(d, n); return body.size() < 8 * 1024 * 1024; },
                 nullptr, cancel, error))
        return false;
    std::string perr;
    json::Value rel = json::parse(body, &perr);
    const json::Value* assets = rel.isObject() ? rel.find("assets") : nullptr;
    if (!assets || !assets->isArray()) {
        error = L"GitHub returned no release information for " + widen(spec.github);
        return false;
    }
    if (const json::Value* tag = rel.find("tag_name"); tag && tag->isString()) out.version = widen(tag->str());
    for (size_t i = 0; i < assets->size(); ++i) {
        const json::Value& a = assets->at(i);
        const json::Value* name = a.find("name");
        const json::Value* url = a.find("browser_download_url");
        if (!name || !name->isString() || !url || !url->isString()) continue;
        if (!globMatch(spec.asset, widen(name->str()))) continue;
        out.name = widen(name->str());
        out.url = widen(url->str());
        if (const json::Value* sz = a.find("size"); sz && sz->isNumber()) out.size = static_cast<uint64_t>(sz->number());
        if (const json::Value* dg = a.find("digest"); dg && dg->isString() && dg->str().rfind("sha256:", 0) == 0)
            out.sha256 = toLower(widen(dg->str().substr(7)));
        return true;
    }
    error = L"The latest " + widen(spec.github) + L" release has no file matching " + spec.asset;
    return false;
}

// Finds `exe` in `dir` or one folder down (some archives wrap everything in a
// top folder). Returns the folder that contains it, or empty.
std::wstring folderWith(const std::wstring& dir, const std::wstring& exe) {
    if (fs::isFile(fs::join(dir, exe))) return dir;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_directory() && fs::isFile(fs::join(e.path().wstring(), exe))) return e.path().wstring();
    return {};
}

void removeTree(const std::wstring& dir) {
    std::error_code ec;
    std::filesystem::remove_all(fs::longPath(dir), ec);
}

}  // namespace

InstallResult installPlayer(const PlayerSpec& player, const Vars& vars, const InstallProgress& progress,
                            const std::atomic<bool>* cancel) {
    InstallResult r;
    InstallStatus st;
    auto report = [&](InstallStatus::Stage s) {
        st.stage = s;
        if (progress) progress(st);
    };
    auto fail = [&](const std::wstring& msg) {
        r.error = msg;
        logger().error(L"Installing " + player.name + L" failed: " + msg);
        report(InstallStatus::Stage::Failed);
        return r;
    };
    const InstallSpec& spec = player.install;
    if (!spec.enabled()) return fail(L"No download is configured for " + player.name);
    std::wstring target = fs::normalize(vars.expand(spec.dir));
    std::wstring exe = spec.exe.empty() ? std::wstring(L"ruffle.exe") : spec.exe;
    if (target.empty() || fs::isDangerousRoot(target)) return fail(L"Unsafe install folder: " + target);

    report(InstallStatus::Stage::Finding);
    Asset asset;
    std::wstring error;
    if (!findAsset(spec, asset, cancel, error)) return fail(error);
    r.version = asset.version;
    logger().info(L"Installing " + player.name + L" " + asset.version + L" from " + asset.url);

    // Download next to the target so the final move is a rename on the same drive.
    std::wstring work = target + L".installing";
    removeTree(work);
    if (!fs::ensureDir(work, nullptr, nullptr)) return fail(L"Cannot create " + work);
    std::wstring archive = fs::join(work, asset.name.empty() ? L"download.zip" : asset.name);
    FILE* f = _wfopen(fs::longPath(archive).c_str(), L"wb");
    if (!f) {
        removeTree(work);
        return fail(L"Cannot write " + archive);
    }
    fs::Sha256 hash;
    st.total = asset.size;
    report(InstallStatus::Stage::Downloading);
    uint64_t length = 0;
    bool ok = httpGet(
        asset.url,
        [&](const char* d, size_t n) {
            hash.update(d, n);
            st.done += n;
            if (!st.total && length) st.total = length;
            if (progress) progress(st);
            return fwrite(d, 1, n, f) == n;
        },
        &length, cancel, error);
    fclose(f);
    if (!ok) {
        removeTree(work);
        return fail(error);
    }

    report(InstallStatus::Stage::Verifying);
    std::string digest = hash.hex();
    if (asset.size && st.done != asset.size) {
        removeTree(work);
        return fail(L"The download is incomplete (" + formatBytes(st.done) + L" of " + formatBytes(asset.size) + L")");
    }
    if (!asset.sha256.empty() && widen(digest) != asset.sha256) {
        removeTree(work);
        return fail(L"The download doesn't match its published checksum, so it was not installed.");
    }

    report(InstallStatus::Stage::Unpacking);
    // tar.exe ships with Windows 10 1803+ and unpacks .zip archives.
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring unpacked = fs::join(work, L"files");
    fs::ensureDir(unpacked, nullptr, nullptr);
    sys::Started tar;
    if (!sys::startProcess(fs::join(sys, L"tar.exe"), {L"-xf", archive, L"-C", unpacked}, work, tar, error, true)) {
        removeTree(work);
        return fail(L"Cannot unpack the download: " + error);
    }
    WaitForSingleObject(tar.process, 120000);
    DWORD code = 1;
    GetExitCodeProcess(tar.process, &code);
    CloseHandle(tar.process);
    std::wstring root = code == 0 ? folderWith(unpacked, exe) : std::wstring();
    if (root.empty()) {
        removeTree(work);
        return fail(code == 0 ? L"The download doesn't contain " + exe : L"Unpacking the download failed");
    }

    // Swap folders: keep the old install until the new one is in place.
    std::wstring old = target + L".old";
    removeTree(old);
    if (fs::exists(target) && !MoveFileExW(fs::longPath(target).c_str(), fs::longPath(old).c_str(), 0)) {
        removeTree(work);
        return fail(lastError((L"Cannot replace " + target).c_str()));
    }
    fs::ensureDir(fs::parent(target), nullptr, nullptr);
    if (!MoveFileExW(fs::longPath(root).c_str(), fs::longPath(target).c_str(), 0)) {
        std::wstring msg = lastError((L"Cannot move the player into " + target).c_str());
        if (fs::exists(old)) MoveFileExW(fs::longPath(old).c_str(), fs::longPath(target).c_str(), 0);
        removeTree(work);
        return fail(msg);
    }
    removeTree(old);
    removeTree(work);
    r.ok = true;
    r.exe = fs::join(target, exe);
    logger().info(player.name + L" installed: " + r.exe);
    report(InstallStatus::Stage::Done);
    return r;
}

}  // namespace fl
