// Keeps the cache directory within its budget, dropping what was used longest ago.
#pragma once

#include "base/types.h"

#include <span>
#include <string>
#include <string_view>

namespace geo::ipc {

/// The cache's budget: 5% of the free space on its volume, at most 20 GB.
u64 cacheBudget(const std::string& directory);

/// Marks a cache file as just used, so eviction keeps it longest. Its modification time records
/// use, since access times are not kept reliably.
void touchCacheFile(const std::string& path);

/// The files `evictCache` counts: those whose names end in one of these.
inline constexpr std::string_view kBlobSuffixes[] = {".geoblob"};

/// Deletes the least recently used cache entries until the directory fits in `budget` bytes. An
/// entry is every file whose name starts with the same key, up to the first dot, and ends in one of
/// `suffixes`: a cache file with its refinements, or a file's pictures with their notes. Its use is
/// the modification time of `<key><suffix>` where there is one, and of its newest file otherwise.
/// Entries with a file in `keep`, such as those open now, are never deleted. Returns the bytes
/// freed.
u64 evictCache(const std::string& directory, u64 budget, std::span<const std::string> keep = {},
               std::span<const std::string_view> suffixes = kBlobSuffixes);

/// The bytes held by files ending in one of `suffixes`.
u64 cacheBytes(const std::string& directory,
               std::span<const std::string_view> suffixes = kBlobSuffixes);

/// Deletes what interrupted writes left behind: files named `<name><suffix>.<six characters>`, as
/// mkstemp names them, last changed more than `olderThanSeconds` ago. Returns how many.
u32 sweepPartialFiles(const std::string& directory, std::span<const std::string_view> suffixes,
                      i64 olderThanSeconds);

} // namespace geo::ipc
