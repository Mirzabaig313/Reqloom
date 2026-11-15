// Project identity helpers — see reqloom/engine/ProjectIdentity.h.
#include <reqloom/engine/ProjectIdentity.h>

#include <reqloom/engine/ExecutionEngine.h>

#include <array>
#include <cstdint>
#include <format>
#include <random>
#include <string>
#include <string_view>

namespace reqloom::engine {

std::expected<void, ReqloomError> validateProjectId(const std::string_view id) {
    const auto invalid = [](const std::string& why) {
        return std::unexpected(
            ReqloomError{ErrorCode::SchemaInvalid, ErrorClass::Schema, "project id " + why});
    };
    if (id.empty()) {
        return invalid("must not be empty");
    }
    if (id.size() > kMaxProjectIdLength) {
        return invalid(std::format("must be at most {} characters", kMaxProjectIdLength));
    }
    for (const char c : id) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '/') {
            return invalid("must not contain '/'");
        }
        if (byte < 0x20 || byte == 0x7F) {
            return invalid("must not contain control characters");
        }
    }
    return {};
}

std::string generateProjectId() {
    // random_device is the OS entropy source on every supported platform
    // (arc4random / getrandom / rand_s), so ids are unpredictable.
    std::random_device rd;
    std::array<std::uint8_t, 16> bytes{};
    for (auto& b : bytes) {
        b = static_cast<std::uint8_t>(rd() & 0xFFU);
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);

    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out += std::format("{:02x}", bytes[i]);
    }
    return out;
}

std::expected<std::string, ReqloomError> projectSecretKey(const Project& project,
                                                          const std::string_view name) {
    if (project.id.empty()) {
        return std::unexpected(ReqloomError{
            ErrorCode::SchemaInvalid,
            ErrorClass::Schema,
            "this project uses secrets but has no id, so its secrets cannot be looked up. "
            "Add `id: " +
                generateProjectId() + "` to reqloom.yaml (any unique value without '/')."});
    }
    if (auto ok = validateProjectId(project.id); !ok) {
        return std::unexpected(ok.error());
    }
    return project.id + "/" + std::string{name};
}

}  // namespace reqloom::engine
