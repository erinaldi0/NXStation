#include "scraper/ScrapeUrls.hpp"

#include <algorithm>
#include <cctype>

namespace sf {

namespace {

/** Extension of a URL or file path, lowercased, with any query string ignored. */
std::string lowerExtension(const std::string& path) {
    std::string clean = path;
    if (const auto q = clean.find('?'); q != std::string::npos)
        clean.resize(q);

    const auto dot = clean.rfind('.');
    if (dot == std::string::npos)
        return {};

    std::string ext = clean.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

} // namespace

std::string withServerResize(const std::string& url, int maxWidth) {
    if (url.find("screenscraper.fr") == std::string::npos)
        return url;

    std::string out = url;
    out += (out.find('?') == std::string::npos) ? '?' : '&';
    out += "maxwidth=" + std::to_string(maxWidth) + "&outputformat=jpg";
    return out;
}

std::string preferJpegUrl(const std::string& url) {
    // A cache-buster query (RomM's `?ts=`) is not part of the extension, so split it off first.
    std::string base = url;
    std::string query;
    if (const auto q = base.find('?'); q != std::string::npos) {
        query = base.substr(q);
        base.resize(q);
    }

    const auto dot = base.rfind('.');
    if (dot == std::string::npos)
        return url;

    std::string ext = base.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == ".png" || ext == ".webp")
        return base.substr(0, dot) + ".jpg" + query;
    return url;
}

bool decodableImageFormat(const std::string& path) {
    const std::string ext = lowerExtension(path);
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp";
}

} // namespace sf
