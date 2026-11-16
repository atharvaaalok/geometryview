#include "base/about.h"

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

} // namespace geo::about
