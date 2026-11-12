#include "gallery/tree.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>

namespace geo::gallery {

double File::size() const {
    if (!facts || !facts->hasBox) {
        return 0.0;
    }
    return std::max({facts->size[0], facts->size[1], facts->size[2]});
}

std::optional<std::array<double, 2>> File::drawnSize() const {
    if (picture && picture->mmPerPixel > 0.0 && picture->part[2] > 0.0f) {
        return std::array<double, 2>{picture->part[2] * picture->mmPerPixel,
                                     picture->part[3] * picture->mmPerPixel};
    }
    if (facts && facts->hasBox) {
        return projectedBox(facts->size);
    }
    return std::nullopt;
}

std::array<double, 2> projectedBox(const std::array<double, 3>& size) {
    const double a = 35.0 * std::numbers::pi / 180.0, e = 30.0 * std::numbers::pi / 180.0;
    const double w = std::cos(a) * size[0] + std::sin(a) * size[1];
    const double h =
        std::sin(e) * (std::sin(a) * size[0] + std::cos(a) * size[1]) + std::cos(e) * size[2];
    return {w, h};
}

u32 Tree::addFolder(std::string_view path) {
    for (u32 i = 0; i < folders_.size(); ++i) {
        if (folders_[i].path == path) {
            return i;
        }
    }
    const usize slash = path.rfind('/');
    const u32 parent =
        path.empty() ? kNone
                     : addFolder(slash == std::string_view::npos ? "" : path.substr(0, slash));
    Folder folder;
    folder.path = std::string(path);
    folder.name = std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
    folder.parent = parent;
    folders_.push_back(std::move(folder));
    const u32 index = u32(folders_.size() - 1);
    if (parent != kNone) {
        folders_[parent].folders.push_back(index);
    }
    return index;
}

void Tree::build(std::string_view title, std::span<const Entry> entries) {
    title_ = std::string(title);
    folders_.clear();
    files_.clear();
    addFolder("");
    // Folders by path, found fast: a scan lists thousands of files in a few folders.
    std::unordered_map<std::string, u32> byPath{{"", 0}};
    const auto folderOf = [&](std::string_view path) -> u32 {
        if (const auto it = byPath.find(std::string(path)); it != byPath.end()) {
            return it->second;
        }
        const u32 index = addFolder(path);
        byPath.emplace(std::string(path), index);
        return index;
    };
    for (const Entry& entry : entries) {
        if (entry.folder) {
            folderOf(entry.path);
            continue;
        }
        const usize slash = entry.path.rfind('/');
        File file;
        file.path = entry.path;
        file.name = slash == std::string::npos ? entry.path : entry.path.substr(slash + 1);
        file.folder =
            folderOf(slash == std::string::npos ? std::string_view()
                                                : std::string_view(entry.path).substr(0, slash));
        file.bytes = entry.bytes;
        file.modified = entry.modified;
        file.names = fileNames(file.name);
        files_.push_back(std::move(file));
        folders_[files_.back().folder].files.push_back(u32(files_.size() - 1));
    }
    for (Folder& folder : folders_) {
        std::sort(folder.folders.begin(), folder.folders.end(), [&](u32 a, u32 b) {
            return compareNatural(folders_[a].name, folders_[b].name) < 0;
        });
        std::sort(folder.files.begin(), folder.files.end(), [&](u32 a, u32 b) {
            return compareNatural(stemOf(files_[a].name), stemOf(files_[b].name)) < 0;
        });
    }
    folderIndex_.clear();
    fileIndex_.clear();
    for (u32 i = 0; i < folders_.size(); ++i) {
        folderIndex_.emplace(folders_[i].path, i);
    }
    for (u32 i = 0; i < files_.size(); ++i) {
        fileIndex_.emplace(files_[i].path, i);
    }
    // Counts, and colours: folders that hold files take turns, in tree order; the rest take their
    // first such folder's.
    u32 next = 0;
    const std::function<void(u32)> walk = [&](u32 index) {
        Folder& folder = folders_[index];
        if (!folder.files.empty()) {
            folder.colour = next++ % 4;
        }
        folder.count = u32(folder.files.size());
        for (const u32 child : folder.folders) {
            walk(child);
            folders_[index].count += folders_[child].count;
        }
        Folder& again = folders_[index];
        if (again.files.empty() && !again.folders.empty()) {
            again.colour = folders_[again.folders.front()].colour;
        }
    };
    walk(0);
}

u32 Tree::folderAt(std::string_view path) const {
    const auto it = folderIndex_.find(std::string(path));
    return it == folderIndex_.end() ? kNone : it->second;
}

u32 Tree::fileAt(std::string_view path) const {
    const auto it = fileIndex_.find(std::string(path));
    return it == fileIndex_.end() ? kNone : it->second;
}

std::vector<u32> Tree::filesUnder(u32 folder) const {
    std::vector<u32> out(folders_[folder].files);
    for (const u32 child : folders_[folder].folders) {
        const std::vector<u32> inner = filesUnder(child);
        out.insert(out.end(), inner.begin(), inner.end());
    }
    return out;
}

std::vector<u32> Tree::familiesUnder(u32 folder) const {
    std::vector<u32> out;
    if (!folders_[folder].files.empty()) {
        out.push_back(folder);
    }
    for (const u32 child : folders_[folder].folders) {
        const std::vector<u32> inner = familiesUnder(child);
        out.insert(out.end(), inner.begin(), inner.end());
    }
    return out;
}

std::string Tree::titleOf(u32 folder) const {
    return folder == 0 ? title_ : folderTitle(folders_[folder].name);
}

void Tree::setFacts(u32 file, Facts facts) {
    File& f = files_[file];
    f.names = fileNames(f.name, facts.product);
    f.facts = std::move(facts);
}

void Tree::setPicture(u32 file, Picture picture) {
    files_[file].picture = picture;
}

void Tree::setFailure(u32 file, std::string message) {
    files_[file].failure = std::move(message);
}

} // namespace geo::gallery
