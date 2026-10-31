#include "scraper/WebpDecoder.hpp"

#include "util/Logger.hpp"

#include <cstring>
#include <fstream>

// libwebp is a C library and its headers do not wrap themselves in extern "C", so the mangled names
// C++ would look for do not match the symbols in libwebp.a.
extern "C" {
#include "webp/decode.h"
}

namespace sf {

namespace {

// Assets are small, and a file read straight into memory has to be bounded: an entry the server has
// not finished scanning, or a corrupted length prefix, would otherwise allocate on the console.
constexpr size_t kMaxWebpBytes = 16 * 1024 * 1024;

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;

    out.clear();
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0 || static_cast<size_t>(size) > kMaxWebpBytes)
        return false;

    out.resize(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    return static_cast<size_t>(in.gcount()) == out.size();
}

} // namespace

bool isWebpFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char magic[12] = { 0 };
    if (!in.read(reinterpret_cast<char*>(magic), 12))
        return false;

    // RIFF....WEBP — the container the decoder inspects, not the chunk inside it (VP8, VP8L or
    // VP8X), which is what tells the lossy and lossless paths apart.
    return std::memcmp(magic, "RIFF", 4) == 0 && std::memcmp(magic + 8, "WEBP", 4) == 0;
}

bool decodeWebpToRgb(const std::string& path, int& width, int& height, std::vector<uint8_t>& rgb) {
    width = 0;
    height = 0;

    std::vector<uint8_t> data;
    if (!readFile(path, data))
        return false;

    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)) {
        SF_LOG_W("Scraper", "WebP config failed: %s", path.c_str());
        return false;
    }

    // RGB24 is what the optimizer writes JPEG from; MODE_RGBA would carry an alpha channel that a
    // JPEG cannot hold, and the covers scraped here never need one.
    config.output.colorspace = MODE_RGB;

    const VP8StatusCode status = WebPDecode(data.data(), data.size(), &config);
    if (status != VP8_STATUS_OK) {
        SF_LOG_W("Scraper", "WebP decode failed (%d): %s", static_cast<int>(status), path.c_str());
        WebPFreeDecBuffer(&config.output);
        return false;
    }

    const int w = config.output.width;
    const int h = config.output.height;
    const uint8_t* pixels = config.output.u.RGBA.rgba;
    const int stride = config.output.u.RGBA.stride;
    if (w <= 0 || h <= 0 || stride < w * 3 || !pixels) {
        WebPFreeDecBuffer(&config.output);
        return false;
    }

    rgb.clear();
    rgb.reserve(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = pixels + static_cast<size_t>(y) * static_cast<size_t>(stride);
        rgb.insert(rgb.end(), row, row + w * 3);
    }

    WebPFreeDecBuffer(&config.output);
    width = w;
    height = h;
    return !rgb.empty();
}

} // namespace sf
