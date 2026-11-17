// What GeometryView is: its version and how to cite it, the same in every app and in the citation
// file at the repository's root (CITATION.cff), which a test holds them to.
#pragma once

#include "base/types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

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

/// What a saved picture says of itself, as PNG text: its Software, GeometryView and this release,
/// and a Comment with where GeometryView is and how to cite it.
std::string pictureSoftware();
std::string_view pictureComment();

/// `png` carrying that text. Empty if it is not a PNG.
std::vector<u8> taggedPicture(std::span<const u8> png);

} // namespace geo::about
