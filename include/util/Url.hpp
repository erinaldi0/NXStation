#pragma once

#include <cstring>
#include <string>

namespace sf::url {

/**
 * Percent-encode a URL path segment. RomM file names commonly contain spaces and punctuation,
 * and a raw path in a curl URL is rejected, so every segment has to be encoded.
 * Shared by RomMLibraryService (API content URLs) and RomMClient (cached asset URLs).
 */
inline std::string encodePathSegment(const std::string& value) {
    static const char* safe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if (strchr(safe, c)) {
            out.push_back(static_cast<char>(c));
        } else if (c == '/') {
            out.push_back('/');
        } else {
            out += "%";
            out += "0123456789ABCDEF"[(c >> 4) & 0xF];
            out += "0123456789ABCDEF"[c & 0xF];
        }
    }
    return out;
}

} // namespace sf::url
