#include "ipc/cache_eviction.h"

#include <algorithm>
#include <dirent.h>
#include <span>
#include <string_view>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace geo::ipc {
namespace {

constexpr u64 kMaxBudget = 20ull << 30;

// A cache entry: the main file and its refinements, `<key>.geoblob` and `<key>.r*.geoblob`.
struct Entry {
    u64 bytes = 0;
    i64 used = 0; // the main file's modification time, in nanoseconds
    std::vector<std::string> files;
};

} // namespace

u64 cacheBudget(const std::string& directory) {
    struct statfs volume{};
    if (statfs(directory.c_str(), &volume) != 0) {
        return kMaxBudget;
    }
    const u64 free = u64(volume.f_bavail) * u64(volume.f_bsize);
    return std::min(free / 20, kMaxBudget);
}

void touchCacheFile(const std::string& path) {
    utimes(path.c_str(), nullptr);
}

namespace {

// The suffix a name ends in, if it is one of them.
std::string_view suffixOf(std::string_view name, std::span<const std::string_view> suffixes) {
    for (const std::string_view suffix : suffixes) {
        if (name.ends_with(suffix)) {
            return suffix;
        }
    }
    return {};
}

i64 modified(const struct stat& info) {
    return i64(info.st_mtimespec.tv_sec) * 1'000'000'000 + i64(info.st_mtimespec.tv_nsec);
}

} // namespace

u64 evictCache(const std::string& directory, u64 budget, std::span<const std::string> keep,
               std::span<const std::string_view> suffixes) {
    std::unordered_map<std::string, Entry> entries;
    std::unordered_map<std::string, bool> hasMain;
    u64 total = 0;
    if (DIR* listing = opendir(directory.c_str())) {
        while (const dirent* item = readdir(listing)) {
            const std::string_view name(item->d_name);
            const std::string_view suffix = suffixOf(name, suffixes);
            if (suffix.empty()) {
                continue; // partial files and anything else are left alone
            }
            const std::string path = directory + "/" + std::string(name);
            struct stat info{};
            if (stat(path.c_str(), &info) != 0) {
                continue;
            }
            // The key is the name up to the first dot.
            const std::string key(name.substr(0, name.find('.')));
            Entry& entry = entries[key];
            bool& main = hasMain[key];
            entry.bytes += u64(info.st_size);
            entry.files.push_back(path);
            if (name.size() == key.size() + suffix.size()) {
                entry.used = modified(info);
                main = true;
            } else if (!main) {
                entry.used = std::max(entry.used, modified(info));
            }
            total += u64(info.st_size);
        }
        closedir(listing);
    }

    std::vector<std::pair<std::string, Entry*>> order;
    for (auto& [key, entry] : entries) {
        const bool kept = std::any_of(keep.begin(), keep.end(), [&](const std::string& path) {
            return std::find(entry.files.begin(), entry.files.end(), path) != entry.files.end();
        });
        if (!kept) {
            order.emplace_back(key, &entry);
        }
    }
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.second->used < b.second->used; });

    u64 freed = 0;
    for (const auto& [key, entry] : order) {
        if (total - freed <= budget) {
            break;
        }
        for (const std::string& file : entry->files) {
            unlink(file.c_str());
        }
        freed += entry->bytes;
    }
    return freed;
}

u64 cacheBytes(const std::string& directory, std::span<const std::string_view> suffixes) {
    u64 total = 0;
    if (DIR* listing = opendir(directory.c_str())) {
        while (const dirent* item = readdir(listing)) {
            if (suffixOf(item->d_name, suffixes).empty()) {
                continue;
            }
            struct stat info{};
            if (stat((directory + "/" + item->d_name).c_str(), &info) == 0) {
                total += u64(info.st_size);
            }
        }
        closedir(listing);
    }
    return total;
}

u32 sweepPartialFiles(const std::string& directory, std::span<const std::string_view> suffixes,
                      i64 olderThanSeconds) {
    timeval now{};
    gettimeofday(&now, nullptr);
    u32 swept = 0;
    if (DIR* listing = opendir(directory.c_str())) {
        while (const dirent* item = readdir(listing)) {
            const std::string_view name(item->d_name);
            // mkstemp's six characters after a dot, after one of the suffixes.
            if (name.size() < 7 || name[name.size() - 7] != '.' ||
                suffixOf(name.substr(0, name.size() - 7), suffixes).empty()) {
                continue;
            }
            const std::string path = directory + "/" + std::string(name);
            struct stat info{};
            if (stat(path.c_str(), &info) == 0 &&
                i64(now.tv_sec) - i64(info.st_mtimespec.tv_sec) > olderThanSeconds &&
                unlink(path.c_str()) == 0) {
                ++swept;
            }
        }
        closedir(listing);
    }
    return swept;
}

} // namespace geo::ipc
