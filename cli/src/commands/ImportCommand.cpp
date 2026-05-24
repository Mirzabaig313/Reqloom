#include "ImportCommand.h"

#include <print>

namespace chainapi::cli {

int importCommand(const QStringList& args) {
    if (args.isEmpty()) {
        std::println(stderr, "chainapi import: missing <file>");
        return 2;
    }
    std::println("[stub] would import: {}", args.first().toStdString());
    return 0;
}

}  // namespace chainapi::cli
