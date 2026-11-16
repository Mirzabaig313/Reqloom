// ImportCredentials — detect credentials in an imported project and move them into secrets.
#include "../domain/DependencyResolver.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/ImportCredentials.h>
#include <reqloom/engine/ProjectIdentity.h>
#include <reqloom/engine/ProjectTrustStore.h>
#include <reqloom/engine/SecretStore.h>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace reqloom::engine {

namespace {

namespace fs = std::filesystem;

[[nodiscard]] bool isAlnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

[[nodiscard]] bool isSpace(char c) {
    return c == ' ' || c == '\t';
}

[[nodiscard]] std::string toLower(std::string_view s) {
    std::string out{s};
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return out;
}

/// `api key` → `API_KEY`: letters and digits upper-cased, every other run becomes one `_`.
[[nodiscard]] std::string upperSnake(std::string_view s) {
    std::string out;
    bool pendingSep{false};
    for (const char c : s) {
        if (!isAlnum(c)) {
            pendingSep = true;
            continue;
        }
        if (pendingSep && !out.empty()) {
            out.push_back('_');
        }
        pendingSep = false;
        out.push_back(static_cast<char>((c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c));
    }
    return out;
}

/// A valid, unused secret name built from `base`; records it in `taken`.
[[nodiscard]] std::string claimName(std::string base, std::set<std::string>& taken) {
    if (base.empty() || (base.front() >= '0' && base.front() <= '9')) {
        base = "SECRET_" + base;
        if (base.back() == '_') {
            base.pop_back();
        }
    }
    std::string name = base;
    for (int n{2}; taken.contains(name); ++n) {
        name = std::format("{}_{}", base, n);
    }
    taken.insert(name);
    return name;
}

[[nodiscard]] bool isLiteral(std::string_view value) {
    return !value.empty() && value.find("{{") == std::string_view::npos;
}

struct HeaderCredential {
    std::string prefix;
    std::string credential;
};

/// The credential part of a literal auth header value, or nothing when the header carries
/// no credential under the import rule (see `detectImportedCredentials`).
[[nodiscard]] std::optional<HeaderCredential> headerCredential(std::string_view header,
                                                               std::string_view value) {
    const auto name = toLower(header);
    const bool authorization = name == "authorization" || name == "proxy-authorization";
    if (!authorization && name != "x-api-key") {
        return std::nullopt;
    }
    std::size_t start = 0;
    while (start < value.size() && isSpace(value[start])) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start && isSpace(value[end - 1])) {
        --end;
    }
    if (authorization) {
        std::size_t word = start;
        while (word < end && (isAlnum(value[word]) || value[word] == '-' || value[word] == '_' ||
                              value[word] == '.')) {
            ++word;
        }
        std::size_t rest = word;
        while (rest < end && isSpace(value[rest])) {
            ++rest;
        }
        if (word > start && rest > word && rest < end) {
            start = rest;
        }
        // A bare scheme word (`Bearer ` with the token left blank) carries no credential.
        const auto only = toLower(value.substr(start, end - start));
        if (word == end && (only == "bearer" || only == "basic" || only == "digest")) {
            return std::nullopt;
        }
    }
    HeaderCredential out{std::string{value.substr(0, start)},
                         std::string{value.substr(start, end - start)}};
    if (!isLiteral(out.credential)) {
        return std::nullopt;
    }
    return out;
}

void addFlaggedCredentials(const Project& project,
                           const std::vector<SourceSecretVariable>& flagged,
                           std::set<std::string>& taken,
                           std::vector<ImportedCredential>& out) {
    // variable → (environment → value), each pair once.
    std::map<std::string, std::map<std::string, std::string>> byVariable;
    std::vector<std::string> order;
    for (const auto& f : flagged) {
        const auto env = project.environments.find(f.environment);
        if (env == project.environments.end()) {
            continue;
        }
        const auto var = env->second.find(f.variable);
        if (var == env->second.end() || !isLiteral(var->second)) {
            continue;
        }
        if (!byVariable.contains(f.variable)) {
            order.push_back(f.variable);
        }
        byVariable[f.variable][f.environment] = var->second;
    }
    for (const auto& variable : order) {
        const auto& values = byVariable.at(variable);
        const bool shared = std::ranges::all_of(
            values, [&](const auto& kv) { return kv.second == values.begin()->second; });
        if (shared) {
            ImportedCredential cred{
                claimName(upperSnake(variable), taken), values.begin()->second, {}};
            for (const auto& env : values | std::views::keys) {
                cred.sites.push_back(
                    CredentialSite{.kind = CredentialSite::Kind::EnvironmentVariable,
                                   .environment = env,
                                   .variable = variable});
            }
            out.push_back(std::move(cred));
            continue;
        }
        for (const auto& [env, value] : values) {
            out.push_back(ImportedCredential{
                claimName(upperSnake(env + "_" + variable), taken),
                value,
                {CredentialSite{.kind = CredentialSite::Kind::EnvironmentVariable,
                                .environment = env,
                                .variable = variable}}});
        }
    }
}

void addHeaderCredentials(const Project& project,
                          std::set<std::string>& taken,
                          std::vector<ImportedCredential>& out) {
    // One secret per distinct value, so a token pasted on every request is stored once.
    std::map<std::string, std::vector<CredentialSite>> byValue;
    std::vector<std::string> order;
    for (const auto& [resourceId, resource] : project.resources) {
        for (const auto& [opKey, op] : resource.operations) {
            for (const auto& [header, value] : op.headers) {
                auto found = headerCredential(header, value);
                if (!found) {
                    continue;
                }
                if (!byValue.contains(found->credential)) {
                    order.push_back(found->credential);
                }
                byValue[found->credential].push_back(
                    CredentialSite{.kind = CredentialSite::Kind::Header,
                                   .resource = resourceId.value,
                                   .operation = opKey,
                                   .header = header,
                                   .prefix = std::move(found->prefix)});
            }
        }
    }
    std::string projectPart = upperSnake(project.name);
    if (projectPart.empty()) {
        projectPart = "IMPORTED";
    }
    for (const auto& value : order) {
        auto& sites = byValue.at(value);
        const auto& first = sites.front();
        const std::string base =
            sites.size() == 1
                ? upperSnake(first.resource + "_" + first.operation + "_" + first.header)
                : projectPart + "_" + upperSnake(first.header);
        out.push_back(ImportedCredential{claimName(base, taken), value, std::move(sites)});
    }
}

[[nodiscard]] std::string describeSite(const CredentialSite& site) {
    if (site.kind == CredentialSite::Kind::EnvironmentVariable) {
        return std::format("environment `{}` variable `{}`", site.environment, site.variable);
    }
    return std::format("operation {}.{} header `{}`", site.resource, site.operation, site.header);
}

[[nodiscard]] std::string describeSites(const std::vector<CredentialSite>& sites) {
    constexpr std::size_t kListed = 3;
    std::string out;
    for (std::size_t i = 0; i < sites.size() && i < kListed; ++i) {
        out += (i == 0 ? "" : ", ") + describeSite(sites[i]);
    }
    if (sites.size() > kListed) {
        out += std::format(" and {} more", sites.size() - kListed);
    }
    return out;
}

/// `text` with every occurrence of `secret` masked, for messages built from other components.
[[nodiscard]] std::string scrub(std::string text, std::string_view secret) {
    if (secret.empty()) {
        return text;
    }
    const std::string mask = std::string{secret.substr(0, 2)} + "…";
    for (auto pos = text.find(secret); pos != std::string::npos;
         pos = text.find(secret, pos + mask.size())) {
        text.replace(pos, secret.size(), mask);
    }
    return text;
}

void replaceWithReference(Project& project, const ImportedCredential& cred) {
    const std::string ref = "{{secret." + cred.name + "}}";
    for (const auto& site : cred.sites) {
        if (site.kind == CredentialSite::Kind::EnvironmentVariable) {
            project.environments[site.environment][site.variable] = ref;
            continue;
        }
        const auto res = project.resources.find(ResourceId{site.resource});
        if (res == project.resources.end()) {
            continue;
        }
        const auto op = res->second.operations.find(site.operation);
        if (op == res->second.operations.end()) {
            continue;
        }
        op->second.headers[site.header] = site.prefix + ref;
    }
}

/// The valid id already in an existing reqloom.yaml, so a re-import keeps the project's
/// secrets namespace.
[[nodiscard]] std::optional<std::string> existingProjectId(const fs::path& rootYaml) {
    std::error_code ec;
    if (!fs::is_regular_file(rootYaml, ec)) {
        return std::nullopt;
    }
    try {
        const auto node = YAML::LoadFile(rootYaml.string())["id"];
        if (node && node.IsScalar()) {
            auto id = node.as<std::string>();
            if (validateProjectId(id)) {
                return id;
            }
        }
    } catch (const YAML::Exception&) {
        return std::nullopt;
    }
    return std::nullopt;
}

void keepAll(CredentialMoveReport& report,
             const std::vector<ImportedCredential>& credentials,
             const std::string& reason) {
    for (const auto& cred : credentials) {
        report.kept.push_back(
            KeptCredential{cred.name, describeSites(cred.sites), scrub(reason, cred.value)});
    }
}

/// The checks the writer makes before touching the disk, so a writer failure after them is an I/O
/// failure.
[[nodiscard]] std::expected<void, ReqloomError> checkWritable(const Project& project) {
    if (!project.id.empty()) {
        if (auto ok = validateProjectId(project.id); !ok) {
            return ok;
        }
    }
    for (const auto& id : project.actors | std::views::keys) {
        if (auto ok = checkEntityFileStem("actor id", id.value); !ok) {
            return ok;
        }
    }
    for (const auto& id : project.resources | std::views::keys) {
        if (auto ok = checkEntityFileStem("resource id", id.value); !ok) {
            return ok;
        }
    }
    for (const auto& name : project.environments | std::views::keys) {
        if (auto ok = checkEntityFileStem("environment name", name); !ok) {
            return ok;
        }
    }
    return {};
}

/// The secret name in a `<project id>/<name>` store key.
[[nodiscard]] std::string nameOfKey(const std::string& key) {
    const auto slash = key.find('/');
    return slash == std::string::npos ? key : key.substr(slash + 1);
}

}  // namespace

