#pragma once

#include <string>
#include <optional>
#include <ctime>

namespace sf::romm {

/** Conflict resolution policy for save synchronization. */
enum class ConflictPolicy {
    KeepBoth,   // Archive local copy, keep remote
    KeepLocal,  // Keep local, don't upload
    KeepRemote, // Download remote, overwrite local
    Ask         // Prompt user (not yet implemented)
};

/** RomM sync configuration stored in user settings. */
struct RomMConfig {
    bool enabled = false;
    std::string serverUrl;          // e.g. "https://romm.example.com"
    std::string deviceId;           // Assigned after registration
    std::string deviceName;         // Friendly name for this device
    bool syncSaves = true;          // Sync in-game save files (.srm, .sav, etc.)
    bool syncStates = true;         // Sync emulator save states
    bool syncBeforeLaunch = true;   // Download before launching a game
    bool syncAfterReturn = true;    // Upload on next NXStation startup
    ConflictPolicy conflictPolicy = ConflictPolicy::KeepBoth;
    bool stateCoreMatching = true;  // Only restore states to compatible cores
    std::string romMappingFile = "romm_mappings.json"; // ROM ID mapping file (must not collide with the library cache)

    /**
     * Art + metadata come from the RomM server's already-cached provider images instead of
     * querying ScreenScraper per ROM (one request per ROM instead of hash + API query +
     * rate-limited asset GETs). WebP is not decodable by stb_image, so PNG/JPEG sources are
     * preferred and WebP-only entries fall back to the ScreenScraper URL.
     */
    bool fetchArtFromRomM = true;
    bool artFallbackScraper = true;   // ScreenScraper URL for entries RomM has no cached media for
    /**
     * Also fetch covers/thumbnails for rows that are still remote. Art is resolved by stem from
     * the system's images folder, so no ROM file is needed — but a large library writes an image
     * for every ROM on the server, so this is separate from the downloaded-ROM path. Videos and
     * manuals stay with downloaded ROMs.
     */
    bool artForRemoteRoms = true;
    /**
     * Worker threads the background art queue fans out over (1-4). CurlHelper is mutex-guarded per
     * instance, so each worker needs its own client; the queue keeps one fork per worker rather than
     * sharing handles.
     */
    int artWorkers = 3;

    // Runtime-only (not persisted to config file)
    std::string accessToken;        // Bearer token for API calls
    std::string clientToken;        // Pairing client token
    time_t tokenExpiresAt = 0; // Token expiration
};

/** RomM sync state persisted separately from config. */
struct RomMSyncState {
    std::string deviceId;
    std::string lastSyncSessionId;
    time_t lastSuccessfulSync = 0;
    std::string lastSyncResult;     // Human-readable summary
};

/** Convert ConflictPolicy to string for storage. */
std::string conflictPolicyToString(ConflictPolicy policy);

/** Convert string to ConflictPolicy. */
ConflictPolicy conflictPolicyFromString(const std::string& str);

/** Default configuration file path. */
std::string defaultConfigPath();

/** Default sync state file path. */
std::string defaultSyncStatePath();

} // namespace sf::romm
