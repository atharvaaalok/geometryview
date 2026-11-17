#include "picture_file.h"

#include "engine.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

bool tagPictureFile(const char* path) {
    std::vector<uint8_t> png;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return false;
        }
        png.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    GeoPng tagged{};
    if (!geo_tag_picture(png.data(), png.size(), &tagged)) {
        return false;
    }
    // Written beside it and moved over it, so a reader never sees half a picture.
    const std::string partial = std::string(path) + ".partial";
    bool written = false;
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(tagged.bytes), std::streamsize(tagged.size));
        written = bool(out);
    }
    geo_png_free(&tagged);
    if (!written || std::rename(partial.c_str(), path) != 0) {
        std::remove(partial.c_str());
        return false;
    }
    return true;
}
