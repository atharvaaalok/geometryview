// Keeps the cache directory within its budget, dropping what was used longest ago.
#pragma once

#include "base/types.h"

#include <span>
#include <string>

namespace geo::ipc {

/// The cache's budget: 5% of the free space on its volume, at most 20 GB.
u64 cacheBudget(const std::string& directory);

/// Marks a cache file as just used, so eviction keeps it longest. Its modification time records
/// use, since access times are not kept reliably.
void touchCacheFile(const std::string& path);

/// Deletes the least recently used cache files, with their refinements, until the directory fits
/// in `budget` bytes. Files in `keep`, such as those open now, are never deleted. Returns the bytes
/// freed.
u64 evictCache(const std::string& directory, u64 budget, std::span<const std::string> keep = {});

} // namespace geo::ipc
