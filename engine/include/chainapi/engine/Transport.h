// Per-environment transport knobs. Resolved by the schema parser, held
// by Project, and stamped onto every outbound HttpRequest by the
// executor. The HTTP-client layer (CurlHttpClient) then translates
// these into CURLOPT_* settings.
//
// Defaults are deliberately safe: TLS verified end to end, no proxy,
// 5-second connect timeout. Schemas that omit `transport:` keep the
// pre-existing behavior verbatim.
#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace chainapi::engine {

struct TransportConfig {
    /// Verify the peer's TLS certificate (`CURLOPT_SSL_VERIFYPEER`).
    /// Set to `false` for self-signed dev certs.
    bool tlsVerify{true};

    /// Verify the certificate's hostname matches the URL
    /// (`CURLOPT_SSL_VERIFYHOST`). Mirrors `tlsVerify` by default.
    bool tlsVerifyHost{true};

    /// Optional path to a custom CA bundle (`CURLOPT_CAINFO`). When set,
    /// libcurl uses this file in place of the system trust store. Useful
    /// for corporate roots and dev environments.
    std::optional<std::string> caBundlePath;

    /// Outbound proxy URL (`CURLOPT_PROXY`). Schemes: `http://`,
    /// `https://`, `socks5://`, `socks5h://`. Empty means "no override"
    /// — libcurl still honours `HTTPS_PROXY` / `NO_PROXY` env vars per
    /// request, matching the `curl` CLI's behavior.
    std::optional<std::string> proxy;

    /// TCP connect timeout (`CURLOPT_CONNECTTIMEOUT_MS`). Default 5s
    /// matches the previous hard-coded value.
    std::chrono::milliseconds connectTimeout{5'000};
};

}  // namespace chainapi::engine
