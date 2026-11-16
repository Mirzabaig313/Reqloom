// Manage Secrets dialog behaviour through a real QQuickWindow, against the
// SecretsController stub under tests/ui/qml. Views resolve the widgets from
// their own directory in the app module, so the test copies both into one
// temporary directory.
#include <gtest/gtest.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QString>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrl>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QTest>

#include <memory>

namespace reqloom::desktop::tests {

namespace {

QString sourceDir(const char* variable, const char* fallback) {
    return qEnvironmentVariableIsSet(variable) ? qEnvironmentVariable(variable)
                                               : QString::fromUtf8(fallback);
}

bool copyQml(const QString& from, const QString& to) {
    const QDir dir(from);
    for (const QString& name : dir.entryList({QStringLiteral("*.qml")}, QDir::Files)) {
        if (!QFile::copy(dir.filePath(name), QDir(to).filePath(name))) {
            return false;
        }
    }
    return true;
}

class SecretsScene {
public:
    SecretsScene() {
        // Overrides let the same tests run against another checkout of the views.
        ok_ = dir_.isValid() &&
              copyQml(sourceDir("REQLOOM_QML_WIDGETS_DIR", REQLOOM_QML_WIDGETS_DIR), dir_.path()) &&
              copyQml(sourceDir("REQLOOM_QML_VIEWS_DIR", REQLOOM_QML_VIEWS_DIR), dir_.path());
        if (!ok_) {
            error_ = QStringLiteral("could not copy the QML sources");
            return;
        }
        engine_.addImportPath(QStringLiteral(REQLOOM_QML_TEST_STUBS_DIR));
        const QString source = QStringLiteral(
                                   "import QtQuick\nimport Reqloom\nimport \"%1\"\n"
                                   "Window { width: 900; height: 700; visible: true\n"
                                   "SecretsDialog { objectName: \"dlg\" }\n}\n")
                                   .arg(QUrl::fromLocalFile(dir_.path()).toString());
        QQmlComponent component(&engine_);
        component.setData(
            source.toUtf8(),
            QUrl::fromLocalFile(QStringLiteral(REQLOOM_QML_TEST_STUBS_DIR "/scene.qml")));
        window_.reset(qobject_cast<QQuickWindow*>(component.create()));
        error_ = component.errorString();
        ok_ = window_ != nullptr;
        if (ok_) {
            window_->requestActivate();
            ok_ = QTest::qWaitForWindowActive(window_.get());
        }
    }

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] QString error() const { return error_; }

    [[nodiscard]] QObject* find(const char* name) const {
        const QString wanted = QString::fromLatin1(name);
        if (auto* found = window_->findChild<QObject*>(wanted)) {
            return found;
        }
        // ListView delegates have a visual parent only, so findChild misses them.
        return findItem(window_->contentItem(), wanted);
    }

    [[nodiscard]] QObject* findByText(const QString& text) const {
        for (QObject* child : window_->findChildren<QObject*>()) {
            if (child->inherits("QQuickAbstractButton") && child->property("text") == text) {
                return child;
            }
        }
        return nullptr;
    }

    [[nodiscard]] QObject* stub() {
        return engine_.singletonInstance<QObject*>(QStringLiteral("Reqloom"),
                                                   QStringLiteral("SecretsController"));
    }

    void open() {
        QMetaObject::invokeMethod(find("dlg"), "openDialog");
        settle();
    }

    void type(const char* text) {
        for (const char* c = text; *c != '\0'; ++c) {
            QTest::keyClick(window_.get(), *c);
        }
        settle();
    }

    void key(Qt::Key k) {
        QTest::keyClick(window_.get(), k);
        settle();
    }

    static void click(QObject* button) {
        ASSERT_NE(button, nullptr);
        QMetaObject::invokeMethod(button, "click");
        settle();
    }

    // Popup transitions and Qt.callLater land on later event-loop turns.
    static void settle() { QTest::qWait(400); }

private:
    static QQuickItem* findItem(QQuickItem* item, const QString& name) {
        if (item->objectName() == name) {
            return item;
        }
        for (QQuickItem* child : item->childItems()) {
            if (auto* found = findItem(child, name)) {
                return found;
            }
        }
        return nullptr;
    }

