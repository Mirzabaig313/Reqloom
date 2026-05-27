#pragma once

#include <QtWidgets/QMainWindow>

namespace chainapi::desktop {

/// Shell window. Explorer / request editor / response viewer / timeline
/// layout fills in once the UI panels land.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;
    ~MainWindow() override;
};

}  // namespace chainapi::desktop
