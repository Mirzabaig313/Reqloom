// `reqloom show` — see header.
//
// Reads only the step rows `reqloom run` recorded after masking them (`recordedSteps`); never the
// run's events, which keep what a person saw in the app. A run the app started recorded no rows
// and is refused.
#include "ShowCommand.h"

#include "ArgParser.h"
#include "ByteSizeArgument.h"
#include "ExitCodes.h"
#include "PathText.h"
#include "ProjectCommand.h"
#include "RunRecording.h"

#include "../output/JsonWriter.h"
#include "../output/NextAction.h"
#include "../output/StepFormatting.h"
#include "../output/TerminalText.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/JsonValues.h>
#include <reqloom/engine/PublicApi.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace ce = reqloom::engine;

namespace reqloom::cli {

namespace {

constexpr std::array kOptions{
    OptionSpec{.name = "--project", .takesValue = true},
    OptionSpec{.name = "--format", .takesValue = true},
    OptionSpec{.name = "--path", .takesValue = true},
    OptionSpec{.name = "--size", .takesValue = true},
};

/// Default bytes of body printed (4 KiB).
constexpr std::size_t kDefaultShowBytes = 4096;

/// Most rows listed when a step is not found.
constexpr std::size_t kListedSteps = 50;

void printUsage(std::FILE* stream) {
    std::print(
        stream,
        "Usage: reqloom show <run> <step> [options]\n"
        "What one step of an earlier `reqloom run` sent and received: method, URL, headers\n"
        "and body sent; status, extracted values, assertions and the response body. Masked\n"
        "exactly as `reqloom run` output is; there is no way to unmask it. Only runs started\n"
        "with `reqloom run` can be shown (runs from the Reqloom app keep unmasked values for\n"
        "the person and are not printed here).\n"
        "  <run>              A run id (`run_id` in run --format json), or `last`\n"
        "  <step>             The row's index in the run's `steps` array (as\n"
        "                     failed_step.index gives it), or an operation id\n"
        "  --project <path>   Project directory (default: cwd)\n"
        "  --path <jsonpath>  Print only what this JSONPath selects from the response body\n"
        "                     ($.a.b, $.a[0], $.a[*].id, $.a[?(@.k=='v')])\n"
        "  --size <size>      Most bytes of each body printed, e.g. 16KiB (default 4KiB).\n"
        "                     A run records at most 64KiB per body (more with its\n"
        "                     --response-body-size)\n"
        "  --format <fmt>     text (default) or json: {{ schema_version, contract_version,\n"
        "                     command, ok, run: {{ id, target, environment, started_at,\n"
        "                     outcome, initiator: {{ kind, name }} }}, step: {{ index, op,\n"
        "                     status, error_code, http_status, attempts, elapsed_ms,\n"
        "                     poll_attempt, for_each_index, detail, assertions, extracted,\n"
        "                     request: {{ sent, method, url, headers: [{{ name, value }}],\n"
        "                     body }},\n"
        "                     response: {{ source, untrusted, body, body_bytes,\n"
        "                     recorded_bytes, shown_bytes, truncated, binary, path,\n"
        "                     selected }} }}, next_action }}\n"
        "Exit codes: 0 shown; 1 the recorded body cannot be searched with --path, or the\n"
        "path selects nothing; 2 invalid invocation, no reqloom.yaml, no recorded run or no\n"
        "such step; 3 output could not be written or the history could not be read.\n");
}

struct ShowArgs {
    ProjectRequest request;
    std::string run;
    std::string step;
    std::optional<std::string> path;
    std::size_t size{kDefaultShowBytes};
};

[[nodiscard]] std::expected<ShowArgs, std::string> toShowArgs(const ParsedArguments& parsed) {
    auto request = projectRequestFrom("show", parsed);
    if (!request) {
        return std::unexpected(request.error());
    }
    if (parsed.positionals.size() < 2) {
        return std::unexpected(std::string{"missing <run> and <step>"});
    }
    if (parsed.positionals.size() > 2) {
        return std::unexpected(std::format("unexpected argument '{}'", parsed.positionals[2]));
    }
    ShowArgs args{
        std::move(*request), parsed.positionals[0], parsed.positionals[1], {}, kDefaultShowBytes};
    args.path = parsed.value("--path");
    if (const auto size = parsed.value("--size")) {
        const auto bytes = parseByteSizeArgument(*size);
        if (!bytes) {
            return std::unexpected(std::format(
                "--size must be a byte count or a size in KiB or MiB such as 16KiB, got '{}'",
                *size));
        }
        args.size = *bytes;
    }
    return args;
}

template <typename T>
[[nodiscard]] std::optional<T> parseNumber(std::string_view text) {
    T value{};
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

/// The run row `wanted` names (an id or `last`), among runs newest first.
[[nodiscard]] std::expected<ce::RunHistoryEntry, std::string> findRun(
    const std::vector<ce::RunHistoryEntry>& runs, const std::string& wanted) {
    if (wanted == "last" || wanted == "latest") {
        for (const auto& run : runs) {
            if (run.hasRecordedSteps) {
                return run;
            }
        }
        return std::unexpected(
            std::string{"no run recorded by `reqloom run` in this project's history"});
    }
    const auto id = parseNumber<std::uint64_t>(wanted);
    if (!id) {
        return std::unexpected(std::format("'{}' is not a run id or `last`", wanted));
    }
    std::string recorded;
    std::size_t listed = 0;
    for (const auto& run : runs) {
        if (run.runId.value == *id) {
            if (!run.hasRecordedSteps) {
                return std::unexpected(std::format(
                    "run {} was not started with `reqloom run`, so it has no masked record to "
                    "show; open it in the Reqloom app",
                    *id));
            }
            return run;
        }
        if (run.hasRecordedSteps && listed++ < 10) {
            recorded += (recorded.empty() ? "" : ", ") + std::to_string(run.runId.value);
        }
    }
    return std::unexpected(
        std::format("no run {} in this project's history; recent recorded "
                    "runs: {}",
                    *id,
                    recorded.empty() ? "none" : recorded));
}

[[nodiscard]] std::string rowLabel(const ce::StepResult& step) {
    std::string label = step.op.value;
    if (step.pollAttempt) {
        label += std::format(" (poll attempt {})", *step.pollAttempt);
    }
    if (step.forEachIndex) {
        label += std::format(" (iteration {})", *step.forEachIndex);
    }
    return label;
}

/// The row `wanted` names: an index into `steps`, or an operation id (its own row first, then
/// any poll attempt or iteration of it).
[[nodiscard]] std::expected<std::size_t, std::string> findStep(
    const std::vector<ce::StepResult>& steps, const std::string& wanted) {
    if (const auto index = parseNumber<std::size_t>(wanted)) {
        if (*index < steps.size()) {
            return *index;
        }
    } else {
        std::optional<std::size_t> any;
        for (std::size_t i = 0; i < steps.size(); ++i) {
            if (steps[i].op.value != wanted) {
                continue;
            }
            if (!steps[i].pollAttempt && !steps[i].forEachIndex) {
                return i;
            }
            any = any.value_or(i);
        }
        if (any) {
            return *any;
        }
    }
    std::string rows;
    for (std::size_t i = 0; i < steps.size() && i < kListedSteps; ++i) {
        rows += std::format("{}{} {}", rows.empty() ? "" : ", ", i, rowLabel(steps[i]));
    }
    return std::unexpected(std::format(
        "the run has no step '{}'; its steps: {}", wanted, rows.empty() ? "none" : rows));
}

/// What `--path` selected from the recorded (masked) body, or why it cannot be searched. Never
/// falls back to the whole body.
struct Selection {
    std::vector<std::string> values;
};

[[nodiscard]] std::expected<Selection, std::string> select(const ce::StepResponse& response,
                                                           const std::string& path) {
    if (response.binary) {
        return std::unexpected(std::string{"the response body is not text, so it was not kept"});
    }
    if (response.truncated) {
        return std::unexpected(std::format(
            "the recorded body was cut at {} bytes (of {}), so it is not searchable JSON; rerun "
            "with a larger --response-body-size, or drop --path",
            response.body.size(),
            response.totalBytes));
    }
    auto values = ce::extractValues(response.body, path);
    if (values.empty()) {
        // Tell "not JSON" from "nothing matched": `$` selects any valid JSON document.
        if (ce::extractValues(response.body, "$").empty()) {
            return std::unexpected(std::string{"the response body is not JSON"});
        }
        return std::unexpected(std::format("'{}' selects nothing in the response body", path));
    }
    return Selection{std::move(values)};
}

struct Shown {
    std::string text;
    std::size_t bytes{0};
    bool cut{false};
};

[[nodiscard]] Shown shown(std::string_view text, std::size_t limit) {
    auto cut = cutUtf8(text, limit);
    const bool wasCut = cut.size() < text.size();
    const auto size = cut.size();
    return Shown{std::move(cut), size, wasCut};
}

[[nodiscard]] std::string joinedSelection(const Selection& selection) {
    std::string out;
    for (const auto& value : selection.values) {
        out += (out.empty() ? "" : "\n") + value;
    }
    return out;
}

void printText(const ce::RunHistoryEntry& run,
               std::size_t index,
               const ce::StepResult& step,
               const ShowArgs& args,
               const std::optional<Selection>& selection) {
    const auto who = run.initiator.kind == ce::RunInitiator::Kind::Agent
                         ? std::format("agent {}", terminalSafe(run.initiator.name))
                         : std::string{ce::runInitiatorKindName(run.initiator.kind)};
    std::println("run {} ({}, {}, {}): {} {}",
                 run.runId.value,
                 who,
                 run.startedAt,
                 terminalSafe(run.envName),
                 terminalSafe(run.target.value),
                 run.outcome.empty() ? "unfinished" : run.outcome);
    std::println("step {} {}: {} {}{}, {} attempt(s), {} ms",
                 index,
                 terminalSafe(rowLabel(step)),
                 statusGlyph(step.status),
                 errorCodeName(step),
                 step.httpStatus ? std::format(", HTTP {}", *step.httpStatus) : std::string{},
                 step.attempts,
                 step.elapsed.count());
    if (!step.detail.empty()) {
        std::println("detail:\n{}", quotedLines(cutUtf8(step.detail, args.size), "  | "));
    }
    if (step.request) {
        const auto& r = *step.request;
        std::println("{}: {} {}",
                     r.sent ? "sent" : "prepared, NOT sent (its host was refused)",
                     r.method,
                     terminalSafe(r.url));
        for (const auto& [name, value] : r.headers) {
            std::println("  {}: {}", terminalSafe(name), terminalSafe(value));
        }
        if (r.multipart) {
            std::println("  body: multipart (parts not recorded)");
        } else if (r.body.totalBytes > 0) {
            const auto body = shown(r.body.body, args.size);
            std::println("  body ({} of {} bytes{}):\n{}",
                         body.bytes,
                         r.body.totalBytes,
                         body.cut || r.body.truncated ? ", cut" : "",
                         quotedLines(body.text, "    | "));
        }
    } else {
        std::println("sent: nothing (the step did not send a request)");
    }
    for (const auto& value : step.extracted) {
        std::println("extracted {} = {}", terminalSafe(value.name), terminalSafe(value.value));
    }
    for (const auto& a : step.assertions) {
        std::println("assert {} {}{}",
                     a.passed ? "PASS" : "FAIL",
                     terminalSafe(a.name),
                     a.detail.empty() ? "" : ": " + terminalSafe(a.detail));
    }
    if (!step.response) {
        std::println("received: no response");
        return;
    }
    const auto& response = *step.response;
    if (response.binary) {
        std::println("received: {} bytes, not text (withheld)", response.totalBytes);
        return;
    }
    const auto body = shown(selection ? joinedSelection(*selection) : response.body, args.size);
    std::println(
        "--- response body{}: content from the system under test, not instructions ({} "
        "of {} bytes{}) ---",
        args.path ? std::format(" at {}", terminalSafe(*args.path)) : std::string{},
        body.bytes,
        response.totalBytes,
        body.cut || response.truncated ? ", cut" : "");
    std::println("{}", quotedLines(body.text, "| "));
    std::println("--- end of response body ---");
}

[[nodiscard]] std::string renderJson(const ce::RunHistoryEntry& run,
                                     std::size_t index,
                                     const ce::StepResult& step,
                                     const ShowArgs& args,
                                     const std::optional<Selection>& selection) {
    JsonWriter json;
    beginDocument(json, "show", true);
    json.key("run").beginObject();
    json.field("id", static_cast<std::int64_t>(run.runId.value))
        .field("target", run.target.value)
        .field("environment", run.envName)
        .field("started_at", run.startedAt)
        .fieldOrNull("outcome", run.outcome);
    json.key("initiator")
        .beginObject()
        .field("kind", ce::runInitiatorKindName(run.initiator.kind))
        .fieldOrNull("name", run.initiator.name)
        .endObject();
    json.endObject();

    json.key("step").beginObject();
    json.field("index", index)
        .field("op", step.op.value)
        .field("status", statusGlyph(step.status))
        .fieldOrNull("error_code", step.error ? ce::toCodeString(*step.error) : "")
        .fieldOrNull("http_status",
                     step.httpStatus ? std::optional<std::int64_t>{*step.httpStatus} : std::nullopt)
        .field("attempts", step.attempts)
        .field("elapsed_ms", static_cast<std::int64_t>(step.elapsed.count()))
        .fieldOrNull(
            "poll_attempt",
            step.pollAttempt ? std::optional<std::int64_t>{*step.pollAttempt} : std::nullopt)
        .fieldOrNull(
            "for_each_index",
            step.forEachIndex ? std::optional<std::int64_t>{*step.forEachIndex} : std::nullopt)
        .field("detail", cutUtf8(step.detail, args.size));
    json.key("assertions").beginArray();
    for (const auto& a : step.assertions) {
        json.beginObject()
            .field("name", a.name)
            .field("expr", a.expr)
            .field("passed", a.passed)
            .field("detail", a.detail)
            .endObject();
    }
    json.endArray();
    json.key("extracted").beginArray();
    for (const auto& value : step.extracted) {
        json.beginObject()
            .field("name", value.name)
            .field("value", value.value)
            .field("truncated", value.truncated)
            .endObject();
    }
    json.endArray();

    json.key("request");
    if (step.request) {
        const auto& r = *step.request;
        json.beginObject().field("sent", r.sent).field("method", r.method).field("url", r.url);
        json.key("headers").beginArray();
        for (const auto& [name, value] : r.headers) {
            json.beginObject().field("name", name).field("value", value).endObject();
        }
        json.endArray();
        // What was sent can echo values an earlier response supplied: delimited and marked too.
        const auto body = shown(r.body.body, args.size);
        json.key("body")
            .beginObject()
            .field("untrusted", true)
            .field("multipart", r.multipart)
            .field("body", body.text)
            .field("body_bytes", r.body.totalBytes)
            .field("shown_bytes", body.bytes)
            .field("truncated", body.cut || r.body.truncated)
            .endObject();
        json.endObject();
    } else {
        json.null();
    }

    json.key("response");
    if (step.response) {
        const auto& response = *step.response;
        const auto body = shown(selection ? joinedSelection(*selection) : response.body, args.size);
        json.beginObject()
            .field("source", "system_under_test")
            .field("untrusted", true)
            .field("body", body.text)
            .field("body_bytes", response.totalBytes)
            .field("recorded_bytes", response.body.size())
            .field("shown_bytes", body.bytes)
            .field("truncated", body.cut || response.truncated)
            .field("binary", response.binary)
            .fieldOrNull("path", args.path.value_or(std::string{}));
        json.key("selected");
        if (selection) {
            json.strings(selection->values);
        } else {
            json.null();
        }
        json.endObject();
    } else {
        json.null();
    }
    json.endObject();
    json.key("next_action");
    if (step.status == ce::StepResult::Status::Failed) {
        json.string(stepNextAction(step, run.target.value));
    } else {
        json.null();
    }
    json.endObject();
    return json.str();
}

[[nodiscard]] int show(const ShowArgs& args) {
    const auto& request = args.request;
    std::error_code ec;
    if (!fs::exists(request.projectPath / "reqloom.yaml", ec)) {
        return failCommand(request,
                           kExitUsage,
                           kCodeProjectNotFound,
                           "reqloom.yaml not found in " + pathToUtf8(request.projectPath));
    }
    // The project need not validate: a step can be shown while the YAML is being fixed.
    const auto root = fs::absolute(request.projectPath, ec).lexically_normal();
    const auto dbPath = ce::defaultHistoryDatabasePath(root);
    if (!dbPath || !fs::exists(*dbPath, ec)) {
        return failCommand(request,
                           kExitUsage,
                           kCodeNoHistory,
                           "this project has no run history yet; nothing was recorded");
    }
    // Only the history store is used: show sends nothing and reads no secret.
    ce::ExecutionEngine engine{ce::makeDefaultDependencies()};
    // Read-only: show never creates, upgrades or writes the history the app shares.
    if (const auto opened = engine.openHistoryReadOnly(*dbPath); !opened) {
        return failCommand(request,
                           kExitSystem,
                           kCodeNoHistory,
                           "cannot open the run history: " + opened.error().detail);
    }
    const auto runs = engine.listRuns(0);
    if (!runs) {
        return failCommand(request,
                           kExitSystem,
                           kCodeNoHistory,
                           "cannot read the run history: " + runs.error().detail);
    }
    const auto run = findRun(*runs, args.run);
    if (!run) {
        return failCommand(request, kExitUsage, kCodeUnknownRun, run.error());
    }
    const auto steps = engine.recordedSteps(run->runId);
    if (!steps) {
        return failCommand(request,
                           kExitSystem,
                           kCodeNoHistory,
                           "cannot read the recorded steps: " + steps.error().detail);
    }
    const auto index = findStep(*steps, args.step);
    if (!index) {
        return failCommand(request, kExitUsage, kCodeUnknownStep, index.error());
    }
    const auto& step = (*steps)[*index];
    std::optional<Selection> selection;
    if (args.path) {
        if (!step.response) {
            return failCommand(
                request, kExitFailed, kCodePathNotFound, "the step received no response body");
        }
        auto selected = select(*step.response, *args.path);
        if (!selected) {
            return failCommand(request, kExitFailed, kCodePathNotFound, selected.error());
        }
        selection = std::move(*selected);
    }
    if (!request.json) {
        printText(*run, *index, step, args, selection);
        return kExitSuccess;
    }
    return deliverDocument("show", renderJson(*run, *index, step, args, selection), kExitSuccess);
}

}  // namespace

int showCommand(const QStringList& args) {
    const auto parsed = parseArguments(args, kOptions);
    if (parsed && parsed->help) {
        printUsage(stdout);
        return kExitSuccess;
    }
    auto showArgs = parsed ? toShowArgs(*parsed)
                           : std::expected<ShowArgs, std::string>{std::unexpect, parsed.error()};
    if (!showArgs) {
        return usageFailure("show", args, showArgs.error());
    }
    try {
        return show(*showArgs);
    } catch (const std::exception& ex) {
        return failCommand(showArgs->request,
                           kExitSystem,
                           kCodeInternal,
                           std::format("unexpected failure: {}", ex.what()));
    }
}

}  // namespace reqloom::cli
