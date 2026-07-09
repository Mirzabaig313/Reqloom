// TabModel — the open editor tabs (endpoints + actors) for the centre pane.
// Each row is one open tab; TabState carries a full snapshot of that tab's
// live editor state so AppController can swap the single set of live models /
// scalars in and out on tab switch (per-tab state preservation). C++ owns the
// buffers; QML renders the tab strip and drives activate/close.
#pragma once

#include "AuthStepListModel.h"  // AuthStepListModel::StepSeed for actor login steps
#include "TimelineModel.h"      // TimelineModel::Snapshot for the per-tab run timeline

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractListModel>
#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstdint>
#include <utility>
#include <vector>

namespace reqloom::desktop::qml {

/// Ordered key/value rows as captured from an EditableKeyValueModel.
using TabKvPairs = std::vector<std::pair<QString, QString>>;

/// A full snapshot of one tab's editor state. An operation tab uses the op*/
/// edit*/response fields; an actor tab uses the actor* fields. Defaults mirror
/// AppController's member initializers so a freshly-opened tab matches a fresh
/// controller.
struct TabState {
    enum class Kind : std::uint8_t { Operation, Actor };

    Kind kind{Kind::Operation};
    /// Operation tabs: fully-qualified "module.op". Actor tabs: actor id.
    /// A never-saved new-actor draft has an empty id.
    QString id;
    QString module;       ///< Operation tabs: owning module.
    QString projectRoot;  ///< Owning collection (for multi-project activation).
    QString title;        ///< Tab label (op name or actor name; "New actor" when blank).
    QString method;       ///< Operation tabs: HTTP method for the badge.
    QString subtitle;     ///< Operation: path. Actor: strategy label.
    bool dirty{false};    ///< Has unsaved edits.

    // ── Operation identity ──
    // Read fields (method/path/headers/…) are re-derived from the project on
    // restore (loadOperationReadState), so only the op name is stored here.
    QString opName;

    // ── Operation edit state ──
    bool editing{false};
    QString editMethod;
    QString editPath;
    QString editActor;
    QString editExpectStatus;
    int editTimeout{0};
    bool editForce{false};
    QString editBody;
    bool editBodyIsForm{false};
    QString editBodyType{QStringLiteral("none")};
    bool chainFieldsLoaded{false};

    QString editAuthType{QStringLiteral("none")};
    QString editAuthToken;
    QString editAuthUsername;
    QString editAuthPassword;
    QString editAuthApiKeyName;
    QString editAuthApiKeyValue;
    bool editAuthApiKeyInQuery{false};
    QString editAuthAwsAccessKey;
    QString editAuthAwsSecretKey;
    QString editAuthAwsRegion;
    QString editAuthAwsService;
    QString editAuthAwsSessionToken;
    QString editAuthOauthConsumerKey;
    QString editAuthOauthConsumerSecret;
    QString editAuthOauthToken;
    QString editAuthOauthTokenSecret;
    QString editAuthOauth2GrantType{QStringLiteral("client_credentials")};
    QString editAuthOauth2ClientAuth{QStringLiteral("basic")};
    QString editAuthOauth2AuthUrl;
    QString editAuthOauth2CallbackUrl{QStringLiteral("http://127.0.0.1:8080/callback")};
    QString editAuthOauth2PkceMethod{QStringLiteral("S256")};
    QString editAuthOauth2AccessToken;  ///< Ephemeral; kept per tab, never persisted to YAML.
    QString editAuthOauth2TokenUrl;
    QString editAuthOauth2ClientId;
    QString editAuthOauth2ClientSecret;
    QString editAuthOauth2Scope;
    QString editAuthJwtAlgorithm{QStringLiteral("HS256")};
    QString editAuthJwtSecret;
    QString editAuthJwtPayload;
    QString editAuthMtlsFormat{QStringLiteral("pem")};
    QString editAuthMtlsCertPath;
    QString editAuthMtlsKeyPath;
    QString editAuthMtlsKeyPassword;
    QString editAuthMtlsCaCertPath;

    TabKvPairs editHeaders;
    TabKvPairs editQuery;
    TabKvPairs editForm;
    TabKvPairs editExtractions;
    TabKvPairs editAssertions;
    std::vector<QString> editDependencies;

    // ── Actor state ──
    QString actorId;
    QString actorName;
    QString actorDescription;
    QString actorStrategy;
    TabKvPairs actorConfig;
    TabKvPairs actorAuthExtract;
    TabKvPairs actorRefreshExtract;
    std::vector<AuthStepListModel::StepSeed> actorAuthSteps;
    QString actorAuthMethod{QStringLiteral("POST")};
    QString actorAuthPath;
    QString actorAuthBody;
    QString actorAuthExpect;
    bool actorHasRefresh{false};
    QString actorRefreshMethod{QStringLiteral("POST")};
    QString actorRefreshPath;
    QString actorRefreshBody;

    // ── Response snapshot (per tab) ──
    bool hasResponse{false};
    int respStatus{0};
    int respElapsedMs{0};
    int respBodySize{0};
    QString respHeaders;
    QString respBody;
    QString runOutcome;
    QString shownExample;

    // ── Run timeline snapshot (per tab) ──
    // The live TimelineModel is a single instance; each tab parks its own run
    // timeline here so switching tabs restores that tab's timeline instead of
    // wiping it.
    TimelineModel::Snapshot timeline;
};

/// List of open tabs. Owns the TabState buffers; exposes lightweight display
/// roles for the QML tab strip. Mutation goes through AppController.
class TabModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by AppController")

public:
    enum Roles : int {
        KindRole = Qt::UserRole + 1,  ///< int: 0 = operation, 1 = actor
        IdRole,                       ///< QString: operationId or actorId
        TitleRole,                    ///< QString: tab label
        MethodRole,                   ///< QString: HTTP method (operation tabs)
        SubtitleRole,                 ///< QString: path (op) / strategy (actor)
        DirtyRole,                    ///< bool: unsaved edits
    };

    explicit TabModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] int count() const { return static_cast<int>(tabs_.size()); }
    [[nodiscard]] bool valid(int index) const {
        return index >= 0 && index < static_cast<int>(tabs_.size());
    }

    /// Mutable access to a tab's buffer (AppController snapshots/restores here).
    [[nodiscard]] TabState& stateAt(int index) { return tabs_.at(static_cast<std::size_t>(index)); }
    [[nodiscard]] const TabState& stateAt(int index) const {
        return tabs_.at(static_cast<std::size_t>(index));
    }

    /// Index of the tab matching (kind, id), or -1. A blank-id actor draft is
    /// never matched (each "New actor" opens its own tab).
    [[nodiscard]] int indexOf(TabState::Kind kind, const QString& id) const;

    /// Append `state` as a new tab; returns its index.
    int append(TabState state);
    /// Remove the tab at `index` (no-op if out of range).
    void removeAt(int index);
    /// Remove every tab (used by "close others" which re-appends the kept one).
    void clearAll();
    /// Re-emit dataChanged for `index` after its title/method/dirty/subtitle
    /// changed (AppController calls this after a snapshot or rename).
    void refreshRow(int index);

private:
    std::vector<TabState> tabs_;
};

}  // namespace reqloom::desktop::qml
