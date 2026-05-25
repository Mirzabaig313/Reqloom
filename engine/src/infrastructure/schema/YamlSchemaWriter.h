// YamlSchemaWriter — yaml-cpp-backed implementation of SchemaWriter.
// PRD §10.3.6: importers persist their Project here; provenance round-trips.
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
