// HostApprovalStoreTest — host keys (how two URLs are decided to be the same host) and the
// per-user file that records approved hosts per project (FR-15.9). Uses the real file-backed store
// pointed at a temp file.
#include "support/TempPath.h"

#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/Factories.h>
#include <reqloom/engine/HostApproval.h>

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::string keyOf(std::string_view url) {
    auto key = ce::hostKeyForUrl(url);
    return key ? *key : "error: " + key.error().detail;
}

[[nodiscard]] std::string entryOf(std::string_view entry) {
    auto key = ce::hostKeyForEntry(entry);
    return key ? *key : "error";
}

}  // namespace

TEST(HostKey, url_key_is_lowercase_host_and_effective_port) {
    EXPECT_EQ(keyOf("https://API.Example.com/v1/x?q=1"), "https://api.example.com:443");
    EXPECT_EQ(keyOf("http://api.example.com/v1"), "http://api.example.com:80");
    EXPECT_EQ(keyOf("https://api.example.com:8443/"), "https://api.example.com:8443");
    EXPECT_EQ(keyOf("http://127.0.0.1:3000/login"), "http://127.0.0.1:3000");
    EXPECT_EQ(keyOf("http://[::1]:3000/"), "http://[::1]:3000");
    EXPECT_EQ(keyOf("https://api.example.com./x"), "https://api.example.com:443");
}

TEST(HostKey, url_key_ignores_user_info_and_reads_the_real_host) {
    // `approved.example.com` here is a user name; the request goes to evil.example.
    EXPECT_EQ(keyOf("https://approved.example.com@evil.example/x"), "https://evil.example:443");
    EXPECT_EQ(keyOf("https://user:pw@api.example.com/"), "https://api.example.com:443");
}

TEST(HostKey, scheme_and_port_make_different_keys) {
    EXPECT_NE(keyOf("http://api.example.com/"), keyOf("https://api.example.com/"));
    EXPECT_NE(keyOf("http://127.0.0.1:3000/"), keyOf("http://127.0.0.1:3001/"));
    EXPECT_NE(keyOf("http://localhost:3000/"), keyOf("http://127.0.0.1:3000/"));
}

TEST(HostKey, url_without_a_usable_host_is_an_error) {
    EXPECT_FALSE(ce::hostKeyForUrl("not a url").has_value());
    EXPECT_FALSE(ce::hostKeyForUrl("/relative/path").has_value());
    EXPECT_FALSE(ce::hostKeyForUrl("").has_value());
}

TEST(HostKey, non_ascii_host_is_refused) {
    const auto key = ce::hostKeyForUrl(
        "https://b\xC3\xBC"
        "cher.example/");
    ASSERT_FALSE(key.has_value());
    EXPECT_NE(key.error().detail.find("xn--"), std::string::npos) << key.error().detail;
}

TEST(HostKey, typed_entries_default_to_port_443) {
    EXPECT_EQ(entryOf("api.example.com"), "https://api.example.com:443");
    EXPECT_EQ(entryOf(" API.example.com:8443 "), "https://api.example.com:8443");
    EXPECT_EQ(entryOf("http://127.0.0.1:3000"), "http://127.0.0.1:3000");
    EXPECT_EQ(entryOf("[::1]:3000"), "https://[::1]:3000");
}

TEST(HostKey, typed_entries_refuse_anything_but_a_host_and_port) {
    for (const char* bad : {"ftp://api.example.com",
                            "https://api.example.com/x",
                            "api.example.com/path",
                            "user@api.example.com",
                            "*.example.com",
                            "api.example.com:0",
                            "api.example.com:99999",
                            "api.example.com:",
                            "",
                            "api example.com"}) {
        EXPECT_FALSE(ce::hostKeyForEntry(bad).has_value()) << bad;
    }
}

