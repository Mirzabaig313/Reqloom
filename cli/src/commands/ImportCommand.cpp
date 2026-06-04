#include "ImportCommand.h"

#include <print>

namespace reqloom::cli {

int importCommand(const QStringList& args) {
    if (args.isEmpty()) {
        std::println(stderr, "reqloom import: missing <file>");
        return 2;
    }
    std::println("[stub] would import: {}", args.first().toStdString());
    return 0;
}

}  // namespace reqloom::cli