std::vector<ImportedCredential> detectImportedCredentials(
    const Project& project, const std::vector<SourceSecretVariable>& flagged) {
    std::vector<ImportedCredential> out;
    std::set<std::string> taken;
    addFlaggedCredentials(project, flagged, taken, out);
    addHeaderCredentials(project, taken, out);
    return out;
}

std::vector<std::string> CredentialMoveReport::warnings() const {
    std::vector<std::string> out;
    out.reserve(kept.size());
    for (const auto& k : kept) {
        out.push_back(std::format(
            "{}: a credential was left in the YAML in plain text ({}). To move it into secrets "
            "later, store it as secret `{}` (desktop: Manage Secrets) and write "
            "`{{{{secret.{}}}}}` in its place",
            k.where,
            k.reason,
            k.name,
            k.name));
    }
    return out;
}

CredentialMoveReport moveImportedCredentials(Project& project,
                                             const std::vector<ImportedCredential>& credentials,
                                             SecretStore* store,
                                             ProjectTrustStore* trust,
                                             const fs::path& targetDir) {
    CredentialMoveReport report;
    if (credentials.empty()) {
        return report;
    }
    // The id namespaces the secret keys, so it must be fixed before anything is stored.
    if (project.id.empty()) {
        project.id = existingProjectId(targetDir / "reqloom.yaml").value_or(generateProjectId());
    }
    if (store == nullptr) {
        keepAll(report, credentials, "no secret store is available");
        return report;
    }
    Project scope;
    scope.id = project.id;
    std::error_code ec;
    scope.rootDir = fs::absolute(targetDir, ec).lexically_normal();
    if (auto trusted = checkProjectFolder(trust, scope); !trusted) {
        keepAll(report, credentials, trusted.error().detail);
        return report;
    }

    for (const auto& cred : credentials) {
        auto keep = [&](const std::string& reason) {
            report.kept.push_back(
                KeptCredential{cred.name, describeSites(cred.sites), scrub(reason, cred.value)});
        };
        auto key = projectSecretKey(scope, cred.name);
        if (!key) {
            keep(key.error().detail);
            continue;
        }
        auto existing = store->read(*key);
        if (!existing) {
            keep(existing.error().detail);
            continue;
        }
        if (existing->has_value()) {
            if (**existing != cred.value) {
                keep(
                    std::format("this project already has a secret `{}` with a different value, "
                                "which was kept",
                                cred.name));
                continue;
            }
        } else if (auto written = store->write(*key, cred.value); !written) {
            keep(written.error().detail);
            continue;
        } else {
            report.newlyStoredKeys.push_back(*key);
        }
        replaceWithReference(project, cred);
        report.moved.push_back(cred.name);
    }
    return report;
}