    QTemporaryDir dir_;
    QQmlEngine engine_;
    std::unique_ptr<QQuickWindow> window_;
    QString error_;
    bool ok_{false};
};

QString text(QObject* object) {
    return object == nullptr ? QStringLiteral("<missing>") : object->property("text").toString();
}

bool shown(QObject* item) {
    return item != nullptr && item->property("visible").toBool();
}

}  // namespace

TEST(SecretsDialog, saving_an_empty_value_shows_why_in_the_set_dialog) {
    SecretsScene scene;
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    scene.open();
    SecretsScene::click(scene.find("setSecretButton"));

    scene.key(Qt::Key_Return);

    EXPECT_TRUE(shown(scene.find("secretValueField")));
    EXPECT_EQ(text(scene.find("setSecretError")),
              QStringLiteral("Failed to set secret: Enter a value."));
}

TEST(SecretsDialog, a_failed_save_stays_in_the_set_dialog) {
    SecretsScene scene;
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    scene.stub()->setProperty("failStore", true);
    scene.open();
    SecretsScene::click(scene.find("setSecretButton"));

    scene.type("value");
    scene.key(Qt::Key_Return);

    EXPECT_TRUE(shown(scene.find("setSecretError")));
    EXPECT_EQ(text(scene.find("setSecretError")),
              QStringLiteral("Failed to set secret: keychain locked"));
}

TEST(SecretsDialog, typed_value_is_cleared_after_a_successful_save) {
    SecretsScene scene;
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    scene.open();
    SecretsScene::click(scene.find("setSecretButton"));

    scene.type("value");
    scene.key(Qt::Key_Return);

    EXPECT_EQ(scene.stub()->property("storedCount").toInt(), 1);
    EXPECT_FALSE(shown(scene.find("secretValueField")));
    EXPECT_EQ(text(scene.find("secretValueField")), QString{});
}

TEST(SecretsDialog, closing_the_dialog_clears_the_typed_value) {
    SecretsScene scene;
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    scene.open();
    SecretsScene::click(scene.find("setSecretButton"));
    scene.type("value");

    QMetaObject::invokeMethod(scene.find("dlg"), "close");
    SecretsScene::settle();

    EXPECT_FALSE(shown(scene.find("secretValueField")));
    EXPECT_EQ(text(scene.find("secretValueField")), QString{});
}

TEST(SecretsDialog, move_names_both_folders_and_confirms_the_folder_shown) {
    SecretsScene scene;
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    scene.stub()->setProperty("folderConflict", QStringLiteral("/projects/old"));
    scene.open();
    SecretsScene::click(scene.find("confirmMoveButton"));
    const QString folders = text(scene.find("moveFolders"));
    scene.stub()->setProperty("folderConflict", QStringLiteral("/projects/other"));

    SecretsScene::click(scene.findByText(QStringLiteral("Use secrets here")));

    EXPECT_TRUE(folders.contains(QStringLiteral("/projects/old")));
    EXPECT_TRUE(folders.contains(QStringLiteral("/projects/here")));
    EXPECT_EQ(scene.stub()->property("confirmedOwner").toString(), QStringLiteral("/projects/old"));
    EXPECT_EQ(text(scene.find("moveError")),
              QStringLiteral("Couldn't use the secrets here: refused"));
}

TEST(SecretsDialog, conflict_banner_names_the_folder_open_here_too) {
    SecretsScene scene;
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    scene.stub()->setProperty("folderConflict", QStringLiteral("/projects/old"));
    scene.open();

    const QString banner = text(scene.find("conflictText"));

    EXPECT_TRUE(banner.contains(QStringLiteral("/projects/old"))) << banner.toStdString();
    EXPECT_TRUE(banner.contains(QStringLiteral("/projects/here"))) << banner.toStdString();
}

TEST(SecretsDialog, folder_texts_break_between_words_not_inside_them) {
    SecretsScene scene;
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    scene.stub()->setProperty("folderConflict", QStringLiteral("/projects/old"));
    scene.open();
    SecretsScene::click(scene.find("confirmMoveButton"));

    // Text.Wrap: at word boundaries, and inside a word only when it is longer than a line.
    constexpr int kWrapAtWordBoundaryOrAnywhere = 4;
    for (const char* name : {"moveFolders", "conflictText"}) {
        QObject* label = scene.find(name);
        ASSERT_NE(label, nullptr) << name;
        EXPECT_EQ(label->property("wrapMode").toInt(), kWrapAtWordBoundaryOrAnywhere) << name;
    }
}

}  // namespace reqloom::desktop::tests
