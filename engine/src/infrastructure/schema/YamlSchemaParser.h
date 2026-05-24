#pragma once

#include "SchemaParser.h"

namespace chainapi::engine {

class YamlSchemaParser final : public SchemaParser {
public:
    YamlSchemaParser();
    ~YamlSchemaParser() override;

    SchemaParseResult parse(const std::filesystem::path& rootYaml) override;
};

}  // namespace chainapi::engine
