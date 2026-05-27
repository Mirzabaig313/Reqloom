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

#include <print>

namespace {

void printUsage() {
    std::println(
        "ChainAPI CLI\n"
        "  chainapi run <operation>      Execute a chain ending at <operation>\n"
        "  chainapi lint                 Validate the schema in current project\n"
        "  chainapi import <file>        Import an external API spec\n"
        "  chainapi --help               Show this message");
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("chainapi"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));

    const QStringList args = QCoreApplication::arguments();
    if (args.size() < 2 || args.at(1) == QStringLiteral("--help")) {
        printUsage();
        return args.size() < 2 ? 1 : 0;
    }

    const QString verb = args.at(1);
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
}
