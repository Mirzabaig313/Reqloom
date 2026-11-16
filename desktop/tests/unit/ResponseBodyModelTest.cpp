// ResponseBodyModel: exact number text, JSONPath addressability against the engine's own path
// validation, and row-level expand/collapse.
#include "ResponseBodyModel.h"

#include <reqloom/engine/Predicate.h>

#include <gtest/gtest.h>

#include <QtCore/QModelIndex>
#include <QtCore/QString>

#include <string>

namespace reqloom::desktop::qml::tests {

namespace {

using Roles = ResponseBodyModel::Roles;

[[nodiscard]] int rowOf(const ResponseBodyModel& model, const QString& field, int depth) {
    for (int row = 0; row < model.rowCount(); ++row) {
        const QModelIndex idx = model.index(row);
        if (model.data(idx, Roles::FieldRole).toString() == field &&
            model.data(idx, Roles::DepthRole).toInt() == depth) {
            return row;
        }
    }
    return -1;
}

[[nodiscard]] QVariant roleOf(const ResponseBodyModel& model,
                              const QString& field,
                              int depth,
                              Roles role) {
    const int row = rowOf(model, field, depth);
    EXPECT_GE(row, 0) << field.toStdString();
    return model.data(model.index(row), role);
}

[[nodiscard]] bool addressable(const ResponseBodyModel& model, const QString& field, int depth) {
    return roleOf(model, field, depth, Roles::AddressableRole).toBool();
}

[[nodiscard]] std::string pathOf(const ResponseBodyModel& model, const QString& field, int depth) {
    return roleOf(model, field, depth, Roles::PathRole).toString().toStdString();
}

void expectEveryAddressablePathValidates(const ResponseBodyModel& model) {
    for (int row = 0; row < model.rowCount(); ++row) {
        const QModelIndex idx = model.index(row);
        const QString path = model.data(idx, Roles::PathRole).toString();
        if (model.data(idx, Roles::AddressableRole).toBool()) {
            EXPECT_TRUE(engine::validateSingleValueJsonPath(path.toStdString()).has_value())
                << path.toStdString();
        } else {
            EXPECT_TRUE(path.isEmpty()) << path.toStdString();
            EXPECT_FALSE(model.data(idx, Roles::PathIssueRole).toString().isEmpty());
        }
    }
}

}  // namespace

TEST(ResponseBodyModel, numbers_keep_the_digits_of_the_response_text) {
    ResponseBodyModel model;
    model.setBody(QStringLiteral(
        R"({"big":9007199254740993,"max":18446744073709551615,"min":-9223372036854775808,)"
        R"("wide":123456789012345678901234567890,"dec":1.10,"exp":2.5E-3})"));

    EXPECT_EQ(roleOf(model, "big", 0, Roles::RawValueRole).toString().toStdString(),
              "9007199254740993");
    EXPECT_EQ(roleOf(model, "big", 0, Roles::ValueRole).toString().toStdString(),
              "9007199254740993");
    EXPECT_EQ(roleOf(model, "max", 0, Roles::RawValueRole).toString().toStdString(),
              "18446744073709551615");
    EXPECT_EQ(roleOf(model, "min", 0, Roles::RawValueRole).toString().toStdString(),
              "-9223372036854775808");
    EXPECT_EQ(roleOf(model, "wide", 0, Roles::RawValueRole).toString().toStdString(),
              "123456789012345678901234567890");
    EXPECT_EQ(roleOf(model, "dec", 0, Roles::RawValueRole).toString().toStdString(), "1.10");
    EXPECT_EQ(roleOf(model, "exp", 0, Roles::RawValueRole).toString().toStdString(), "2.5E-3");
}

TEST(ResponseBodyModel, dotted_key_is_not_addressable_and_never_aliases_a_nested_path) {
    ResponseBodyModel model;
    model.setBody(QStringLiteral(R"({"a.b":1,"a":{"b":2}})"));

    EXPECT_FALSE(addressable(model, "a.b", 0));
    EXPECT_TRUE(pathOf(model, "a.b", 0).empty());
    EXPECT_TRUE(addressable(model, "a", 0));
    EXPECT_EQ(pathOf(model, "b", 1), "$.a.b");
    expectEveryAddressablePathValidates(model);
}

TEST(ResponseBodyModel, bracket_and_empty_keys_and_their_children_are_not_addressable) {
    ResponseBodyModel model;
    model.setBody(QStringLiteral(R"({"x[0]":{"y":1},"":2,"c]":3,"ok":[{"z":4}]})"));

