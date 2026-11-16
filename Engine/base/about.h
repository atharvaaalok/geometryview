// What GeometryView is: its version and how to cite it, the same in every app and in the citation
// file at the repository's root (CITATION.cff), which a test holds them to.
#pragma once

#include <string>
#include <string_view>

namespace geo::about {

/// The release, from the project's version in CMake.
std::string_view version();

/// The citation, the same for every release so that citations to each version gather in one place:
/// one title, author and year.
std::string_view citationTitle();
std::string_view citationUrl();

/// The citation as a line of text, in APA's style.
std::string citationText();

/// The citation as a BibTeX entry, with this release as its version.
std::string citationBibtex();

} // namespace geo::about
