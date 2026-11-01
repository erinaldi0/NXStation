#pragma once

#include "cloud/CurlHelper.hpp"
#include "romm/RomMConfig.hpp"

#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <atomic>

namespace sf::romm {

/** Represents a RomM device. */
struct Device {
    std::string id;
    std::string name;
    std::string platform;
    std::string lastSeen;
};

/**
 * A save file or state, both as the client reports it during negotiate and as the server's
 * operation refers to it. The same shape is used in both directions.
 */
struct RomMSave {
    std::string id;            // Server save id; empty for upload and delete operations
    std::string romId;
    std::string slot;
    std::string deviceId;
    std::string sessionId;
    /** content_hash: this client's file hash when sending, the server's when parsing. */
    std::string hash;
    int64_t size = 0;
    std::string type;      // "save" or "state"
    std::string coreId;    // For save states
    /** file_name as the server stores it; needed to find the local file for a download. */
    std::string fileName;
    /** Negotiate operation: upload, download, conflict, delete, no_op. */
    std::string action;
    std::string reason;
    /** The local file this entry refers to; empty for an operation the server describes. */
    std::string localPath;
    /** Client mtime when sending, server updated_at when parsing (epoch seconds). */
    time_t updatedAt = 0;
    std::string createdAt;
};

/** One file of a ROM, as returned by RomM's RomFileSchema. */
struct RomFileEntry {
    long long id = 0;
    std::string fileName;
    int64_t sizeBytes = 0;
    bool topLevel = true;
};

/** Represents a ROM in the RomM library (SimpleRomSchema fields). */
struct RomMLibraryEntry {
    std::string id;
    std::string name;
    /** platform_fs_slug — this is what matches NXStation system ids (e.g. "psx"). */
    std::string platform;
    std::string platformDisplay;
    std::string region;
    std::string version;
    std::vector<RomFileEntry> files; // File names for multi-file ROMs
    /** File name as stored on the server (fs_name); used when the list response omits `files`. */
    std::string fsName;
    int64_t totalSize = 0;
    /** Folder the ROM lives in on the server (fs_path); empty for loose single files. */
    std::string folderName;
    bool multiFile = false;
    /** Metadata RomM already resolved (ScreenScraper / IGDB), used for the focus panel. */
    std::string summary;
    std::string genres;
    std::string developer;
    std::string publisher;
    std::string releaseDate;
    std::string coverUrl;
    /**
     * Provider images RomM has already fetched and cached on disk (SimpleRomSchema paths).
     * Served under `{baseUrl}/assets/romm/resources/...`, so art downloads need no provider
     * query. Paths may carry a `?ts=` cache-buster; `RomMArtFetcher::assetUrl` resolves them.
     */
    std::string coverSmallPath;
    std::string coverLargePath;
    std::vector<std::string> screenshotPaths;
    std::string videoPath;
    std::string manualPath;
    /** Sibling ROMs of a multi-disc set (e.g. The Legend of Dragoon discs). */
    std::vector<std::string> siblingNames;

    /** Convenience: plain file names, for callers that do not need the per-file id/size. */
    std::vector<std::string> fileNames() const
    {
        std::vector<std::string> names;
        names.reserve(files.size());
        for (const auto& f : files)
            names.push_back(f.fileName);
        return names;
    }
};

/** Pairing response from /api/client-tokens/exchange. */
struct PairingResponse {
    std::string clientToken;
    std::string accessToken;
    time_t expiresAt = 0;
};

/** Sync negotiation response: the session plus the operations the client must perform. */
struct NegotiateResponse {
    std::string sessionId;
    std::vector<RomMSave> operations;
    size_t totalUpload = 0;
    size_t totalDownload = 0;
    size_t totalConflict = 0;
    size_t totalDelete = 0;
    size_t totalNoOp = 0;
};

/** Upload save response. */
struct UploadResponse {
    std::string saveId;
    std::string sessionId;
};

/** Result of a sync operation. */
struct SyncResult {
    bool ok = false;
    std::string message;
    size_t uploaded = 0;
    size_t downloaded = 0;
    size_t conflicts = 0;
    size_t failed = 0;
};

/** RomM HTTP client using the project's existing libcurl infrastructure. */
class RomMClient {
public:
    RomMClient();
    ~RomMClient();