    EXPECT_FALSE(addressable(model, "x[0]", 0));
    EXPECT_FALSE(addressable(model, "y", 1));
    EXPECT_FALSE(addressable(model, "", 0));
    // The engine's parser ends a field name only at '.' or '[', so ']' inside a key is addressable.
    EXPECT_TRUE(addressable(model, "c]", 0));
    EXPECT_EQ(pathOf(model, "z", 2), "$.ok[0].z");
    expectEveryAddressablePathValidates(model);
}

TEST(ResponseBodyModel, earlier_duplicate_key_is_not_addressable) {
    ResponseBodyModel model;
    model.setBody(QStringLiteral(R"({"k":{"v":1},"k":2})"));

    ASSERT_EQ(model.rowCount(), 3);
    EXPECT_FALSE(model.data(model.index(0), Roles::AddressableRole).toBool());
    EXPECT_FALSE(model.data(model.index(1), Roles::AddressableRole).toBool());
    EXPECT_TRUE(model.data(model.index(2), Roles::AddressableRole).toBool());
    EXPECT_EQ(model.data(model.index(2), Roles::PathRole).toString(), "$.k");
}

TEST(ResponseBodyModel, paths_the_engine_reads_as_header_cookie_or_status_are_not_addressable) {
    ResponseBodyModel model;
    model.setBody(QStringLiteral(
        R"({"headers":{"h":1},"cookies":{"c":1},"status_code":5,"data":{"status_code":6}})"));

    EXPECT_TRUE(addressable(model, "headers", 0));
    EXPECT_FALSE(addressable(model, "h", 1));
    EXPECT_FALSE(addressable(model, "c", 1));
    EXPECT_FALSE(addressable(model, "status_code", 0));
    EXPECT_TRUE(addressable(model, "status_code", 1));
}

TEST(ResponseBodyModel, body_nested_deeper_than_the_engine_extracts_from_is_not_addressable) {
    ResponseBodyModel model;
    model.setBody(QStringLiteral(R"({"a":)") + QString(300, QLatin1Char('[')) +
                  QString(300, QLatin1Char(']')) + QStringLiteral("}"));

    ASSERT_GT(model.rowCount(), 0);
    EXPECT_FALSE(model.data(model.index(0), Roles::AddressableRole).toBool());
}

TEST(ResponseBodyModel, top_level_scalar_is_one_addressable_row) {
    ResponseBodyModel model;
    model.setBody(QStringLiteral("12345678901234567890123"));

    ASSERT_TRUE(model.isJson());
    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_EQ(model.data(model.index(0), Roles::RawValueRole).toString().toStdString(),
              "12345678901234567890123");
    EXPECT_EQ(model.data(model.index(0), Roles::PathRole).toString(), "$");
}

TEST(ResponseBodyModel, non_json_body_is_one_raw_row) {
    ResponseBodyModel model;
    model.setBody(QStringLiteral("<html>"));

    EXPECT_FALSE(model.isJson());
    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_EQ(model.data(model.index(0), Roles::RawValueRole).toString().toStdString(), "<html>");
}

TEST(ResponseBodyModel, toggle_removes_and_inserts_only_the_affected_rows) {
    ResponseBodyModel model;
    model.setBody(QStringLiteral(R"({"o":{"a":1,"in":{"x":1,"y":2}},"z":3})"));
    ASSERT_EQ(model.rowCount(), 6);
    int resets = 0;
    int removedFirst = -1;
    int removedLast = -1;
    int insertedFirst = -1;
    int insertedLast = -1;
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&] { ++resets; });
    QObject::connect(&model,
                     &QAbstractItemModel::rowsRemoved,
                     &model,
                     [&](const QModelIndex&, int first, int last) {
                         removedFirst = first;
                         removedLast = last;
                     });
    QObject::connect(&model,
                     &QAbstractItemModel::rowsInserted,
                     &model,
                     [&](const QModelIndex&, int first, int last) {
                         insertedFirst = first;
                         insertedLast = last;
                     });

    model.toggle(rowOf(model, "in", 1));
    EXPECT_EQ(removedFirst, 3);
    EXPECT_EQ(removedLast, 4);
    model.toggle(0);
    EXPECT_EQ(removedFirst, 1);
    EXPECT_EQ(removedLast, 2);
    ASSERT_EQ(model.rowCount(), 2);
    model.toggle(0);

    EXPECT_EQ(resets, 0);
    EXPECT_EQ(insertedFirst, 1);
    EXPECT_EQ(insertedLast, 2);
    ASSERT_EQ(model.rowCount(), 4);
    EXPECT_TRUE(model.data(model.index(2), Roles::CollapsedRole).toBool());
    EXPECT_EQ(model.data(model.index(3), Roles::FieldRole).toString(), "z");
}

}  // namespace reqloom::desktop::qml::tests