TEST(HostKey, allowed_hosts_entry_without_port_allows_any_port) {
    EXPECT_TRUE(ce::allowedHostMatches("api.example.com", "https://api.example.com:443"));
    EXPECT_TRUE(ce::allowedHostMatches("API.example.com", "https://api.example.com:8443"));
    EXPECT_TRUE(ce::allowedHostMatches("api.example.com:8443", "https://api.example.com:8443"));
    EXPECT_FALSE(ce::allowedHostMatches("api.example.com:8443", "https://api.example.com:443"));
    EXPECT_FALSE(ce::allowedHostMatches("example.com", "https://api.example.com:443"));
    EXPECT_FALSE(ce::allowedHostMatches("api.example.com", "https://api.example.com.evil:443"));
    EXPECT_TRUE(ce::allowedHostMatches("[::1]", "http://[::1]:3000"));
}

TEST(HostKey, host_list_parses_comma_separated_entries) {
    auto list = ce::parseHostList(" api.example.com , http://127.0.0.1:3000,,");
    ASSERT_TRUE(list.has_value()) << list.error().detail;
    EXPECT_EQ(*list,
              (std::vector<std::string>{"https://api.example.com:443", "http://127.0.0.1:3000"}));

    auto bad = ce::parseHostList("api.example.com,https://evil.example/x");
    ASSERT_FALSE(bad.has_value());
    EXPECT_NE(bad.error().detail.find("https://evil.example/x"), std::string::npos);
}

// ─── File-backed store ──────────────────────────────────────────────────────

class HostApprovalFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = reqloom::tests::uniqueTempPath("reqloom-host-approval");
        fs::create_directories(dir_ / "project-a");
        fs::create_directories(dir_ / "project-b");
        file_ = dir_ / "config" / "projects.json";
        project_.name = "Shop";
        project_.id = "shop-0001";
        project_.rootDir = dir_ / "project-a";
    }
    void TearDown() override { fs::remove_all(dir_); }

    [[nodiscard]] std::unique_ptr<ce::HostApprovalStore> store() const {
        return ce::makeFileHostApprovalStore(file_);
    }

    [[nodiscard]] nlohmann::json document() const {
        std::ifstream in{file_};
        return nlohmann::json::parse(std::string{std::istreambuf_iterator<char>{in}, {}});
    }

    fs::path dir_;
    fs::path file_;
    ce::Project project_;
};

TEST_F(HostApprovalFileTest, nothing_is_approved_at_first) {
    auto approved = store()->isApproved(project_, "api.example.com:443");
    ASSERT_TRUE(approved.has_value()) << approved.error().detail;
    EXPECT_FALSE(*approved);
}

TEST_F(HostApprovalFileTest, an_approval_is_remembered_across_store_instances) {
    ASSERT_TRUE(store()->approve(project_, "api.example.com:443").has_value());
    auto approved = store()->isApproved(project_, "api.example.com:443");
    ASSERT_TRUE(approved.has_value());
    EXPECT_TRUE(*approved);
    EXPECT_FALSE(*store()->isApproved(project_, "api.example.com:8443"));
    EXPECT_EQ(*store()->approvedHosts(project_), (std::vector<std::string>{"api.example.com:443"}));

    const auto doc = document();
    EXPECT_EQ(doc["projects"]["shop-0001"]["approved_hosts"],
              nlohmann::json::array({"api.example.com:443"}));
    EXPECT_TRUE(doc["projects"]["shop-0001"]["folder"].is_string());
}

TEST_F(HostApprovalFileTest, approvals_are_per_project_id) {
    ASSERT_TRUE(store()->approve(project_, "api.example.com:443").has_value());
    ce::Project other = project_;
    other.id = "other-0002";
    other.rootDir = dir_ / "project-b";
    EXPECT_FALSE(*store()->isApproved(other, "api.example.com:443"));
}

TEST_F(HostApprovalFileTest, a_copy_claiming_the_id_from_another_folder_gets_no_approvals) {
    ASSERT_TRUE(store()->approve(project_, "evil.example:443").has_value());
    ce::Project copy = project_;
    copy.rootDir = dir_ / "project-b";
    auto approved = store()->isApproved(copy, "https://evil.example:443");
    ASSERT_FALSE(approved.has_value()) << "a copy in another folder inherited the approvals";
    EXPECT_NE(approved.error().detail.find("project-a"), std::string::npos)
        << approved.error().detail;
    EXPECT_FALSE(store()->approve(copy, "evil.example:443").has_value());
}

