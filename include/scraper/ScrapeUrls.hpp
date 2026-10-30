#pragma once

#include <string>

namespace sf {

/**
 * URL helpers shared by the ScreenScraper service and the RomM art fetcher.
 */

/** Ask ScreenScraper to resize/transcode server-side so the download itself is small. */
std::string withServerResize(const std::string& url, int maxWidth);

/** Point a provider image URL at its .jpg sibling when that is decodable and the original is not. */
std::string preferJpegUrl(const std::string& url);

/**
 * True when the URL/path is in a format stb_image can decode (JPEG or PNG).
 *
 * borealis renders images through nanovg's stb_image, which has no WebP decoder, so a WebP
 * asset can be downloaded but never displayed. Anything served as WebP has to be fetched as
 * JPEG instead — or, on RomM, skipped and left to the ScreenScraper URL.
 */
bool decodableImageFormat(const std::string& path);

} // namespace sf
