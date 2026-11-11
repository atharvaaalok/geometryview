// A gallery's folder and its files, as a tree, with what is known of each file so far: its facts
// from the library, its picture, or why it failed.
#pragma once

#include "base/types.h"
#include "gallery/names.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace geo::gallery {

inline constexpr u32 kNone = ~0u;

/// A file or folder as a scan lists it, its path relative to the gallery's root.
struct Entry {
    std::string path;
    bool folder = false;
    u64 bytes = 0;
    double modified = 0.0;
};

/// What the library measured of a file.
struct Facts {
    std::array<double, 3> size{}; // the box's sides in millimetres
    bool hasBox = false;
    bool estimated = true;
    u32 parts = 0;
    std::string product; // the name inside the file
};

/// A file's picture: its pixels' size, its scale, and where in it the part and its ink are.
struct Picture {
    u32 width = 0, height = 0;
    double mmPerPixel = 0.0;
    std::array<float, 4> part{}; // x, y, width, height in pixels
    std::array<float, 4> ink{};
};

struct File {
    std::string path;
    std::string name; // the file's own name, with its extension
    u32 folder = kNone;
    u64 bytes = 0;
    double modified = 0.0;
    FileNames names;
    std::optional<Facts> facts;
    std::optional<Picture> picture;
    std::string failure; // why it could not be read, or empty

    bool failed() const { return !failure.empty(); }
    /// Its largest side in millimetres, or 0 when unknown.
    double size() const;
    /// The part's size as drawn, in millimetres: from its picture, or estimated from its box as the
    /// gallery's camera sees it. Nothing when neither is known.
    std::optional<std::array<double, 2>> drawnSize() const;
};

struct Folder {
    std::string name;
    std::string path; // "" for the root
    u32 parent = kNone;
    std::vector<u32> folders;
    std::vector<u32> files;
    u32 count = 0;  // files under it, at any depth
    u32 colour = 0; // mustard, mint, tomato, blush: its own if it holds files, else its first
};

class Tree {
public:
    /// Builds the tree from a scan: folders from the paths of files and of folders listed alone.
    /// Folders and files are ordered as names read, numbers by value.
    void build(std::string_view title, std::span<const Entry> entries);

    const Folder& folder(u32 index) const { return folders_[index]; }
    const File& file(u32 index) const { return files_[index]; }
    File& file(u32 index) { return files_[index]; }
    u32 folderCount() const { return u32(folders_.size()); }
    u32 fileCount() const { return u32(files_.size()); }
    const std::string& title() const { return title_; }

    /// A folder by its path, or kNone.
    u32 folderAt(std::string_view path) const;
    /// A file by its path, or kNone.
    u32 fileAt(std::string_view path) const;

    /// Every file under a folder, in tree order: its own, then its folders'.
    std::vector<u32> filesUnder(u32 folder) const;
    /// Folders under `folder`, itself included, that hold files of their own.
    std::vector<u32> familiesUnder(u32 folder) const;
    /// A folder's name as a heading: the gallery's title at the root.
    std::string titleOf(u32 folder) const;

    /// Records facts, and names the file again by its product.
    void setFacts(u32 file, Facts facts);
    void setPicture(u32 file, Picture picture);
    void setFailure(u32 file, std::string message);

private:
    u32 addFolder(std::string_view path);

    std::string title_;
    std::vector<Folder> folders_;
    std::vector<File> files_;
    std::unordered_map<std::string, u32> folderIndex_;
    std::unordered_map<std::string, u32> fileIndex_;
};

/// The gallery's camera: azimuth 35°, elevation 30°, z up. The part's drawn width and height, in
/// the units of `size`, of a box of these sides.
std::array<double, 2> projectedBox(const std::array<double, 3>& size);

} // namespace geo::gallery
