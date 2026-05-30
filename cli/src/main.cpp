// chainapi CLI entry point.
//
// Subcommands:
//   chainapi run <op>      — execute an operation chain
//   chainapi lint          — validate a project schema
//   chainapi import <file> — import OpenAPI / Postman / Bruno
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
        "ChainAPI CLI\n"
        "  chainapi run <operation> [opts]   Execute a chain ending at <operation>\n"
        "    --project <path>                Project directory (default: cwd)\n"
        "    --env <name>                    Environment to run against\n"
        "    --var KEY=VALUE                 Override an env variable (repeatable)\n"
        "    --format text|json|junit        Output format (default: text)\n"
        "    --output <file>                 Write rendered output to <file>\n"
        "    --quiet                         Suppress live progress on stdout\n"
        "  chainapi lint                     Validate the schema in current project\n"
        "    --project <path>                Project directory (default: cwd)\n"
        "  chainapi import <file>            Import an external API spec\n"
        "  chainapi --help                   Show this message");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        // The QCoreApplication instance must outlive the argument parsing
        // below: it initializes the Qt application state that
        // QCoreApplication::arguments() reads from.
        [[maybe_unused]] const QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("chainapi"));
        QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

        const QStringList args = QCoreApplication::arguments();
        if (args.size() < 2 || args.at(1) == QStringLiteral("--help")) {
            printUsage();
            return args.size() < 2 ? 1 : 0;
        }

        const QString& verb = args.at(1);
        if (verb == QStringLiteral("run")) {
            return chainapi::cli::runCommand(args.mid(2));
        }
        if (verb == QStringLiteral("lint")) {
            return chainapi::cli::lintCommand(args.mid(2));
        }
        if (verb == QStringLiteral("import")) {
            return chainapi::cli::importCommand(args.mid(2));
        }

        std::println(stderr, "Unknown command: {}", verb.toStdString());
        printUsage();
        return 2;
    } catch (const std::exception& ex) {
        // Last-resort handler. Surface a stable exit code (3) so CI can
        // distinguish a crash from a normal validation failure (2) or a
        // successful run with failing checks (1). std::fputs is noexcept,
        // unlike std::println — using it here keeps the catch handler
        // itself from throwing and re-arming the exception-escape lint.
        // We discard fputs return values intentionally: if stderr is
        // closed or full, there is no recovery action available in a
        // last-resort handler — we still exit 3.
        // NOLINTBEGIN(cert-err33-c)
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
