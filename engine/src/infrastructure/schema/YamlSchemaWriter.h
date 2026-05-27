// YamlSchemaWriter — yaml-cpp-backed implementation of SchemaWriter.
// Importers persist their Project here; provenance round-trips intact.
#pragma once

#include "SchemaWriter.h"

namespace chainapi::engine {

class YamlSchemaWriter final : public SchemaWriter {
public:
    YamlSchemaWriter();
    ~YamlSchemaWriter() override;

    SchemaWriteResult write(const std::filesystem::path& targetDir,
                            const Project& project,
                            bool overwrite = false) override;
};

}  // namespace chainapi::engine
