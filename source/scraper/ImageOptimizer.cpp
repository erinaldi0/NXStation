#include "scraper/ImageOptimizer.hpp"
#include "scraper/WebpDecoder.hpp"

#include "util/Logger.hpp"

#include <borealis/extern/nanovg/stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace sf {

namespace {

constexpr int kBoxMaxWidth = 512;
constexpr int kThumbMaxWidth = 320;
constexpr int kBoxJpegQuality = 80;
constexpr int kThumbJpegQuality = 78;

void resizeBilinear(const uint8_t* src, int srcW, int srcH, int channels, uint8_t* dst, int dstW,
                    int dstH)
{
    const float xScale = static_cast<float>(srcW) / static_cast<float>(dstW);
    const float yScale = static_cast<float>(srcH) / static_cast<float>(dstH);

    for (int y = 0; y < dstH; ++y) {
        const float srcY = (static_cast<float>(y) + 0.5f) * yScale - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(srcY)), 0, srcH - 1);
        const int y1 = std::min(y0 + 1, srcH - 1);
        const float fy = srcY - static_cast<float>(y0);

        for (int x = 0; x < dstW; ++x) {
            const float srcX = (static_cast<float>(x) + 0.5f) * xScale - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(srcX)), 0, srcW - 1);
            const int x1 = std::min(x0 + 1, srcW - 1);
            const float fx = srcX - static_cast<float>(x0);

            for (int c = 0; c < channels; ++c) {
                const float p00 = src[(y0 * srcW + x0) * channels + c];
                const float p10 = src[(y0 * srcW + x1) * channels + c];
                const float p01 = src[(y1 * srcW + x0) * channels + c];
                const float p11 = src[(y1 * srcW + x1) * channels + c];
                const float top = p00 + (p10 - p00) * fx;
                const float bottom = p01 + (p11 - p01) * fx;
                const float value = top + (bottom - top) * fy;
                dst[(y * dstW + x) * channels + c] =
                    static_cast<uint8_t>(std::clamp(value, 0.f, 255.f));
            }
        }
    }
}

std::vector<uint8_t> toRgb(const uint8_t* src, int w, int h, int channels)
{
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int i = 0; i < w * h; ++i) {
        rgb[static_cast<size_t>(i) * 3 + 0] = src[static_cast<size_t>(i) * channels + 0];
        rgb[static_cast<size_t>(i) * 3 + 1] =
            channels > 1 ? src[static_cast<size_t>(i) * channels + 1] : src[static_cast<size_t>(i) * channels + 0];
        rgb[static_cast<size_t>(i) * 3 + 2] =
            channels > 2 ? src[static_cast<size_t>(i) * channels + 2] : src[static_cast<size_t>(i) * channels + 0];
    }
    return rgb;
}

/**
 * Magic bytes, not the extension: the scraper writes every asset under a .jpg name even when the
 * provider file is a PNG, so the extension says nothing about what is actually on disk.
 */
bool isJpegFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char magic[2] = { 0, 0 };
    return in.read(reinterpret_cast<char*>(magic), 2) && magic[0] == 0xFF && magic[1] == 0xD8;
}

/** Resize to the target width when needed, then write the JPEG in place. `rgb` is RGB24. */
bool writeResizedJpeg(const std::string& path, const std::vector<uint8_t>& rgb, int w, int h,
                      int maxWidth, int quality)
{
    int outW = w;
    int outH = h;
    if (w > maxWidth) {
        outW = maxWidth;
        outH = std::max(1, static_cast<int>(std::lround(static_cast<double>(h) * maxWidth / w)));
    }

    std::vector<uint8_t> buffer;
    const uint8_t* source = rgb.data();
    if (outW != w || outH != h) {
        buffer.resize(static_cast<size_t>(outW) * static_cast<size_t>(outH) * 3);
        resizeBilinear(rgb.data(), w, h, 3, buffer.data(), outW, outH);
        source = buffer.data();
    }

    if (!stbi_write_jpg(path.c_str(), outW, outH, 3, source, quality)) {
        SF_LOG_W("Scraper", "Optimize write failed: %s", path.c_str());
        return false;
    }

    SF_LOG_I("Scraper", "Optimized %s (%dx%d q=%d)", path.c_str(), outW, outH, quality);
    return true;
}

} // namespace

bool optimizeScrapedImage(const std::string& path, bool thumbnail)
{
    const int maxWidth = thumbnail ? kThumbMaxWidth : kBoxMaxWidth;
    const int quality = thumbnail ? kThumbJpegQuality : kBoxJpegQuality;

    // WebP is checked before anything else because stb_image cannot decode it at all, so an asset
    // stored as WebP has to become a JPEG on disk before any view can draw it. This is what makes
    // RomM's small cover usable: about 19 KB of WebP instead of the ~388 KB big cover, for the ROMs
    // whose small cover is WebP.
    if (isWebpFile(path)) {
        int w = 0;
        int h = 0;
        std::vector<uint8_t> rgb;
        if (!decodeWebpToRgb(path, w, h, rgb))
            return false;
        return writeResizedJpeg(path, rgb, w, h, maxWidth, quality);
    }

    // stbi_info reads only the header (PNG IHDR / JPEG SOF), so a file that is already a JPEG at or
    // below the target width is left as it is. Re-encoding it would mean a full decode plus encode
    // on the console for an image that is already the right size — which is what happens when the
    // source (ScreenScraper with maxwidth, or RomM's small cover) was small to begin with.
    int infoW = 0;
    int infoH = 0;
    if (isJpegFile(path) && stbi_info(path.c_str(), &infoW, &infoH, nullptr) && infoW <= maxWidth) {
        SF_LOG_I("Scraper", "Optimize skipped (already %dx%d JPEG): %s", infoW, infoH, path.c_str());
        return true;
    }

    int w = 0;
    int h = 0;
    int channels = 0;
    uint8_t* pixels = stbi_load(path.c_str(), &w, &h, &channels, 0);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels)
            stbi_image_free(pixels);
        SF_LOG_W("Scraper", "Optimize skipped (decode failed): %s", path.c_str());
        return false;
    }

    const std::vector<uint8_t> rgb = toRgb(pixels, w, h, channels);
    stbi_image_free(pixels);

    return writeResizedJpeg(path, rgb, w, h, maxWidth, quality);
}

bool finalizeScrapedImage(const std::string& path, bool thumbnail, bool optimize)
{
    if (!optimize && !isWebpFile(path))
        return true;

    return optimizeScrapedImage(path, thumbnail);
}

} // namespace sf