    /** Initialize with server URL and credentials. */
    void configure(const std::string& serverUrl, const std::string& accessToken = "",
                   const std::string& clientToken = "");

    /** Check if client is configured and ready. */
    bool isReady() const { return ready_; }

    /** Get last error message. */
    std::string lastError() const { return lastError_; }

    // Pairing & Device Registration
    bool exchangePairingCode(const std::string& code, PairingResponse& out);
    bool registerDevice(const std::string& deviceName, Device& out);
    bool updateDevice(const std::string& deviceId, const std::string& name);

    /**
     * Device id the sync calls carry: RomM takes it at the top level of negotiate (a device-bound
     * token is the only way to omit it) and as a query parameter on the save endpoints, so it is
     * set here once the device is registered rather than per save.
     */
    void setDeviceId(const std::string& deviceId) { deviceId_ = deviceId; }

    // Save Sync Protocol
    /**
     * rom_ids scopes the negotiation to the ROMs this device holds. RomM offers downloads for the
     * whole save library when no scope is given, so a client that carries part of the library has
     * to send the scope; at most 500 ids per request.
     */
    bool negotiate(const std::vector<RomMSave>& localSaves, NegotiateResponse& out,
                   const std::vector<std::string>& romIds = {});
    bool uploadSave(const std::string& localPath, const RomMSave& meta, UploadResponse& out);
    bool downloadSave(const std::string& saveId, const std::string& localPath,
                      const std::string& sessionId = "");
    bool completeSyncSession(const std::string& sessionId,
                             size_t operationsCompleted, size_t operationsFailed);

    // Library API
    bool fetchLibrary(std::vector<RomMLibraryEntry>& out,
                      const std::function<void(int fetched, int total)>& progress = {},
                      std::atomic<bool>* abort = nullptr);
    /** Single ROM as SimpleRomSchema; used when the list response omits the `files` array. */
    bool fetchSimpleRom(const std::string& romId, RomMLibraryEntry& out);
    bool downloadRom(const std::string& romId, const std::string& fileName,
                     const std::string& localPath, const sf::cloud::ProgressFn& progress = {},
                     bool allowResume = false);
    bool downloadToUrl(const std::string& url, const std::string& localPath,
                       const sf::cloud::ProgressFn& progress = {}, bool allowResume = false);
    std::string buildUrlPublic(const std::string& path) const { return buildUrl(path); }

    /**
     * A second client on its own curl handle, for a worker thread. CurlHelper is mutex-guarded per
     * instance, so a fan-out over several workers would serialize on one handle; each worker takes a
     * fork. Same server and tokens, independent transfers.
     */
    std::unique_ptr<RomMClient> fork() const;

    /**
     * URL for one of RomM's cached provider images. Resource paths returned by the API
     * (`path_cover_small`, `path_video`, ...) are relative to the static mount
     * `{baseUrl}/assets/romm/resources/`, so this must not use buildUrl(), which injects /api.
     * A `?ts=` cache-buster is kept, and an already-absolute URL is passed through unchanged.
     */
    std::string assetUrl(const std::string& path) const;

private:
    sf::cloud::CurlHelper curl_;
    std::string baseUrl_;
    std::string accessToken_;
    std::string clientToken_;
    std::string deviceId_;
    bool ready_ = false;
    mutable std::string lastError_;

    std::string buildUrl(const std::string& path) const;
    void updateAuthHeaders();
    bool checkResponse(const sf::cloud::CurlResponse& resp, const char* operation);
    /** FastAPI answers a 4xx with {"detail": "..."}; without it the user only sees a status code. */
    static std::string errorDetail(const std::string& body);
};

} // namespace sf::romm
