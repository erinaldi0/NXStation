#pragma once

#include <string>
#include <vector>

namespace sf {

/**
 * WebP support for scraped assets.
 *
 * borealis renders images through nanovg's stb_image, which understands JPEG and PNG only, so a
 * WebP asset can never be displayed as it is. This uses the libwebp decode API (the devkitPro
 * portlib, switch-libwebp) to turn a WebP file into a JPEG at fetch time, which keeps every view
 * reading the formats stb_image can decode.
 */

/** Magic bytes, not the extension: assets are stored under a .jpg name whatever the source format is. */
bool isWebpFile(const std::string& path);

/**
 * Decode a WebP file into RGB24 (row stride is width). Returns false for anything the decoder
 * rejects — a truncated file, an unsupported chunk, or a bitmap larger than the read limit.
 */
bool decodeWebpToRgb(const std::string& path, int& width, int& height, std::vector<uint8_t>& rgb);

} // namespace sf
