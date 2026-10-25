#include "core/mod/package.h"

#include <filesystem>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/process.h"
#include "core/strings.h"
#include "core/win.h"

namespace fl::mod {

namespace {

std::wstring field(const json::Value& o, const char* key) {
    const json::Value* v = o.find(key);
    return v && v->isString() ? widen(v->str()) : std::wstring();
}

void removeTree(const std::wstring& dir) {
    std::error_code ec;
    std::filesystem::remove_all(fs::longPath(dir), ec);
}

std::wstring baseFolder(const std::wstring& base) {
    std::wstring b = base;
    while (!b.empty() && (b.back() == L'/' || b.back() == L'\\')) b.pop_back();
    return b + L"/";
}

}  // namespace

bool fetchLatest(const std::wstring& base, const std::wstring& modsInfoId, Latest& out, std::wstring& error) {
    if (base.empty() || modsInfoId.empty()) {
        error = L"No mods-info address or mod id";
        return false;
    }
    std::string text;
    if (!httpGetText(baseFolder(base) + L"mods/" + modsInfoId + L".json", text, error)) return false;
    std::string perr;
    json::Value v = json::parse(text, &perr);
    if (!perr.empty() || !v.isObject()) {
        error = L"mods-info has no readable file for " + modsInfoId;
        return false;
    }
    out = {};
    out.version = field(v, "latest_version");
    out.minVersion = field(v, "min_version");
    out.downloadUrl = field(v, "download_url");
    out.sha256 = toLower(field(v, "download_sha256"));
    out.status = field(v, "status");
    out.notice = field(v, "notice");
    // Only https downloads; mods-info is read by every player.
    if (!out.downloadUrl.empty() && !startsWithI(out.downloadUrl, L"https://") &&
        !startsWithI(out.downloadUrl, L"http://127.0.0.1")) {
        error = L"mods-info lists a download that isn't https://";
        out.downloadUrl.clear();
        return false;
    }
    return true;
}

std::wstring githubAssetDigest(const std::wstring& assetUrl, const std::wstring& apiBase, std::wstring& error) {
    // https://github.com/<owner>/<repo>/releases/download/<tag>/<file>
    const std::wstring marker = L"github.com/";
    size_t at = assetUrl.find(marker);
    if (at == std::wstring::npos || !startsWithI(assetUrl, L"https://")) {
        error = L"No checksum is published for this download";
        return {};
    }
    auto parts = split(assetUrl.substr(at + marker.size()), L'/');
    if (parts.size() < 6 || parts[2] != L"releases" || parts[3] != L"download") {
        error = L"No checksum is published for this download";
        return {};
    }
    std::wstring api = (apiBase.empty() ? std::wstring(L"https://api.github.com") : apiBase) + L"/repos/" + parts[0] +
                       L"/" + parts[1] + L"/releases/tags/" + parts[4];
    std::string body;
    if (!httpGetText(api, body, error)) return {};
    std::string perr;
    json::Value rel = json::parse(body, &perr);
    const json::Value* assets = rel.isObject() ? rel.find("assets") : nullptr;
    if (assets && assets->isArray())
        for (size_t i = 0; i < assets->size(); ++i) {
            const json::Value& a = assets->at(i);
            if (field(a, "name") != parts[5]) continue;
            std::wstring digest = field(a, "digest");
            if (startsWithI(digest, L"sha256:")) return toLower(digest.substr(7));
        }
    error = L"GitHub publishes no SHA-256 for " + parts[5];
    return {};
}

bool unpackZip(const std::wstring& archive, const std::wstring& destDir, std::wstring& error) {
    removeTree(destDir);
    if (!fs::ensureDir(destDir, nullptr, nullptr)) {
        error = L"Cannot create " + destDir;
        return false;
    }
    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    sys::Started tar;
    std::wstring startError;
    // tar.exe ships with Windows 10 1803+ and unpacks .zip archives.
    if (!sys::startProcess(fs::join(sysDir, L"tar.exe"), {L"-xf", archive, L"-C", destDir}, fs::parent(archive), tar,
                           startError, true)) {
        error = L"Cannot unpack the package: " + startError;
        return false;
    }
    WaitForSingleObject(tar.process, 180000);
    DWORD code = 1;
    GetExitCodeProcess(tar.process, &code);
    CloseHandle(tar.process);
    if (code != 0) {
        error = L"Unpacking the package failed";
        removeTree(destDir);
        return false;
    }
    // Nothing may be a link, and everything must sit inside destDir.
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(fs::longPath(destDir), ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_symlink(ec) || !fs::isUnder(fs::normalize(it->path().wstring()), destDir)) {
            error = L"The package contains a link or a path outside its folder; it was not used.";
            removeTree(destDir);
            return false;
        }
    }
    return true;
}

bool preparePackage(const std::wstring& id, const std::wstring& version, const std::wstring& url, std::wstring sha256,
                    const std::wstring& cacheDir, const std::wstring& githubApi, const InstallProgress& progress,
                    const std::atomic<bool>* cancel, std::wstring& payloadDir, std::wstring& error) {
    if (url.empty()) {
        error = L"This mod has no download yet";
        return false;
    }
    std::wstring root = fs::join(fs::join(cacheDir, id), version);
    payloadDir = fs::join(root, L"payload");
    std::wstring marker = fs::join(root, L"verified.txt");
    sha256 = toLower(sha256);
    std::wstring digestError;
    if (sha256.empty()) sha256 = githubAssetDigest(url, githubApi, digestError);

    // Already downloaded and unpacked: reuse it (this also lets Repair work offline).
    std::string saved;
    if (fs::readText(marker, saved) && fs::isDir(payloadDir) && (sha256.empty() || trim(widen(saved)) == sha256)) {
        logger().info(L"Using the cached package " + root);
        return true;
    }
    if (sha256.empty()) {
        error = digestError.empty() ? L"No checksum is published for this download" : digestError;
        error += L" The package was not installed.";
        return false;
    }

    removeTree(root);
    CatalogFile file;
    file.name = L"package.zip";
    file.url = url;
    file.sha256 = sha256;
    InstallResult dl = downloadGameFiles({file}, root, progress, cancel);
    if (!dl.ok) {
        error = dl.error;
        removeTree(root);
        return false;
    }
    std::wstring archive = fs::join(root, L"package.zip");
    if (!unpackZip(archive, payloadDir, error)) {
        removeTree(root);
        return false;
    }
    DeleteFileW(fs::longPath(archive).c_str());
    std::string text = narrow(sha256);
    fs::writeFile(marker, text.data(), text.size(), nullptr);
    return true;
}

void pruneCache(const std::wstring& cacheDir, const std::wstring& id, const std::wstring& keepVersion) {
    std::wstring dir = fs::join(cacheDir, id);
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(fs::longPath(dir), ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        if (it->is_directory(ec) && !iequals(it->path().filename().wstring(), keepVersion))
            removeTree(it->path().wstring());
}

}  // namespace fl::mod
