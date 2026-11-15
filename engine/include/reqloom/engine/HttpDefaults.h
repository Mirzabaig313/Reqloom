// Default request headers the HTTP client adds automatically, shared with the UI's auto-header
// panel.
#pragma once

#include <string_view>

// Injected from CMake (PROJECT_VERSION); the fallback keeps translation units without the define
// compiling.
#ifndef REQLOOM_VERSION
#define REQLOOM_VERSION "dev"
#endif

namespace reqloom::engine {

/// Sent as `User-Agent` unless the request already carries one.
inline constexpr std::string_view kDefaultUserAgent = "Reqloom/" REQLOOM_VERSION;

/// Sent as `Accept-Encoding` unless the request carries one; gzip/deflate only, which libcurl can
/// always decompress.
inline constexpr std::string_view kDefaultAcceptEncoding = "gzip, deflate";

/// Sent as `Connection` unless the request already carries one.
inline constexpr std::string_view kDefaultConnection = "keep-alive";

/// Sent as `Accept` by libcurl's own default (we don't set it), shown in the
/// UI for completeness.
inline constexpr std::string_view kDefaultAccept = "*/*";

}  // namespace reqloom::engine
