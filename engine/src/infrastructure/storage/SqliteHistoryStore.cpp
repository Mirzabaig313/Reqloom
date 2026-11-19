// SqliteHistoryStore — SQLite-backed run-history persistence.
//
// Schema v1:
//   runs(run_id, target_op, env_name, started_at, ended_at, outcome, chain_size, elapsed_ms,
//        initiator, initiator_name)
//   run_events(id, run_id, seq, event_type, step_index, op_id, payload, at)
//   run_steps(run_id, row, payload): a CLI run's step rows, masked for agent-facing output
//        (`appendSteps`); `reqloom show` reads only these, never `run_events`.
//
// Events are stored as JSON in `payload`; `runs` denormalises what the history sidebar needs.
//
// WAL mode + 5s busy timeout: the engine appends from the run thread while the desktop reads on a
// separate connection. Instances are not thread-safe; the caller serialises.

#include "SqliteHistoryStore.h"

#include "../../domain/Codecs.h"

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/Events.h>

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace reqloom::engine {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

// Compile guard: a RunEvent variant without a serialization arm fails the build.
template <typename>
inline constexpr bool kAlwaysFalse = false;

constexpr int kBusyTimeoutMs = 5000;

[[nodiscard]] std::string isoUtc(TimePoint tp) {
    using namespace std::chrono;
    const auto t = system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        // strftime returns 0 if the result overflows the buffer; the fixed format always fits, so
        // this is defensive.
        return {};
    }
    return std::string{buf};
}

[[nodiscard]] TimePoint parseIsoUtc(std::string_view s) {
    std::tm tm{};
    std::stringstream ss{std::string{s}};
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (ss.fail()) {
        return {};
    }
#if defined(_WIN32)
    const auto t = _mkgmtime(&tm);
#else
    const auto t = timegm(&tm);
#endif
    return std::chrono::system_clock::from_time_t(t);
}

[[nodiscard]] std::string_view methodToWire(HttpMethod m) {
    return codecs::methodToString(m);
}

[[nodiscard]] HttpMethod methodFromWire(std::string_view s) noexcept {
    if (s == "POST") {
        return HttpMethod::Post;
    }
    if (s == "PUT") {
        return HttpMethod::Put;
    }
    if (s == "PATCH") {
        return HttpMethod::Patch;
    }
    if (s == "DELETE") {
        return HttpMethod::Delete;
    }
    if (s == "HEAD") {
        return HttpMethod::Head;
    }
    if (s == "OPTIONS") {
        return HttpMethod::Options;
    }
    return HttpMethod::Get;
}

[[nodiscard]] std::string_view skipReasonToWire(SkipReason r) noexcept {
    return r == SkipReason::SessionValid ? "SessionValid" : "ExtractionCached";
}

[[nodiscard]] SkipReason skipReasonFromWire(std::string_view s) noexcept {
    return s == "SessionValid" ? SkipReason::SessionValid : SkipReason::ExtractionCached;
}

[[nodiscard]] std::string outcomeToWire(RunOutcome o) {
    switch (o) {
        case RunOutcome::Succeeded:
            return "Succeeded";
        case RunOutcome::Failed:
            return "Failed";
        case RunOutcome::Cancelled:
            return "Cancelled";
    }
    return "Failed";
}

[[nodiscard]] RunOutcome outcomeFromWire(std::string_view s) noexcept {
    if (s == "Succeeded") {
        return RunOutcome::Succeeded;
    }
    if (s == "Cancelled") {
        return RunOutcome::Cancelled;
    }
    return RunOutcome::Failed;
}

template <typename E, std::size_t N>
using EnumWireMap = std::array<std::pair<E, std::string_view>, N>;

template <typename E, std::size_t N>
[[nodiscard]] std::string_view enumToWire(E value,
                                          const EnumWireMap<E, N>& values,
                                          std::string_view fallback) noexcept {
    for (const auto& [candidate, wire] : values) {
        if (candidate == value) {
            return wire;
        }
    }
    return fallback;
}

template <typename E, std::size_t N>
[[nodiscard]] E enumFromWire(std::string_view wire,
                             const EnumWireMap<E, N>& values,
                             E fallback) noexcept {
    for (const auto& [candidate, candidateWire] : values) {
        if (candidateWire == wire) {
            return candidate;
        }
    }
    return fallback;
}

constexpr EnumWireMap<VariableUseKind, 9> kVariableUseKinds{{
    {VariableUseKind::Unknown, "Unknown"},
    {VariableUseKind::UrlPath, "UrlPath"},
    {VariableUseKind::RawQuery, "RawQuery"},
    {VariableUseKind::Fragment, "Fragment"},
    {VariableUseKind::NamedQuery, "NamedQuery"},
    {VariableUseKind::Header, "Header"},
    {VariableUseKind::Auth, "Auth"},
    {VariableUseKind::Body, "Body"},
    {VariableUseKind::FormField, "FormField"},
}};

constexpr EnumWireMap<VariableSourceKind, 6> kVariableSourceKinds{{
    {VariableSourceKind::Unknown, "Unknown"},
    {VariableSourceKind::Environment, "Environment"},
    {VariableSourceKind::Secret, "Secret"},
    {VariableSourceKind::Actor, "Actor"},
    {VariableSourceKind::Resource, "Resource"},
    {VariableSourceKind::Extraction, "Extraction"},
}};

constexpr EnumWireMap<UnresolvedVariableCause, 9> kUnresolvedVariableCauses{{
    {UnresolvedVariableCause::Unavailable, "Unavailable"},
    {UnresolvedVariableCause::EnvironmentValueMissing, "EnvironmentValueMissing"},
    {UnresolvedVariableCause::SecretValueMissing, "SecretValueMissing"},
    {UnresolvedVariableCause::ActorSessionFieldMissing, "ActorSessionFieldMissing"},
    {UnresolvedVariableCause::ResourceValueMissing, "ResourceValueMissing"},
    {UnresolvedVariableCause::ExtractionMissing, "ExtractionMissing"},
    {UnresolvedVariableCause::ExtractionNull, "ExtractionNull"},
    {UnresolvedVariableCause::ExtractionInvalid, "ExtractionInvalid"},
    {UnresolvedVariableCause::ExtractionUnsupported, "ExtractionUnsupported"},
}};

[[nodiscard]] json diagnosticsToJson(const std::vector<UnresolvedVariableDiagnostic>& diagnostics) {
    json out = json::array();
    for (const auto& diagnostic : diagnostics) {
        json item{
            {"token", diagnostic.token},
            {"useKind", std::string{enumToWire(diagnostic.useKind, kVariableUseKinds, "Unknown")}},
            {"useName", diagnostic.useName},
            {"cause",
             std::string{enumToWire(diagnostic.cause, kUnresolvedVariableCauses, "Unavailable")}},
            {"sourceKind",
             std::string{enumToWire(diagnostic.sourceKind, kVariableSourceKinds, "Unknown")}},
            {"sourceId", diagnostic.sourceId},
            {"sourceField", diagnostic.sourceField}};
        if (diagnostic.producerOp) {
            item["producerOp"] = diagnostic.producerOp->value;
        }
        if (diagnostic.producerStepIndex) {
            item["producerStepIndex"] = *diagnostic.producerStepIndex;
        }
        out.push_back(std::move(item));
    }
    return out;
}

