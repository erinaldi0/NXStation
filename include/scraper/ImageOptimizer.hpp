#pragma once

#include <string>

namespace sf {

/**
 * Re-encode a scraped image in place (JPEG, resized) for smaller SD footprint.
 *
 * A WebP input is transcoded to JPEG, because borealis renders through stb_image and cannot display WebP.
 */
bool optimizeScrapedImage(const std::string& path, bool thumbnail);

/**
 * Call optimizeScrapedImage when optimization is enabled, and always when the file is a WebP: a WebP
 * left as downloaded is an asset no view can draw, so the transcode is not optional.
 */
bool finalizeScrapedImage(const std::string& path, bool thumbnail, bool optimize);

} // namespace sf
