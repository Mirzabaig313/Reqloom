// reqloom CLI entry point.
#include "commands/ExitCodes.h"
#include "commands/ImportCommand.h"
#include "commands/LintCommand.h"
#include "commands/RunCommand.h"

#include <reqloom/engine/HttpDefaults.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <csignal>
#include <cstdio>
#include <exception>
#include <print>

namespace {

void printUsage(std::FILE* stream) {
    std::print(
        stream,
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
        "  reqloom lint [<project-dir>]     Validate a project's schema\n"
        "    --project <path>                Project directory (default: cwd)\n"
        "    --format text|json              Output format (default: text)\n"
        "  reqloom import <file>            Import an external API spec\n"
        "    --out <dir>                     Directory to write the project (default: cwd)\n"
        "    --project-root <dir>            Containment root the spec must resolve under\n"
        "    --force                         Overwrite an existing reqloom.yaml\n"
        "    --no-secrets                    Keep credentials in the YAML\n"
        "  reqloom <command> --help         Show a command's options\n"
        "  reqloom --help, -h               Show this message\n"
        "  reqloom --version                Show the version\n"
        "Exit codes: 0 success; 1 the run or project failed; 2 invalid invocation (also an\n"
        "unknown project, operation or environment); 3 file or system error; `run` also\n"
        "130 after SIGINT and 143 after SIGTERM.\n");
}

[[nodiscard]] int dispatch(const QStringList& args) {
    if (args.size() < 2) {
        std::println(stderr, "reqloom: missing command");
        printUsage(stderr);
        return reqloom::cli::kExitUsage;
    }
    const QString& verb = args.at(1);
    if (verb == QStringLiteral("--help") || verb == QStringLiteral("-h")) {
        printUsage(stdout);
        return reqloom::cli::kExitSuccess;
    }
    if (verb == QStringLiteral("--version")) {
        std::println("reqloom {}", REQLOOM_VERSION);
        return reqloom::cli::kExitSuccess;
    }
    if (verb == QStringLiteral("run")) {
        return reqloom::cli::runCommand(args.mid(2));
    }
    if (verb == QStringLiteral("lint")) {
        return reqloom::cli::lintCommand(args.mid(2));
    }
    if (verb == QStringLiteral("import")) {
        return reqloom::cli::importCommand(args.mid(2));
    }
    std::println(stderr, "reqloom: unknown command '{}'", verb.toStdString());
    printUsage(stderr);
    return reqloom::cli::kExitUsage;
}

}  // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
    // A closed pipe then fails the write with EPIPE, reported as exit 3, instead of killing us.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    try {
        // Must outlive argument parsing: QCoreApplication::arguments() reads its state.
        [[maybe_unused]] const QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("reqloom"));
        QCoreApplication::setApplicationVersion(QStringLiteral(REQLOOM_VERSION));

        const int code = dispatch(QCoreApplication::arguments());
        const bool stdoutFailed = std::fflush(stdout) != 0 || std::ferror(stdout) != 0;
        if (stdoutFailed && code != reqloom::cli::kExitSystem) {
            // NOLINTNEXTLINE(cert-err33-c): nothing more can be done if stderr fails too.
            std::fputs("reqloom: cannot write to standard output\n", stderr);
        }
        if (stdoutFailed) {
            return reqloom::cli::kExitSystem;
        }
        return code;
    } catch (const std::exception& ex) {
        // Last-resort handler: exit 3 so CI can tell a crash from an invalid invocation (2) or
        // failing checks (1). fputs is noexcept and its result is deliberately ignored; nothing can
        // be done if stderr is closed. NOLINTBEGIN(cert-err33-c)
        std::fputs("fatal: ", stderr);
        std::fputs(ex.what(), stderr);
        std::fputs("\n", stderr);
        // NOLINTEND(cert-err33-c)
        return reqloom::cli::kExitSystem;
    } catch (...) {
        // NOLINTNEXTLINE(cert-err33-c) — same rationale as above.
        std::fputs("fatal: unknown exception\n", stderr);
        return reqloom::cli::kExitSystem;
    }
}