[[nodiscard]] std::vector<UnresolvedVariableDiagnostic> diagnosticsFromJson(const json& j) {
    std::vector<UnresolvedVariableDiagnostic> out;
    if (!j.is_array()) {
        return out;
    }
    out.reserve(j.size());
    for (const auto& item : j) {
        if (!item.is_object()) {
            continue;
        }
        UnresolvedVariableDiagnostic diagnostic{};
        diagnostic.token = item.value("token", std::string{});
        diagnostic.useKind = enumFromWire(
            item.value("useKind", std::string{}), kVariableUseKinds, VariableUseKind::Unknown);
        diagnostic.useName = item.value("useName", std::string{});
        diagnostic.cause = enumFromWire(item.value("cause", std::string{}),
                                        kUnresolvedVariableCauses,
                                        UnresolvedVariableCause::Unavailable);
        diagnostic.sourceKind = enumFromWire(item.value("sourceKind", std::string{}),
                                             kVariableSourceKinds,
                                             VariableSourceKind::Unknown);
        diagnostic.sourceId = item.value("sourceId", std::string{});
        diagnostic.sourceField = item.value("sourceField", std::string{});
        if (item.contains("producerOp") && item["producerOp"].is_string()) {
            diagnostic.producerOp = OperationId{item["producerOp"].get<std::string>()};
        }
        if (item.contains("producerStepIndex") && item["producerStepIndex"].is_number_unsigned()) {
            const auto raw = item["producerStepIndex"].get<std::uint64_t>();
            if (raw <= std::numeric_limits<std::size_t>::max()) {
                diagnostic.producerStepIndex = static_cast<std::size_t>(raw);
            }
        }
        out.push_back(std::move(diagnostic));
    }
    return out;
}

[[nodiscard]] json headersToJson(const std::vector<std::pair<std::string, std::string>>& hs) {
    json out = json::array();
    for (const auto& [k, v] : hs) {
        out.push_back({{"k", k}, {"v", v}});
    }
    return out;
}

[[nodiscard]] std::vector<std::pair<std::string, std::string>> headersFromJson(const json& j) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!j.is_array()) {
        return out;
    }
    for (const auto& item : j) {
        out.emplace_back(item.value("k", std::string{}), item.value("v", std::string{}));
    }
    return out;
}

[[nodiscard]] std::string_view stepStatusToWire(StepResult::Status status) noexcept {
    switch (status) {
        case StepResult::Status::Pending:
            return "Pending";
        case StepResult::Status::Ready:
            return "Ready";
        case StepResult::Status::Skipped:
            return "Skipped";
        case StepResult::Status::Succeeded:
            return "Succeeded";
        case StepResult::Status::Failed:
            return "Failed";
        case StepResult::Status::Cancelled:
            return "Cancelled";
        case StepResult::Status::Blocked:
            return "Blocked";
    }
    return "Pending";
}

[[nodiscard]] StepResult::Status stepStatusFromWire(std::string_view s) noexcept {
    using S = StepResult::Status;
    for (const auto status :
         {S::Pending, S::Ready, S::Skipped, S::Succeeded, S::Failed, S::Cancelled, S::Blocked}) {
        if (stepStatusToWire(status) == s) {
            return status;
        }
    }
    return S::Pending;
}

[[nodiscard]] json bodyToJson(const StepResponse& r) {
    return {{"body", r.body},
            {"totalBytes", r.totalBytes},
            {"limitBytes", r.limitBytes},
            {"truncated", r.truncated},
            {"binary", r.binary}};
}

[[nodiscard]] StepResponse bodyFromJson(const json& j) {
    StepResponse r;
    r.body = j.value("body", std::string{});
    r.totalBytes = j.value("totalBytes", std::size_t{0});
    r.limitBytes = j.value("limitBytes", std::size_t{0});
    r.truncated = j.value("truncated", false);
    r.binary = j.value("binary", false);
    return r;
}

template <typename T>
[[nodiscard]] json optionalToJson(const std::optional<T>& value) {
    return value ? json(*value) : json(nullptr);
}

[[nodiscard]] json stepToJson(const StepResult& step) {
    json assertions = json::array();
    for (const auto& a : step.assertions) {
        assertions.push_back(
            {{"name", a.name}, {"expr", a.expr}, {"passed", a.passed}, {"detail", a.detail}});
    }
    json extracted = json::array();
    for (const auto& e : step.extracted) {
        extracted.push_back({{"name", e.name}, {"value", e.value}, {"truncated", e.truncated}});
    }
    json out = {
        {"op", step.op.value},
        {"status", std::string{stepStatusToWire(step.status)}},
        {"error", step.error ? json(std::string{toCodeString(*step.error)}) : json(nullptr)},
        {"attempts", step.attempts},
        {"elapsedMs", step.elapsed.count()},
        {"detail", step.detail},
        {"pollAttempt", optionalToJson(step.pollAttempt)},
        {"forEachIndex", optionalToJson(step.forEachIndex)},
        {"assertions", std::move(assertions)},
        {"httpStatus", optionalToJson(step.httpStatus)},
        {"response", step.response ? bodyToJson(*step.response) : json(nullptr)},
        {"extracted", std::move(extracted)},
        {"request", nullptr}};
    if (step.request) {
        out["request"] = {{"method", step.request->method},
                          {"url", step.request->url},
                          {"headers", headersToJson(step.request->headers)},
                          {"body", bodyToJson(step.request->body)},
                          {"multipart", step.request->multipart},
                          {"sent", step.request->sent}};
    }
    return out;
}

template <typename T>
[[nodiscard]] std::optional<T> optionalFromJson(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return std::nullopt;
    }
    return it->get<T>();
}

[[nodiscard]] StepResult stepFromJson(const json& j) {
    StepResult step;
    step.op = OperationId{j.value("op", std::string{})};
    step.status = stepStatusFromWire(j.value("status", std::string{}));
    if (const auto code = optionalFromJson<std::string>(j, "error")) {
        step.error = fromCodeString(*code).value_or(ErrorCode::Internal);
    }
    step.attempts = j.value("attempts", 0);
    step.elapsed = std::chrono::milliseconds{j.value("elapsedMs", std::int64_t{0})};
    step.detail = j.value("detail", std::string{});
    step.pollAttempt = optionalFromJson<int>(j, "pollAttempt");
    step.forEachIndex = optionalFromJson<int>(j, "forEachIndex");
    for (const auto& a : j.value("assertions", json::array())) {
        step.assertions.push_back(AssertionResult{a.value("name", std::string{}),
                                                  a.value("expr", std::string{}),
                                                  a.value("passed", false),
                                                  a.value("detail", std::string{})});
    }
    step.httpStatus = optionalFromJson<int>(j, "httpStatus");
    if (const auto it = j.find("response"); it != j.end() && it->is_object()) {
        step.response = bodyFromJson(*it);
    }
    for (const auto& e : j.value("extracted", json::array())) {
        step.extracted.push_back(ExtractedValue{e.value("name", std::string{}),
                                                e.value("value", std::string{}),
                                                e.value("truncated", false)});
    }
    if (const auto it = j.find("request"); it != j.end() && it->is_object()) {
        StepRequest request;
        request.method = it->value("method", std::string{});
        request.url = it->value("url", std::string{});
        request.headers = headersFromJson(it->value("headers", json::array()));
        if (const auto body = it->find("body"); body != it->end() && body->is_object()) {
            request.body = bodyFromJson(*body);
        }
        request.multipart = it->value("multipart", false);
        request.sent = it->value("sent", true);
        step.request = std::move(request);
    }
    return step;
}

struct StmtDeleter {
    void operator()(sqlite3_stmt* s) const noexcept {
        if (s != nullptr) {
            sqlite3_finalize(s);
        }
    }
};
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

struct DbDeleter {
    void operator()(sqlite3* db) const noexcept {
        if (db != nullptr) {
            sqlite3_close(db);
        }
    }
};
using DbPtr = std::unique_ptr<sqlite3, DbDeleter>;

