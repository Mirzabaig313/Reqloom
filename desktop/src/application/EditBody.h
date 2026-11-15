// How the request editor's body type maps onto a RequestOverride and its Content-Type header.
// Desktop Requirement §6.2.
#pragma once

#include "application/RunController.h"

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <utility>
#include <vector>

namespace reqloom::desktop {

/// Fill the body of `ov` from the editor: "none" sends neither a body nor a form, the form types
/// send `form`, every other type sends `body` as a raw template.
inline void applyEditBody(RequestOverride& ov,
                          const QString& bodyType,
                          const QString& body,
                          const std::vector<std::pair<QString, QString>>& form) {
    ov.bodyIsForm = bodyType == QStringLiteral("form-data") ||
                    bodyType == QStringLiteral("x-www-form-urlencoded");
    ov.formFields.clear();
    ov.body.clear();
    if (bodyType == QStringLiteral("none")) {
        return;
    }
    if (ov.bodyIsForm) {
        for (const auto& [key, value] : form) {
            ov.formFields.insert_or_assign(key.toStdString(), value.toStdString());
        }
    } else {
        ov.body = body;
    }
}

/// Whether `contentType` (parameters ignored) is one the editor sets itself for a body type.
[[nodiscard]] inline bool isManagedContentType(const QString& contentType) {
    static const QStringList kManaged{
        QStringLiteral("application/json"),
        QStringLiteral("application/xml"),
        QStringLiteral("text/plain"),
        QStringLiteral("multipart/form-data"),
        QStringLiteral("application/x-www-form-urlencoded"),
    };
    return kManaged.contains(contentType.section(QLatin1Char(';'), 0, 0).trimmed(),
                             Qt::CaseInsensitive);
}

/// `headers` without a Content-Type the editor would have set for a body type itself; a custom
/// value is the user's and stays.
[[nodiscard]] inline std::vector<std::pair<QString, QString>> withoutManagedContentType(
    std::vector<std::pair<QString, QString>> headers) {
    std::erase_if(headers, [](const std::pair<QString, QString>& header) {
        return header.first.compare(QStringLiteral("Content-Type"), Qt::CaseInsensitive) == 0 &&
               isManagedContentType(header.second);
    });
    return headers;
}

}  // namespace reqloom::desktop
