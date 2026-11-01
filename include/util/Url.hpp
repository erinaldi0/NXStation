#pragma once

#include <cstring>
#include <string>

namespace sf::url {

/**
 * Percent-encode a URL path segment. RomM file names commonly contain spaces and punctuation,
 * and a raw path in a curl URL is rejected, so every segment has to be encoded.
 * Shared by RomMLibraryService (API content URLs) and RomMClient (cached asset URLs).
 */
/**
 * Percent-encode a query string but keep its structure (`?`, `&`, `=` and the sub-delims). RomM's `?ts=`
 * cache-buster is a date-time with a space in it (`?ts=2026-09-25 17:09:03`), and a raw space in the
 * request line makes the server answer 400, so the values have to be encoded.
 */
inline std::string encodeQuery(const std::string& value) {
    static const char* safe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~"
                              "!$&'()*+,;=?:/";
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if (strchr(safe, c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out += "%";
            out += "0123456789ABCDEF"[(c >> 4) & 0xF];
            out += "0123456789ABCDEF"[c & 0xF];
        }
    }
    return out;
}

/**
 * Percent-encode one query value. Unlike encodeQuery this also encodes the structural characters,
 * so a value containing `&`, `=` or `+` (a slot name, a file name) cannot be read as part of the
 * query.
 */
inline std::string encodeQueryValue(const std::string& value) {
    static const char* safe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if (strchr(safe, c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out += "%";
            out += "0123456789ABCDEF"[(c >> 4) & 0xF];
            out += "0123456789ABCDEF"[c & 0xF];
        }
    }
    return out;
}

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