std::expected<ImportWriteResult, ReqloomError> writeImportedProject(
    const fs::path& targetDir,
    Project project,
    const std::vector<ImportedCredential>& credentials,
    SecretStore* store,
    ProjectTrustStore* trust,
    const ImportWriteOptions& options) {
    std::error_code ec;
    if (!options.overwrite && fs::exists(targetDir / "reqloom.yaml", ec)) {
        return std::unexpected(ReqloomError{ErrorCode::ProjectExists,
                                            ErrorClass::Schema,
                                            "writer: reqloom.yaml exists in " + targetDir.string() +
                                                " (pass overwrite=true to replace)"});
    }
    // The folder check resolves the folder on disk, so it must exist before the check binds it.
    fs::create_directories(targetDir, ec);
    if (ec) {
        return std::unexpected(
            ReqloomError{ErrorCode::WriteFailed,
                         ErrorClass::Schema,
                         "writer: cannot create " + targetDir.string() + ": " + ec.message()});
    }

    ImportWriteResult result;
    if (options.moveCredentials) {
        result.credentials = moveImportedCredentials(project, credentials, store, trust, targetDir);
    } else {
        keepAll(result.credentials, credentials, "moving credentials into secrets was turned off");
    }

    std::expected<fs::path, ReqloomError> written = std::unexpected(ReqloomError{});
    if (auto writable = checkWritable(project); !writable) {
        written = std::unexpected(writable.error());
    } else {
        written = writeProject(targetDir, project, options.overwrite, /*pruneStale=*/false);
        if (!written) {
            written.error().code = ErrorCode::WriteFailed;
        }
    }
    if (!written) {
        auto error = written.error();
        const auto& stored = result.credentials.newlyStoredKeys;
        if (!stored.empty() && store != nullptr) {
            std::string failed;
            for (const auto& key : stored) {
                if (!store->remove(key)) {
                    failed += (failed.empty() ? "" : ", ") + nameOfKey(key);
                }
            }
            error.detail += failed.empty()
                                ? std::format("; the {} secret(s) this import stored were removed",
                                              stored.size())
                                : std::format(
                                      "; these secrets this import stored could not be "
                                      "removed: {} (remove them in Manage Secrets)",
                                      failed);
        }
        return std::unexpected(std::move(error));
    }
    result.reqloomYaml = std::move(*written);
    return result;
}

}  // namespace reqloom::engine
