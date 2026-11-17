// KeychainSecretStore — a write or removal that did not reach a keychain must fail.
#include "infrastructure/secrets/KeychainSecretStore.h"

#include <gtest/gtest.h>

#include <string>

namespace ce = reqloom::engine;

TEST(KeychainSecretStore, write_and_remove_fail_when_no_keychain_is_reachable) {
    // This binary runs no QCoreApplication, so even the QtKeychain backend cannot complete a job.
    ce::KeychainSecretStore store;
    const auto written = store.write("reqloom-test/never-stored", "value-123456");
    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().code, ce::ErrorCode::SecretAccessFailed);
    const auto removed = store.remove("reqloom-test/never-stored");
    ASSERT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().code, ce::ErrorCode::SecretAccessFailed);
}

TEST(KeychainSecretStore, placeholder_build_explains_that_no_backend_exists) {
    if (ce::KeychainSecretStore::backendAvailable()) {
        GTEST_SKIP() << "built with QtKeychain; the placeholder is not compiled";
    }
    ce::KeychainSecretStore store;
    const auto written = store.write("p/name", "value-123456");
    ASSERT_FALSE(written.has_value());
    EXPECT_NE(written.error().detail.find("no OS keychain backend"), std::string::npos);
    EXPECT_NE(written.error().detail.find("REQLOOM_SECRET_"), std::string::npos);
    const auto read = store.read("p/name");
    ASSERT_TRUE(read.has_value());
    EXPECT_FALSE(read->has_value());
}

TEST(KeychainSecretStore, available_reports_whether_a_backend_exists) {
    const ce::KeychainSecretStore store;
    EXPECT_EQ(store.available(), ce::KeychainSecretStore::backendAvailable());
}

TEST(KeychainSecretStore, read_that_cannot_reach_a_keychain_says_so_for_that_read_only) {
    // No QCoreApplication here, so no keychain job can run: the read finds nothing and reports
    // that it could not reach a keychain (FR-11.6). The store stays available: the next read
    // tries again, so a keyring daemon that starts late is not lost for the whole session.
    ce::KeychainSecretStore store;
    const auto read = store.read("reqloom-test/never-stored");
    ASSERT_TRUE(read.has_value());
    EXPECT_FALSE(read->has_value());
    EXPECT_TRUE(store.lastReadUnreachable());
    EXPECT_EQ(store.available(), ce::KeychainSecretStore::backendAvailable());
}
