#pragma once

#include <filesystem>

namespace xerces_launcher {
// Writes 8-bit RGB pixels (rows top to bottom) as a PNG with stored
// (uncompressed) deflate blocks: no compression library, files are large but
// any viewer reads them. Used for screen captures in checks.
bool write_png_rgb(const std::filesystem::path &path, const unsigned char *rgb,
                   int width, int height);
} // namespace xerces_launcher
