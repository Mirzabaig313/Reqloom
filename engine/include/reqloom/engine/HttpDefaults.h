// Default request headers the HTTP client adds automatically. Shared so the
// transport (CurlHttpClient) and the UI's "auto-generated headers" panel show
// the exact same values — one source of truth, no drift.
#pragma once

#include <string_view>

// Injected globally from CMake (PROJECT_VERSION); the fallback keeps stray
// translation units that miss the define compiling.
#ifndef REQLOOM_VERSION
#define REQLOOM_VERSION "dev"
#endif

namespace reqloom::engine {

/// Sent as `User-Agent` unless the request already carries one.
inline constexpr std::string_view kDefaultUserAgent = "Reqloom/" REQLOOM_VERSION;

/// Sent as `Accept-Encoding` unless the request already carries one. Only
/// gzip/deflate — both are built into libcurl wherever zlib is present, so the
/// advertised value always matches what libcurl can actually decompress.
inline constexpr std::string_view kDefaultAcceptEncoding = "gzip, deflate";

/// Sent as `Connection` unless the request already carries one.
inline constexpr std::string_view kDefaultConnection = "keep-alive";

/// Sent as `Accept` by libcurl's own default (we don't set it), shown in the
/// UI for completeness.
inline constexpr std::string_view kDefaultAccept = "*/*";

}  // namespace reqloom::engine