// Scopes a write to one transaction: BEGIN IMMEDIATE takes the write lock up front so a busy
// database fails at begin(); rolls back unless commit() succeeded. All SQL here is trusted and
// fixed.
class SqliteTransaction {
public:
    explicit SqliteTransaction(sqlite3* db) noexcept
        : db_(db),
          active_(sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK) {}
    ~SqliteTransaction() {
        if (active_) {
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }
    SqliteTransaction(const SqliteTransaction&) = delete;
    SqliteTransaction& operator=(const SqliteTransaction&) = delete;
    SqliteTransaction(SqliteTransaction&&) = delete;
    SqliteTransaction& operator=(SqliteTransaction&&) = delete;

    [[nodiscard]] bool began() const noexcept { return active_; }

    [[nodiscard]] bool commit() noexcept {
        if (!active_) {
            return false;
        }
        const bool ok = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK;
        active_ = false;  // committed (or failed) — destructor must not roll back
        return ok;
    }

private:
    sqlite3* db_;
    bool active_{false};
};

[[nodiscard]] ReqloomError sqliteError(sqlite3* db, std::string_view what) {
    const char* msg = (db != nullptr) ? sqlite3_errmsg(db) : "no database";
    return ReqloomError{ErrorCode::SchemaInvalid,
                        ErrorClass::Schema,
                        std::string{what} + ": " + (msg != nullptr ? msg : "?")};
}

// runId / stepIndex / event_type / op_id / at get their own columns for indexing; the rest is
// `payload`.

struct EventEnvelope {
    std::uint64_t runId{0};
    std::optional<std::size_t> stepIndex;
    std::optional<std::string> opId;
    std::string at;
    std::string eventType;
    json payload;
};

// Only a non-default kind is stored, so rows from builds before the field read the same.
void putExchange(json& payload, ExchangeKind kind) {
    if (kind != ExchangeKind::Operation) {
        payload["exchange"] = std::string{exchangeKindName(kind)};
    }
}

[[nodiscard]] ExchangeKind exchangeOf(const json& p) {
    const auto it = p.find("exchange");
    if (it == p.end() || !it->is_string()) {
        return ExchangeKind::Operation;
    }
    return exchangeKindFromName(it->get<std::string>()).value_or(ExchangeKind::Operation);
}

[[nodiscard]] EventEnvelope envelopeOf(const RunEvent& ev) {
    EventEnvelope e;
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            e.runId = v.runId.value;
            e.at = isoUtc(v.at);

            if constexpr (std::is_same_v<T, RunStarted>) {
                e.eventType = "RunStarted";
                e.opId = v.target.value;
                e.payload = {{"target", v.target.value},
                             {"chainSize", v.chainSize},
                             {"envName", v.envName},
                             {"initiator", std::string{runInitiatorKindName(v.initiator.kind)}},
                             {"initiatorName", v.initiator.name}};
            } else if constexpr (std::is_same_v<T, StepStarted>) {
                e.eventType = "StepStarted";
                e.stepIndex = v.stepIndex;
                e.opId = v.op.value;
                e.payload = {{"op", v.op.value}, {"attempt", v.attempt}};
            } else if constexpr (std::is_same_v<T, StepSkipped>) {
                e.eventType = "StepSkipped";
                e.stepIndex = v.stepIndex;
                e.opId = v.op.value;
                e.payload = {{"op", v.op.value},
                             {"reason", std::string{skipReasonToWire(v.reason)}}};
            } else if constexpr (std::is_same_v<T, RequestPrepared>) {
                e.eventType = "RequestPrepared";
                e.stepIndex = v.stepIndex;
                e.payload = {{"method", std::string{methodToWire(v.method)}},
                             {"url", v.url},
                             {"headers", headersToJson(v.maskedHeaders)},
                             {"bodySize", v.bodySize}};
                putExchange(e.payload, v.exchange);
            } else if constexpr (std::is_same_v<T, ResponseReceived>) {
                e.eventType = "ResponseReceived";
                e.stepIndex = v.stepIndex;
                e.payload = {{"status", v.status},
                             {"headers", headersToJson(v.headers)},
                             {"bodySize", v.bodySize},
                             {"elapsedMs", v.elapsed.count()}};
                putExchange(e.payload, v.exchange);
                // `body` exists only when the run captured bodies. Stored base64 (`bodyEnc` marker)
                // because bodies are arbitrary bytes and nlohmann's strict dump() throws on
                // non-UTF-8.
                if (v.body) {
                    e.payload["body"] = codecs::base64Encode(*v.body);
                    e.payload["bodyEnc"] = "base64";
                }
            } else if constexpr (std::is_same_v<T, ExtractionApplied>) {
                e.eventType = "ExtractionApplied";
                e.stepIndex = v.stepIndex;
                e.payload = {{"resource", v.resource.value}, {"variableNames", v.variableNames}};
            } else if constexpr (std::is_same_v<T, ExtractionCompleted>) {
                e.eventType = "ExtractionCompleted";
                e.stepIndex = v.stepIndex;
                e.opId = v.op.value;
                std::string outcome;
                switch (v.outcome) {
                    case ExtractionCompleted::Outcome::Resolved:
                        outcome = "Resolved";
                        break;
                    case ExtractionCompleted::Outcome::Null:
                        outcome = "Null";
                        break;
                    case ExtractionCompleted::Outcome::Missing:
                        outcome = "Missing";
                        break;
                    case ExtractionCompleted::Outcome::InvalidPattern:
                        outcome = "InvalidPattern";
                        break;
                    case ExtractionCompleted::Outcome::Unsupported:
                        outcome = "Unsupported";
                        break;
                }
                e.payload = {{"op", v.op.value},
                             {"variableName", v.variableName},
                             {"sourcePath", v.sourcePath},
                             {"outcome", outcome},
                             {"value", v.value}};
            } else if constexpr (std::is_same_v<T, AssertionCompleted>) {
                e.eventType = "AssertionCompleted";
                e.stepIndex = v.stepIndex;
                e.opId = v.op.value;
                e.payload = {
                    {"op", v.op.value}, {"name", v.name}, {"expr", v.expr}, {"passed", v.passed}};
                if (!v.detail.empty()) {
                    e.payload["detail"] = v.detail;
                }
            } else if constexpr (std::is_same_v<T, StepFailed>) {
                e.eventType = "StepFailed";
                e.stepIndex = v.stepIndex;
                e.opId = v.op.value;
                e.payload = {{"op", v.op.value},
                             {"code", std::string{toCodeString(v.code)}},
                             {"attempt", v.attempt},
                             {"detail", v.detail},
                             {"diagnostics", diagnosticsToJson(v.diagnostics)}};
            } else if constexpr (std::is_same_v<T, StepBlocked>) {
                e.eventType = "StepBlocked";
                e.stepIndex = v.stepIndex;
                e.opId = v.op.value;
                e.payload = {{"op", v.op.value}, {"blockedByStepIndex", v.blockedByStepIndex}};
            } else if constexpr (std::is_same_v<T, StepCancelled>) {
                e.eventType = "StepCancelled";
                e.stepIndex = v.stepIndex;
                e.opId = v.op.value;
                e.payload = {{"op", v.op.value}};
            } else if constexpr (std::is_same_v<T, SessionRefreshed>) {
                e.eventType = "SessionRefreshed";
                e.opId = v.actor.value;
                std::string trigger =
                    (v.trigger == SessionRefreshed::Trigger::Expiry) ? "Expiry" : "Unauthorized";
                e.payload = {{"actor", v.actor.value}, {"trigger", trigger}};
            } else if constexpr (std::is_same_v<T, RunEnded>) {
                e.eventType = "RunEnded";
                e.payload = {{"outcome", outcomeToWire(v.outcome)},
                             {"elapsedMs", v.elapsed.count()}};
            } else {
                static_assert(kAlwaysFalse<T>, "unhandled RunEvent variant in envelopeOf");
            }
        },
        ev);
    return e;
}

