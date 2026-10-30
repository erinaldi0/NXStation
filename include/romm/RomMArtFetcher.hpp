#pragma once

#include "romm/RomMClient.hpp"
#include "romm/RomMConfig.hpp"
#include "romm/RomMLibraryService.hpp"
#include "scraper/ScrapeTypes.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace sf::romm {

/** What one entry produced, for the batch summary and the scrape log. */
struct ArtFetchResult {
    size_t fromRomM = 0;       // assets taken from the server's cached provider images
    size_t fromScraper = 0;    // assets that needed the ScreenScraper URL instead
    size_t alreadyPresent = 0; // assets already on disk, nothing to download
    size_t failed = 0;
    bool metadataWritten = false;

    size_t attempted() const { return fromRomM + fromScraper + failed; }
    bool anyFetched() const { return fromRomM + fromScraper > 0; }
};

using ArtProgressCb = std::function<void(const sf::ScrapeProgress&)>;

/**
 * Downloads the cover / thumbnail / video / manual assets RomM has already fetched from a
 * metadata provider and cached on disk. One GET per asset: no ROM hash, no provider query, no
 * rate limit, which is what makes it far faster than the ScreenScraper path.
 *
 * Assets land in the locations the UI already reads (MetadataCache::artworkPath,
 * MetadataCache::videoCachePath, ManualPages::pdfPath), so no view code has to change.
 *
 * Only JPEG and PNG sources are used — borealis decodes images with stb_image, which has no
 * WebP support, so a WebP-only entry falls back to its ScreenScraper URL (server-resized JPEG).
 *
 * This is the per-entry worker; the queued, multi-threaded form lives in RomMArtQueue, which gives
 * each worker its own client (CurlHelper serializes on one mutex per instance).
 */
class RomMArtFetcher {
public:
    RomMArtFetcher(RomMClient& client, const RomMConfig& config);

    /**
     * Fetch one entry. `progress` carries the running counters and gets one row per asset,
     * the same shape the scrape progress view uses.
     */
    ArtFetchResult fetchEntryArt(const LibraryEntry& entry, sf::ScrapeProgress& progress,
                                 const ArtProgressCb& onProgress, std::atomic<bool>* abort = nullptr);

    /**
     * Entries the fetcher can write to: a matched system plus either a downloaded ROM or, when
     * `artForRemoteRoms` is on, a remote row (images are resolved by stem, so no ROM file is
     * needed for those).
     */
    bool entryIsFetchable(const LibraryEntry& entry) const;

    /**
     * Stem the art is stored under. Downloaded rows use the local file, remote rows fall back to
     * the server's file name, which is what `applyLocalMedia` will probe for the same row.
     */
    static std::string stemFor(const LibraryEntry& entry);

    /** True when every asset the config asks for is already on disk for this entry. */
    bool entryArtComplete(const LibraryEntry& entry) const;

private:
    RomMClient& client_;
    const RomMConfig& config_;

    /** Download to `.part` then rename, so a dropped transfer never leaves half an image. */
    bool downloadTo(const std::string& url, const std::string& dest, std::atomic<bool>* abort);

    /** One image asset: RomM source first, ScreenScraper URL as the fallback. */
    bool fetchImageAsset(const std::string& label, const std::string& dest,
                         const std::vector<std::string>& rommPaths, const std::string& scraperUrl,
                         bool thumbnail, sf::ScrapeProgress& progress, const ArtProgressCb& onProgress,
                         std::atomic<bool>* abort, ArtFetchResult& result);

    /** Video / manual asset, which has no ScreenScraper equivalent here. */
    bool fetchFileAsset(const std::string& label, const std::string& dest, const std::string& rommPath,
                        sf::ScrapeProgress& progress, const ArtProgressCb& onProgress,
                        std::atomic<bool>* abort, ArtFetchResult& result);

    /** Write the metadata RomM already resolved into gamelist.xml for a matched ROM. */
    bool writeMetadata(const LibraryEntry& entry, const std::string& systemId, const std::string& stem);

    void report(sf::ScrapeProgress& progress, const std::string& phase, const std::string& detail,
                bool hasResult, bool success, const ArtProgressCb& onProgress);
};

} // namespace sf::romm
