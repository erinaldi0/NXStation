#pragma once

#include <string>

namespace sf {

/**
 * URL helpers shared by the ScreenScraper service and the RomM art fetcher.
 */

/** Ask ScreenScraper to resize/transcode server-side so the download itself is small. */
std::string withServerResize(const std::string& url, int maxWidth);

/** Point a provider image URL at its .jpg sibling when the original is not a format stb_image decodes. */
std::string preferJpegUrl(const std::string& url);

/**
 * True when the format can end up as something borealis renders.
 *
 * borealis renders images through nanovg's stb_image, which has no WebP decoder, so a WebP asset can
 * never be displayed as downloaded. It is accepted here because the fetcher transcodes it: libwebp
 * (see scraper/WebpDecoder.hpp) decodes WebP at fetch time and the optimizer writes the result as
 * JPEG, so what lands on disk is always JPEG or PNG.
 */
bool decodableImageFormat(const std::string& path);

} // namespace sf