[[nodiscard]] std::optional<RunEvent> eventFromRow(const std::string& eventType,
                                                   std::uint64_t runIdRaw,
                                                   std::optional<std::size_t> stepIndex,
                                                   const std::string& atIso,
                                                   const json& p) {
    const RunId runId{runIdRaw};
    const auto at = parseIsoUtc(atIso);

    if (eventType == "RunStarted") {
        RunStarted ev;
        ev.runId = runId;
        ev.target = OperationId{p.value("target", std::string{})};
        ev.chainSize = p.value("chainSize", std::size_t{0});
        ev.envName = p.value("envName", std::string{});
        ev.initiator.kind = runInitiatorKindFromName(p.value("initiator", std::string{}));
        ev.initiator.name = p.value("initiatorName", std::string{});
        ev.at = at;
        return ev;
    }
    if (eventType == "StepStarted") {
        StepStarted ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.op = OperationId{p.value("op", std::string{})};
        ev.attempt = p.value("attempt", 1);
        ev.at = at;
        return ev;
    }
    if (eventType == "StepSkipped") {
        StepSkipped ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.op = OperationId{p.value("op", std::string{})};
        ev.reason = skipReasonFromWire(p.value("reason", std::string{}));
        ev.at = at;
        return ev;
    }
    if (eventType == "RequestPrepared") {
        RequestPrepared ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.method = methodFromWire(p.value("method", std::string{"GET"}));
        ev.url = p.value("url", std::string{});
        ev.maskedHeaders = headersFromJson(p.value("headers", json::array()));
        ev.bodySize = p.value("bodySize", std::size_t{0});
        ev.exchange = exchangeOf(p);
        ev.at = at;
        return ev;
    }
    if (eventType == "ResponseReceived") {
        ResponseReceived ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.status = p.value("status", 0);
        ev.headers = headersFromJson(p.value("headers", json::array()));
        ev.bodySize = p.value("bodySize", std::size_t{0});
        ev.elapsed = std::chrono::milliseconds{p.value("elapsedMs", std::int64_t{0})};
        ev.exchange = exchangeOf(p);
        // Restored only for capture-era rows; a missing "body" leaves the optional empty, matching
        // the live event. A missing or unknown `bodyEnc` is read as a raw string; malformed base64
        // is dropped, not fatal.
        if (p.contains("body") && p["body"].is_string()) {
            const auto raw = p["body"].get<std::string>();
            if (p.value("bodyEnc", std::string{}) == "base64") {
                if (auto decoded = codecs::base64Decode(raw); decoded) {
                    ev.body = std::move(*decoded);
                }
            } else {
                ev.body = raw;
            }
        }
        ev.at = at;
        return ev;
    }
    if (eventType == "ExtractionApplied") {
        ExtractionApplied ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.resource = ResourceId{p.value("resource", std::string{})};
        if (p.contains("variableNames") && p["variableNames"].is_array()) {
            for (const auto& n : p["variableNames"]) {
                ev.variableNames.push_back(n.get<std::string>());
            }
        }
        ev.at = at;
        return ev;
    }
    if (eventType == "ExtractionCompleted") {
        ExtractionCompleted ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.op = OperationId{p.value("op", std::string{})};
        ev.variableName = p.value("variableName", std::string{});
        ev.sourcePath = p.value("sourcePath", std::string{});
        const auto o = p.value("outcome", std::string{"Missing"});
        if (o == "Resolved") {
            ev.outcome = ExtractionCompleted::Outcome::Resolved;
        } else if (o == "Null") {
            ev.outcome = ExtractionCompleted::Outcome::Null;
        } else if (o == "InvalidPattern") {
            ev.outcome = ExtractionCompleted::Outcome::InvalidPattern;
        } else if (o == "Unsupported") {
            ev.outcome = ExtractionCompleted::Outcome::Unsupported;
        } else {
            ev.outcome = ExtractionCompleted::Outcome::Missing;
        }
        ev.value = p.value("value", std::string{});
        ev.at = at;
        return ev;
    }
    if (eventType == "AssertionCompleted") {
        AssertionCompleted ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.op = OperationId{p.value("op", std::string{})};
        ev.name = p.value("name", std::string{});
        ev.expr = p.value("expr", std::string{});
        ev.passed = p.value("passed", false);
        ev.detail = p.value("detail", std::string{});
        ev.at = at;
        return ev;
    }
    if (eventType == "StepFailed") {
        // Unknown codes (from a newer build) fall back to SchemaInvalid so the row still shows up.
        StepFailed ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.op = OperationId{p.value("op", std::string{})};
        ev.attempt = p.value("attempt", 1);
        ev.detail = p.value("detail", std::string{});
        const auto codeStr = p.value("code", std::string{"E_SCHEMA_INVALID"});
        ev.code = fromCodeString(codeStr).value_or(ErrorCode::SchemaInvalid);
        ev.cls = classify(ev.code);
        ev.diagnostics = diagnosticsFromJson(p.value("diagnostics", json::array()));
        ev.at = at;
        return ev;
    }
    if (eventType == "StepBlocked") {
        if (!stepIndex || !p.contains("blockedByStepIndex") ||
            !p["blockedByStepIndex"].is_number_unsigned()) {
            return std::nullopt;
        }
        const auto blockedBy = p["blockedByStepIndex"].get<std::uint64_t>();
        if (blockedBy > std::numeric_limits<std::size_t>::max() || blockedBy >= *stepIndex) {
            return std::nullopt;
        }
        StepBlocked ev{};
        ev.runId = runId;
        ev.stepIndex = *stepIndex;
        ev.op = OperationId{p.value("op", std::string{})};
        ev.blockedByStepIndex = static_cast<std::size_t>(blockedBy);
        ev.at = at;
        return ev;
    }
    if (eventType == "StepCancelled") {
        StepCancelled ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.op = OperationId{p.value("op", std::string{})};
        ev.at = at;
        return ev;
    }
    if (eventType == "SessionRefreshed") {
        SessionRefreshed ev;
        ev.runId = runId;
        ev.actor = ActorId{p.value("actor", std::string{})};
        ev.trigger = (p.value("trigger", std::string{}) == "Expiry")
                         ? SessionRefreshed::Trigger::Expiry
                         : SessionRefreshed::Trigger::Unauthorized;
        ev.at = at;
        return ev;
    }
    if (eventType == "RunEnded") {
        RunEnded ev;
        ev.runId = runId;
        ev.outcome = outcomeFromWire(p.value("outcome", std::string{"Failed"}));
        ev.elapsed = std::chrono::milliseconds{p.value("elapsedMs", std::int64_t{0})};
        ev.at = at;
        return ev;
    }
    return std::nullopt;
}

}  // namespace

struct SqliteHistoryStore::Impl {
    // The engine worker appends while the GUI thread lists, reads and clears; one lock per public
    // call serialises the connection and its shared prepared statements.
    mutable std::mutex mutex;
    DbPtr db;

    StmtPtr insertEventStmt;
    StmtPtr insertRunStmt;
    StmtPtr updateRunStartedStmt;
    StmtPtr updateRunEndedStmt;
    StmtPtr nextSeqStmt;

    // Runs whose RunStarted was refused (id already in use); their later events are refused
    // too, so they cannot append onto the other run's timeline.
    std::set<std::uint64_t> rejectedRuns;
    /// Runs whose RunStarted this connection stored: the only ones `appendSteps` writes rows for,
    /// so rows never land under another process's run.
    std::set<std::uint64_t> startedRuns;

    [[nodiscard]] std::expected<void, ReqloomError> exec(std::string_view sql) const {
        char* err = nullptr;
        const int rc = sqlite3_exec(db.get(), std::string{sql}.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            ReqloomError e{ErrorCode::SchemaInvalid,
                           ErrorClass::Schema,
                           std::string{"history: exec failed: "} + (err != nullptr ? err : "?")};
            sqlite3_free(err);
            return std::unexpected(std::move(e));
        }
        return {};
    }

