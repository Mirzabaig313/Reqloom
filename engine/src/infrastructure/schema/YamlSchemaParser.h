#pragma once

#include "SchemaParser.h"

namespace chainapi::engine {

class YamlSchemaParser final : public SchemaParser {
public:
    YamlSchemaParser();
    YamlSchemaParser(const YamlSchemaParser&) = delete;
    YamlSchemaParser& operator=(const YamlSchemaParser&) = delete;
    YamlSchemaParser(YamlSchemaParser&&) = delete;
    YamlSchemaParser& operator=(YamlSchemaParser&&) = delete;
    ~YamlSchemaParser() override;

    SchemaParseResult parse(const std::filesystem::path& rootYaml) override;
};

}  // namespace chainapi::engine
