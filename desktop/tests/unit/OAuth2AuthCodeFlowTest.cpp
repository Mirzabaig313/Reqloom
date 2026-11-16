// OAuth2AuthCodeFlow loopback listener: which addresses the redirect host binds.

#include "application/OAuth2AuthCodeFlow.h"

#include <gtest/gtest.h>

#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

namespace reqloom::desktop::tests {

namespace {

[[nodiscard]] quint16 freePort() {
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
}

[[nodiscard]] bool hasIpv6Loopback() {
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHostIPv6, 0);
}

[[nodiscard]] bool accepts(const QHostAddress& address, quint16 port) {
    QTcpSocket socket;
    socket.connectToHost(address, port);
    return socket.waitForConnected(2000);
}

}  // namespace

TEST(OAuth2AuthCodeFlow, localhost_maps_to_both_loopback_families) {
    const auto addresses = OAuth2AuthCodeFlow::loopbackAddressesFor(QStringLiteral("localhost"));
    ASSERT_EQ(addresses.size(), 2);
    EXPECT_EQ(addresses.at(0), QHostAddress(QHostAddress::LocalHost));
    EXPECT_EQ(addresses.at(1), QHostAddress(QHostAddress::LocalHostIPv6));
}

TEST(OAuth2AuthCodeFlow, a_loopback_literal_maps_to_itself_only) {
    EXPECT_EQ(OAuth2AuthCodeFlow::loopbackAddressesFor(QStringLiteral("127.0.0.1")),
              QList<QHostAddress>{QHostAddress(QHostAddress::LocalHost)});
    EXPECT_EQ(OAuth2AuthCodeFlow::loopbackAddressesFor(QStringLiteral("::1")),
              QList<QHostAddress>{QHostAddress(QHostAddress::LocalHostIPv6)});
}

TEST(OAuth2AuthCodeFlow, a_non_loopback_host_maps_to_nothing) {
    for (const char* host : {"0.0.0.0", "::", "example.com", "192.168.1.10", "127.0.0.2", ""}) {
        EXPECT_TRUE(OAuth2AuthCodeFlow::loopbackAddressesFor(QString::fromLatin1(host)).isEmpty())
            << host;
    }
}

TEST(OAuth2AuthCodeFlow, localhost_callback_is_reachable_over_ipv6_and_ipv4) {
    if (!hasIpv6Loopback()) {
        GTEST_SKIP() << "no IPv6 loopback on this machine";
    }
    const quint16 port = freePort();
    ASSERT_NE(port, 0);

    auto servers = OAuth2AuthCodeFlow::listenLoopback(QStringLiteral("localhost"), port);

    ASSERT_TRUE(servers.has_value()) << servers.error().toStdString();
    EXPECT_EQ(servers->size(), 2u);
    EXPECT_TRUE(accepts(QHostAddress(QHostAddress::LocalHostIPv6), port));
    EXPECT_TRUE(accepts(QHostAddress(QHostAddress::LocalHost), port));
}

TEST(OAuth2AuthCodeFlow, ipv6_literal_callback_listens_on_ipv6_loopback) {
    if (!hasIpv6Loopback()) {
        GTEST_SKIP() << "no IPv6 loopback on this machine";
    }
    const quint16 port = freePort();
    ASSERT_NE(port, 0);

    auto servers = OAuth2AuthCodeFlow::listenLoopback(QStringLiteral("::1"), port);

    ASSERT_TRUE(servers.has_value()) << servers.error().toStdString();
    ASSERT_EQ(servers->size(), 1u);
    EXPECT_EQ(servers->front()->serverAddress(), QHostAddress(QHostAddress::LocalHostIPv6));
    EXPECT_TRUE(accepts(QHostAddress(QHostAddress::LocalHostIPv6), port));
}

TEST(OAuth2AuthCodeFlow, a_port_held_on_one_family_fails_instead_of_sharing_the_callback) {
    QTcpServer holder;
    ASSERT_TRUE(holder.listen(QHostAddress::LocalHost, 0));

    const auto servers =
        OAuth2AuthCodeFlow::listenLoopback(QStringLiteral("localhost"), holder.serverPort());

    ASSERT_FALSE(servers.has_value());
    EXPECT_TRUE(servers.error().contains(QString::number(holder.serverPort())));
}

TEST(OAuth2AuthCodeFlow, never_listens_for_a_non_loopback_host) {
    const auto servers = OAuth2AuthCodeFlow::listenLoopback(QStringLiteral("0.0.0.0"), freePort());
    EXPECT_FALSE(servers.has_value());
}

}  // namespace reqloom::desktop::tests
