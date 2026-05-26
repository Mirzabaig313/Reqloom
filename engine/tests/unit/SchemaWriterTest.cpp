// Unit tests for YamlSchemaWriter. PRD §10.3.6 importer-side persistence.
//
// The strongest test for a writer paired with an existing parser is a
// round-trip: build a Project in memory → write → re-parse → compare.
// Whatever the writer fails to emit, or the parser fails to read, will
// surface as a structural mismatch.
//
// Each test fails on the parent commit (no SchemaWriter existed):
// `chainapi::engine::writeProject` is a brand-new public symbol.
#include <chainapi/engine/Factories.h>
#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace ce = chainapi::engine;
namespace fs = std::filesystem;

namespace {

class ScratchDir {
public:
    ScratchDir() {
        const auto unique = "chainapi-writer-" +
                            std::to_string(::getpid()) + "-" +
                            std::to_string(counter_++);
        path_ = fs::temp_directory_path() / unique;
        fs::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
    inline static int counter_{0};
};

ce::Actor makeUserActor() {
    ce::Actor actor;
    actor.id = ce::ActorId{"user"};
    actor.description = "Plain user actor";
    actor.strategy = ce::AuthStrategy::Simple;
    ce::AuthStep step;
    step.id = "login";
    step.method = ce::HttpMethod::Post;
    step.pathTemplate = "/api/v1/auth/login";
    step.bodyTemplate = R"({email: "u@x.test"})";
    step.expectStatus = 200;
    step.extractions.push_back(
        {"token", "$.data.accessToken", ce::Extraction::Source::JsonPath});
    actor.authSteps.push_back(std::move(step));
    actor.sessionTtl = std::chrono::seconds{900};
    actor.inject.headers["Authorization"] = "Bearer {{user.token}}";
    return actor;
}

ce::Resource makePaymentResource() {
    ce::Resource res;
    res.id = ce::ResourceId{"payment"};
    res.description = "Payment ops";

    ce::Operation pay;
    pay.id = ce::OperationId{"payment.pay"};
    pay.resource = res.id;
    pay.actor = ce::ActorId{"user"};
    pay.method = ce::HttpMethod::Post;
    pay.pathTemplate = "/api/v1/payments";
    pay.bodyTemplate = R"({method: "card"})";
    pay.expectStatusList = {200, 202};

    pay.extractions.push_back(
        {"payment_id", "$.id", ce::Extraction::Source::JsonPath});

    ce::PollUntil poll;
    poll.method = ce::HttpMethod::Get;
    poll.pathTemplate = "/api/v1/payments/{{payment.payment_id}}/status";
    poll.successWhen = "$.status == 'COMPLETED'";
    poll.failWhen = "$.status in ['FAILED', 'CANCELLED']";
    poll.interval = std::chrono::milliseconds{500};
    poll.timeout = std::chrono::milliseconds{30'000};
    poll.maxAttempts = 60;
    pay.pollUntil = std::move(poll);

    // Provenance — the whole point of the writer existing.
    ce::Provenance prov;
    prov.source = ce::Provenance::Source::AiImport;
    prov.verifiedAgainst = ce::Provenance::VerifiedAgainst::OpenApiExample;
    prov.model = "gpt-4o";
    prov.importedAt = "2026-05-24T12:00:00Z";
    prov.evidence = {
        {"actor", "inferred from BearerAuth security scheme"},
        {"extract.payment_id",
         "verified against POST /payments examples.default"},
    };
    pay.provenance = std::move(prov);

    res.operations["pay"] = std::move(pay);
    return res;
}

ce::Project makeRoundTripProject() {
    ce::Project p;
    p.name = "WriterRoundTrip";
    p.defaultEnvironment = "local";
    p.actors[ce::ActorId{"user"}] = makeUserActor();
    p.resources[ce::ResourceId{"payment"}] = makePaymentResource();
    p.environments["local"] = {{"baseUrl", "http://localhost:0"}};
    return p;
}

}  // namespace

TEST(SchemaWriter, refuses_to_overwrite_existing_root) {
    ScratchDir scratch;
    auto project = makeRoundTripProject();

    auto first = ce::writeProject(scratch.path(), project, /*overwrite=*/false);
    ASSERT_TRUE(first.has_value()) << first.error().detail;

    auto second = ce::writeProject(scratch.path(), project, /*overwrite=*/false);
    ASSERT_FALSE(second.has_value());
    EXPECT_NE(second.error().detail.find("exists"), std::string::npos);

    // Same call with overwrite=true succeeds.
    auto third = ce::writeProject(scratch.path(), project, /*overwrite=*/true);
    EXPECT_TRUE(third.has_value()) << (third ? "" : third.error().detail);
}

TEST(SchemaWriter, round_trips_actor_resource_environment) {
    ScratchDir scratch;
    auto original = makeRoundTripProject();

    auto written = ce::writeProject(scratch.path(), original);
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    auto reloaded = ce::parseProject(*written);
    ASSERT_TRUE(reloaded.has_value()) << reloaded.error().detail;

    // Project-level fields.
    EXPECT_EQ(reloaded->name, original.name);
    EXPECT_EQ(reloaded->defaultEnvironment, original.defaultEnvironment);

    // Actor structural equality.
    ASSERT_TRUE(reloaded->actors.contains(ce::ActorId{"user"}));
    const auto& actorOut = reloaded->actors.at(ce::ActorId{"user"});
    const auto& actorIn  = original.actors.at(ce::ActorId{"user"});
    EXPECT_EQ(actorOut.id, actorIn.id);
    EXPECT_EQ(actorOut.strategy, actorIn.strategy);
    EXPECT_EQ(actorOut.sessionTtl, actorIn.sessionTtl);
    ASSERT_EQ(actorOut.authSteps.size(), 1u);
    EXPECT_EQ(actorOut.authSteps[0].pathTemplate,
              actorIn.authSteps[0].pathTemplate);
    EXPECT_EQ(actorOut.inject.headers, actorIn.inject.headers);

    // Environment.
    ASSERT_TRUE(reloaded->environments.contains("local"));
    EXPECT_EQ(reloaded->environments.at("local").at("baseUrl"),
              "http://localhost:0");

    // Resource + operation.
    ASSERT_TRUE(reloaded->resources.contains(ce::ResourceId{"payment"}));
    const auto& payOut = reloaded->resources.at(ce::ResourceId{"payment"})
                              .operations.at("pay");
    const auto& payIn  = original.resources.at(ce::ResourceId{"payment"})
                              .operations.at("pay");

    EXPECT_EQ(payOut.method, payIn.method);
    EXPECT_EQ(payOut.pathTemplate, payIn.pathTemplate);
    EXPECT_EQ(payOut.actor, payIn.actor);
    EXPECT_EQ(payOut.expectStatusList, payIn.expectStatusList);

    // Polling round-trip.
    ASSERT_TRUE(payOut.pollUntil.has_value());
    EXPECT_EQ(payOut.pollUntil->successWhen, payIn.pollUntil->successWhen);
    ASSERT_TRUE(payOut.pollUntil->failWhen.has_value());
    EXPECT_EQ(*payOut.pollUntil->failWhen, *payIn.pollUntil->failWhen);
    EXPECT_EQ(payOut.pollUntil->interval,    payIn.pollUntil->interval);
    EXPECT_EQ(payOut.pollUntil->maxAttempts, payIn.pollUntil->maxAttempts);

    // Extractions round-trip (the parser's contract: paths starting with
    // $.headers. become Source::Header automatically; everything else
    // stays JsonPath).
    ASSERT_EQ(payOut.extractions.size(), 1u);
    EXPECT_EQ(payOut.extractions[0].variableName, "payment_id");
    EXPECT_EQ(payOut.extractions[0].sourcePath, "$.id");
}

TEST(SchemaWriter, provenance_round_trips_unaffected_by_runtime) {
    // Provenance is what 6c will write for AI-imported ops. Round-tripping
    // it cleanly is the prerequisite for §10.3.4 runtime diagnostics.
    //
    // The parser does not currently understand the `_provenance` block —
    // that's a follow-up. For Slice 6b, we verify the writer's output
    // contains the expected fields by reading the file directly. The
    // parser will pick it up when 6c lands the parser side.
    ScratchDir scratch;
    auto original = makeRoundTripProject();

    auto written = ce::writeProject(scratch.path(), original);
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    // Read the resource file and assert provenance fields are present.
    const auto resYaml = scratch.path() / "resources" / "payment.yaml";
    std::string content;
    {
        std::ifstream in(resYaml, std::ios::binary);
        ASSERT_TRUE(in.good()) << "cannot open " << resYaml;
        content.assign(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
    }
    EXPECT_NE(content.find("_provenance"), std::string::npos);
    EXPECT_NE(content.find("ai_import"), std::string::npos);
    EXPECT_NE(content.find("openapi_example"), std::string::npos);
    EXPECT_NE(content.find("gpt-4o"), std::string::npos);
    EXPECT_NE(content.find("BearerAuth security scheme"), std::string::npos);
}

TEST(SchemaWriter, written_yaml_emits_all_three_sub_directories) {
    ScratchDir scratch;
    auto project = makeRoundTripProject();

    auto written = ce::writeProject(scratch.path(), project);
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    EXPECT_TRUE(fs::exists(scratch.path() / "chainapi.yaml"));
    EXPECT_TRUE(fs::is_directory(scratch.path() / "actors"));
    EXPECT_TRUE(fs::is_directory(scratch.path() / "resources"));
    EXPECT_TRUE(fs::is_directory(scratch.path() / "environments"));

    EXPECT_TRUE(fs::exists(scratch.path() / "actors" / "user.yaml"));
    EXPECT_TRUE(fs::exists(scratch.path() / "resources" / "payment.yaml"));
    EXPECT_TRUE(fs::exists(scratch.path() / "environments" / "local.yaml"));
}


// ─── Slice 4b — Basic auth strategy round-trip ──────────────────────────────

TEST(SchemaWriter, basic_auth_round_trips) {
    ScratchDir scratch;

    ce::Project original;
    original.name = "BasicRoundTrip";
    original.defaultEnvironment = "local";
    original.environments["local"] = {{"baseUrl", "http://t.test"}};

    ce::Actor client;
    client.id = ce::ActorId{"client"};
    client.description = "Basic-auth client";
    client.strategy = ce::AuthStrategy::Basic;
    client.authConfig = {{"username", "{{secret.API_USER}}"},
                         {"password", "{{secret.API_PASS}}"}};
    client.inject.headers["Authorization"] = "Basic {{client.credential}}";
    original.actors[client.id] = std::move(client);

    auto written = ce::writeProject(scratch.path(), original);
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    auto reloaded = ce::parseProject(*written);
    ASSERT_TRUE(reloaded.has_value()) << reloaded.error().detail;

    const auto& reload = reloaded->actors.at(ce::ActorId{"client"});
    EXPECT_EQ(reload.strategy, ce::AuthStrategy::Basic);
    EXPECT_TRUE(reload.authSteps.empty());
    EXPECT_EQ(reload.authConfig.at("username"), "{{secret.API_USER}}");
    EXPECT_EQ(reload.authConfig.at("password"), "{{secret.API_PASS}}");
    EXPECT_EQ(reload.inject.headers.at("Authorization"),
              "Basic {{client.credential}}");
}


// ─── Slice 4c — api_key auth strategy round-trip ────────────────────────────

TEST(SchemaWriter, api_key_auth_round_trips_with_full_options) {
    ScratchDir scratch;

    ce::Project original;
    original.name = "ApiKeyRoundTrip";
    original.defaultEnvironment = "local";
    original.environments["local"] = {{"baseUrl", "http://t.test"}};

    ce::Actor service;
    service.id = ce::ActorId{"service"};
    service.strategy = ce::AuthStrategy::ApiKey;
    service.authConfig = {{"key", "{{secret.SERVICE_API_KEY}}"},
                          {"location", "header"},
                          {"name", "X-API-Key"}};
    original.actors[service.id] = std::move(service);

    auto written = ce::writeProject(scratch.path(), original);
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    auto reloaded = ce::parseProject(*written);
    ASSERT_TRUE(reloaded.has_value()) << reloaded.error().detail;

    const auto& reload = reloaded->actors.at(ce::ActorId{"service"});
    EXPECT_EQ(reload.strategy, ce::AuthStrategy::ApiKey);
    EXPECT_TRUE(reload.authSteps.empty());
    EXPECT_EQ(reload.authConfig.at("key"), "{{secret.SERVICE_API_KEY}}");
    EXPECT_EQ(reload.authConfig.at("location"), "header");
    EXPECT_EQ(reload.authConfig.at("name"), "X-API-Key");
}

TEST(SchemaWriter, api_key_auth_round_trips_with_only_key) {
    ScratchDir scratch;

    ce::Project original;
    original.name = "ApiKeyManualRoundTrip";
    original.defaultEnvironment = "local";
    original.environments["local"] = {{"baseUrl", "http://t.test"}};

    ce::Actor service;
    service.id = ce::ActorId{"service"};
    service.strategy = ce::AuthStrategy::ApiKey;
    service.authConfig = {{"key", "sk_live_abc"}};
    service.inject.headers["Authorization"] = "Bearer {{service.key}}";
    original.actors[service.id] = std::move(service);

    auto written = ce::writeProject(scratch.path(), original);
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    auto reloaded = ce::parseProject(*written);
    ASSERT_TRUE(reloaded.has_value()) << reloaded.error().detail;

    const auto& reload = reloaded->actors.at(ce::ActorId{"service"});
    EXPECT_EQ(reload.strategy, ce::AuthStrategy::ApiKey);
    EXPECT_EQ(reload.authConfig.at("key"), "sk_live_abc");
    EXPECT_FALSE(reload.authConfig.contains("location"));
    EXPECT_FALSE(reload.authConfig.contains("name"));
    EXPECT_EQ(reload.inject.headers.at("Authorization"),
              "Bearer {{service.key}}");
}


// ─── Slice 4d — oauth2_client_credentials round-trip ────────────────────────

TEST(SchemaWriter, oauth2_client_credentials_round_trips_with_scope) {
    ScratchDir scratch;

    ce::Project original;
    original.name = "OAuth2RoundTrip";
    original.defaultEnvironment = "local";
    original.environments["local"] = {{"baseUrl", "http://t.test"}};

    ce::Actor service;
    service.id = ce::ActorId{"service"};
    service.strategy = ce::AuthStrategy::OAuth2ClientCredentials;
    service.authConfig = {
        {"token_url",     "{{env.baseUrl}}/oauth/token"},
        {"client_id",     "{{secret.OAUTH_CLIENT_ID}}"},
        {"client_secret", "{{secret.OAUTH_CLIENT_SECRET}}"},
        {"scope",         "read:orders write:orders"},
    };
    original.actors[service.id] = std::move(service);

    auto written = ce::writeProject(scratch.path(), original);
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    auto reloaded = ce::parseProject(*written);
    ASSERT_TRUE(reloaded.has_value()) << reloaded.error().detail;

    const auto& reload = reloaded->actors.at(ce::ActorId{"service"});
    EXPECT_EQ(reload.strategy, ce::AuthStrategy::OAuth2ClientCredentials);
    EXPECT_EQ(reload.authConfig.at("token_url"),
              "{{env.baseUrl}}/oauth/token");
    EXPECT_EQ(reload.authConfig.at("client_id"),
              "{{secret.OAUTH_CLIENT_ID}}");
    EXPECT_EQ(reload.authConfig.at("client_secret"),
              "{{secret.OAUTH_CLIENT_SECRET}}");
    EXPECT_EQ(reload.authConfig.at("scope"),
              "read:orders write:orders");
}

TEST(SchemaWriter, oauth2_client_credentials_round_trips_without_scope) {
    ScratchDir scratch;

    ce::Project original;
    original.name = "OAuth2NoScopeRoundTrip";
    original.defaultEnvironment = "local";
    original.environments["local"] = {{"baseUrl", "http://t.test"}};

    ce::Actor service;
    service.id = ce::ActorId{"service"};
    service.strategy = ce::AuthStrategy::OAuth2ClientCredentials;
    service.authConfig = {
        {"token_url",     "https://idp.test/oauth/token"},
        {"client_id",     "id"},
        {"client_secret", "sec"},
    };
    original.actors[service.id] = std::move(service);

    auto written = ce::writeProject(scratch.path(), original);
    ASSERT_TRUE(written.has_value());
    auto reloaded = ce::parseProject(*written);
    ASSERT_TRUE(reloaded.has_value());

    const auto& reload = reloaded->actors.at(ce::ActorId{"service"});
    EXPECT_EQ(reload.strategy, ce::AuthStrategy::OAuth2ClientCredentials);
    EXPECT_FALSE(reload.authConfig.contains("scope"));
}


// ─── Slice 4e — oauth2_password round-trip ──────────────────────────────────

TEST(SchemaWriter, oauth2_password_round_trips_with_scope) {
    ScratchDir scratch;

    ce::Project original;
    original.name = "OAuth2PasswordRoundTrip";
    original.defaultEnvironment = "local";
    original.environments["local"] = {{"baseUrl", "http://t.test"}};

    ce::Actor user;
    user.id = ce::ActorId{"user"};
    user.strategy = ce::AuthStrategy::OAuth2Password;
    user.authConfig = {
        {"token_url",     "{{env.baseUrl}}/oauth/token"},
        {"client_id",     "{{secret.OAUTH_CLIENT_ID}}"},
        {"client_secret", "{{secret.OAUTH_CLIENT_SECRET}}"},
        {"username",      "alice@example.com"},
        {"password",      "{{secret.RO_PASS}}"},
        {"scope",         "read:notes write:notes"},
    };
    original.actors[user.id] = std::move(user);

    auto written = ce::writeProject(scratch.path(), original);
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    auto reloaded = ce::parseProject(*written);
    ASSERT_TRUE(reloaded.has_value()) << reloaded.error().detail;

    const auto& reload = reloaded->actors.at(ce::ActorId{"user"});
    EXPECT_EQ(reload.strategy, ce::AuthStrategy::OAuth2Password);
    EXPECT_EQ(reload.authConfig.at("token_url"),
              "{{env.baseUrl}}/oauth/token");
    EXPECT_EQ(reload.authConfig.at("client_id"),
              "{{secret.OAUTH_CLIENT_ID}}");
    EXPECT_EQ(reload.authConfig.at("client_secret"),
              "{{secret.OAUTH_CLIENT_SECRET}}");
    EXPECT_EQ(reload.authConfig.at("username"), "alice@example.com");
    EXPECT_EQ(reload.authConfig.at("password"), "{{secret.RO_PASS}}");
    EXPECT_EQ(reload.authConfig.at("scope"),    "read:notes write:notes");
}


// ─── Slice 4f — oauth1 round-trip ───────────────────────────────────────────

TEST(SchemaWriter, oauth1_round_trips_three_legged_with_realm) {
    ScratchDir scratch;

    ce::Project original;
    original.name = "OAuth1RoundTrip";
    original.defaultEnvironment = "local";
    original.environments["local"] = {{"baseUrl", "http://t.test"}};

    ce::Actor tw;
    tw.id = ce::ActorId{"twitter"};
    tw.strategy = ce::AuthStrategy::OAuth1;
    tw.authConfig = {
        {"consumer_key",    "{{secret.OAUTH_KEY}}"},
        {"consumer_secret", "{{secret.OAUTH_SECRET}}"},
        {"token",           "{{secret.OAUTH_TOKEN}}"},
        {"token_secret",    "{{secret.OAUTH_TOKEN_SECRET}}"},
        {"realm",           "Photos"},
    };
    original.actors[tw.id] = std::move(tw);

    auto written = ce::writeProject(scratch.path(), original);
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    auto reloaded = ce::parseProject(*written);
    ASSERT_TRUE(reloaded.has_value()) << reloaded.error().detail;

    const auto& reload = reloaded->actors.at(ce::ActorId{"twitter"});
    EXPECT_EQ(reload.strategy, ce::AuthStrategy::OAuth1);
    EXPECT_EQ(reload.authConfig.at("consumer_key"),
              "{{secret.OAUTH_KEY}}");
    EXPECT_EQ(reload.authConfig.at("consumer_secret"),
              "{{secret.OAUTH_SECRET}}");
    EXPECT_EQ(reload.authConfig.at("token"),
              "{{secret.OAUTH_TOKEN}}");
    EXPECT_EQ(reload.authConfig.at("token_secret"),
              "{{secret.OAUTH_TOKEN_SECRET}}");
    EXPECT_EQ(reload.authConfig.at("realm"), "Photos");
}

TEST(SchemaWriter, oauth1_round_trips_two_legged) {
    ScratchDir scratch;

    ce::Project original;
    original.name = "OAuth1TwoLeggedRoundTrip";
    original.defaultEnvironment = "local";
    original.environments["local"] = {{"baseUrl", "http://t.test"}};

    ce::Actor app;
    app.id = ce::ActorId{"app"};
    app.strategy = ce::AuthStrategy::OAuth1;
    app.authConfig = {
        {"consumer_key",    "ck"},
        {"consumer_secret", "cs"},
    };
    original.actors[app.id] = std::move(app);

    auto written = ce::writeProject(scratch.path(), original);
    ASSERT_TRUE(written.has_value());

    auto reloaded = ce::parseProject(*written);
    ASSERT_TRUE(reloaded.has_value());

    const auto& reload = reloaded->actors.at(ce::ActorId{"app"});
    EXPECT_EQ(reload.strategy, ce::AuthStrategy::OAuth1);
    EXPECT_EQ(reload.authConfig.at("consumer_key"), "ck");
    EXPECT_FALSE(reload.authConfig.contains("token"));
    EXPECT_FALSE(reload.authConfig.contains("realm"));
}

TEST(SchemaWriter, aws_sigv4_round_trips_with_session_token_and_sign_payload) {
    ScratchDir scratch;

    ce::Project original;
    original.name = "AwsRoundTrip";
    original.defaultEnvironment = "local";
    original.environments["local"] = {{"baseUrl", "https://iam.amazonaws.com"}};

    ce::Actor aws;
    aws.id = ce::ActorId{"aws"};
    aws.strategy = ce::AuthStrategy::AwsSigV4;
    aws.authConfig = {
        {"access_key",    "{{secret.AWS_ACCESS_KEY}}"},
        {"secret_key",    "{{secret.AWS_SECRET_KEY}}"},
        {"region",        "us-east-1"},
        {"service",       "iam"},
        {"session_token", "{{secret.AWS_SESSION_TOKEN}}"},
        {"sign_payload",  "true"},
    };
    original.actors[aws.id] = std::move(aws);

    auto written = ce::writeProject(scratch.path(), original);
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    auto reloaded = ce::parseProject(*written);
    ASSERT_TRUE(reloaded.has_value()) << reloaded.error().detail;

    const auto& reload = reloaded->actors.at(ce::ActorId{"aws"});
    EXPECT_EQ(reload.strategy, ce::AuthStrategy::AwsSigV4);
    EXPECT_EQ(reload.authConfig.at("access_key"),
              "{{secret.AWS_ACCESS_KEY}}");
    EXPECT_EQ(reload.authConfig.at("secret_key"),
              "{{secret.AWS_SECRET_KEY}}");
    EXPECT_EQ(reload.authConfig.at("region"),  "us-east-1");
    EXPECT_EQ(reload.authConfig.at("service"), "iam");
    EXPECT_EQ(reload.authConfig.at("session_token"),
              "{{secret.AWS_SESSION_TOKEN}}");
    EXPECT_EQ(reload.authConfig.at("sign_payload"), "true");
}

TEST(SchemaWriter, aws_sigv4_round_trips_minimal_actor) {
    ScratchDir scratch;

    ce::Project original;
    original.name = "AwsMinimalRoundTrip";
    original.defaultEnvironment = "local";
    original.environments["local"] = {{"baseUrl", "https://s3.amazonaws.com"}};

    ce::Actor s3;
    s3.id = ce::ActorId{"s3"};
    s3.strategy = ce::AuthStrategy::AwsSigV4;
    s3.authConfig = {
        {"access_key", "AKIAEXAMPLE"},
        {"secret_key", "secret"},
        {"region",     "us-east-1"},
        {"service",    "s3"},
    };
    original.actors[s3.id] = std::move(s3);

    auto written = ce::writeProject(scratch.path(), original);
    ASSERT_TRUE(written.has_value());

    auto reloaded = ce::parseProject(*written);
    ASSERT_TRUE(reloaded.has_value());

    const auto& reload = reloaded->actors.at(ce::ActorId{"s3"});
    EXPECT_EQ(reload.strategy, ce::AuthStrategy::AwsSigV4);
    EXPECT_FALSE(reload.authConfig.contains("session_token"));
    EXPECT_FALSE(reload.authConfig.contains("sign_payload"));
}
