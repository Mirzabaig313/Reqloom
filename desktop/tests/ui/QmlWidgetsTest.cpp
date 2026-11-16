// Keyboard and focus behaviour of the editor completion widgets and the toast
// stack, driven through a real QQuickWindow. The widgets load from desktop/qml;
// the Reqloom module they import is a stub under tests/ui/qml (see its qmldir).
#include <gtest/gtest.h>

#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QTest>

#include <memory>

namespace reqloom::desktop::tests {

namespace {

class QmlScene {
public:
    explicit QmlScene(const QString& body) {
        engine_.addImportPath(QStringLiteral(REQLOOM_QML_TEST_STUBS_DIR));
        // The override lets the same tests run against another checkout of the widgets.
        const QString dir = qEnvironmentVariableIsSet("REQLOOM_QML_WIDGETS_DIR")
                                ? qEnvironmentVariable("REQLOOM_QML_WIDGETS_DIR")
                                : QStringLiteral(REQLOOM_QML_WIDGETS_DIR);
        const QString widgets = QUrl::fromLocalFile(dir).toString();
        const QString source = QStringLiteral(
                                   "import QtQuick\n"
                                   "import QtQuick.Controls.Basic\n"
                                   "import Reqloom\n"
                                   "import \"%1\"\n"
                                   "Window { width: 640; height: 480; visible: true\n%2\n}\n")
                                   .arg(widgets, body);
        QQmlComponent component(&engine_);
        component.setData(
            source.toUtf8(),
            QUrl::fromLocalFile(QStringLiteral(REQLOOM_QML_TEST_STUBS_DIR "/scene.qml")));
        window_.reset(qobject_cast<QQuickWindow*>(component.create()));
        error_ = component.errorString();
        if (window_) {
            window_->requestActivate();
            ok_ = QTest::qWaitForWindowActive(window_.get());
        }
    }

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] QQuickWindow* window() const { return window_.get(); }

    [[nodiscard]] QObject* find(const char* name) const {
        return window_->findChild<QObject*>(QString::fromLatin1(name));
    }

    [[nodiscard]] QObject* stubController() {
        return engine_.singletonInstance<QObject*>(QStringLiteral("Reqloom"),
                                                   QStringLiteral("AppController"));
    }

    void type(const char* text) {
        for (const char* c = text; *c != '\0'; ++c) {
            QTest::keyClick(window_.get(), *c);
            settle();
        }
    }

    void key(Qt::Key k) {
        QTest::keyClick(window_.get(), k);
        settle();
    }

    // Qt.callLater and popup state changes land on later event-loop turns.
    static void settle() { QTest::qWait(30); }

private:
    QQmlEngine engine_;
    std::unique_ptr<QQuickWindow> window_;
    QString error_;
    bool ok_{false};
};

// The first Popup declared inside `owner`; its `visible` is what the user sees.
bool popupVisible(QObject* owner) {
    for (QObject* child : owner->findChildren<QObject*>()) {
        if (child->inherits("QQuickPopup")) {
            return child->property("visible").toBool();
        }
    }
    ADD_FAILURE() << "no Popup under " << owner->objectName().toStdString();
    return false;
}

void focus(QObject* item) {
    QMetaObject::invokeMethod(item, "forceActiveFocus");
    QmlScene::settle();
}

QString text(QObject* field) {
    return field->property("text").toString();
}

const char* kHeaderScene = R"(
    Column {
        TextField {
            id: keyField
            objectName: "keyField"
            width: 300
            Keys.forwardTo: [ac.keyTarget]
            HeaderNameAutocomplete {
                id: ac
                objectName: "ac"
                field: keyField
                onPicked: function (name) { keyField.text = name; }
            }
        }
        TextField { objectName: "next"; width: 300 }
    }
)";

}  // namespace

