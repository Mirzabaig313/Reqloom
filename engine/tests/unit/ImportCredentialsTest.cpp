// Credential detection, naming and moving into secrets for imported projects. In-memory
// secret and trust stores only: no test touches the OS keychain or the user's config.

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/ImportCredentials.h>
#include <reqloom/engine/ProjectTrustStore.h>
#include <reqloom/engine/SecretStore.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

#include <algorithm>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

namespace {

class FakeStore final : public ce::SecretStore {
public:
    std::map<std::string, std::string> values;
    std::set<std::string> failWrites;  ///< secret names (after the `/`) whose write fails
    bool noBackend{false};
    std::vector<std::string> writtenKeys;

    std::expected<std::optional<std::string>, ce::ReqloomError> read(
        const std::string& key) override {
        if (const auto it = values.find(key); it != values.end()) {
            return std::optional<std::string>{it->second};
        }
        return std::optional<std::string>{};
    }
    std::expected<void, ce::ReqloomError> write(const std::string& key,
                                                const std::string& value) override {
        const auto name = key.substr(key.find('/') + 1);
        if (noBackend || failWrites.contains(name)) {
            return std::unexpected(ce::ReqloomError{
                ce::ErrorCode::SecretAccessFailed, ce::ErrorClass::Auth, "no keychain backend"});
        }
        writtenKeys.push_back(key);
        values[key] = value;
        return {};
    }
    std::expected<void, ce::ReqloomError> remove(const std::string& key) override {
        values.erase(key);
        return {};
    }
};

class FakeTrust final : public ce::ProjectTrustStore {
public:
    std::map<std::string, fs::path> folders;