    [[nodiscard]] std::expected<StmtPtr, ReqloomError> prepare(std::string_view sql) const {
        sqlite3_stmt* raw = nullptr;
        const int rc =
            sqlite3_prepare_v2(db.get(), sql.data(), static_cast<int>(sql.size()), &raw, nullptr);
        if (rc != SQLITE_OK) {
            return std::unexpected(sqliteError(db.get(), "history: prepare"));
        }
        return StmtPtr{raw};
    }

    /// Names one query returns in its first column (table names, column names).
    [[nodiscard]] std::expected<std::set<std::string>, ReqloomError> names(std::string_view sql,
                                                                           int column = 0) const {
        auto stmt = prepare(sql);
        if (!stmt) {
            return std::unexpected(stmt.error());
        }
        std::set<std::string> out;
        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(stmt->get())) == SQLITE_ROW) {
            const auto* text = sqlite3_column_text(stmt->get(), column);
            out.emplace(text != nullptr ? reinterpret_cast<const char*>(text) : "");
        }
        if (rc != SQLITE_DONE) {
            return std::unexpected(sqliteError(db.get(), "history: read schema"));
        }
        return out;
    }

    static constexpr std::array<std::string_view, 5> kTables{
        "schema_version", "runs", "run_events", "run_id_allocator", "run_steps"};
    static constexpr std::array<std::string_view, 2> kIndexes{"idx_run_events_run_seq",
                                                              "idx_runs_started_at"};
    /// Columns added to `runs` after v1 was first shipped, with their declarations.
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kAddedColumns{{
        {"elapsed_ms", "INTEGER"},
        {"initiator", "TEXT"},
        {"initiator_name", "TEXT"},
    }};

    /// The `runs` columns still missing, or every table, index and column when one is missing.
    [[nodiscard]] std::expected<bool, ReqloomError> schemaComplete() const {
        const auto present = names("SELECT name FROM sqlite_master;");
        if (!present) {
            return std::unexpected(present.error());
        }
        for (const auto name : kTables) {
            if (!present->contains(std::string{name})) {
                return false;
            }
        }
        for (const auto name : kIndexes) {
            if (!present->contains(std::string{name})) {
                return false;
            }
        }
        const auto columns = names("PRAGMA table_info(runs);", 1);
        if (!columns) {
            return std::unexpected(columns.error());
        }
        for (const auto& [column, _] : kAddedColumns) {
            if (!columns->contains(std::string{column})) {
                return false;
            }
        }
        const auto versions = names("SELECT version FROM schema_version;");
        return versions && versions->contains("1");
    }

    [[nodiscard]] std::expected<void, ReqloomError> ensureSchema() const {
        if (auto r = exec("PRAGMA journal_mode = WAL;"); !r) {
            return r;
        }
        if (auto r = exec("PRAGMA foreign_keys = ON;"); !r) {
            return r;
        }
        if (auto r = exec("PRAGMA synchronous = NORMAL;"); !r) {
            return r;
        }
        // An up-to-date database is only read, so opening it never waits for another process's
        // write (the desktop app and the CLI share the file).
        if (const auto complete = schemaComplete(); complete && *complete) {
            return {};
        }
        const auto upgradeError = [this](std::string_view what) {
            auto error = sqliteError(db.get(), "history: cannot upgrade the run history");
            error.detail += std::string{" ("} + std::string{what} + ")";
            return std::unexpected(std::move(error));
        };
        // Create, migrate and stamp in one write transaction: a second process waits for the
        // first, then finds the work done; any failure is an error, never ignored.
        SqliteTransaction txn{db.get()};
        if (!txn.began()) {
            return upgradeError("begin");
        }
        const char* createSchema = R"SQL(
            CREATE TABLE IF NOT EXISTS schema_version (
                version INTEGER PRIMARY KEY
            );
            CREATE TABLE IF NOT EXISTS runs (
                run_id      INTEGER PRIMARY KEY,
                target_op   TEXT,
                env_name    TEXT,
                started_at  TEXT,
                ended_at    TEXT,
                outcome     TEXT,
                chain_size  INTEGER,
                elapsed_ms  INTEGER
            );
            CREATE TABLE IF NOT EXISTS run_events (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                run_id      INTEGER NOT NULL,
                seq         INTEGER NOT NULL,
                event_type  TEXT NOT NULL,
                step_index  INTEGER,
                op_id       TEXT,
                payload     TEXT NOT NULL,
                at          TEXT NOT NULL,
                FOREIGN KEY (run_id) REFERENCES runs(run_id) ON DELETE CASCADE
            );
            CREATE INDEX IF NOT EXISTS idx_run_events_run_seq
                ON run_events(run_id, seq);
            CREATE INDEX IF NOT EXISTS idx_runs_started_at
                ON runs(started_at DESC);
            CREATE TABLE IF NOT EXISTS run_id_allocator (
                id      INTEGER PRIMARY KEY CHECK (id = 1),
                last_id INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS run_steps (
                run_id  INTEGER NOT NULL,
                row     INTEGER NOT NULL,
                payload TEXT NOT NULL,
                PRIMARY KEY (run_id, row),
                FOREIGN KEY (run_id) REFERENCES runs(run_id) ON DELETE CASCADE
            );
        )SQL";
        if (auto r = exec(createSchema); !r) {
            return upgradeError("create tables");
        }
        const auto columns = names("PRAGMA table_info(runs);", 1);
        if (!columns) {
            return std::unexpected(columns.error());
        }
        for (const auto& [column, type] : kAddedColumns) {
            if (columns->contains(std::string{column})) {
                continue;
            }
            const auto alter = std::format("ALTER TABLE runs ADD COLUMN {} {};", column, type);
            if (auto r = exec(alter); !r) {
                return upgradeError("add column " + std::string{column});
            }
        }
        // v1 is the only version; future migrations check this row and bump it.
        if (auto r = exec("INSERT OR IGNORE INTO schema_version(version) VALUES (1);"); !r) {
            return upgradeError("stamp version");
        }
        if (!txn.commit()) {
            return upgradeError("commit");
        }
        return {};
    }

    [[nodiscard]] std::expected<void, ReqloomError> prepareStatements() {
        auto p1 = prepare(
            "INSERT INTO run_events"
            "  (run_id, seq, event_type, step_index, op_id, payload, at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?);");
        if (!p1) {
            return std::unexpected(p1.error());
        }
        insertEventStmt = std::move(*p1);

        auto p2 = prepare(
            "INSERT OR IGNORE INTO runs"
            "  (run_id, target_op, env_name, started_at, chain_size, initiator, initiator_name) "
            "VALUES (?, ?, ?, ?, ?, ?, ?);");
        if (!p2) {
            return std::unexpected(p2.error());
        }
        insertRunStmt = std::move(*p2);

        // `started_at IS NULL` limits this to a placeholder row: a started run is never overwritten
        // by another run's RunStarted with the same id.
        auto p3 = prepare(
            "UPDATE runs SET target_op = ?, env_name = ?, started_at = ?, chain_size = ?, "
            "initiator = ?, initiator_name = ? WHERE run_id = ? AND started_at IS NULL;");
        if (!p3) {
            return std::unexpected(p3.error());
        }
        updateRunStartedStmt = std::move(*p3);

        auto p4 =
            prepare("UPDATE runs SET ended_at = ?, outcome = ?, elapsed_ms = ? WHERE run_id = ?;");
        if (!p4) {
            return std::unexpected(p4.error());
        }
        updateRunEndedStmt = std::move(*p4);

        auto p5 = prepare("SELECT COALESCE(MAX(seq), 0) + 1 FROM run_events WHERE run_id = ?;");
        if (!p5) {
            return std::unexpected(p5.error());
        }
        nextSeqStmt = std::move(*p5);

        return {};
    }

    void close() {
        insertEventStmt.reset();
        insertRunStmt.reset();
        updateRunStartedStmt.reset();
        updateRunEndedStmt.reset();
        nextSeqStmt.reset();
        rejectedRuns.clear();
        startedRuns.clear();
        db.reset();
    }

    [[nodiscard]] std::int64_t nextSeq(std::uint64_t runId) const {
        sqlite3_reset(nextSeqStmt.get());
        sqlite3_bind_int64(nextSeqStmt.get(), 1, static_cast<sqlite3_int64>(runId));
        if (sqlite3_step(nextSeqStmt.get()) != SQLITE_ROW) {
            return 1;
        }
        return sqlite3_column_int64(nextSeqStmt.get(), 0);
    }
};

