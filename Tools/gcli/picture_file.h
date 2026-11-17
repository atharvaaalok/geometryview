// The pictures gcli writes are for people and AI apps, so each says what made it: GeometryView and
// its version, and how to cite it (geo_tag_picture), unlike the gallery's cached thumbnails.
#pragma once

/// Tags the PNG at `path` in place. False, leaving it as it was, if it can't.
bool tagPictureFile(const char* path);
