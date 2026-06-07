// NewEndpointDialog — create a new endpoint (engine operation) under a module
// from the GUI. Collects module, name, method, path, and actor; validates the
// name live (non-empty, no id-breaking chars). Chain wiring (depends_on /
// extract) is added in a later step; this dialog reports those as empty.
#pragma once

#include <reqloom/engine/Operation.h>

#include <QtCore/QStringList>
#include <QtWidgets/QDialog>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;

namespace reqloom::desktop {

class NewEndpointDialog : public QDialog {
    Q_OBJECT

public:
    /// `resources` and `actors` populate the pickers. `preselectedResource`
    /// (when non-empty and present in `resources`) is selected and locked-in
    /// as the default — e.g. when launched from a resource folder.
    NewEndpointDialog(const QStringList& resources,
                      const QStringList& actors,
                      const QString& preselectedResource,
                      QWidget* parent = nullptr);
    ~NewEndpointDialog() override;

    NewEndpointDialog(const NewEndpointDialog&) = delete;
    NewEndpointDialog& operator=(const NewEndpointDialog&) = delete;
    NewEndpointDialog(NewEndpointDialog&&) = delete;
    NewEndpointDialog& operator=(NewEndpointDialog&&) = delete;

    [[nodiscard]] QString resourceId() const;
    [[nodiscard]] QString endpointName() const;
    [[nodiscard]] engine::HttpMethod method() const;
    [[nodiscard]] QString pathTemplate() const;
    [[nodiscard]] QString actorId() const;

private:
    void revalidate();

    QComboBox* resourceCombo_{nullptr};
    QLineEdit* nameEdit_{nullptr};
    QComboBox* methodCombo_{nullptr};
    QLineEdit* pathEdit_{nullptr};
    QComboBox* actorCombo_{nullptr};
    QLabel* errorLabel_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
};

}  // namespace reqloom::desktop
