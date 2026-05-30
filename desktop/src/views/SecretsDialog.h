// Secret management dialog (PRD §13.3 / FR-11.3/11.4). Lists the secrets a
// project references via `{{secret.NAME}}`, shows whether each is present in
// the OS keychain, and lets the user set or clear values. Stored values are
// never displayed — only presence — so secrets can't leak through the UI.
#pragma once

#include "../theming/Theme.h"

#include <QtWidgets/QDialog>

class QLabel;
class QPushButton;
class QTableWidget;

namespace chainapi::desktop {

class ProjectModel;
class SecretManager;

class SecretsDialog : public QDialog {
    Q_OBJECT

public:
    SecretsDialog(SecretManager& secrets,
                  const ProjectModel& project,
                  const theming::Theme& theme,
                  QWidget* parent = nullptr);
    ~SecretsDialog() override;

    SecretsDialog(const SecretsDialog&) = delete;
    SecretsDialog& operator=(const SecretsDialog&) = delete;
    SecretsDialog(SecretsDialog&&) = delete;
    SecretsDialog& operator=(SecretsDialog&&) = delete;

private:
    void refresh();
    void onSetSelected();
    void onClearSelected();
    [[nodiscard]] QString selectedSecretName() const;

    SecretManager& secrets_;
    const ProjectModel& project_;
    const theming::Theme& theme_;

    QLabel* backendBanner_{nullptr};
    QTableWidget* table_{nullptr};
    QPushButton* setButton_{nullptr};
    QPushButton* clearButton_{nullptr};
};

}  // namespace chainapi::desktop