SqliteHistoryStore::SqliteHistoryStore() : impl_(std::make_unique<Impl>()) {}
SqliteHistoryStore::~SqliteHistoryStore() = default;

std::expected<void, ReqloomError> SqliteHistoryStore::open(const fs::path& dbPath) {
    const std::lock_guard lock(impl_->mutex);
    // Re-openable: drop any previous connection and statements so switching projects leaks nothing.
    impl_->close();

    if (dbPath.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(dbPath.parent_path(), ec);
        if (ec) {
            return std::unexpected(
                ReqloomError{ErrorCode::SchemaInvalid,
                             ErrorClass::Schema,
                             "history: cannot create parent dir: " + ec.message()});
        }
    }

    sqlite3* raw = nullptr;
    const auto pathStr = dbPath.string();
    const int rc = sqlite3_open_v2(pathStr.c_str(),
                                   &raw,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                                   nullptr);
    if (rc != SQLITE_OK) {
        ReqloomError e = sqliteError(raw, "history: open");
        sqlite3_close(raw);
        return std::unexpected(std::move(e));
    }
    impl_->db.reset(raw);
    sqlite3_busy_timeout(impl_->db.get(), kBusyTimeoutMs);

    if (auto r = impl_->ensureSchema(); !r) {
        return r;
    }
    if (auto r = impl_->prepareStatements(); !r) {
        return r;
    }
    return {};
}

std::expected<void, ReqloomError> SqliteHistoryStore::openReadOnly(const fs::path& dbPath) {
    const std::lock_guard lock(impl_->mutex);
    impl_->close();
    std::error_code ec;
    if (!fs::is_regular_file(dbPath, ec)) {
        return std::unexpected(ReqloomError{
            ErrorCode::Internal, ErrorClass::Run, "history: no run history database yet"});
    }
    sqlite3* raw = nullptr;
    const auto pathStr = dbPath.string();
    if (sqlite3_open_v2(
            pathStr.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) !=
        SQLITE_OK) {
        ReqloomError e = sqliteError(raw, "history: open read-only");
        sqlite3_close(raw);
        return std::unexpected(std::move(e));
    }
    impl_->db.reset(raw);
    sqlite3_busy_timeout(impl_->db.get(), kBusyTimeoutMs);
    const auto complete = impl_->schemaComplete();
    if (!complete || !*complete) {
        impl_->close();
        return std::unexpected(
            ReqloomError{ErrorCode::Internal,
                         ErrorClass::Run,
                         "history: the run history predates this version of Reqloom; a "
                         "`reqloom run` or the app upgrades it"});
    }
    return {};
}

std::expected<RunId, ReqloomError> SqliteHistoryStore::allocateRunId() {
    const std::lock_guard lock(impl_->mutex);
    if (!impl_->db) {
        return std::unexpected(ReqloomError{
            ErrorCode::SchemaInvalid, ErrorClass::Schema, "history: allocateRunId before open"});
    }
    // BEGIN IMMEDIATE takes the write lock, serialising concurrent allocators. The counter never
    // moves backwards (even after clear()) and starts above any run id already present.
    SqliteTransaction txn{impl_->db.get()};
    if (!txn.began()) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: begin allocate txn"));
    }
    if (auto r = impl_->exec(
            "INSERT OR IGNORE INTO run_id_allocator(id, last_id) VALUES (1, 0);"
            "UPDATE run_id_allocator SET last_id = "
            "  MAX(last_id, (SELECT COALESCE(MAX(run_id), 0) FROM runs)) + 1 WHERE id = 1;");
        !r) {
        return std::unexpected(r.error());
    }
    auto select = impl_->prepare("SELECT last_id FROM run_id_allocator WHERE id = 1;");
    if (!select) {
        return std::unexpected(select.error());
    }
    if (sqlite3_step(select->get()) != SQLITE_ROW) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: read allocated run id"));
    }
    const auto allocated = static_cast<std::uint64_t>(sqlite3_column_int64(select->get(), 0));
    select->reset();
    if (!txn.commit()) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: commit allocate txn"));
    }
    return RunId{allocated};
}

