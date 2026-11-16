// How to cite GeometryView: the apps' citation is the repository's, in CITATION.cff and the
// README, and the version the apps give is the one the citation file names.
#include "base/about.h"
#include "base/types.h"
#include "engine.h"

#include <doctest/doctest.h>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

using namespace geo;

namespace {

std::string readSource(const char* path) {
    std::ifstream file(std::string(GEO_TEST_SOURCE_DIR) + "/" + path);
    REQUIRE(file.good());
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// The value of the first line that starts with `name: `, unquoted.
std::string field(const std::string& text, std::string_view name) {
    const std::string key = "\n" + std::string(name) + ": ";
    const usize at = text.find(key);
    REQUIRE(at != std::string::npos);
    const usize start = at + key.size();
    std::string value = text.substr(start, text.find('\n', start) - start);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

} // namespace

TEST_CASE("the apps cite GeometryView as its citation file does, at the version it names") {
    const std::string cff = readSource("CITATION.cff");
    CHECK(field(cff, "title") == about::citationTitle());
    CHECK(field(cff, "repository-code") == about::citationUrl());
    CHECK(field(cff, "version") == geo_engine_version());
    CHECK(std::string_view(geo_engine_version()) == about::version());

    CHECK(std::string_view(geo_citation(GeoCitationText)) == about::citationText());
    CHECK(std::string_view(geo_citation(GeoCitationBibTeX)) == about::citationBibtex());
    CHECK(about::citationText() ==
          "Aalok, A. (2026). GeometryView: A STEP and STL viewer for people and AI agents "
          "[Computer software]. https://github.com/atharvaaalok/geometryview");
}

TEST_CASE("the README's citation is the apps', word for word") {
    const std::string readme = readSource("README.md");
    // Its BibTeX block, exactly.
    CHECK(readme.find("```bibtex\n" + about::citationBibtex() + "```") != std::string::npos);
    // Its text, a quoted paragraph with the title in italics.
    const usize start = readme.find("> Aalok, A.");
    REQUIRE(start != std::string::npos);
    const std::string quote = readme.substr(start + 2, readme.find("\n\n", start) - start - 2);
    std::string text;
    for (usize i = 0; i < quote.size(); ++i) {
        if (quote.compare(i, 3, "\n> ") == 0) {
            text += ' ';
            i += 2;
        } else if (quote[i] != '*') {
            text += quote[i];
        }
    }
    CHECK(text == about::citationText());
}
