// NewEndpointDialog — create a new endpoint (engine operation) under a module
// from the GUI. Collects module, name, method, path, and actor; validates the
// name live (non-empty, no id-breaking chars). Chain wiring (depends_on /
// extract) is added in a later step; this dialog reports those as empty.
#pragma once

#include <reqloom/engine/Operation.h>

#include "../theming/Theme.h"

#include <QtCore/QStringList>
#include <QtWidgets/QDialog>

#include <vector>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QToolButton;

namespace reqloom::desktop {

namespace widgets {
class DependencyListEditor;
class ExtractionTableEditor;
}  // namespace widgets

class NewEndpointDialog : public QDialog {
    Q_OBJECT

public:
    /// `resources` and `actors` populate the pickers. `dependencyCandidates`
    /// are the operation ids the optional Chain section may depend on.
    /// `preselectedResource` (when non-empty and present in `resources`) is
    /// selected — e.g. when launched from a resource folder.
    NewEndpointDialog(const QStringList& resources,
                      const QStringList& actors,
                      const QStringList& dependencyCandidates,
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

    /// Apply the app theme to the embedded chain editors (mono fields, fonts).
    void setTheme(const theming::Theme& theme);

    /// Chain wiring from the optional section (empty when collapsed/untouched).
    [[nodiscard]] std::vector<engine::OperationId> dependencies() const;
    [[nodiscard]] std::vector<engine::Extraction> extractions() const;

private:
    void revalidate();

    QComboBox* resourceCombo_{nullptr};
    QLineEdit* nameEdit_{nullptr};
    QComboBox* methodCombo_{nullptr};
    QLineEdit* pathEdit_{nullptr};
    QComboBox* actorCombo_{nullptr};
    QToolButton* chainToggle_{nullptr};
    QWidget* chainSection_{nullptr};
    widgets::DependencyListEditor* dependencyEditor_{nullptr};
    widgets::ExtractionTableEditor* extractionEditor_{nullptr};
    QLabel* errorLabel_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
};

}  // namespace reqloom::desktop
