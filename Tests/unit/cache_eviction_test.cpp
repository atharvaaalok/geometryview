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

TEST_CASE("pictures are evicted by file, with their notes, newest use first kept") {
    const fs::path dir = fs::temp_directory_path() / "geo-eviction-pictures-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // Two styles of one file, each a picture and its note; a second file drawn once, later.
    write(dir / "aaaa.11.png", 100, 300);
    write(dir / "aaaa.11.json", 10, 300);
    write(dir / "aaaa.22.png", 100, 50); // its newest picture
    write(dir / "aaaa.22.json", 10, 50);
    write(dir / "bbbb.11.png", 100, 100);
    write(dir / "bbbb.11.json", 10, 100);
    write(dir / "cccc.11.png.Ab12Cd", 100, 1); // being written

    const std::string_view suffixes[] = {".png", ".json"};
    CHECK(ipc::cacheBytes(dir.string(), suffixes) == 330);
    // bbbb was used longer ago than aaaa's newest picture.
    CHECK(ipc::evictCache(dir.string(), 250, {}, suffixes) == 110);
    CHECK_FALSE(fs::exists(dir / "bbbb.11.png"));
    CHECK_FALSE(fs::exists(dir / "bbbb.11.json"));
    CHECK(fs::exists(dir / "aaaa.11.png"));
    CHECK(fs::exists(dir / "cccc.11.png.Ab12Cd"));
    CHECK(ipc::cacheBytes(dir.string()) == 0); // no cache files of the default kind
    fs::remove_all(dir);
}

TEST_CASE("partial files left by interrupted writes are swept once old") {
    const fs::path dir = fs::temp_directory_path() / "geo-sweep-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    write(dir / "aaaa.geoblob.Xy12Zw", 100, 3 * 3600);           // old: swept
    write(dir / "bbbb.geoblob.Qr34St", 100, 60);                 // recent: an import running now
    write(dir / "cccc.structure.geoblob.Uv56Wx", 100, 3 * 3600); // an old structure pass
    write(dir / "dddd.geoblob", 100, 3 * 3600);                  // a cache file
    write(dir / "notes.txt", 100, 3 * 3600);
    CHECK(ipc::sweepPartialFiles(dir.string(), ipc::kBlobSuffixes, 2 * 3600) == 2);
    CHECK_FALSE(fs::exists(dir / "aaaa.geoblob.Xy12Zw"));
    CHECK(fs::exists(dir / "bbbb.geoblob.Qr34St"));
    CHECK_FALSE(fs::exists(dir / "cccc.structure.geoblob.Uv56Wx"));
    CHECK(fs::exists(dir / "dddd.geoblob"));
    CHECK(fs::exists(dir / "notes.txt"));
    fs::remove_all(dir);
}
