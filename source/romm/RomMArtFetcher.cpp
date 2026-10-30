#include "romm/RomMArtFetcher.hpp"

#include "app/AppState.hpp"
#include "app/Config.hpp"
#include "media/ManualPages.hpp"
#include "scraper/ImageOptimizer.hpp"
#include "scraper/MetadataCache.hpp"
#include "scraper/ScraperService.hpp"
#include "scraper/ScrapeUrls.hpp"
#include "util/FileSystem.hpp"
#include "util/Logger.hpp"
#include "util/Network.hpp"
#include "util/RomTitle.hpp"
#include "util/ScrapeLog.hpp"

#include <borealis.hpp>
#include <algorithm>
#include <cctype>

namespace sf::romm {

namespace {

constexpr int kBoxMaxWidth = 512;
constexpr int kThumbMaxWidth = 320;

bool present(const std::string& path) {
    return !path.empty() && FileSystem::exists(path);
}

} // namespace

RomMArtFetcher::RomMArtFetcher(RomMClient& client, const RomMConfig& config)
    : client_(client), config_(config) {}

std::string RomMArtFetcher::stemFor(const LibraryEntry& entry) {
    if (entry.downloadedLocally && !entry.localPath.empty())
        return FileSystem::stemOf(entry.localPath);

    // Remote row: the stem has to be the one the row itself is resolved from, otherwise the art
    // lands under a name no view looks up.
    std::string name;
    if (!entry.rom.files.empty())
        name = entry.rom.files.front().fileName;
    if (name.empty())
        name = entry.rom.fsName;
    if (name.empty())
        name = entry.rom.name;
    return name.empty() ? std::string() : FileSystem::stemOf(name);
}

bool RomMArtFetcher::entryIsFetchable(const LibraryEntry& entry) const {
    if (entry.matchedSystemId.empty() || Config::instance().findSystem(entry.matchedSystemId) == nullptr)
        return false;

    if (entry.downloadedLocally && !entry.localPath.empty())
        return true;

    // Remote rows get images only, and only when the toggle is on: a big library would otherwise
    // write an asset for every ROM on the server rather than the ones present on the console.
    return config_.artForRemoteRoms && !stemFor(entry).empty();
}

bool RomMArtFetcher::entryArtComplete(const LibraryEntry& entry) const {
    if (!entryIsFetchable(entry))
        return true;

    auto& cfg = Config::instance();
    auto& cache = AppState::instance().metaCache();
    const std::string stem = stemFor(entry);

    if (cfg.scrapeBoxArt() && !present(cache.artworkPath(entry.matchedSystemId, stem, "box")))
        return false;
    if (cfg.scrapeThumbnail() && !present(cache.artworkPath(entry.matchedSystemId, stem, "thumb")))
        return false;

    // Video, manual and gamelist.xml are not written for remote rows, so they do not make an
    // entry incomplete.
    if (!entry.downloadedLocally)
        return true;

    if (cfg.scrapeVideo() && AppState::instance().videoAllowed() && !entry.rom.videoPath.empty()
        && !present(cache.videoCachePath(entry.matchedSystemId, stem)))
        return false;
    if (cfg.scrapeManual() && !entry.rom.manualPath.empty()) {
        const SystemConfig* sys = cfg.findSystem(entry.matchedSystemId);
        if (sys && !present(ManualPages::pdfPath(sys->path, stem)))
            return false;
    }
    return true;
}

void RomMArtFetcher::report(sf::ScrapeProgress& progress, const std::string& phase,
                            const std::string& detail, bool hasResult, bool success,
                            const ArtProgressCb& onProgress) {
    progress.phase = phase;
    progress.detail = detail;
    progress.hasResult = hasResult;
    progress.success = success;

    if (hasResult) {
        ScrapeLog::writef("[RomMArt] [%zu/%zu] %s — %s: %s", progress.index, progress.total,
                          progress.gameName.c_str(), phase.c_str(), detail.c_str());
    }

    if (onProgress)
        onProgress(progress);
}

bool RomMArtFetcher::downloadTo(const std::string& url, const std::string& dest,
                                std::atomic<bool>* abort) {
    if (url.empty() || dest.empty() || (abort && abort->load()))
        return false;

    FileSystem::createDirectories(FileSystem::parentPath(dest));

    // Same .part + atomic rename pattern as the ROM downloads, so an interrupted transfer never
    // leaves a half-written image that the UI would then try to decode.
    const std::string partPath = dest + ".part";
    sf::cloud::ProgressFn onBytes = [abort](int64_t, int64_t) -> bool {
        return abort && abort->load();
    };

    SF_LOG_I("RomMArt", "GET %s -> %s", url.c_str(), dest.c_str());
    if (!client_.downloadToUrl(url, partPath, onBytes, /*allowResume=*/false)) {
        SF_LOG_W("RomMArt", "%s", client_.lastError().c_str());
        return false;
    }

    // A 200 with no body can happen for a resource the server has not finished scanning; a
    // non-2xx (the SPA's HTML 404 page) is caught by the client's response check. Both would
    // otherwise be renamed into place and then fail to decode.
    const uint64_t size = FileSystem::fileSize(partPath);
    if (size == 0) {
        SF_LOG_W("RomMArt", "Empty response from %s", url.c_str());
        FileSystem::removeFile(partPath);
        return false;
    }

    if (!FileSystem::renameFileOverwrite(partPath, dest)) {
        SF_LOG_E("RomMArt", "Failed to rename %s to %s", partPath.c_str(), dest.c_str());
        FileSystem::removeFile(partPath);
        return false;
    }
    return true;
}

bool RomMArtFetcher::fetchImageAsset(const std::string& label, const std::string& dest,
                                     const std::vector<std::string>& rommPaths,
                                     const std::string& scraperUrl, bool thumbnail,
                                     sf::ScrapeProgress& progress, const ArtProgressCb& onProgress,
                                     std::atomic<bool>* abort, ArtFetchResult& result) {
    if (dest.empty())
        return false;

    if (present(dest)) {
        ++result.alreadyPresent;
        report(progress, label, "Skipped (already present)", true, false, onProgress);
        return true;
    }

    report(progress, label, "Downloading…", false, false, onProgress);

    auto& cfg = Config::instance();
    bool ok = false;
    bool viaRomM = false;

    for (const auto& path : rommPaths) {
        if (abort && abort->load())
            return false;
        if (path.empty())
            continue;

        std::string url = client_.assetUrl(path);
        // stb_image has no WebP decoder, so a WebP asset has to be fetched as its JPEG sibling.
        if (!decodableImageFormat(url))
            url = preferJpegUrl(url);
        if (!decodableImageFormat(url))
            continue;

        if (downloadTo(url, dest, abort)) {
            ok = true;
            viaRomM = true;
            break;
        }
    }

    if (!ok && config_.artFallbackScraper && !scraperUrl.empty()) {
        const int maxWidth = thumbnail ? kThumbMaxWidth : kBoxMaxWidth;
        const std::string url = cfg.scrapeOptimizedMedia() ? withServerResize(scraperUrl, maxWidth)
                                                           : scraperUrl;
        if (AppState::instance().scraper().downloadAsset(url, dest))
            ok = true;
    }

    if (ok) {
        // RomM serves the provider's original file, often larger than the UI needs.
        if (cfg.scrapeOptimizedMedia())
            optimizeScrapedImage(dest, thumbnail);
        if (viaRomM)
            ++result.fromRomM;
        else
            ++result.fromScraper;
    } else {
        ++result.failed;
    }

    const std::string detail = ok ? (viaRomM ? "OK (RomM cache)" : "OK (ScreenScraper fallback)") : "Failed";
    report(progress, label, detail, true, ok, onProgress);
    return ok;
}

bool RomMArtFetcher::fetchFileAsset(const std::string& label, const std::string& dest,
                                    const std::string& rommPath, sf::ScrapeProgress& progress,
                                    const ArtProgressCb& onProgress, std::atomic<bool>* abort,
                                    ArtFetchResult& result) {
    if (dest.empty())
        return false;

    if (present(dest)) {
        ++result.alreadyPresent;
        report(progress, label, "Skipped (already present)", true, false, onProgress);
        return true;
    }

    if (rommPath.empty()) {
        report(progress, label, "Not available", true, false, onProgress);
        return false;
    }

    report(progress, label, "Downloading…", false, false, onProgress);

    const bool ok = downloadTo(client_.assetUrl(rommPath), dest, abort);
    if (ok)
        ++result.fromRomM;
    else
        ++result.failed;

    report(progress, label, ok ? "OK (RomM cache)" : "Failed", true, ok, onProgress);
    return ok;
}

bool RomMArtFetcher::writeMetadata(const LibraryEntry& entry, const std::string& systemId,
                                   const std::string& stem) {
    if (entry.rom.name.empty() && entry.rom.summary.empty() && entry.rom.genres.empty()
        && entry.rom.developer.empty() && entry.rom.publisher.empty() && entry.rom.releaseDate.empty())
        return false;

    auto& cache = AppState::instance().metaCache();

    GameMetadata meta;
    if (const auto existing = cache.load(systemId, stem))
        meta = *existing;

    meta.systemId = systemId;
    meta.romPath = entry.localPath;

    // RomM has already resolved this metadata, so for a matched ROM it wins — the whole point of
    // this path is not to re-query ScreenScraper for the same game.
    if (!entry.rom.name.empty())
        meta.title = entry.rom.name;
    if (!entry.rom.summary.empty())
        meta.description = entry.rom.summary;
    if (!entry.rom.genres.empty())
        meta.genre = entry.rom.genres;
    if (!entry.rom.developer.empty())
        meta.developer = entry.rom.developer;
    if (!entry.rom.publisher.empty())
        meta.publisher = entry.rom.publisher;
    if (!entry.rom.releaseDate.empty())
        meta.releaseDate = entry.rom.releaseDate;
    meta.scraped = true;

    // Pick up the assets written by this pass, plus anything already on disk.
    cache.applyLocalMedia(meta, systemId, stem);
    return cache.save(meta);
}

ArtFetchResult RomMArtFetcher::fetchEntryArt(const LibraryEntry& entry, sf::ScrapeProgress& progress,
                                             const ArtProgressCb& onProgress, std::atomic<bool>* abort) {
    ArtFetchResult result;
    if (!entryIsFetchable(entry))
        return result;

    ScrapeLog::open();

    auto& cfg = Config::instance();
    auto& cache = AppState::instance().metaCache();
    const SystemConfig* sys = cfg.findSystem(entry.matchedSystemId);
    const std::string systemId = entry.matchedSystemId;
    const std::string stem = stemFor(entry);
    const bool remoteRow = !entry.downloadedLocally;

    progress.gameName = entry.rom.name.empty() ? RomTitle::fromStem(stem) : entry.rom.name;

    const std::string boxDest = cache.artworkPath(systemId, stem, "box");
    const std::string thumbDest = cache.artworkPath(systemId, stem, "thumb");
    // Remote rows are images only: the video player and the manual viewer both need the ROM's own
    // folder, and gamelist.xml is written only for ROMs the scanner can see.
    const std::string videoDest = (!remoteRow && cfg.scrapeVideo() && AppState::instance().videoAllowed())
                                      ? cache.videoCachePath(systemId, stem)
                                      : std::string();
    const std::string manualDest = (!remoteRow && cfg.scrapeManual() && sys)
                                       ? ManualPages::pdfPath(sys->path, stem)
                                       : std::string();

    // The big cover is preferred for the box because RomM stores it as PNG/JPEG more often than
    // the small one, which is usually WebP. Screenshots are JPEG, so they make a good thumbnail.
    if (cfg.scrapeBoxArt()) {
        fetchImageAsset("Box art", boxDest, {entry.rom.coverLargePath, entry.rom.coverSmallPath},
                        entry.rom.coverUrl, false, progress, onProgress, abort, result);
    }

    if (cfg.scrapeThumbnail()) {
        std::vector<std::string> sources = entry.rom.screenshotPaths;
        sources.push_back(entry.rom.coverLargePath);
        sources.push_back(entry.rom.coverSmallPath);
        fetchImageAsset("Thumbnail", thumbDest, sources, entry.rom.coverUrl, true, progress, onProgress,
                        abort, result);
    }

    if (!videoDest.empty())
        fetchFileAsset("Video", videoDest, entry.rom.videoPath, progress, onProgress, abort, result);

    if (!manualDest.empty())
        fetchFileAsset("Manual", manualDest, entry.rom.manualPath, progress, onProgress, abort, result);

    // gamelist.xml is only written for ROMs the scanner can see: an entry for a file that does not
    // exist locally would be dropped by the next scan. Remote rows get their art through
    // populateVirtualLibrary, which probes the images folder by stem.
    if (!remoteRow && writeMetadata(entry, systemId, stem)) {
        result.metadataWritten = true;
        report(progress, "Metadata", "gamelist.xml updated", true, true, onProgress);
    }

    return result;
}

} // namespace sf::romm
