#include "library/scan.h"

#include "base/time.h"

#define XXH_INLINE_ALL
#include "base/xxhash.h"

#include <cerrno>
#include <cstring>
#include <format>
#include <fts.h>
#include <sys/stat.h>
#include <unordered_map>

namespace geo::library {
namespace {

bool endsWithNoCase(std::string_view name, std::string_view suffix) {
    if (name.size() < suffix.size()) {
        return false;
    }
    const std::string_view tail = name.substr(name.size() - suffix.size());
    for (usize i = 0; i < suffix.size(); ++i) {
        char c = tail[i];
        if (c >= 'A' && c <= 'Z') {
            c = char(c - 'A' + 'a');
        }
        if (c != suffix[i]) {
            return false;
        }
    }
    return true;
}

double secondsOf(const struct timespec& time) {
    return double(time.tv_sec) + double(time.tv_nsec) * 1e-9;
}

} // namespace

bool isModelFile(std::string_view name) {
    return endsWithNoCase(name, ".step") || endsWithNoCase(name, ".stp") ||
           endsWithNoCase(name, ".stl");
}

ScanResult scan(const std::string& root,
                const std::function<bool(std::span<const ScanEntry>)>& batch,
                const ScanLimits& limits) {
    ScanResult result;
    struct stat rootInfo{};
    if (stat(root.c_str(), &rootInfo) != 0) {
        result.error = errno == ENOENT ? "This folder could not be found."
                                       : std::format("This folder could not be read: {}.",
                                                     std::strerror(errno));
        return result;
    }
    if (!S_ISDIR(rootInfo.st_mode)) {
        result.error = "This is not a folder.";
        return result;
    }
    char* paths[] = {const_cast<char*>(root.c_str()), nullptr};
    FTS* walk = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, nullptr);
    if (walk == nullptr) {
        result.error = std::format("This folder could not be read: {}.", std::strerror(errno));
        return result;
    }

    std::vector<ScanEntry> pending;
    double flushed = nowSeconds();
    const auto flush = [&](bool force) {
        if (pending.empty() || (!force && pending.size() < limits.batch &&
                                nowSeconds() - flushed < limits.batchSeconds)) {
            return true;
        }
        const bool more = batch(pending);
        pending.clear();
        flushed = nowSeconds();
        return more;
    };
    // Files found under each folder being walked, by depth.
    std::vector<u32> counts(1, 0);
    const usize rootLength = std::strlen(paths[0]);
    const auto relative = [&](const FTSENT* entry) {
        std::string_view path(entry->fts_path, entry->fts_pathlen);
        path.remove_prefix(std::min(path.size(), rootLength));
        while (!path.empty() && path.front() == '/') {
            path.remove_prefix(1);
        }
        return std::string(path);
    };

    while (const FTSENT* entry = fts_read(walk)) {
        const std::string_view name(entry->fts_name, entry->fts_namelen);
        const bool hidden = entry->fts_level > 0 && name.starts_with('.');
        switch (entry->fts_info) {
        case FTS_D:
            if (hidden || entry->fts_level >= i16(limits.maxDepth)) {
                if (!hidden && entry->fts_level >= i16(limits.maxDepth)) {
                    result.truncated = true;
                }
                fts_set(walk, const_cast<FTSENT*>(entry), FTS_SKIP);
                break;
            }
            counts.resize(usize(entry->fts_level) + 1);
            counts[usize(entry->fts_level)] = 0;
            break;
        case FTS_DP: {
            // Skipped folders close too.
            if (entry->fts_level == 0 || hidden || entry->fts_level >= i16(limits.maxDepth)) {
                break;
            }
            const u32 files = counts[usize(entry->fts_level)];
            counts[usize(entry->fts_level) - 1] += files;
            ScanEntry folder;
            folder.path = relative(entry);
            folder.folder = true;
            folder.files = files;
            folder.modified = secondsOf(entry->fts_statp->st_mtimespec);
            pending.push_back(std::move(folder));
            ++result.folders;
            break;
        }
        case FTS_F: {
            if (hidden || !isModelFile(name)) {
                break;
            }
            if (result.files == limits.maxFiles) {
                result.truncated = true;
                break;
            }
            ScanEntry file;
            file.path = relative(entry);
            file.bytes = u64(entry->fts_statp->st_size);
            file.modified = secondsOf(entry->fts_statp->st_mtimespec);
            pending.push_back(std::move(file));
            ++result.files;
            ++counts[usize(entry->fts_level) - 1];
            break;
        }
        default:
            break; // links, unreadable folders and the like are left out
        }
        if (!flush(false)) {
            result.stopped = true;
            break;
        }
    }
    fts_close(walk);
    if (!result.stopped && !flush(true)) {
        result.stopped = true;
    }
    return result;
}

json::Value Listing::toJson() const {
    json::Value value = json::Value::object();
    value.set("root", root);
    value.set("scanned", scanned);
    value.set("truncated", truncated);
    // Each entry as [path, bytes, modified] for a file and [path, files] for a folder: a listing
    // can hold twenty thousand.
    json::Value list = json::Value::array();
    for (const ScanEntry& entry : entries) {
        json::Value row = json::Value::array();
        row.push(entry.path);
        if (entry.folder) {
            row.push(entry.files);
        } else {
            row.push(entry.bytes);
            row.push(entry.modified);
        }
        list.push(std::move(row));
    }
    value.set("entries", std::move(list));
    return value;
}

bool Listing::fromJson(const json::Value& value, Listing& listing) {
    if (!value["root"].isString() || !value["entries"].isArray()) {
        return false;
    }
    Listing read;
    read.root = value["root"].asString();
    read.scanned = value["scanned"].asNumber();
    read.truncated = value["truncated"].asBool();
    for (const json::Value& row : value["entries"].items()) {
        ScanEntry entry;
        entry.path = row[0].asString();
        if (entry.path.empty()) {
            return false;
        }
        if (row.size() == 2) {
            entry.folder = true;
            entry.files = u32(row[1].asNumber());
        } else {
            entry.bytes = u64(row[1].asNumber());
            entry.modified = row[2].asNumber();
        }
        read.entries.push_back(std::move(entry));
    }
    listing = std::move(read);
    return true;
}

std::string listingPath(const std::string& directory, const std::string& root) {
    return std::format("{}/{:016x}.json", directory, XXH3_64bits(root.data(), root.size()));
}

} // namespace geo::library
