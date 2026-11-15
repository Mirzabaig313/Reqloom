// Pure helpers behind ProjectModel's structural edits: entity-name rules, placeholder-aware
// reference rewriting and lookup, extraction merging and environment selection.
#pragma once

#include <reqloom/engine/PublicApi.h>

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reqloom::desktop {

/// Mirror of the engine's file-stem rule for actor, resource and environment names: non-empty, no
/// leading '.', and no '/', '\', ':' or NUL.
[[nodiscard]] bool isSafeFileStem(std::string_view name);

/// A usable actor, resource, operation or environment name: a safe file stem without '.', which
/// would split a "<resource>.<op>" id or a `{{scope.field}}` reference.
[[nodiscard]] bool isValidEntityName(std::string_view name);

/// The message for a name `isValidEntityName` rejects.
[[nodiscard]] QString invalidNameMessage();

/// The entry of `existing`, other than `self`, equal to `name` ignoring case, or nullopt. Names
/// that differ only in case share one file on case-insensitive volumes (macOS, Windows).
[[nodiscard]] std::optional<std::string> caseInsensitiveClash(
    const std::vector<std::string>& existing, std::string_view name, std::string_view self = {});

/// Rewrite every `{{from.…}}` reference in `text` (whitespace inside the braces and a builtin
/// call's bare argument included) to `{{to.…}}`, reading references exactly as the engine's
/// dependency scanner does. Text outside `{{ }}` is left alone.
[[nodiscard]] std::string renameReferenceScope(std::string_view text,
                                               std::string_view from,
                                               std::string_view to);

/// Apply `renameReferenceScope` to every template of every operation, actor, environment value
/// and the project default auth.
void renameScopeInProject(engine::Project& project, std::string_view from, std::string_view to);

/// One place that still refers to an entity: its owner ("orders.list", "actor admin",
/// "environment prod", "default auth") and what refers ("{{users.id}}", "for_each", ...).
struct EntityReference {
    std::string owner;
    std::string what;
};

/// Template references whose scope is `scope` (and whose field is `field`, when given) anywhere
/// in the project, in the same places `renameScopeInProject` rewrites.
[[nodiscard]] std::vector<EntityReference> findScopeReferences(
    const engine::Project& project,
    std::string_view scope,
    std::optional<std::string_view> field = std::nullopt);

/// "orders.list ({{users.id}}), actor admin ({{users.token}})" for a refusal message.
[[nodiscard]] QString describeReferences(const std::vector<EntityReference>& references);

/// The extraction source the engine's parser infers from a path without an explicit `source:`.
[[nodiscard]] engine::Extraction::Source impliedExtractionSource(std::string_view path);

/// Build extractions from editor rows (variable, path); rows with an empty variable are skipped.
/// A row whose variable already exists keeps that extraction's source unless the source was the
/// one implied by its old path, in which case the new path decides; a new row takes the source
/// its path implies.
[[nodiscard]] std::vector<engine::Extraction> mergeExtractions(
    const std::vector<engine::Extraction>& existing,
    const std::vector<std::pair<QString, QString>>& rows);

/// The fields of one auth step the actor editor shows.
struct AuthStepEdit {
    std::string id;
    engine::HttpMethod method{engine::HttpMethod::Post};
    std::string pathTemplate;
    std::optional<std::string> bodyTemplate;
    std::optional<int> expectStatus;
    std::vector<std::pair<QString, QString>> extractions;
};

/// Apply editor steps on top of `existing`: each edit starts from the existing step with the same
/// id (else the one at the same position, if no edit claims its id), so fields the editor does not
/// show, such as step headers, survive.
[[nodiscard]] std::vector<engine::AuthStep> mergeAuthSteps(
    const std::vector<engine::AuthStep>& existing, const std::vector<AuthStepEdit>& edits);

/// The environment to select: `saved` when it exists, else `defaultEnvironment` when it exists,
/// else the first of `available`, else empty.
[[nodiscard]] QString chooseEnvironment(const QStringList& available,
                                        const QString& saved,
                                        const QString& defaultEnvironment);

/// The operation the engine runs to supply `token` ("<resource>.<variable>") for the plan's
/// target, or why there is none (`problem` set, `producer` empty).
struct ProducerLookup {
    std::string producer;
    std::string sourcePath;
    QString problem;
};

[[nodiscard]] ProducerLookup producerFromPlan(
    const std::expected<engine::ResolvedPlan, engine::ReqloomError>& plan,
    const engine::Project& project,
    const engine::OperationId& target,
    std::string_view token);

}  // namespace reqloom::desktop