std::expected<void, ReqloomError> SqliteHistoryStore::append(const RunEvent& event) {
    const std::lock_guard lock(impl_->mutex);
    if (!impl_->db) {
        return std::unexpected(ReqloomError{
            ErrorCode::SchemaInvalid, ErrorClass::Schema, "history: append before open"});
    }

    EventEnvelope env{};
    std::string payloadStr{};
    try {
        env = envelopeOf(event);
        payloadStr = env.payload.dump();
    } catch (const json::exception&) {
        return std::unexpected(ReqloomError{ErrorCode::SchemaInvalid,
                                            ErrorClass::Schema,
                                            "history: event payload is not serializable"});
    }
    if (impl_->rejectedRuns.contains(env.runId)) {
        return std::unexpected(ReqloomError{
            ErrorCode::Internal,
            ErrorClass::Run,
            "history: run id " + std::to_string(env.runId) + " was refused at RunStarted"});
    }

    bool startsRun = false;
    // The run row, optional run-ended update and event insert commit as one unit, so an event never
    // lands without its parent run row.
    SqliteTransaction txn{impl_->db.get()};
    if (!txn.began()) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: begin append txn"));
    }

    // RunStarted creates the runs row, RunEnded stamps the terminal state, any other event
    // backfills a placeholder row so the FK holds when events arrive out of order.
    if (env.eventType == "RunStarted") {
        sqlite3_reset(impl_->insertRunStmt.get());
        sqlite3_bind_int64(impl_->insertRunStmt.get(), 1, static_cast<sqlite3_int64>(env.runId));
        const auto target = env.payload.value("target", std::string{});
        const auto envName = env.payload.value("envName", std::string{});
        const auto chainSize = env.payload.value("chainSize", std::size_t{0});
        sqlite3_bind_text(impl_->insertRunStmt.get(), 2, target.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(impl_->insertRunStmt.get(), 3, envName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(impl_->insertRunStmt.get(), 4, env.at.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(impl_->insertRunStmt.get(), 5, static_cast<sqlite3_int64>(chainSize));
        const auto initiator = env.payload.value("initiator", std::string{"app"});
        const auto initiatorName = env.payload.value("initiatorName", std::string{});
        sqlite3_bind_text(impl_->insertRunStmt.get(), 6, initiator.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(
            impl_->insertRunStmt.get(), 7, initiatorName.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(impl_->insertRunStmt.get()) != SQLITE_DONE) {
            return std::unexpected(sqliteError(impl_->db.get(), "history: insert run"));
        }
        const bool insertedFreshRun = sqlite3_changes(impl_->db.get()) > 0;
        // INSERT OR IGNORE leaves an existing row, so UPDATE it from the canonical RunStarted
        // payload.
        sqlite3_reset(impl_->updateRunStartedStmt.get());
        sqlite3_bind_text(
            impl_->updateRunStartedStmt.get(), 1, target.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(
            impl_->updateRunStartedStmt.get(), 2, envName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(
            impl_->updateRunStartedStmt.get(), 3, env.at.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(
            impl_->updateRunStartedStmt.get(), 4, static_cast<sqlite3_int64>(chainSize));
        sqlite3_bind_text(
            impl_->updateRunStartedStmt.get(), 5, initiator.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(
            impl_->updateRunStartedStmt.get(), 6, initiatorName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(
            impl_->updateRunStartedStmt.get(), 7, static_cast<sqlite3_int64>(env.runId));
        if (sqlite3_step(impl_->updateRunStartedStmt.get()) != SQLITE_DONE) {
            return std::unexpected(sqliteError(impl_->db.get(), "history: update run start"));
        }
        if (!insertedFreshRun && sqlite3_changes(impl_->db.get()) == 0) {
            impl_->rejectedRuns.insert(env.runId);
            return std::unexpected(ReqloomError{ErrorCode::Internal,
                                                ErrorClass::Run,
                                                "history: run id " + std::to_string(env.runId) +
                                                    " already belongs to another run"});
        }
        startsRun = true;
    } else {
        sqlite3_reset(impl_->insertRunStmt.get());
        sqlite3_bind_int64(impl_->insertRunStmt.get(), 1, static_cast<sqlite3_int64>(env.runId));
        sqlite3_bind_null(impl_->insertRunStmt.get(), 2);
        sqlite3_bind_null(impl_->insertRunStmt.get(), 3);
        sqlite3_bind_null(impl_->insertRunStmt.get(), 4);
        sqlite3_bind_int64(impl_->insertRunStmt.get(), 5, 0);
        sqlite3_bind_null(impl_->insertRunStmt.get(), 6);
        sqlite3_bind_null(impl_->insertRunStmt.get(), 7);
        if (sqlite3_step(impl_->insertRunStmt.get()) != SQLITE_DONE) {
            return std::unexpected(sqliteError(impl_->db.get(), "history: backfill run"));
        }
    }

    if (env.eventType == "RunEnded") {
        sqlite3_reset(impl_->updateRunEndedStmt.get());
        sqlite3_bind_text(impl_->updateRunEndedStmt.get(), 1, env.at.c_str(), -1, SQLITE_TRANSIENT);
        const auto outcome = env.payload.value("outcome", std::string{"Failed"});
        sqlite3_bind_text(
            impl_->updateRunEndedStmt.get(), 2, outcome.c_str(), -1, SQLITE_TRANSIENT);
        const auto elapsedMs = env.payload.value("elapsedMs", std::int64_t{0});
        sqlite3_bind_int64(
            impl_->updateRunEndedStmt.get(), 3, static_cast<sqlite3_int64>(elapsedMs));
        sqlite3_bind_int64(
            impl_->updateRunEndedStmt.get(), 4, static_cast<sqlite3_int64>(env.runId));
        if (sqlite3_step(impl_->updateRunEndedStmt.get()) != SQLITE_DONE) {
            return std::unexpected(sqliteError(impl_->db.get(), "history: update run end"));
        }
    }

    const auto seq = impl_->nextSeq(env.runId);

    sqlite3_reset(impl_->insertEventStmt.get());
    sqlite3_bind_int64(impl_->insertEventStmt.get(), 1, static_cast<sqlite3_int64>(env.runId));
    sqlite3_bind_int64(impl_->insertEventStmt.get(), 2, seq);
    sqlite3_bind_text(impl_->insertEventStmt.get(), 3, env.eventType.c_str(), -1, SQLITE_TRANSIENT);
    if (env.stepIndex) {
        sqlite3_bind_int64(
            impl_->insertEventStmt.get(), 4, static_cast<sqlite3_int64>(*env.stepIndex));
    } else {
        sqlite3_bind_null(impl_->insertEventStmt.get(), 4);
    }
    if (env.opId) {
        sqlite3_bind_text(impl_->insertEventStmt.get(), 5, env.opId->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(impl_->insertEventStmt.get(), 5);
    }
    sqlite3_bind_text(impl_->insertEventStmt.get(), 6, payloadStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(impl_->insertEventStmt.get(), 7, env.at.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(impl_->insertEventStmt.get()) != SQLITE_DONE) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: insert event"));
    }
    if (!txn.commit()) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: commit append txn"));
    }
    if (startsRun) {
        impl_->startedRuns.insert(env.runId);
    }
    return {};
}

std::expected<std::vector<RunEvent>, ReqloomError> SqliteHistoryStore::eventsFor(RunId run) const {
    const std::lock_guard lock(impl_->mutex);
    if (!impl_->db) {
        return std::unexpected(ReqloomError{
            ErrorCode::SchemaInvalid, ErrorClass::Schema, "history: eventsFor before open"});
    }

    sqlite3_stmt* raw = nullptr;
    const char* sql =
        "SELECT event_type, step_index, payload, at "
        "FROM run_events WHERE run_id = ? ORDER BY seq ASC;";
    if (sqlite3_prepare_v2(impl_->db.get(), sql, -1, &raw, nullptr) != SQLITE_OK) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: prepare eventsFor"));
    }
    StmtPtr const stmt{raw};

    sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(run.value));

    std::vector<RunEvent> out;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const auto* eventType = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        const bool stepIndexNull = (sqlite3_column_type(stmt.get(), 1) == SQLITE_NULL);
        const auto stepIndex = stepIndexNull ? std::optional<std::size_t>{}
                                             : std::optional<std::size_t>{static_cast<std::size_t>(
                                                   sqlite3_column_int64(stmt.get(), 1))};
        const auto* payloadStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        const auto* atStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));

        try {
            const auto payload = json::parse(payloadStr != nullptr ? payloadStr : "{}");
            if (auto ev = eventFromRow(eventType != nullptr ? eventType : "",
                                       run.value,
                                       stepIndex,
                                       atStr != nullptr ? atStr : "",
                                       payload)) {
                out.push_back(std::move(*ev));
            }
        } catch (const json::exception&) {
            // Keep other history available when one row is malformed or
            // contains field types this build cannot safely interpret.
            continue;
        }
    }
    return out;
}

std::expected<std::vector<RunHistoryRow>, ReqloomError> SqliteHistoryStore::listRuns(
    std::size_t limit) const {
    const std::lock_guard lock(impl_->mutex);
    if (!impl_->db) {
        return std::unexpected(ReqloomError{
            ErrorCode::SchemaInvalid, ErrorClass::Schema, "history: listRuns before open"});
    }

    std::string sql =
        "SELECT run_id, COALESCE(target_op, ''), COALESCE(env_name, ''), "
        "       COALESCE(started_at, ''), COALESCE(ended_at, ''), "
        "       COALESCE(outcome, ''), COALESCE(chain_size, 0), "
        "       COALESCE(elapsed_ms, -1), COALESCE(initiator, ''), "
        "       COALESCE(initiator_name, ''), "
        "       EXISTS (SELECT 1 FROM run_steps WHERE run_steps.run_id = runs.run_id) "
        "FROM runs ORDER BY started_at DESC, run_id DESC";
    if (limit > 0) {
        sql += " LIMIT " + std::to_string(limit);
    }
    sql += ";";

    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(impl_->db.get(), sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: prepare listRuns"));
    }
    StmtPtr const stmt{raw};

    std::vector<RunHistoryRow> out;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        RunHistoryRow row;
        row.runId = RunId{static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 0))};
        row.targetOp =
            OperationId{reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1))};
        row.envName = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        row.startedAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        row.endedAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
        row.outcome = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
        row.chainSize = static_cast<std::size_t>(sqlite3_column_int64(stmt.get(), 6));
        row.elapsedMs = static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 7));
        row.initiator.kind = runInitiatorKindFromName(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 8)));
        row.initiator.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 9));
        row.hasRecordedSteps = sqlite3_column_int(stmt.get(), 10) != 0;
        out.push_back(std::move(row));
    }
    return out;
}

