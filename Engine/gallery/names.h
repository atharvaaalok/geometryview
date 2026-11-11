// What the gallery calls files and folders, by rules that hold for any library: nothing here knows
// one library's names.
#pragma once

#include <string>
#include <string_view>

namespace geo::gallery {

/// A folder's name as a heading: underscores and runs of spaces become one space, standard codes
/// are spaced (iso4032 to ISO 4032), words without vowels are capitals (dgbb to DGBB), and other
/// words start with a capital.
std::string folderTitle(std::string_view name);

/// A file's names on its tile.
struct FileNames {
    std::string label; // its name, or the name inside it
    std::string sub;   // what tells it apart: "#id", "unnamed", or empty
    bool named = true; // the label is a real name, not an id
};

/// The name of a file at `fileName` (its last path component), with `product` the name inside the
/// file, if known. The extension is dropped, and a zero-padded ordering prefix ("01_", "007 - ").
/// A name that is only an id (digits, hashes, and words such as "step" or "part") gives way to the
/// product name, with the id as its sub; with no product name the id stays, marked unnamed.
FileNames fileNames(std::string_view fileName, std::string_view product = {});

/// The file name without its extension or ordering prefix: what searches and sorts compare.
std::string stemOf(std::string_view fileName);

/// Compares names as people read numbers in them: part2 before part10. Case does not matter.
int compareNatural(std::string_view a, std::string_view b);

} // namespace geo::gallery
