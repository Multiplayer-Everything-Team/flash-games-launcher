#include "core/mod/manager.h"

#include "core/fs.h"
#include "core/log.h"
#include "core/mod/package.h"
#include "core/mod/resources.h"
#include "core/process.h"
#include "core/strings.h"
#include "core/win.h"

namespace fl::mod {

Vars withSpec(const Vars& base, const Spec& spec) {
    Vars v = base;
    Vars own = spec.baseVars();
    for (const wchar_t* name : {L"MOD_ID", L"MOD_NAME", L"MOD_VERSION", L"MOD_PUBLISHER", L"MOD_WEBSITE",
                                L"MOD_SUPPORT_URL", L"MOD_DESCRIPTION", L"GAME_NAME", L"GAME_EXE", L"STEAM_APP_ID"})
        v.set(name, own.get(name));
    return v;
}

GameLocation findGame(const Spec& spec, const Vars& base, const std::wstring& chosenDir) {
    Vars v = withSpec(base, spec);
    if (!chosenDir.empty()) {
        std::wstring dir = fs::normalize(chosenDir);
        if (looksLikeGameDir(spec, v, dir)) return {dir, L"Chosen folder"};
    }
    for (const auto& c : detectGame(spec, v))
        if (c.valid) return {c.path, c.source};
    return {};
}

Status inspect(const Spec& spec, const Vars& base, const std::wstring& chosenDir) {
    Status s;
    s.game = findGame(spec, base, chosenDir);
    if (!s.game.found()) return s;
    Vars v = resolveDirs(spec, withSpec(base, spec), s.game.dir);
    std::wstring stateDir = v.get(L"STATE_DIR");
    std::wstring error;
    if (auto st = loadState(stateDir, &error)) {
        if (!iequals(st->modId, spec.mod.id)) {
            s.problem = spec.t("error.foreign", v, {{L"OTHER", st->modName}});
        } else if (std::wstring e = checkRecord(spec, *st, stateDir, s.game.dir, withSpec(base, spec)); !e.empty()) {
            s.problem = e;
        } else {
            s.installed = true;
            s.installedVersion = st->version;
        }
    } else if (fs::isFile(fs::join(stateDir, kManifestName))) {
        s.problem = error;  // there, but unreadable: never guess what the mod owns
    }
    s.running = sys::isProcessRunning(spec.game.processNames);
    return s;
}

Outcome install(const Spec& spec, const Vars& base, Operation op, const std::wstring& gameDir,
                const std::wstring& payloadDir, const ProgressFn& progress, CancelToken& cancel,
                EngineOptions options) {
    Outcome o;
    auto res = openDirectory(payloadDir);
    Vars v = withSpec(base, spec);
    Selection sel = defaultSelection(spec, std::nullopt);
    // Updating or repairing keeps what was chosen at the first install.
    Vars dirs = resolveDirs(spec, v, gameDir);
    if (auto prev = loadState(dirs.get(L"STATE_DIR"))) sel = defaultSelection(spec, prev);
    sel.gameDir = gameDir;
    PlanResult plan = buildPlan(spec, *res, op, sel, v);
    if (!plan.ok) {
        o.message = plan.error;
        o.detail = plan.detail;
        return o;
    }
    Engine engine(spec, res.get(), std::move(options));
    return engine.run(plan.plan, progress, cancel);
}

Outcome uninstall(const Spec& spec, const Vars& base, const std::wstring& gameDir, const ProgressFn& progress,
                  CancelToken& cancel, EngineOptions options) {
    Outcome o;
    Vars v = withSpec(base, spec);
    Vars dirs = resolveDirs(spec, v, gameDir);
    PlanResult plan = buildUninstallPlan(spec, dirs.get(L"STATE_DIR"), v);
    if (!plan.ok) {
        o.message = plan.error;
        o.detail = plan.detail;
        return o;
    }
    Engine engine(spec, nullptr, std::move(options));
    return engine.run(plan.plan, progress, cancel);
}

bool resolveSource(const Spec& spec, const std::wstring& modsInfoBase, Source& out, std::wstring& error) {
    out = {};
    if (!spec.package.modsInfo.empty()) {
        Latest l;
        if (!modsInfoBase.empty() && !fetchLatest(modsInfoBase, spec.package.modsInfo, l, error)) return false;
        out.version = l.version;
        out.url = l.downloadUrl;
        out.sha256 = l.sha256;
        out.minVersion = l.minVersion;
        out.notice = l.notice;
    }
    // A package pinned in launcher.json wins over mods-info.
    if (!spec.package.url.empty()) {
        out.url = spec.package.url;
        out.sha256 = toLower(spec.package.sha256);
        if (out.version.empty()) out.version = L"1";
    }
    return true;
}

Outcome run(const Spec& spec, const Vars& base, const RunRequest& req, const Report& report, CancelToken& cancel,
            EngineOptions options) {
    Outcome o;
    ProgressFn engineProgress = [&](const Progress& p) {
        if (report) report(p.phase, p.fraction, p.item);
    };
    if (req.op == Operation::Uninstall) return uninstall(spec, base, req.gameDir, engineProgress, cancel, std::move(options));

    if (!req.source.published()) {
        o.message = spec.t("mod.notPublished", withSpec(base, spec));
        return o;
    }
    Spec versioned = spec;
    versioned.mod.version = req.source.version;
    if (report) report(spec.t("mod.phase.downloading", withSpec(base, spec)), 0, L"");
    std::atomic<bool> stop{false};
    std::wstring payload, error;
    InstallProgress dl = [&](const InstallStatus& s) {
        if (cancel.cancelled()) stop = true;
        if (report && s.total) report(versioned.t("mod.phase.downloading", withSpec(base, versioned)),
                                      static_cast<double>(s.done) / static_cast<double>(s.total), L"");
    };
    if (!preparePackage(widen(narrow(spec.mod.id)), req.source.version, req.source.url, req.source.sha256, req.cacheDir,
                        req.githubApi, dl, &stop, payload, error)) {
        o.result = cancel.cancelled() ? Outcome::Result::Cancelled : Outcome::Result::Failed;
        o.message = cancel.cancelled() ? spec.t("error.cancelledBody", withSpec(base, spec)) : error;
        return o;
    }
    o = install(versioned, base, req.op, req.gameDir, payload, engineProgress, cancel, std::move(options));
    if (o.result == Outcome::Result::Success) pruneCache(req.cacheDir, spec.mod.id, req.source.version);
    return o;
}

bool launchGame(const Spec& spec, const Vars& base, const std::wstring& gameDir, std::wstring& error) {
    Vars v = resolveDirs(spec, withSpec(base, spec), gameDir);
    if (!spec.launch.uri.empty()) {
        std::wstring uri = v.expand(spec.launch.uri);
        if (sys::openPath(uri)) return true;
        error = L"Couldn't open " + uri;
        return false;
    }
    std::wstring exe = fs::normalize(v.expand(spec.launch.executable));
    std::vector<std::wstring> args;
    std::wstring a = v.expand(spec.launch.arguments);
    if (!a.empty()) args.push_back(a);
    sys::Started p;
    if (!sys::startProcess(exe, args, fs::normalize(v.expand(spec.launch.workingDir)), p, error)) return false;
    CloseHandle(p.process);
    return true;
}

}  // namespace fl::mod
