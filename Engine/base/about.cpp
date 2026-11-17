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
// Zenodo's DOI for every version, so citations to each release gather in one record.
constexpr std::string_view kDoi = "10.5281/zenodo.24295273";

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

std::string_view citationDoi() {
    return kDoi;
}

std::string citationText() {
    return "Aalok, A. (2026). " + std::string(kTitle) + " [Computer software]. Zenodo. " +
           "https://doi.org/" + std::string(kDoi);
}

std::string citationBibtex() {
    return "@software{aalok_geometryview_2026,\n"
           "  author    = {Aalok, Atharva},\n"
           "  title     = {{" +
           std::string(kTitle) +
           "}},\n"
           "  year      = {2026},\n"
           "  publisher = {Zenodo},\n"
           "  version   = {" +
           std::string(version()) +
           "},\n"
           "  doi       = {" +
           std::string(kDoi) +
           "},\n"
           "  url       = {" +
           std::string(kUrl) +
           "}\n"
           "}\n";
}

std::string pictureSoftware() {
    return "GeometryView " + std::string(version());
}

std::string pictureComment() {
    return "Made with GeometryView, " + std::string(kUrl) + ". To cite it: https://doi.org/" +
           std::string(kDoi);
}

std::vector<u8> taggedPicture(std::span<const u8> png) {
    const std::string software = pictureSoftware(), comment = pictureComment();
    const std::array<PngText, 2> texts = {
        PngText{"Software", software},
        PngText{"Comment", comment},
    };
    return withPngText(png, texts);
}

} // namespace geo::about
