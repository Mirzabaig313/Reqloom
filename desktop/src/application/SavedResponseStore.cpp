// SavedResponseStore — see header. JSON-file persistence under the project root.
#include "SavedResponseStore.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

namespace reqloom::desktop {

namespace {

constexpr auto kFileName = ".reqloom-responses.json";

[[nodiscard]] SavedResponse fromJson(const QJsonObject& obj) {
    SavedResponse r;
    r.name = obj.value(QStringLiteral("name")).toString();
    r.status = obj.value(QStringLiteral("status")).toInt();
    r.headers = obj.value(QStringLiteral("headers")).toString();
    r.body = obj.value(QStringLiteral("body")).toString();
    r.elapsedMs = static_cast<qint64>(obj.value(QStringLiteral("elapsedMs")).toDouble());
    return r;
}

[[nodiscard]] QJsonObject toJson(const SavedResponse& r) {
    QJsonObject obj;
    obj.insert(QStringLiteral("name"), r.name);
    obj.insert(QStringLiteral("status"), r.status);
    obj.insert(QStringLiteral("headers"), r.headers);
    obj.insert(QStringLiteral("body"), r.body);
    obj.insert(QStringLiteral("elapsedMs"), static_cast<double>(r.elapsedMs));
    return obj;
}

}  // namespace

void SavedResponseStore::setProjectRoot(const QString& rootPath) {
    rootPath_ = rootPath;
    byOperation_.clear();
    if (!rootPath_.isEmpty()) {
        load();
    }
}

QString SavedResponseStore::filePath() const {
    return QDir(rootPath_).filePath(QString::fromUtf8(kFileName));
}

QList<SavedResponse> SavedResponseStore::list(const QString& operationId) const {
    for (const auto& [id, responses] : byOperation_) {
        if (id == operationId) {
            return responses;
        }
    }
    return {};
}

QStringList SavedResponseStore::operationIds() const {
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(byOperation_.size()));
    for (const auto& [id, responses] : byOperation_) {
        if (!responses.isEmpty()) {
            ids.append(id);
        }
    }
    return ids;
}

bool SavedResponseStore::save(const QString& operationId, const SavedResponse& response) {
    if (rootPath_.isEmpty()) {
        return false;
    }
    for (auto& [id, responses] : byOperation_) {
        if (id == operationId) {
            // Overwrite an example with the same name; otherwise append.
            for (auto& existing : responses) {
                if (existing.name == response.name) {
                    existing = response;
                    persist();
                    return true;
                }
            }
            responses.append(response);
            persist();
            return true;
        }
    }
    byOperation_.append({operationId, {response}});
    persist();
    return true;
}

bool SavedResponseStore::rename(const QString& operationId,
                                const QString& oldName,
                                const QString& newName) {
    if (newName.trimmed().isEmpty()) {
        return false;
    }
    for (auto& [id, responses] : byOperation_) {
        if (id != operationId) {
            continue;
        }
        // Reject a clash with a different example.
        for (const auto& r : responses) {
            if (r.name == newName && r.name != oldName) {
                return false;
            }
        }
        for (auto& r : responses) {
            if (r.name == oldName) {
                r.name = newName;
                persist();
                return true;
            }
        }
    }
    return false;
}

QString SavedResponseStore::duplicate(const QString& operationId, const QString& name) {
    for (auto& [id, responses] : byOperation_) {
        if (id != operationId) {
            continue;
        }
        const SavedResponse* src = nullptr;
        for (const auto& r : responses) {
            if (r.name == name) {
                src = &r;
                break;
            }
        }
        if (src == nullptr) {
            return {};
        }
        const auto taken = [&responses](const QString& candidate) {
            for (const auto& r : responses) {
                if (r.name == candidate) {
                    return true;
                }
            }
            return false;
        };
        QString candidate = QStringLiteral("%1 copy").arg(name);
        int n = 2;
        while (taken(candidate)) {
            candidate = QStringLiteral("%1 copy %2").arg(name).arg(n++);
        }
        SavedResponse copy = *src;
        copy.name = candidate;
        responses.append(copy);
        persist();
        return candidate;
    }
    return {};
}

void SavedResponseStore::remove(const QString& operationId, const QString& name) {
    for (auto& [id, responses] : byOperation_) {
        if (id == operationId) {
            const auto removed =
                responses.removeIf([&name](const SavedResponse& r) { return r.name == name; });
            if (removed > 0) {
                persist();
            }
            return;
        }
    }
}

void SavedResponseStore::load() {
    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject ops = doc.object().value(QStringLiteral("operations")).toObject();
    for (auto it = ops.constBegin(); it != ops.constEnd(); ++it) {
        QList<SavedResponse> responses;
        const QJsonArray arr = it.value().toArray();
        for (const QJsonValue& v : arr) {
            responses.append(fromJson(v.toObject()));
        }
        byOperation_.append({it.key(), responses});
    }
}

void SavedResponseStore::persist() const {
    if (rootPath_.isEmpty()) {
        return;
    }
    QJsonObject ops;
    for (const auto& [id, responses] : byOperation_) {
        QJsonArray arr;
        for (const SavedResponse& r : responses) {
            arr.append(toJson(r));
        }
        ops.insert(id, arr);
    }
    QJsonObject root;
    root.insert(QStringLiteral("operations"), ops);

    QFile file(filePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
}

}  // namespace reqloom::desktop
