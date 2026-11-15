// reqloom CLI entry point.
#include "commands/ImportCommand.h"
#include "commands/LintCommand.h"
#include "commands/RunCommand.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstdio>
#include <print>

namespace {

void printUsage() {
    std::println(
        "Reqloom CLI\n"
        "  reqloom run <operation> [opts]   Execute a chain ending at <operation>\n"
        "    --project <path>                Project directory (default: cwd)\n"
        "    --env <name>                    Environment to run against\n"
        "    --var KEY=VALUE                 Override an env variable (repeatable)\n"
        "    --format text|json|junit        Output format (default: text)\n"
        "    --output <file>                 Write rendered output to <file>\n"
        "    --quiet                         Suppress live progress on stdout\n"
        "    --failure-body-size <size>      Response text in a failure message, e.g. 16KiB\n"
        "                                    (default 4KiB, 0 = none, at most 1MiB)\n"
        "  reqloom lint                     Validate the schema in current project\n"
        "    --project <path>                Project directory (default: cwd)\n"
        "  reqloom import <file>            Import an external API spec\n"
        "    --out <dir>                     Directory to write the project (default: cwd)\n"
        "    --project-root <dir>            Containment root the spec must resolve under\n"
        "    --force                         Overwrite an existing reqloom.yaml\n"
        "  reqloom --help                   Show this message");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        // Must outlive argument parsing: QCoreApplication::arguments() reads its state.
        [[maybe_unused]] const QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("reqloom"));
        QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

        const QStringList args = QCoreApplication::arguments();
        if (args.size() < 2 || args.at(1) == QStringLiteral("--help")) {
            printUsage();
            return args.size() < 2 ? 1 : 0;
        }

        const QString& verb = args.at(1);
        if (verb == QStringLiteral("run")) {
            return reqloom::cli::runCommand(args.mid(2));
        }
        if (verb == QStringLiteral("lint")) {
            return reqloom::cli::lintCommand(args.mid(2));
        }
        if (verb == QStringLiteral("import")) {
            return reqloom::cli::importCommand(args.mid(2));
        }

        std::println(stderr, "Unknown command: {}", verb.toStdString());
        printUsage();
        return 2;
    } catch (const std::exception& ex) {
        // Last-resort handler: exit 3 so CI can tell a crash from a validation failure (2) or
        // failing checks (1). fputs is noexcept and its result is deliberately ignored; nothing can
        // be done if stderr is closed. NOLINTBEGIN(cert-err33-c)
        std::fputs("fatal: ", stderr);
        std::fputs(ex.what(), stderr);
        std::fputs("\n", stderr);
        // NOLINTEND(cert-err33-c)
        return 3;
    } catch (...) {
        // NOLINTNEXTLINE(cert-err33-c) — same rationale as above.
        std::fputs("fatal: unknown exception\n", stderr);
        return 3;
    }
}
