// Shared expectations for importer tests: file fields carrying paths from
// the exporting machine are reduced to base names with warnings.
//
// Each importer test builds a form with these fields:
//   unix   (file) /Users/x/.ssh/id_rsa   -> @id_rsa      + warning
//   win    (file) C:\Users\x\a.png       -> @a.png       + warning
//   rel    (file) ../../secret.txt       -> @secret.txt  + warning
//   bare   (file) photo.png              -> @photo.png   (no warning)
//   sneaky (text) @/etc/passwd           -> @passwd      + warning
#pragma once

#include <gtest/gtest.h>

#include <map>
#include <string>

namespace reqloom::tests {

inline void expectImportedFileRefsSanitised(const std::map<std::string, std::string>& form,
                                            const std::string& warnings,
                                            const std::string& opId,
                                            const bool hasTextField = true) {
    EXPECT_EQ(form.at("unix"), "@id_rsa");
    EXPECT_EQ(form.at("win"), "@a.png");
    EXPECT_EQ(form.at("rel"), "@secret.txt");
    EXPECT_EQ(form.at("bare"), "@photo.png");
    for (const char* field : {"`unix`", "`win`", "`rel`"}) {
        EXPECT_NE(warnings.find(field), std::string::npos) << field << '\n' << warnings;
    }
    EXPECT_EQ(warnings.find("`bare`"), std::string::npos) << warnings;
    EXPECT_NE(warnings.find("operation " + opId), std::string::npos) << warnings;
    EXPECT_NE(warnings.find("current working directory"), std::string::npos) << warnings;
    if (hasTextField) {
        EXPECT_EQ(form.at("sneaky"), "@passwd");
        EXPECT_NE(warnings.find("`sneaky`"), std::string::npos) << warnings;
    }
}

}  // namespace reqloom::tests
