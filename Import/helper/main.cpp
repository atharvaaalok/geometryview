// geometry-import: the sandboxed helper that parses source files.
//
// Untrusted files are parsed here, in a process of its own, so a malformed file can only crash this
// process, its memory returns to the system when it exits, and the viewer never links a parser.
//
// Usage: geometry-import <source-fd> <output-fd>. The result is one line on stdout: "ok" or
// "error <message>".
#include "import.h"

#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/stat.h>

namespace {

int fail(const std::string& message) {
    std::printf("error %s\n", message.c_str());
    return 1;
}

} // namespace

int main(int argc, const char** argv) {
    if (argc != 3) {
        return fail("The helper was started incorrectly.");
    }
    const int sourceFd = std::atoi(argv[1]);
    const int outputFd = std::atoi(argv[2]);

    struct stat info{};
    if (fstat(sourceFd, &info) != 0 || info.st_size <= 0) {
        return fail("This file is empty.");
    }
    const auto size = static_cast<geo::usize>(info.st_size);
    void* mapping = mmap(nullptr, size, PROT_READ, MAP_SHARED, sourceFd, 0);
    if (mapping == MAP_FAILED) {
        return fail("This file could not be read.");
    }

    geo::import::ImportResult result;
    std::string error;
    if (!geo::import::importMesh({static_cast<const geo::u8*>(mapping), size}, outputFd, result,
                                 error)) {
        return fail(error);
    }
    std::printf("ok\n");
    return 0;
}
