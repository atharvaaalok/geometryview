#include "base/about.h"

#include "base/png_text.h"

#include <array>

#ifndef GEO_VERSION
#error "GEO_VERSION comes from the project's version in CMake"
#endif

namespace geo::about {

namespace {

constexpr std::string_view kTitle = "GeometryView: A STEP and STL viewer for people and AI agents";
constexpr std::string_view kUrl = "https://github.com/atharvaaalok/geometryview";

} // namespace

std::string_view version() {
    return GEO_VERSION;
}

std::string_view citationTitle() {
    return kTitle;
}

std::string_view citationUrl() {
    return kUrl;
}

std::string citationText() {
    return "Aalok, A. (2026). " + std::string(kTitle) + " [Computer software]. " +
           std::string(kUrl);
}

std::string citationBibtex() {
    return "@software{aalok_geometryview_2026,\n"
           "  author  = {Aalok, Atharva},\n"
           "  title   = {{" +
           std::string(kTitle) +
           "}},\n"
           "  year    = {2026},\n"
           "  url     = {" +
           std::string(kUrl) +
           "},\n"
           "  version = {" +
           std::string(version()) +
           "}\n"
           "}\n";
}

std::string pictureSoftware() {
    return "GeometryView " + std::string(version());
}

std::string_view pictureComment() {
    return "Made with GeometryView, https://github.com/atharvaaalok/geometryview. To cite it, see "
           "CITATION.cff there.";
}

std::vector<u8> taggedPicture(std::span<const u8> png) {
    const std::string software = pictureSoftware();
    const std::array<PngText, 2> texts = {
        PngText{"Software", software},
        PngText{"Comment", pictureComment()},
    };
    return withPngText(png, texts);
}

} // namespace geo::about
