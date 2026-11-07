#include "ipc/cache_eviction.h"

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/time.h>
#include <vector>

using namespace geo;
namespace fs = std::filesystem;

namespace {

void write(const fs::path& path, usize bytes, long secondsAgo) {
    std::ofstream(path) << std::string(bytes, 'x');
    timeval now{};
    gettimeofday(&now, nullptr);
    const timeval times[2] = {{now.tv_sec - secondsAgo, 0}, {now.tv_sec - secondsAgo, 0}};
    utimes(path.c_str(), times);
}

} // namespace

TEST_CASE("eviction drops the least recently used entries, with their refinements") {
    const fs::path dir = fs::temp_directory_path() / "geo-eviction-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    write(dir / "aaaa.geoblob", 100, 300);      // oldest
    write(dir / "aaaa.r1-ff.geoblob", 100, 10); // its refinement, recent itself
    write(dir / "bbbb.geoblob", 100, 200);
    write(dir / "cccc.geoblob", 100, 100);
    write(dir / "dddd.geoblob", 100, 1);          // newest
    write(dir / "eeee.geoblob.XXXXXX", 500, 400); // a partial file, never touched

    const std::vector<std::string> keep = {(dir / "bbbb.geoblob").string()};
    // 500 bytes of entries, a budget of 250: aaaa (200 with its refinement) goes, then cccc,
    // since bbbb is kept.
    const u64 freed = ipc::evictCache(dir.string(), 250, keep);
    CHECK(freed == 300);
    CHECK_FALSE(fs::exists(dir / "aaaa.geoblob"));
    CHECK_FALSE(fs::exists(dir / "aaaa.r1-ff.geoblob"));
    CHECK(fs::exists(dir / "bbbb.geoblob"));
    CHECK_FALSE(fs::exists(dir / "cccc.geoblob"));
    CHECK(fs::exists(dir / "dddd.geoblob"));
    CHECK(fs::exists(dir / "eeee.geoblob.XXXXXX"));

    // Touching marks an entry as just used.
    ipc::touchCacheFile((dir / "bbbb.geoblob").string());
    CHECK(ipc::evictCache(dir.string(), 100) == 100);
    CHECK(fs::exists(dir / "bbbb.geoblob"));
    CHECK_FALSE(fs::exists(dir / "dddd.geoblob"));
    fs::remove_all(dir);

    CHECK(ipc::cacheBudget(fs::temp_directory_path().string()) <= (20ull << 30));
}