TEST_F(HostApprovalFileTest, a_project_without_an_id_cannot_be_approved) {
    project_.id.clear();
    auto approved = store()->approve(project_, "api.example.com:443");
    ASSERT_FALSE(approved.has_value());
    EXPECT_NE(approved.error().detail.find("id:"), std::string::npos) << approved.error().detail;
}

TEST_F(HostApprovalFileTest, revoke_forgets_one_host) {
    ASSERT_TRUE(store()->approve(project_, "a.example:443").has_value());
    ASSERT_TRUE(store()->approve(project_, "b.example:443").has_value());
    ASSERT_TRUE(store()->revoke(project_, "a.example:443").has_value());
    EXPECT_EQ(*store()->approvedHosts(project_), (std::vector<std::string>{"b.example:443"}));
    EXPECT_TRUE(store()->revoke(project_, "never.example:443").has_value());
}

TEST_F(HostApprovalFileTest, malformed_entries_in_the_file_approve_nothing) {
    fs::create_directories(file_.parent_path());
    {
        std::ofstream out{file_};
        out << R"({"version":1,"projects":{"shop-0001":{"folder":")"
            << fs::weakly_canonical(project_.rootDir).generic_string()
            << R"(","approved_hosts":"api.example.com:443"}}})";
    }
    auto approved = store()->isApproved(project_, "api.example.com:443");
    ASSERT_TRUE(approved.has_value()) << approved.error().detail;
    EXPECT_FALSE(*approved);
}

TEST_F(HostApprovalFileTest, cli_approval_is_allowed_until_turned_off) {
    EXPECT_TRUE(*store()->cliApprovalAllowed());
    ASSERT_TRUE(store()->setCliApprovalAllowed(false).has_value());
    EXPECT_FALSE(*store()->cliApprovalAllowed());
    EXPECT_EQ(document()["settings"]["cli_host_approval"], false);
    ASSERT_TRUE(store()->setCliApprovalAllowed(true).has_value());
    EXPECT_TRUE(*store()->cliApprovalAllowed());
}

TEST_F(HostApprovalFileTest, approving_keeps_the_folder_record_and_unknown_keys) {
    fs::create_directories(file_.parent_path());
    {
        std::ofstream out{file_};
        out << R"({"version":1,"future":"kept","projects":{}})";
    }
    ASSERT_TRUE(store()->approve(project_, "api.example.com:443").has_value());
    const auto doc = document();
    EXPECT_EQ(doc["future"], "kept");
    EXPECT_EQ(doc["projects"]["shop-0001"]["folder"],
              fs::weakly_canonical(project_.rootDir).generic_string());
}

TEST(HostKey, ip_literals_are_keyed_as_the_url_parser_normalises_them) {
    // libcurl normalises IPv4 shorthand and hex, so these are the same host as 127.0.0.1.
    EXPECT_EQ(keyOf("http://127.1:3000/"), "http://127.0.0.1:3000");
    EXPECT_EQ(keyOf("http://0x7f.0.0.1:3000/"), "http://127.0.0.1:3000");
    // An IPv6 zone id is not part of the key.
    EXPECT_EQ(keyOf("http://[fe80::1%25en0]:3000/"), "http://[fe80::1]:3000");
}

// Review finding 3: a damaged file never reads as "CLI approval allowed" and is never rewritten.
TEST_F(HostApprovalFileTest, damaged_file_reads_cli_approval_as_off_and_refuses_writes) {
    fs::create_directories(file_.parent_path());
    for (const std::string damaged : {std::string{"{ not json"}, std::string{"[1,2]"}}) {
        {
            std::ofstream out{file_, std::ios::binary};
            out << damaged;
        }
        auto allowed = store()->cliApprovalAllowed();
        ASSERT_TRUE(allowed.has_value());
        EXPECT_FALSE(*allowed) << damaged;
        EXPECT_FALSE(store()->approve(project_, "https://api.example.com:443").has_value());
        EXPECT_FALSE(store()->setCliApprovalAllowed(true).has_value());
        std::ifstream in{file_, std::ios::binary};
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>{in}, {}), damaged);
    }
}

TEST_F(HostApprovalFileTest, missing_file_allows_cli_approval) {
    EXPECT_TRUE(*store()->cliApprovalAllowed());
}
