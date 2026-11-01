#pragma once

#include "romm/RomMClient.hpp"
#include "romm/RomMConfig.hpp"
#include "romm/RomMSaveMapper.hpp"
#include "romm/RomMStateStore.hpp"
#include "romm/RomMSyncCandidate.hpp"

#include <string>
#include <vector>
#include <functional>
#include <atomic>

namespace sf::romm {

/** Progress callback for sync operations. */
using SyncProgressCallback = std::function<void(const std::string& stage, int percent)>;

/** Detailed progress for UI. */
struct SyncDetailedProgress {
    enum class Stage {
        Idle,
        Connecting,
        Negotiating,
        Downloading,
        Uploading,
        ResolvingConflicts,
        Completing,
        Done,
        Error
    };
    Stage stage = Stage::Idle;
    std::string message;
    std::string detail;
    int percent = 0;
    size_t currentFile = 0;
    size_t totalFiles = 0;
};

using SyncDetailedCallback = std::function<void(const SyncDetailedProgress&)>;

/**
 * RomM sync engine — orchestrates the sync protocol.
 *
 * The engine is split into two phases so the file picker can sit between them:
 * prepare (scan + negotiate, no side effects) and apply (run the selected candidates).
 * The full-sync entry points are just prepare + select-all + apply, so the automatic
 * launch/return flows behave exactly as before.
 */
class RomMSyncEngine {
public:
    RomMSyncEngine(RomMClient& client, RomMSaveMapper& mapper, RomMStateStore& store,
                   const RomMConfig& config);

    /**
     * Prepare candidates for one ROM (by NXStation game path) without touching anything.
     * Returns an empty vector when `errorOut` is not empty.
     */
    std::vector<SyncCandidate> prepareCandidates(const std::string& romPath, const std::string& systemId,
                                                  const std::string& coreId, std::string& errorOut,
                                                  std::atomic<bool>* abort = nullptr);

    /** Prepare candidates for every mapped ROM that is downloaded locally. */
    std::vector<SyncCandidate> prepareCandidatesAll(std::string& errorOut,
                                                     std::atomic<bool>* abort = nullptr);

    /** Run only the selected candidates; each candidate gets its result written back. */
    bool applyCandidates(std::vector<SyncCandidate>& candidates,
                         SyncDetailedCallback progress = {}, std::atomic<bool>* abort = nullptr);

    /** Run a full sync for a specific ROM (prepare + select-all + apply). */
    bool syncForRom(const std::string& romPath, const std::string& systemId,
                    const std::string& coreId, SyncDetailedCallback progress = {},
                    std::atomic<bool>* abort = nullptr);

    /** Run a full sync for all configured ROMs (prepare + select-all + apply). */
    bool syncAll(SyncDetailedCallback progress = {}, std::atomic<bool>* abort = nullptr);

    /** Get last sync result summary. */
    std::string lastResult() const { return lastResult_; }

    /** Check if sync is in progress. */
    bool isSyncInProgress() const { return syncInProgress_.load(); }

private:
    RomMClient& client_;
    RomMSaveMapper& mapper_;
    RomMStateStore& store_;
    const RomMConfig& config_;

    std::atomic<bool> syncInProgress_{false};
    std::string lastResult_;

    /** Local scan entries for one mapped ROM; what negotiate is sent. */
    struct PreparedRom {
        RomMapping mapping;
        std::vector<RomMSave> localEntries;
    };

    /** The picker's default per-file choice, seeded from the global policy. */
    SyncCandidate::Resolution seedResolution() const;

    /** Best-effort device name cache fill; failures are not fatal, the cache is just thinner. */
    void cacheDeviceNames();
    std::string deviceNameFor(const std::string& deviceId) const;

    std::vector<RomMSave> collectLocalFiles(const RomMapping& mapping, std::atomic<bool>* abort);
    bool negotiateFor(const std::vector<PreparedRom>& roms, NegotiateResponse& out,
                      std::atomic<bool>* abort);

    /** Runs one candidate under its resolution; false = failed. */
    bool applyCandidate(const SyncCandidate& c, std::string& result);
    bool performDownload(const SyncCandidate& c, std::string& result);
    bool performUpload(const SyncCandidate& c, std::string& result);
    bool performDelete(const SyncCandidate& c, std::string& result);

    /** Apply without taking the guard; callers that hold it (the full-sync wrappers) use this. */
    bool applyCandidatesLocked(std::vector<SyncCandidate>& candidates,
                               const SyncDetailedCallback& progress, std::atomic<bool>* abort);

    /** RomM hashes archives over their entries rather than the raw file, so that hash is not comparable. */
    static bool looksLikeZipArchive(const std::string& path);

    void emitProgress(const SyncDetailedCallback& progress, SyncDetailedProgress::Stage stage,
                      const std::string& message, const std::string& detail = "",
                      int percent = -1, size_t current = 0, size_t total = 0);

    std::string computeFileHash(const std::string& path);
    bool writeFileAtomic(const std::string& path, const std::string& tempPath);
};

} // namespace sf::romm
