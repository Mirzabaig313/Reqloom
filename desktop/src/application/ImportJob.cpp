// ImportJob — see header.
#include "ImportJob.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/ImportCredentials.h>
#include <reqloom/engine/ProjectTrustStore.h>
#include <reqloom/engine/SecretStore.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>

#include <exception>
#include <fstream>
#include <system_error>
#include <utility>

namespace reqloom::desktop {

namespace {

namespace fs = std::filesystem;

/// Sniff a failed import's file head for Postman / Swagger 2.0 so the toast is actionable.
/// Empty when nothing is recognized, so the caller falls back to the engine's detail.
[[nodiscard]] QString importFailureHint(const fs::path& spec) {
    std::ifstream in{spec, std::ios::binary};
    if (!in) {
        return {};
    }
    std::string head(8192, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));

    if (head.find("schema.getpostman.com") != std::string::npos ||
        head.find("_postman_id") != std::string::npos) {
        // Postman is supported, so a failure here likely means a malformed export or unsupported
        // schema version.
        return QStringLiteral(
            "This Postman collection couldn't be imported. Re-export it as Collection v2.1 "
            "and try again.");
    }
    if (head.find("\"swagger\"") != std::string::npos ||
        head.find("swagger:") != std::string::npos) {
        return QStringLiteral(
            "This Swagger 2.0 document couldn't be imported. Check that it's valid and has a "
            "non-empty `paths` section.");
    }
    return {};
}

/// The credential part of the import review notes: secret names only, never values.
[[nodiscard]] QString importCredentialNotes(const engine::CredentialMoveReport& report) {
    QString notes;
    if (!report.moved.empty()) {
        QStringList names;
        for (const auto& name : report.moved) {
            names << QString::fromStdString(name);
        }
        notes +=
            QCoreApplication::translate("AppController", "Moved into the project's secrets: %1\n")
                .arg(names.join(QStringLiteral(", ")));
    }
    for (const auto& line : report.warnings()) {
        notes += QString::fromStdString(line) + QLatin1Char('\n');
    }
    return notes;
}

[[nodiscard]] ImportOutcome refused(ImportOutcome out, QString detail) {
    out.status = ImportOutcome::Status::Refused;
    out.errorDetail = std::move(detail);
    return out;
}

/// An exception escaping the import (e.g. `std::bad_alloc` on a huge file) must not cross the
/// future into a GUI slot, where it would end the app.
[[nodiscard]] ImportOutcome failed(ImportOutcome out, const QString& what) {
    const bool written = !out.loadDir.isEmpty();
    out.status = written ? ImportOutcome::Status::WriteFailed : ImportOutcome::Status::ImportFailed;
    out.errorDetail =
        QCoreApplication::translate("AppController", "unexpected error: %1").arg(what);
    return out;
}

/// Same folder on disk when both exist (symlinks, letter case), else the same normalised path.
[[nodiscard]] bool sameFolder(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec)) {
        const bool same = fs::equivalent(a, b, ec);
        if (!ec) {
            return same;
        }
    }
    return fs::weakly_canonical(a, ec).lexically_normal() ==
           fs::weakly_canonical(b, ec).lexically_normal();
}

}  // namespace

std::string projectFolderSlug(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    bool pendingDash = false;
    for (const char c : name) {
        const bool alnum =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (alnum) {
            if (pendingDash && !out.empty()) {
                out.push_back('-');
            }
            pendingDash = false;
            out.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c));
        } else {
            pendingDash = true;
        }
    }
    return out.empty() ? std::string{"imported-project"} : out;
}

ImportOutcome runImportJob(const ImportRequest& request,
                           engine::SecretStore* store,
                           engine::ProjectTrustStore* trust) noexcept {
    ImportOutcome out;
    try {
        const fs::path& spec = request.spec;
        out.specPath = QString::fromStdString(spec.string());
        const fs::path base = request.baseDir.empty() ? spec.parent_path() : request.baseDir;
        out.baseDir = QString::fromStdString(base.string());

        auto imported = engine::importAny(spec, spec.parent_path());
        if (!imported) {
            out.status = ImportOutcome::Status::ImportFailed;
            const QString hint = importFailureHint(spec);
            out.errorDetail =
                hint.isEmpty() ? QString::fromStdString(imported.error().detail) : hint;
            return out;
        }
        const fs::path projectDir = base / projectFolderSlug(imported->project.name);
        out.loadDir = QString::fromStdString(projectDir.string());

        for (const auto& blocked : request.blocked) {
            if (sameFolder(blocked.root, projectDir)) {
                return refused(
                    out,
                    QCoreApplication::translate("AppController",
                                                "The project in %1 is open and %2; nothing was "
                                                "imported.")
                        .arg(out.loadDir, blocked.reason));
            }
        }
        std::error_code ec;
        const bool hasProject = fs::exists(projectDir / "reqloom.yaml", ec);
        if (!hasProject && fs::is_directory(projectDir, ec) && !fs::is_empty(projectDir, ec)) {
            // Without a reqloom.yaml there is nothing to confirm replacing, but the writer would
            // still replace same-named files in the folder.
            return refused(
                out,
                QCoreApplication::translate("AppController",
                                            "%1 already exists and holds other files; nothing "
                                            "was imported. Move or rename it, then import "
                                            "again.")
                    .arg(out.loadDir));
        }
        if (!request.overwrite && hasProject) {
            out.status = ImportOutcome::Status::NeedsOverwrite;
            return out;
        }

        auto written = engine::writeImportedProject(projectDir,
                                                    imported->project,
                                                    imported->credentials,
                                                    store,
                                                    trust,
                                                    {.overwrite = request.overwrite});
        if (!written) {
            out.status = written.error().code == engine::ErrorCode::ProjectExists
                             ? ImportOutcome::Status::NeedsOverwrite
                             : ImportOutcome::Status::WriteFailed;
            out.errorDetail = QString::fromStdString(written.error().detail);
            return out;
        }
        for (const auto& [resId, resource] : imported->project.resources) {
            out.operations += static_cast<int>(resource.operations.size());
        }
        out.status = ImportOutcome::Status::Success;
        out.resources = static_cast<int>(imported->project.resources.size());
        out.notes = importCredentialNotes(written->credentials) +
                    QString::fromStdString(imported->warnings);
        out.movedCredentials = static_cast<int>(written->credentials.moved.size());
        out.keptCredentials = static_cast<int>(written->credentials.kept.size());
        return out;
    } catch (const std::exception& e) {
        return failed(out, QString::fromUtf8(e.what()));
    } catch (...) {
        return failed(out, QStringLiteral("unknown error"));
    }
}

ImportOutcome runImportJobWithUserStores(const ImportRequest& request) noexcept {
    try {
        const auto store = engine::makeKeychainSecretStore();
        const auto trust = engine::makeFileProjectTrustStore();
        return runImportJob(request, store.get(), trust.get());
    } catch (const std::exception& e) {
        return failed(ImportOutcome{}, QString::fromUtf8(e.what()));
    } catch (...) {
        return failed(ImportOutcome{}, QStringLiteral("unknown error"));
    }
}

}  // namespace reqloom::desktop