std::expected<void, ReqloomError> SqliteHistoryStore::clear() {
    const std::lock_guard lock(impl_->mutex);
    if (!impl_->db) {
        return std::unexpected(ReqloomError{
            ErrorCode::SchemaInvalid, ErrorClass::Schema, "history: clear before open"});
    }
    SqliteTransaction txn{impl_->db.get()};
    if (!txn.began()) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: begin clear txn"));
    }
    if (auto r = impl_->exec("DELETE FROM run_events;"); !r) {
        return r;
    }
    if (auto r = impl_->exec("DELETE FROM run_steps;"); !r) {
        return r;
    }
    if (auto r = impl_->exec("DELETE FROM runs;"); !r) {
        return r;
    }
    if (!txn.commit()) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: commit clear txn"));
    }
    return {};
}

std::expected<void, ReqloomError> SqliteHistoryStore::appendSteps(
    RunId run, const std::vector<StepResult>& steps) {
    const std::lock_guard lock(impl_->mutex);
    if (!impl_->db) {
        return std::unexpected(ReqloomError{
            ErrorCode::SchemaInvalid, ErrorClass::Schema, "history: appendSteps before open"});
    }
    if (impl_->rejectedRuns.contains(run.value) || !impl_->startedRuns.contains(run.value)) {
        return std::unexpected(ReqloomError{ErrorCode::Internal,
                                            ErrorClass::Run,
                                            "history: run id " + std::to_string(run.value) +
                                                " was not started through this connection"});
    }
    std::vector<std::string> payloads;
    payloads.reserve(steps.size());
    try {
        for (const auto& step : steps) {
            payloads.push_back(stepToJson(step).dump());
        }
    } catch (const json::exception&) {
        return std::unexpected(ReqloomError{
            ErrorCode::SchemaInvalid, ErrorClass::Schema, "history: step is not serializable"});
    }
    SqliteTransaction txn{impl_->db.get()};
    if (!txn.began()) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: begin steps txn"));
    }
    auto remove = impl_->prepare("DELETE FROM run_steps WHERE run_id = ?;");
    if (!remove) {
        return std::unexpected(remove.error());
    }
    sqlite3_bind_int64(remove->get(), 1, static_cast<sqlite3_int64>(run.value));
    if (sqlite3_step(remove->get()) != SQLITE_DONE) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: replace steps"));
    }
    auto insert = impl_->prepare("INSERT INTO run_steps (run_id, row, payload) VALUES (?, ?, ?);");
    if (!insert) {
        return std::unexpected(insert.error());
    }
    for (std::size_t i = 0; i < payloads.size(); ++i) {
        sqlite3_reset(insert->get());
        sqlite3_bind_int64(insert->get(), 1, static_cast<sqlite3_int64>(run.value));
        sqlite3_bind_int64(insert->get(), 2, static_cast<sqlite3_int64>(i));
        sqlite3_bind_text(insert->get(), 3, payloads[i].c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insert->get()) != SQLITE_DONE) {
            return std::unexpected(sqliteError(impl_->db.get(), "history: insert step"));
        }
    }
    remove->reset();
    insert->reset();
    if (!txn.commit()) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: commit steps txn"));
    }
    return {};
}

std::expected<void, ReqloomError> SqliteHistoryStore::rewriteEvents(
    RunId run, const std::function<void(RunEvent&)>& edit) {
    const std::lock_guard lock(impl_->mutex);
    if (!impl_->db) {
        return std::unexpected(ReqloomError{
            ErrorCode::SchemaInvalid, ErrorClass::Schema, "history: rewriteEvents before open"});
    }
    if (!impl_->startedRuns.contains(run.value)) {
        return std::unexpected(ReqloomError{ErrorCode::Internal,
                                            ErrorClass::Run,
                                            "history: run id " + std::to_string(run.value) +
                                                " was not started through this connection"});
    }
    SqliteTransaction txn{impl_->db.get()};
    if (!txn.began()) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: begin rewrite txn"));
    }
    auto select = impl_->prepare(
        "SELECT id, event_type, step_index, payload, at FROM run_events WHERE run_id = ?;");
    auto update = impl_->prepare("UPDATE run_events SET payload = ? WHERE id = ?;");
    if (!select || !update) {
        return std::unexpected(select ? update.error() : select.error());
    }
    sqlite3_bind_int64(select->get(), 1, static_cast<sqlite3_int64>(run.value));
    std::vector<std::pair<std::int64_t, std::string>> changed;
    while (sqlite3_step(select->get()) == SQLITE_ROW) {
        const auto id = sqlite3_column_int64(select->get(), 0);
        const auto* type = reinterpret_cast<const char*>(sqlite3_column_text(select->get(), 1));
        const auto stepIndex = sqlite3_column_type(select->get(), 2) == SQLITE_NULL
                                   ? std::optional<std::size_t>{}
                                   : std::optional<std::size_t>{static_cast<std::size_t>(
                                         sqlite3_column_int64(select->get(), 2))};
        const auto* payload = reinterpret_cast<const char*>(sqlite3_column_text(select->get(), 3));
        const auto* at = reinterpret_cast<const char*>(sqlite3_column_text(select->get(), 4));
        try {
            const auto before = json::parse(payload != nullptr ? payload : "{}");
            auto event = eventFromRow(
                type != nullptr ? type : "", run.value, stepIndex, at != nullptr ? at : "", before);
            if (!event) {
                continue;
            }
            edit(*event);
            auto after = envelopeOf(*event).payload;
            if (after != before) {
                changed.emplace_back(id, after.dump());
            }
        } catch (const json::exception&) {
            // A row this build cannot read cannot be rewritten either; it is left as it was.
            continue;
        }
    }
    for (const auto& [id, payload] : changed) {
        sqlite3_reset(update->get());
        sqlite3_bind_text(update->get(), 1, payload.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(update->get(), 2, id);
        if (sqlite3_step(update->get()) != SQLITE_DONE) {
            return std::unexpected(sqliteError(impl_->db.get(), "history: rewrite event"));
        }
    }
    select->reset();
    update->reset();
    if (!txn.commit()) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: commit rewrite txn"));
    }
    return {};
}

std::expected<std::vector<StepResult>, ReqloomError> SqliteHistoryStore::stepsFor(RunId run) const {
    const std::lock_guard lock(impl_->mutex);
    if (!impl_->db) {
        return std::unexpected(ReqloomError{
            ErrorCode::SchemaInvalid, ErrorClass::Schema, "history: stepsFor before open"});
    }
    auto select =
        impl_->prepare("SELECT payload FROM run_steps WHERE run_id = ? ORDER BY row ASC;");
    if (!select) {
        return std::unexpected(select.error());
    }
    sqlite3_bind_int64(select->get(), 1, static_cast<sqlite3_int64>(run.value));
    std::vector<StepResult> out;
    while (sqlite3_step(select->get()) == SQLITE_ROW) {
        const auto* payload = reinterpret_cast<const char*>(sqlite3_column_text(select->get(), 0));
        try {
            out.push_back(stepFromJson(json::parse(payload != nullptr ? payload : "{}")));
        } catch (const json::exception&) {
            // A row this build cannot read fails the whole read: a step list with a hole would
            // shift every index after it.
            return std::unexpected(ReqloomError{
                ErrorCode::SchemaInvalid, ErrorClass::Schema, "history: unreadable step row"});
        }
    }
    return out;
}

void SqliteHistoryStore::close() {
    const std::lock_guard lock(impl_->mutex);
    impl_->close();
}

}  // namespace reqloom::engine
