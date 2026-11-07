#include "scene/mapped_blob.h"

#include <cerrno>
#include <cstring>
#include <format>
#include <sys/mman.h>
#include <sys/stat.h>

namespace geo::scene {

MappedBlob::~MappedBlob() {
    if (data_ != nullptr) {
        munmap(data_, size_);
    }
}

bool MappedBlob::open(int fd, std::string& error) {
    struct stat info{};
    if (fstat(fd, &info) != 0 || info.st_size <= 0) {
        error = "The cache file could not be read.";
        return false;
    }
    size_ = usize(info.st_size);
    void* mapping = mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        error = std::format("The cache file could not be mapped: {}.", std::strerror(errno));
        return false;
    }
    data_ = mapping;
    return format::parseBlob({static_cast<const u8*>(data_), size_}, view_, error);
}

} // namespace geo::scene