TEST(HeaderNameAutocomplete, focus_alone_shows_nothing_and_tab_leaves_empty_row_empty) {
    QmlScene scene(QString::fromLatin1(kHeaderScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    QObject* field = scene.find("keyField");
    focus(field);
    EXPECT_FALSE(popupVisible(scene.find("ac")));
    scene.key(Qt::Key_Tab);
    EXPECT_EQ(text(field), QString());
    EXPECT_TRUE(scene.find("next")->property("activeFocus").toBool());
}

TEST(HeaderNameAutocomplete, tab_past_existing_key_keeps_it) {
    QmlScene scene(QString::fromLatin1(kHeaderScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    QObject* field = scene.find("keyField");
    field->setProperty("text", QStringLiteral("Authorization"));
    focus(field);
    EXPECT_FALSE(popupVisible(scene.find("ac")));
    scene.key(Qt::Key_Tab);
    EXPECT_EQ(text(field), QStringLiteral("Authorization"));
}

TEST(HeaderNameAutocomplete, typing_opens_with_prefix_matches_first) {
    QmlScene scene(QString::fromLatin1(kHeaderScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    QObject* ac = scene.find("ac");
    focus(scene.find("keyField"));
    scene.type("auth");
    ASSERT_TRUE(popupVisible(ac));
    const QVariantList items = ac->property("_filtered").toList();
    ASSERT_GE(items.size(), 2);
    EXPECT_EQ(items.at(0).toString(), QStringLiteral("Authorization"));
    EXPECT_EQ(items.at(1).toString(), QStringLiteral("Proxy-Authorization"));
}

TEST(HeaderNameAutocomplete, tab_and_plain_enter_never_accept) {
    QmlScene scene(QString::fromLatin1(kHeaderScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    QObject* field = scene.find("keyField");
    focus(field);
    scene.type("acc");
    ASSERT_TRUE(popupVisible(scene.find("ac")));
    scene.key(Qt::Key_Return);
    EXPECT_EQ(text(field), QStringLiteral("acc"));
    EXPECT_FALSE(popupVisible(scene.find("ac")));

    scene.type("e");
    ASSERT_TRUE(popupVisible(scene.find("ac")));
    scene.key(Qt::Key_Tab);
    EXPECT_EQ(text(field), QStringLiteral("acce"));
    EXPECT_FALSE(popupVisible(scene.find("ac")));
}

TEST(HeaderNameAutocomplete, enter_accepts_a_row_chosen_with_arrows) {
    QmlScene scene(QString::fromLatin1(kHeaderScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    QObject* field = scene.find("keyField");
    focus(field);
    scene.type("auth");
    scene.key(Qt::Key_Down);
    scene.key(Qt::Key_Return);
    EXPECT_EQ(text(field), QStringLiteral("Authorization"));
}

TEST(HeaderNameAutocomplete, exact_match_shows_no_popup) {
    QmlScene scene(QString::fromLatin1(kHeaderScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    focus(scene.find("keyField"));
    scene.type("accept");
    EXPECT_FALSE(popupVisible(scene.find("ac")));
}

namespace {

const char* kVariableScene = R"(
    TextField {
        id: f
        objectName: "f"
        width: 400
        Keys.forwardTo: [vac.keyTarget]
        VariableAutocomplete {
            id: vac
            objectName: "vac"
            field: f
            operationId: "users.get"
        }
    }
)";

}  // namespace

TEST(VariableAutocomplete, caret_moved_into_placeholder_shows_nothing) {
    QmlScene scene(QString::fromLatin1(kVariableScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    QObject* field = scene.find("f");
    field->setProperty("text", QStringLiteral("/x/{{ab"));
    focus(field);
    field->setProperty("cursorPosition", 7);
    QmlScene::settle();
    EXPECT_FALSE(popupVisible(scene.find("vac")));
}

TEST(VariableAutocomplete, pick_inside_closed_placeholder_replaces_through_braces) {
    QmlScene scene(QString::fromLatin1(kVariableScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    QObject* field = scene.find("f");
    field->setProperty("text", QStringLiteral("/x/{{a}}/y"));
    focus(field);
    field->setProperty("cursorPosition", 6);
    scene.type("b");
    ASSERT_TRUE(popupVisible(scene.find("vac")));
    scene.key(Qt::Key_Down);
    scene.key(Qt::Key_Return);
    EXPECT_EQ(text(field), QStringLiteral("/x/{{abc.id}}/y"));
}

TEST(VariableAutocomplete, tab_and_plain_enter_never_accept) {
    QmlScene scene(QString::fromLatin1(kVariableScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    QObject* field = scene.find("f");
    focus(field);
    scene.type("{{us");
    ASSERT_TRUE(popupVisible(scene.find("vac")));
    scene.key(Qt::Key_Return);
    EXPECT_EQ(text(field), QStringLiteral("{{us"));
    scene.type("e");
    ASSERT_TRUE(popupVisible(scene.find("vac")));
    scene.key(Qt::Key_Tab);
    EXPECT_EQ(text(field), QStringLiteral("{{use"));
    EXPECT_FALSE(popupVisible(scene.find("vac")));
}

TEST(VariableAutocomplete, prefix_matches_rank_first) {
    QmlScene scene(QString::fromLatin1(kVariableScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    QObject* vac = scene.find("vac");
    focus(scene.find("f"));
    scene.type("{{ta");
    ASSERT_TRUE(popupVisible(vac));
    const QVariantList items = vac->property("_filtered").toList();
    ASSERT_FALSE(items.isEmpty());
    EXPECT_EQ(items.at(0).toMap().value(QStringLiteral("token")).toString(), QStringLiteral("tab"));
}

namespace {

const char* kValuePickerScene = R"(
    TextField {
        id: f
        objectName: "f"
        width: 400
        ValuePicker {
            objectName: "vp"
            field: f
        }
    }
)";

}  // namespace

TEST(ValuePicker, caret_hint_keeps_typing_in_the_editor_and_pins_nothing) {
    QmlScene scene(QString::fromLatin1(kValuePickerScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    QObject* field = scene.find("f");
    field->setProperty("text", QStringLiteral("/users/{{users.id}}"));
    focus(field);
    field->setProperty("cursorPosition", 12);
    QmlScene::settle();
    ASSERT_TRUE(popupVisible(scene.find("vp")));
    EXPECT_TRUE(field->property("activeFocus").toBool());

    scene.type("9");
    scene.key(Qt::Key_Return);
    EXPECT_EQ(text(field), QStringLiteral("/users/{{use9rs.id}}"));
    EXPECT_TRUE(scene.stubController()->property("pins").toList().isEmpty());
}

namespace {

const char* kPathScene = R"(
    TextField {
        id: f
        objectName: "f"
        width: 400
        PathAutocomplete {
            objectName: "pac"
            field: f
            enabled: !f.readOnly
            operationId: "users.list"
            onPicked: function (p) { f.text = p; }
        }
    }
)";

}  // namespace

TEST(PathAutocomplete, read_only_field_never_opens) {
    QmlScene scene(QString::fromLatin1(kPathScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    QObject* field = scene.find("f");
    field->setProperty("text", QStringLiteral("data"));
    field->setProperty("readOnly", true);
    focus(field);
    EXPECT_FALSE(popupVisible(scene.find("pac")));
}

TEST(PathAutocomplete, opens_on_typing_only_and_not_for_an_exact_path) {
    QmlScene scene(QString::fromLatin1(kPathScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    QObject* field = scene.find("f");
    QObject* pac = scene.find("pac");
    field->setProperty("text", QStringLiteral("dat"));
    focus(field);
    EXPECT_FALSE(popupVisible(pac));
    scene.type("a");
    EXPECT_TRUE(popupVisible(pac));
    field->setProperty("text", QString());
    scene.type("id");
    EXPECT_FALSE(popupVisible(pac));
}

namespace {

const char* kToastScene = R"(
    Toast {
        objectName: "toast"
        duration: 100
    }
)";

}  // namespace

TEST(Toast, error_stays_while_info_times_out_and_neither_overwrites) {
    QmlScene scene(QString::fromLatin1(kToastScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    QObject* toast = scene.find("toast");
    QMetaObject::invokeMethod(toast,
                              "show",
                              Q_ARG(QVariant, QStringLiteral("boom at a.yaml:3:5")),
                              Q_ARG(QVariant, true),
                              Q_ARG(QVariant, QVariant{}));
    QMetaObject::invokeMethod(toast,
                              "show",
                              Q_ARG(QVariant, QStringLiteral("Saved")),
                              Q_ARG(QVariant, false),
                              Q_ARG(QVariant, QVariant{}));
    QmlScene::settle();
    EXPECT_EQ(toast->property("count").toInt(), 2);
    QTest::qWait(400);
    EXPECT_EQ(toast->property("count").toInt(), 1);
    EXPECT_TRUE(toast->property("visible").toBool());
}

TEST(Toast, a_warning_stays_until_dismissed_and_is_not_an_error) {
    QmlScene scene(QString::fromLatin1(kToastScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    QObject* toast = scene.find("toast");
    QMetaObject::invokeMethod(
        toast, "warn", Q_ARG(QVariant, QStringLiteral("loaded with 1 warning: a.yaml:8:1")));
    QmlScene::settle();
    QTest::qWait(400);

    ASSERT_EQ(toast->property("count").toInt(), 1);
    QVariant kind;
    QMetaObject::invokeMethod(toast, "kindAt", Q_RETURN_ARG(QVariant, kind), Q_ARG(QVariant, 0));
    EXPECT_EQ(kind.toString(), QStringLiteral("warning"));
}

TEST(Toast, stack_is_bounded_and_counts_the_rest) {
    QmlScene scene(QString::fromLatin1(kToastScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    QObject* toast = scene.find("toast");
    for (int i = 0; i < 5; ++i) {
        QMetaObject::invokeMethod(toast,
                                  "show",
                                  Q_ARG(QVariant, QStringLiteral("error %1").arg(i)),
                                  Q_ARG(QVariant, true),
                                  Q_ARG(QVariant, QVariant{}));
    }
    QmlScene::settle();
    EXPECT_EQ(toast->property("count").toInt(), 5);
    EXPECT_EQ(toast->property("hiddenCount").toInt(), 2);
    QMetaObject::invokeMethod(toast, "dismissAll");
    QmlScene::settle();
    EXPECT_EQ(toast->property("count").toInt(), 0);
}

namespace {

const char* kUnsavedScene = R"(
    UnsavedChangesDialog {
        id: dlg
        objectName: "dlg"
        property int answered: 0
        property int last: -1
        onChoice: value => { dlg.last = value; dlg.answered += 1; }
    }
)";

void askUnsaved(QObject* dialog) {
    QMetaObject::invokeMethod(dialog,
                              "ask",
                              Q_ARG(QVariant, QStringLiteral("Unsaved changes")),
                              Q_ARG(QVariant, QStringLiteral("2 tabs have unsaved changes: a, b.")),
                              Q_ARG(QVariant, QStringLiteral("Save all")),
                              Q_ARG(QVariant, QStringLiteral("Discard all")),
                              Q_ARG(QVariant, QStringLiteral("Cancel")));
    QmlScene::settle();
}

int answerWith(const char* button) {
    QmlScene scene(QString::fromLatin1(kUnsavedScene));
    if (!scene.ok()) {
        ADD_FAILURE() << scene.error().toStdString();
        return -2;
    }
    QObject* dialog = scene.find("dlg");
    askUnsaved(dialog);
    QObject* target = scene.find(button);
    if (target == nullptr) {
        ADD_FAILURE() << "no " << button;
        return -2;
    }
    QMetaObject::invokeMethod(target, "click");
    QmlScene::settle();
    EXPECT_EQ(dialog->property("answered").toInt(), 1);
    EXPECT_FALSE(dialog->property("visible").toBool());
    return dialog->property("last").toInt();
}

}  // namespace

TEST(UnsavedChangesDialog, shows_the_texts_it_is_asked_with) {
    QmlScene scene(QString::fromLatin1(kUnsavedScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    askUnsaved(scene.find("dlg"));
    EXPECT_TRUE(scene.find("dlg")->property("visible").toBool());
    EXPECT_EQ(text(scene.find("unsavedChangesMessage")),
              QStringLiteral("2 tabs have unsaved changes: a, b."));
    EXPECT_EQ(text(scene.find("unsavedSaveButton")), QStringLiteral("Save all"));
    EXPECT_EQ(text(scene.find("unsavedDiscardButton")), QStringLiteral("Discard all"));
    EXPECT_EQ(text(scene.find("unsavedCancelButton")), QStringLiteral("Cancel"));
}

TEST(UnsavedChangesDialog, each_button_answers_once_with_its_choice) {
    EXPECT_EQ(answerWith("unsavedSaveButton"), 0);
    EXPECT_EQ(answerWith("unsavedDiscardButton"), 1);
    EXPECT_EQ(answerWith("unsavedCancelButton"), 2);
}

TEST(UnsavedChangesDialog, escape_answers_cancel) {
    QmlScene scene(QString::fromLatin1(kUnsavedScene));
    ASSERT_TRUE(scene.ok()) << scene.error().toStdString();
    QObject* dialog = scene.find("dlg");
    askUnsaved(dialog);
    scene.key(Qt::Key_Escape);
    EXPECT_EQ(dialog->property("answered").toInt(), 1);
    EXPECT_EQ(dialog->property("last").toInt(), 2);
}

}  // namespace reqloom::desktop::tests