    std::expected<std::optional<fs::path>, ce::ReqloomError> folderFor(
        const std::string& id) override {
        if (const auto it = folders.find(id); it != folders.end()) {
            return std::optional<fs::path>{it->second};
        }
        return std::optional<fs::path>{};
    }
    std::expected<fs::path, ce::ReqloomError> recordIfAbsent(const std::string& id,
                                                             const fs::path& folder) override {
        return folders.try_emplace(id, folder).first->second;
    }
    std::expected<void, ce::ReqloomError> rebind(const std::string& id,
                                                 const fs::path& folder) override {
        folders[id] = folder;
        return {};
    }
};

class ScratchDir {
public:
    ScratchDir() : path_(reqloom::tests::uniqueTempPath("reqloom-import-creds")) {
        fs::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

ce::Operation& addOp(ce::Project& p, const std::string& res, const std::string& op) {
    auto& resource = p.resources[ce::ResourceId{res}];
    resource.id = ce::ResourceId{res};
    auto& o = resource.operations[op];
    o.id = ce::OperationId{res + "." + op};
    o.resource = ce::ResourceId{res};
    o.pathTemplate = "/" + op;
    return o;
}

/// Two operations share one bearer token, a third has its own key, and `dev` has a flagged
/// `api key` plus an ordinary variable.
ce::Project sampleProject() {
    ce::Project p;
    p.name = "Shop API";
    p.defaultEnvironment = "dev";
    p.environments["dev"] = {
        {"baseUrl", "https://api.example.com"}, {"api key", "k-flagged-1"}, {"region", "eu"}};
    addOp(p, "orders", "create").headers["Authorization"] = "Bearer abc123def456";
    addOp(p, "orders", "list").headers["Authorization"] = "Bearer abc123def456";
    addOp(p, "users", "get").headers["X-API-Key"] = "xk-777";
    return p;
}

const std::vector<ce::SourceSecretVariable> kFlagged{{"dev", "api key"}};

std::string readAll(const fs::path& dir) {
    std::string all;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            std::ifstream in{entry.path()};
            std::stringstream ss;
            ss << in.rdbuf();
            all += ss.str();
        }
    }
    return all;
}

const ce::ImportedCredential* byName(const std::vector<ce::ImportedCredential>& creds,
                                     const std::string& name) {
    for (const auto& c : creds) {
        if (c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

}  // namespace

TEST(ImportCredentials, shared_bearer_token_becomes_one_credential) {
    const auto creds = ce::detectImportedCredentials(sampleProject(), {});
    const auto* shared = byName(creds, "SHOP_API_AUTHORIZATION");
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(shared->value, "abc123def456");
    ASSERT_EQ(shared->sites.size(), 2u);
    EXPECT_EQ(shared->sites[0].prefix, "Bearer ");
    const auto* single = byName(creds, "USERS_GET_X_API_KEY");
    ASSERT_NE(single, nullptr);
    EXPECT_EQ(single->value, "xk-777");
    EXPECT_EQ(creds.size(), 2u);
}

TEST(ImportCredentials, header_rule_splits_scheme_and_skips_references) {
    ce::Project p;
    p.name = "P";
    addOp(p, "a", "basic").headers["authorization"] = "Basic dXNlcjpwYXNz";
    addOp(p, "a", "bare").headers["Authorization"] = "rawtoken";
    addOp(p, "a", "proxy").headers["Proxy-Authorization"] = "Digest u=1, r=2";
    addOp(p, "a", "ref").headers["Authorization"] = "Bearer {{env.token}}";
    addOp(p, "a", "keyref").headers["X-API-Key"] = "{{secret.KEY}}";
    addOp(p, "a", "other").headers["X-Token"] = "not-a-credential-header";
    addOp(p, "a", "empty").headers["Authorization"] = "Bearer ";

    const auto creds = ce::detectImportedCredentials(p, {});
    ASSERT_EQ(creds.size(), 3u);
    const auto* basic = byName(creds, "A_BASIC_AUTHORIZATION");
    ASSERT_NE(basic, nullptr);
    EXPECT_EQ(basic->value, "dXNlcjpwYXNz");
    EXPECT_EQ(basic->sites[0].prefix, "Basic ");
    const auto* bare = byName(creds, "A_BARE_AUTHORIZATION");
    ASSERT_NE(bare, nullptr);
    EXPECT_EQ(bare->value, "rawtoken");
    EXPECT_EQ(bare->sites[0].prefix, "");
    const auto* proxy = byName(creds, "A_PROXY_PROXY_AUTHORIZATION");
    ASSERT_NE(proxy, nullptr);
    EXPECT_EQ(proxy->value, "u=1, r=2");
}

TEST(ImportCredentials, flagged_variable_moves_and_ordinary_one_does_not) {
    const auto creds = ce::detectImportedCredentials(sampleProject(), kFlagged);
    const auto* key = byName(creds, "API_KEY");
    ASSERT_NE(key, nullptr);
    EXPECT_EQ(key->value, "k-flagged-1");
    ASSERT_EQ(key->sites.size(), 1u);
    EXPECT_EQ(key->sites[0].environment, "dev");
    for (const auto& c : creds) {
        EXPECT_NE(c.value, "eu");
        EXPECT_NE(c.value, "https://api.example.com");
    }
}

TEST(ImportCredentials, flagged_variable_with_different_values_per_environment_gets_one_name_each) {
    ce::Project p;
    p.name = "P";
    addOp(p, "a", "b");
    p.environments["dev"] = {{"token", "dev-secret"}};
    p.environments["prod"] = {{"token", "prod-secret"}};
    p.environments["stage"] = {{"other", "s"}};
    const auto creds =
        ce::detectImportedCredentials(p, {{"dev", "token"}, {"prod", "token"}, {"stage", "x"}});
    ASSERT_EQ(creds.size(), 2u);
    ASSERT_NE(byName(creds, "DEV_TOKEN"), nullptr);
    EXPECT_EQ(byName(creds, "DEV_TOKEN")->value, "dev-secret");
    ASSERT_NE(byName(creds, "PROD_TOKEN"), nullptr);
    EXPECT_EQ(byName(creds, "PROD_TOKEN")->value, "prod-secret");

    p.environments["prod"]["token"] = "dev-secret";
    const auto same = ce::detectImportedCredentials(p, {{"dev", "token"}, {"prod", "token"}});
    ASSERT_EQ(same.size(), 1u);
    EXPECT_EQ(same[0].name, "TOKEN");
    EXPECT_EQ(same[0].sites.size(), 2u);
}

TEST(ImportCredentials, different_values_never_share_a_name) {
    ce::Project p;
    p.name = "P";
    addOp(p, "a", "one").headers["Authorization"] = "Bearer t1";
    addOp(p, "a", "two").headers["Authorization"] = "Bearer t1";
    addOp(p, "b", "one").headers["Authorization"] = "Bearer t2";
    addOp(p, "b", "two").headers["Authorization"] = "Bearer t2";
    p.environments["default"] = {{"P_AUTHORIZATION", "v"}};
    const auto creds = ce::detectImportedCredentials(p, {{"default", "P_AUTHORIZATION"}});
    ASSERT_EQ(creds.size(), 3u);
    std::set<std::string> names;
    for (const auto& c : creds) {
        names.insert(c.name);
    }
    EXPECT_EQ(names,
              (std::set<std::string>{"P_AUTHORIZATION", "P_AUTHORIZATION_2", "P_AUTHORIZATION_3"}));
}

TEST(ImportCredentials, all_stored_leaves_only_references_in_the_yaml) {
    ScratchDir dir;
    FakeStore store;
    FakeTrust trust;
    const auto project = sampleProject();
    const auto creds = ce::detectImportedCredentials(project, kFlagged);

    auto written =
        ce::writeImportedProject(dir.path(), project, creds, &store, &trust, {.overwrite = false});
    ASSERT_TRUE(written.has_value()) << written.error().detail;
    EXPECT_TRUE(written->credentials.kept.empty());
    EXPECT_EQ(written->credentials.moved.size(), 3u);

    const auto yaml = readAll(dir.path());
    EXPECT_EQ(yaml.find("abc123def456"), std::string::npos) << yaml;
    EXPECT_EQ(yaml.find("xk-777"), std::string::npos) << yaml;
    EXPECT_EQ(yaml.find("k-flagged-1"), std::string::npos) << yaml;
    EXPECT_NE(yaml.find("Bearer {{secret.SHOP_API_AUTHORIZATION}}"), std::string::npos) << yaml;

    auto loaded = ce::parseProject(dir.path() / "reqloom.yaml");
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    ASSERT_FALSE(loaded->id.empty());
    EXPECT_EQ(store.values.at(loaded->id + "/SHOP_API_AUTHORIZATION"), "abc123def456");
    EXPECT_EQ(store.values.at(loaded->id + "/USERS_GET_X_API_KEY"), "xk-777");
    EXPECT_EQ(store.values.at(loaded->id + "/API_KEY"), "k-flagged-1");
}

TEST(ImportCredentials, written_project_loads_and_validates_with_secret_references) {
    ScratchDir dir;
    FakeStore store;
    FakeTrust trust;
    const auto project = sampleProject();
    auto written = ce::writeImportedProject(
        dir.path(), project, ce::detectImportedCredentials(project, kFlagged), &store, &trust, {});
    ASSERT_TRUE(written.has_value()) << written.error().detail;
    auto loaded = ce::parseProject(dir.path() / "reqloom.yaml");
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    EXPECT_TRUE(loaded->loadWarnings.empty())
        << (loaded->loadWarnings.empty() ? "" : loaded->loadWarnings.front());
    EXPECT_TRUE(ce::validateProject(*loaded).has_value());
    EXPECT_EQ(
        ce::collectSecretReferences(*loaded),
        (std::vector<std::string>{"API_KEY", "SHOP_API_AUTHORIZATION", "USERS_GET_X_API_KEY"}));
    EXPECT_EQ(loaded->environments.at("dev").at("api key"), "{{secret.API_KEY}}");
    EXPECT_EQ(loaded->environments.at("dev").at("region"), "eu");
}

TEST(ImportCredentials, a_failing_store_write_keeps_that_value_in_plain_text_and_warns) {
    ScratchDir dir;
    FakeStore store;
    store.failWrites = {"USERS_GET_X_API_KEY"};
    FakeTrust trust;
    const auto project = sampleProject();
    auto written = ce::writeImportedProject(
        dir.path(), project, ce::detectImportedCredentials(project, kFlagged), &store, &trust, {});
    ASSERT_TRUE(written.has_value()) << written.error().detail;
    ASSERT_EQ(written->credentials.kept.size(), 1u);
    const auto warnings = written->credentials.warnings();
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("operation users.get header `X-API-Key`"), std::string::npos)
        << warnings[0];
    EXPECT_NE(warnings[0].find("{{secret.USERS_GET_X_API_KEY}}"), std::string::npos);
    EXPECT_EQ(warnings[0].find("xk-777"), std::string::npos);

    const auto yaml = readAll(dir.path());
    EXPECT_NE(yaml.find("xk-777"), std::string::npos);
    EXPECT_EQ(yaml.find("abc123def456"), std::string::npos);
    EXPECT_EQ(store.values.size(), 2u);
}

TEST(ImportCredentials, no_backend_keeps_everything_and_loses_nothing) {
    for (const bool nullStore : {false, true}) {
        ScratchDir dir;
        FakeStore store;
        store.noBackend = true;
        FakeTrust trust;
        const auto project = sampleProject();
        const auto creds = ce::detectImportedCredentials(project, kFlagged);
        auto written = ce::writeImportedProject(
            dir.path(), project, creds, nullStore ? nullptr : &store, &trust, {});
        ASSERT_TRUE(written.has_value()) << written.error().detail;
        EXPECT_TRUE(written->credentials.moved.empty());
        EXPECT_EQ(written->credentials.kept.size(), creds.size());
        EXPECT_EQ(written->credentials.warnings().size(), creds.size());

        const auto yaml = readAll(dir.path());
        for (const auto& c : creds) {
            bool inStore = false;
            for (const auto& value : store.values | std::views::values) {
                inStore = inStore || value == c.value;
            }
            EXPECT_TRUE(inStore || yaml.find(c.value) != std::string::npos) << c.name;
        }
        for (const auto& w : written->credentials.warnings()) {
            for (const auto& c : creds) {
                EXPECT_EQ(w.find(c.value), std::string::npos) << w;
            }
        }
    }
}

TEST(ImportCredentials, a_refused_folder_check_keeps_everything) {
    ScratchDir dir;
    ScratchDir elsewhere;
    FakeStore store;
    FakeTrust trust;
    auto project = sampleProject();
    project.id = "taken-id";
    trust.folders["taken-id"] = elsewhere.path();
    auto written = ce::writeImportedProject(
        dir.path(), project, ce::detectImportedCredentials(project, kFlagged), &store, &trust, {});
    ASSERT_TRUE(written.has_value()) << written.error().detail;
    EXPECT_TRUE(written->credentials.moved.empty());
    EXPECT_TRUE(store.values.empty());
    EXPECT_NE(readAll(dir.path()).find("abc123def456"), std::string::npos);
}

TEST(ImportCredentials, an_existing_secret_with_another_value_is_not_overwritten) {
    ScratchDir dir;
    FakeStore store;
    FakeTrust trust;
    auto project = sampleProject();
    project.id = "proj-1";
    store.values["proj-1/SHOP_API_AUTHORIZATION"] = "older-token";
    store.values["proj-1/USERS_GET_X_API_KEY"] = "xk-777";
    auto written = ce::writeImportedProject(
        dir.path(), project, ce::detectImportedCredentials(project, kFlagged), &store, &trust, {});
    ASSERT_TRUE(written.has_value()) << written.error().detail;
    EXPECT_EQ(store.values.at("proj-1/SHOP_API_AUTHORIZATION"), "older-token");
    ASSERT_EQ(written->credentials.kept.size(), 1u);
    EXPECT_EQ(written->credentials.kept[0].name, "SHOP_API_AUTHORIZATION");
    EXPECT_EQ(written->credentials.kept[0].reason.find("older-token"), std::string::npos);

    const auto yaml = readAll(dir.path());
    EXPECT_NE(yaml.find("Bearer abc123def456"), std::string::npos);
    EXPECT_NE(yaml.find("{{secret.USERS_GET_X_API_KEY}}"), std::string::npos);
    EXPECT_EQ(std::ranges::count(store.writtenKeys, "proj-1/USERS_GET_X_API_KEY"), 0);
}

TEST(ImportCredentials, reimport_keeps_the_existing_project_id) {
    ScratchDir dir;
    FakeStore store;
    FakeTrust trust;
    const auto project = sampleProject();
    const auto creds = ce::detectImportedCredentials(project, kFlagged);
    ASSERT_TRUE(ce::writeImportedProject(dir.path(), project, creds, &store, &trust, {}));
    const auto firstId = ce::parseProject(dir.path() / "reqloom.yaml")->id;
    store.writtenKeys.clear();

    auto again =
        ce::writeImportedProject(dir.path(), project, creds, &store, &trust, {.overwrite = true});
    ASSERT_TRUE(again.has_value()) << again.error().detail;
    EXPECT_EQ(ce::parseProject(dir.path() / "reqloom.yaml")->id, firstId);
    EXPECT_EQ(again->credentials.moved.size(), 3u);
    EXPECT_TRUE(store.writtenKeys.empty());
}

TEST(ImportCredentials, the_project_gets_an_id_before_anything_is_stored) {
    ScratchDir dir;
    FakeStore store;
    FakeTrust trust;
    auto project = sampleProject();
    ASSERT_TRUE(project.id.empty());
    const auto report = ce::moveImportedCredentials(
        project, ce::detectImportedCredentials(project, kFlagged), &store, &trust, dir.path());
    ASSERT_FALSE(project.id.empty());
    ASSERT_EQ(store.writtenKeys.size(), 3u);
    for (const auto& key : store.writtenKeys) {
        EXPECT_TRUE(key.starts_with(project.id + "/")) << key;
    }
    EXPECT_EQ(report.newlyStoredKeys, store.writtenKeys);
}

TEST(ImportCredentials, a_failed_write_removes_the_secrets_this_import_stored) {
    ScratchDir dir;
    FakeStore store;
    FakeTrust trust;
    auto project = sampleProject();
    project.id = "proj-2";
    store.values["proj-2/API_KEY"] = "k-flagged-1";
    // A resource id that is not a safe file stem makes the writer refuse the project.
    addOp(project, "bad/id", "op");
    auto written = ce::writeImportedProject(
        dir.path(), project, ce::detectImportedCredentials(project, kFlagged), &store, &trust, {});
    ASSERT_FALSE(written.has_value());
    EXPECT_NE(written.error().detail.find("were removed"), std::string::npos)
        << written.error().detail;
    EXPECT_EQ(store.values,
              (std::map<std::string, std::string>{{"proj-2/API_KEY", "k-flagged-1"}}));
}

TEST(ImportCredentials, refuses_an_existing_project_without_overwrite_before_storing) {
    ScratchDir dir;
    FakeStore store;
    FakeTrust trust;
    {
        std::ofstream out{dir.path() / "reqloom.yaml"};
        out << "version: 1\nname: x\n";
    }
    const auto project = sampleProject();
    auto written = ce::writeImportedProject(
        dir.path(), project, ce::detectImportedCredentials(project, kFlagged), &store, &trust, {});
    ASSERT_FALSE(written.has_value());
    EXPECT_TRUE(store.writtenKeys.empty());
}

TEST(ImportCredentials, turning_secrets_off_keeps_everything_with_warnings) {
    ScratchDir dir;
    FakeStore store;
    FakeTrust trust;
    const auto project = sampleProject();
    const auto creds = ce::detectImportedCredentials(project, kFlagged);
    auto written = ce::writeImportedProject(
        dir.path(), project, creds, &store, &trust, {.moveCredentials = false});
    ASSERT_TRUE(written.has_value()) << written.error().detail;
    EXPECT_TRUE(store.writtenKeys.empty());
    EXPECT_EQ(written->credentials.kept.size(), creds.size());
    EXPECT_NE(readAll(dir.path()).find("k-flagged-1"), std::string::npos);
}

namespace {

class ImportFixture {
public:
    fs::path write(const std::string& rel, const std::string& body) {
        const auto full = dir_.path() / rel;
        fs::create_directories(full.parent_path());
        std::ofstream out{full};
        out << body;
        return full;
    }
    [[nodiscard]] const fs::path& root() const { return dir_.path(); }

private:
    ScratchDir dir_;
};

const ce::Operation* opWithHeader(const ce::Project& p, const std::string& header) {
    for (const auto& res : p.resources | std::views::values) {
        for (const auto& op : res.operations | std::views::values) {
            if (op.headers.contains(header)) {
                return &op;
            }
        }
    }
    return nullptr;
}

/// Import `input`: the bearer token pasted on two requests is one credential with two
/// sites; after moving, both operations reference that secret. `ordinary` values (plain
/// environment variables) are never credentials.
void expectSharedBearerMoves(const fs::path& input,
                             const fs::path& root,
                             const std::vector<std::string>& ordinary) {
    auto outcome = ce::importAny(input, root);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const ce::ImportedCredential* token = nullptr;
    for (const auto& c : outcome->credentials) {
        if (c.value == "abc123def456") {
            ASSERT_EQ(token, nullptr) << "the token must be one credential";
            token = &c;
        }
        for (const auto& value : ordinary) {
            EXPECT_NE(c.value, value) << c.name;
        }
    }
    ASSERT_NE(token, nullptr);
    EXPECT_EQ(token->name, "SHOP_AUTHORIZATION");
    ASSERT_EQ(token->sites.size(), 2u);

    ScratchDir target;
    FakeStore store;
    FakeTrust trust;
    auto project = outcome->project;
    const auto report =
        ce::moveImportedCredentials(project, outcome->credentials, &store, &trust, target.path());
    EXPECT_TRUE(report.kept.empty());
    for (const auto& site : token->sites) {
        const auto& op =
            project.resources.at(ce::ResourceId{site.resource}).operations.at(site.operation);
        EXPECT_EQ(op.headers.at(site.header), "Bearer {{secret.SHOP_AUTHORIZATION}}");
    }
    EXPECT_EQ(store.values.at(project.id + "/SHOP_AUTHORIZATION"), "abc123def456");
}

}  // namespace

TEST(ImportCredentialsPerImporter, postman) {
    ImportFixture f;
    const auto file = f.write("shop.json", R"JSON(
{
  "info": { "name": "Shop", "schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json" },
  "variable": [
    { "key": "token", "value": "pm-flagged-1", "type": "secret" },
    { "key": "region", "value": "eu", "type": "string" }
  ],
  "item": [
    { "name": "One", "request": { "method": "GET", "url": "https://api.example.com/one",
      "header": [ { "key": "Authorization", "value": "Bearer abc123def456" } ] } },
    { "name": "Two", "request": { "method": "GET", "url": "https://api.example.com/two",
      "header": [ { "key": "Authorization", "value": "Bearer abc123def456" } ] } }
  ]
}
)JSON");
    expectSharedBearerMoves(file, f.root(), {"eu"});
    auto outcome = ce::importAny(file, f.root());
    ASSERT_TRUE(outcome.has_value());
    const auto* flagged = byName(outcome->credentials, "TOKEN");
    ASSERT_NE(flagged, nullptr);
    EXPECT_EQ(flagged->value, "pm-flagged-1");
    EXPECT_EQ(flagged->sites.at(0).environment, "default");
}

TEST(ImportCredentialsPerImporter, httpie) {
    ImportFixture f;
    const auto file = f.write("shop.json", R"JSON(
{
  "meta": { "format": "httpie", "version": "2024.1.0", "contentType": "workspace" },
  "entry": {
    "name": "Shop",
    "collections": [ { "name": "Shop", "requests": [
      { "name": "One", "method": "GET", "url": "https://api.example.com/one",
        "headers": [ { "name": "Authorization", "value": "Bearer abc123def456", "enabled": true } ] },
      { "name": "Two", "method": "GET", "url": "https://api.example.com/two",
        "headers": [ { "name": "Authorization", "value": "Bearer abc123def456", "enabled": true } ] }
    ] } ],
    "environments": [
      { "isDefault": true, "name": "Local", "variables": [
        { "name": "apiKey", "value": "hp-local-key", "isSecret": true },
        { "name": "region", "value": "eu", "isSecret": false } ] },
      { "name": "Prod", "variables": [
        { "name": "apiKey", "value": "hp-prod-key", "isSecret": true } ] }
    ]
  }
}
)JSON");
    expectSharedBearerMoves(file, f.root(), {"eu"});
    auto outcome = ce::importAny(file, f.root());
    ASSERT_TRUE(outcome.has_value());
    ASSERT_NE(byName(outcome->credentials, "LOCAL_APIKEY"), nullptr);
    EXPECT_EQ(byName(outcome->credentials, "LOCAL_APIKEY")->value, "hp-local-key");
    ASSERT_NE(byName(outcome->credentials, "PROD_APIKEY"), nullptr);
    EXPECT_EQ(byName(outcome->credentials, "PROD_APIKEY")->value, "hp-prod-key");
}

TEST(ImportCredentialsPerImporter, insomnia) {
    ImportFixture f;
    const auto file = f.write("shop.json", R"JSON(
{
  "_type": "export", "__export_format": 4,
  "resources": [
    { "_id": "wrk_1", "_type": "workspace", "name": "Shop" },
    { "_id": "env_1", "_type": "environment", "parentId": "wrk_1", "name": "Base",
      "data": { "region": "eu" } },
    { "_id": "req_1", "_type": "request", "parentId": "wrk_1", "name": "One", "method": "GET",
      "url": "https://api.example.com/one",
      "headers": [ { "name": "Authorization", "value": "Bearer abc123def456" } ] },
    { "_id": "req_2", "_type": "request", "parentId": "wrk_1", "name": "Two", "method": "GET",
      "url": "https://api.example.com/two",
      "headers": [ { "name": "Authorization", "value": "Bearer abc123def456" } ] }
  ]
}
)JSON");
    expectSharedBearerMoves(file, f.root(), {"eu"});
}

TEST(ImportCredentialsPerImporter, bruno) {
    ImportFixture f;
    f.write("bruno.json", R"({"version":"1","name":"Shop","type":"collection"})");
    f.write("environments/Local.bru", "vars {\n  region: eu\n}\n");
    for (const char* name : {"One", "Two"}) {
        f.write(std::string{"Shop/"} + name + ".bru",
                std::string{"meta {\n  name: "} + name +
                    "\n}\n\nget {\n  url: https://api.example.com/x\n}\n\n"
                    "headers {\n  Authorization: Bearer abc123def456\n}\n");
    }
    expectSharedBearerMoves(f.root(), f.root(), {"eu"});
}

TEST(ImportCredentialsPerImporter, hoppscotch) {
    ImportFixture f;
    const auto file = f.write("shop.json", R"JSON(
{
  "v": 1, "name": "Shop", "folders": [],
  "requests": [
    { "v": "1", "name": "One", "method": "GET", "endpoint": "https://api.example.com/one",
      "headers": [ { "key": "Authorization", "value": "Bearer abc123def456", "active": true } ],
      "params": [] },
    { "v": "1", "name": "Two", "method": "GET", "endpoint": "https://api.example.com/two",
      "headers": [ { "key": "Authorization", "value": "Bearer abc123def456", "active": true } ],
      "params": [] }
  ]
}
)JSON");
    expectSharedBearerMoves(file, f.root(), {});
}

TEST(ImportCredentialsPerImporter, apidog) {
    ImportFixture f;
    const auto file = f.write("shop.json", R"JSON(
{
  "apidogProject": "1.0.0",
  "info": { "name": "Shop" },
  "apiCollection": [ { "name": "Root", "id": 1, "parentId": 0, "items": [
    { "name": "One", "api": { "id": "10", "method": "get", "path": "/one",
      "parameters": { "header": [
        { "name": "Authorization", "example": "Bearer abc123def456", "enable": true } ] } } },
    { "name": "Two", "api": { "id": "11", "method": "get", "path": "/two",
      "parameters": { "header": [
        { "name": "Authorization", "example": "Bearer abc123def456", "enable": true } ] } } }
  ] } ]
}
)JSON");
    expectSharedBearerMoves(file, f.root(), {});
}

TEST(ImportCredentialsPerImporter, thunder_client) {
    ImportFixture f;
    const auto file = f.write("shop.json", R"JSON(
[ { "_id": "col_1", "colName": "Shop", "folders": [], "requests": [
  { "_id": "r1", "colId": "col_1", "containerId": "", "name": "One", "method": "GET",
    "url": "https://api.example.com/one",
    "headers": [ { "name": "Authorization", "value": "Bearer abc123def456" } ] },
  { "_id": "r2", "colId": "col_1", "containerId": "", "name": "Two", "method": "GET",
    "url": "https://api.example.com/two",
    "headers": [ { "name": "Authorization", "value": "Bearer abc123def456" } ] }
] } ]
)JSON");
    expectSharedBearerMoves(file, f.root(), {});
}

TEST(ImportCredentialsPerImporter, http_file) {
    ImportFixture f;
    const auto file = f.write("shop.http",
                              "@region = eu\n\n"
                              "GET https://api.example.com/one\n"
                              "Authorization: Bearer abc123def456\n\n"
                              "###\n\n"
                              "GET https://api.example.com/two\n"
                              "Authorization: Bearer abc123def456\n");
    auto outcome = ce::importAny(file, f.root());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto shared = "_AUTHORIZATION";
    ASSERT_EQ(outcome->credentials.size(), 1u);
    EXPECT_TRUE(outcome->credentials[0].name.ends_with(shared)) << outcome->credentials[0].name;
    EXPECT_EQ(outcome->credentials[0].sites.size(), 2u);
    EXPECT_NE(opWithHeader(outcome->project, "Authorization"), nullptr);
}
