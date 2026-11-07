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
constexpr std::string_view kSuffix = ".geoblob";

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

u64 evictCache(const std::string& directory, u64 budget, std::span<const std::string> keep) {
    std::unordered_map<std::string, Entry> entries;
    u64 total = 0;
    if (DIR* listing = opendir(directory.c_str())) {
        while (const dirent* item = readdir(listing)) {
            const std::string_view name(item->d_name);
            if (!name.ends_with(kSuffix)) {
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
            entry.bytes += u64(info.st_size);
            entry.files.push_back(path);
            if (name.size() == key.size() + kSuffix.size()) {
                entry.used =
                    i64(info.st_mtimespec.tv_sec) * 1'000'000'000 + i64(info.st_mtimespec.tv_nsec);
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

} // namespace geo::ipc
