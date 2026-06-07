// NewModuleDialog — create a new module (engine resource) from the GUI instead
// of hand-editing resources/<id>.yaml. Collects a name + optional description
// and validates the name live (non-empty, no id-breaking '.', '/', '\').
#pragma once

#include <QtWidgets/QDialog>

class QDialogButtonBox;
class QLabel;
class QLineEdit;

namespace reqloom::desktop {

class NewModuleDialog : public QDialog {
    Q_OBJECT

public:
    explicit NewModuleDialog(QWidget* parent = nullptr);
    ~NewModuleDialog() override;

    NewModuleDialog(const NewModuleDialog&) = delete;
    NewModuleDialog& operator=(const NewModuleDialog&) = delete;
    NewModuleDialog(NewModuleDialog&&) = delete;
    NewModuleDialog& operator=(NewModuleDialog&&) = delete;

    [[nodiscard]] QString moduleName() const;
    [[nodiscard]] QString description() const;

private:
    void revalidate();

    QLineEdit* nameEdit_{nullptr};
    QLineEdit* descriptionEdit_{nullptr};
    QLabel* errorLabel_{nullptr};
    QLabel* hintLabel_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
};

}  // namespace reqloom::desktop
