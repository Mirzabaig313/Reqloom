// Per-environment transport knobs, stamped onto every outbound HttpRequest and translated to
// CURLOPT_* by CurlHttpClient. Defaults are safe: TLS verified, no proxy, 5-second connect timeout.
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace reqloom::engine {

struct TransportConfig {
    /// Verify the peer's TLS certificate (`CURLOPT_SSL_VERIFYPEER`).
    /// Set to `false` for self-signed dev certs.
    bool tlsVerify{true};

    /// Verify the certificate's hostname matches the URL
    /// (`CURLOPT_SSL_VERIFYHOST`). Mirrors `tlsVerify` by default.
    bool tlsVerifyHost{true};

    /// Optional custom CA bundle (`CURLOPT_CAINFO`), used in place of the system trust store.
    std::optional<std::string> caBundlePath;

    /// Outbound proxy URL (`CURLOPT_PROXY`): `http://`, `https://`, `socks5://` or `socks5h://`.
    /// Empty means no override; libcurl still honours `HTTPS_PROXY` / `NO_PROXY`.
    std::optional<std::string> proxy;

    /// TCP connect timeout (`CURLOPT_CONNECTTIMEOUT_MS`). Default 5s.
    std::chrono::milliseconds connectTimeout{5'000};

    /// Mutual TLS client certificate (`CURLOPT_SSLCERT`) and private key (`CURLOPT_SSLKEY`); empty
    /// means none.
    std::optional<std::string> clientCertPath;
    std::optional<std::string> clientKeyPath;
    /// Passphrase for an encrypted client key, or the PKCS#12 password
    /// (`CURLOPT_KEYPASSWD`).
    std::optional<std::string> clientKeyPassword;
    /// Certificate type (`CURLOPT_SSLCERTTYPE`): "PEM" (default) or "P12", where `clientCertPath`
    /// is the .p12/.pfx and `clientKeyPath` is unused.
    std::optional<std::string> clientCertType;

    /// Upper bound on the response body, counted after decompression; a larger body fails with
    /// `E_NETWORK_POLICY`. Default 64 MiB. YAML `transport.max_response_size`.
    std::size_t maxResponseBytes{std::size_t{64} * 1024 * 1024};
};

}  // namespace reqloom::engine
