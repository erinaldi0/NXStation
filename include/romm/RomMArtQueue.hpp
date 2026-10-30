#pragma once

#include "romm/RomMArtFetcher.hpp"
#include "romm/RomMClient.hpp"
#include "romm/RomMConfig.hpp"
#include "romm/RomMLibraryService.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sf::romm {

/** Live counters for the background job — what the settings row and the home indicator draw. */
struct ArtJobStatus {
    bool started = false;
    bool running = false;
    bool stopped = false;
    size_t total = 0;     // entries this run was asked to walk
    size_t processed = 0; // entries finished since the run started
    size_t succeeded = 0;
    size_t failed = 0;
    size_t skipped = 0;
    std::string current;  // entry being worked on
    std::string summary;  // last finished run
};

using ArtStatusCb = std::function<void(const ArtJobStatus&)>;

/**
 * Background art job: entries are queued and drained by a few worker threads, so a large library
 * never keeps the user parked on a progress page. The queue owns pending work, the counters, the
 * listeners and the persisted pending ids; RomMArtFetcher stays the per-entry worker and is given
 * one forked client per worker (CurlHelper serializes on one mutex per instance).
 *
 * Workers run on their own threads rather than AppState's pool, which has 3 slots (1 in applet mode)
 * shared by the library fetch, save sync, ROM downloads and texture loads — a fan-out there would
 * starve the rest of the app.
 *
 * Pending rom ids survive a launch handoff in `data/romm_art_queue.json`. NXStation exits when a core
 * is chain-loaded, so the job stops at handoff and resumes at the next startup from what is left.
 */
class RomMArtQueue {
public:
    RomMArtQueue(RomMClient& client, const RomMConfig& config, RomMLibraryService& library);
    ~RomMArtQueue();

    RomMArtQueue(const RomMArtQueue&) = delete;
    RomMArtQueue& operator=(const RomMArtQueue&) = delete;

    /**
     * Queue the entries that still need an asset — the filter is entryIsFetchable + entryArtComplete,
     * so a rerun carries only the work left over instead of the whole library. Returns how many rows
     * were actually queued. Copies are stored: the library cache can reallocate its entries.
     */
    size_t enqueue(const std::vector<LibraryEntry>& entries);
    size_t enqueueEntry(const LibraryEntry& entry);

    /** Spawn the workers on the first call and drain the queue. No-op when nothing is pending. */
    void start();

    /**
     * Stop after the asset in flight. Queued entries stay queued for the next start; `stopped` shows
     * in the status row, and the persisted id list is what a later launch reloads.
     */
    void requestStop();

    bool isRunning() const;
    size_t pendingCount() const;

    ArtJobStatus status() const;
    /** One line for a settings row or the home indicator, e.g. "Running — 12/340 · Chrono Cross". */
    std::string statusLine() const;

    /**
     * Status listeners, called on the main thread. Settings rows must deregister in their destructor
     * because the settings panel rebuilds and destroys them; the returned id is what to remove.
     */
    int addListener(ArtStatusCb cb);
    void removeListener(int id);

    /** Rebuild the pending set from data/romm_art_queue.json (main thread, after the library cache). */
    bool loadPending();

private:
    void workerLoop(int index);
    void notifyLocked();
    void savePendingLocked();

    RomMClient& client_;
    const RomMConfig& config_;
    RomMLibraryService& library_;
    /** Only filters entries at enqueue time; each worker gets its own fetcher over its own client. */
    RomMArtFetcher filter_;

    std::vector<LibraryEntry> pending_;
    ArtJobStatus status_;
    mutable std::mutex mutex_;
    std::map<int, ArtStatusCb> listeners_;
    int nextListenerId_ = 1;

    std::vector<std::thread> workers_;
    std::vector<bool> finished_;
    size_t activeWorkers_ = 0;
    std::atomic<bool> stopFlag_{false};
    size_t processedSinceSave_ = 0;
};

} // namespace sf::romm
