#include "romm/RomMArtQueue.hpp"

#include "app/AppState.hpp"
#include "scraper/MetadataCache.hpp"
#include "util/FileSystem.hpp"
#include "util/Json.hpp"
#include "util/Logger.hpp"
#include "util/Network.hpp"
#include "util/Paths.hpp"
#include "util/ScrapeLog.hpp"

#include <borealis.hpp>

namespace sf::romm {

namespace {

/** Pending rom ids, so a job interrupted by a launch (or a power off) continues at the next startup. */
std::string queuePath() {
    return std::string(sf::paths::DATA_DIR) + "/romm_art_queue.json";
}

int clampWorkers(int value) {
    if (value < 1)
        return 1;
    if (value > 4)
        return 4;
    return value;
}

std::string buildSummary(const ArtJobStatus& status, bool stopped) {
    std::string summary = stopped ? "Stopped — " : "Finished — ";
    summary += std::to_string(status.succeeded) + " OK, " + std::to_string(status.failed) + " failed, "
               + std::to_string(status.skipped) + " skipped";
    if (status.processed < status.total)
        summary += ", " + std::to_string(status.total - status.processed) + " left";
    return summary;
}

} // namespace

RomMArtQueue::RomMArtQueue(RomMClient& client, const RomMConfig& config, RomMLibraryService& library)
    : client_(client), config_(config), library_(library), filter_(client, config) {}

RomMArtQueue::~RomMArtQueue() {
    stopFlag_.store(true);
    // Every worker exits after the asset in flight. Joining here is what the singleton does at
    // shutdown, the same way the app pool's destructor waits for its tasks.
    for (auto& worker : workers_) {
        if (worker.joinable())
            worker.join();
    }
}

size_t RomMArtQueue::enqueue(const std::vector<LibraryEntry>& entries) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t queued = 0;
    for (const auto& entry : entries) {
        // The same filter the old serial batch used: only rows that still need an asset are carried,
        // so a rerun is the remaining work rather than the whole library.
        if (!filter_.entryIsFetchable(entry) || filter_.entryArtComplete(entry))
            continue;
        pending_.push_back(entry);
        ++queued;
    }

    if (queued == 0)
        return 0;

    status_.started = true;
    status_.total += queued;
    ScrapeLog::open();
    ScrapeLog::writef("[RomMArt] Queued %zu entries (queue now %zu)", queued, pending_.size());
    savePendingLocked();
    return queued;
}

size_t RomMArtQueue::enqueueEntry(const LibraryEntry& entry) {
    return enqueue(std::vector<LibraryEntry>{entry});
}

void RomMArtQueue::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (pending_.empty() || !client_.isReady())
        return;

    // Join workers that have already exited (join on a finished thread returns at once), so a job can
    // be stopped and started again without accumulating threads. Only done when nothing is draining,
    // which is what keeps this from blocking the caller on a worker still mid-transfer.
    if (activeWorkers_ == 0) {
        for (auto& worker : workers_) {
            if (worker.joinable())
                worker.join();
        }
        workers_.clear();
        finished_.clear();
    }

    stopFlag_.store(false);
    status_.stopped = false;
    status_.started = true;
    status_.running = true;
    ScrapeLog::open();

    if (activeWorkers_ > 0) {
        // Workers are already draining; the new entries are picked up by the existing loop.
        return;
    }

    const int workers = clampWorkers(config_.artWorkers);
    for (int i = 0; i < workers; ++i) {
        finished_.push_back(false);
        workers_.emplace_back([this, i] { workerLoop(i); });
    }
    activeWorkers_ = workers;
    ScrapeLog::writef("[RomMArt] Job start: %zu entries, %d workers", pending_.size(), workers);
    notifyLocked();
}

void RomMArtQueue::requestStop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status_.running)
        return;

    stopFlag_.store(true);
    status_.stopped = true;
    status_.running = false;
    status_.summary = buildSummary(status_, true);
    savePendingLocked();
    ScrapeLog::write("[RomMArt] Job stopped — queued entries kept for the next start");
    notifyLocked();
}

bool RomMArtQueue::isRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_.running;
}

size_t RomMArtQueue::pendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

ArtJobStatus RomMArtQueue::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::string RomMArtQueue::statusLine() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (status_.stopped) {
        std::string line = "Stopped — " + std::to_string(status_.processed) + "/" + std::to_string(status_.total);
        if (!pending_.empty())
            line += ", " + std::to_string(pending_.size()) + " left";
        return line;
    }
    if (status_.running) {
        std::string line = "Running — " + std::to_string(status_.processed) + "/" + std::to_string(status_.total);
        if (!status_.current.empty())
            line += " · " + status_.current;
        return line;
    }
    if (!status_.summary.empty())
        return status_.summary;
    if (!pending_.empty())
        return std::to_string(pending_.size()) + " entries queued";
    return "Idle";
}

int RomMArtQueue::addListener(ArtStatusCb cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int id = nextListenerId_++;
    listeners_[id] = std::move(cb);
    return id;
}

void RomMArtQueue::removeListener(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.erase(id);
}

bool RomMArtQueue::loadPending() {
    const std::string data = FileSystem::readFile(queuePath());
    if (data.empty())
        return false;

    Json j;
    try {
        j = Json::parse(data);
    } catch (const std::exception& ex) {
        SF_LOG_W("RomMArt", "queue file parse error: %s", ex.what());
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();
    for (const auto& item : j["pending"]) {
        const std::string id = item.get<std::string>();
        if (id.empty())
            continue;
        // Ids only: the entries themselves come from the library cache, which is what the UI rows are
        // built from. Ids whose ROM was removed since the last run drop out here.
        for (const auto& entry : library_.entries()) {
            if (entry.rom.id == id) {
                pending_.push_back(entry);
                break;
            }
        }
    }

    status_.processed = 0;
    status_.succeeded = 0;
    status_.failed = 0;
    status_.skipped = 0;
    status_.total = pending_.size();
    status_.stopped = false;
    status_.current.clear();
    if (!pending_.empty())
        ScrapeLog::writef("[RomMArt] Restored %zu pending entries from %s", pending_.size(), queuePath().c_str());
    return !pending_.empty();
}

void RomMArtQueue::workerLoop(int index) {
    // One handle per worker: sharing the service client would serialize every GET on CurlHelper's mutex.
    auto client = client_.fork();
    RomMArtFetcher fetcher(*client, config_);

    struct NetGuard {
        NetGuard() { Network::acquireSession(); }
        ~NetGuard() { Network::releaseSession(); }
    } net;

    while (!stopFlag_.load()) {
        // NXStation exits when a core is chain-loaded, so "pause until the game exits" means stopping
        // here and letting the persisted queue carry the rest to the next startup. The check is before
        // the pop so the entry is never lost.
        if (AppState::instance().isHandoffPending()) {
            ScrapeLog::write("[RomMArt] Launch handoff pending — job pauses, queue preserved for the next start");
            std::lock_guard<std::mutex> lock(mutex_);
            savePendingLocked();
            break;
        }

        LibraryEntry entry;
        size_t indexSnapshot = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_.empty())
                break;
            entry = std::move(pending_.front());
            pending_.erase(pending_.begin());
            ++status_.processed;
            status_.current = entry.rom.name;
            indexSnapshot = status_.processed;
            if (++processedSinceSave_ >= 25) {
                savePendingLocked();
                processedSinceSave_ = 0;
            }
        }

        sf::ScrapeProgress progress;
        progress.total = status().total;
        progress.index = indexSnapshot;
        progress.gameName = entry.rom.name;

        const ArtFetchResult result = fetcher.fetchEntryArt(entry, progress, ArtProgressCb{}, &stopFlag_);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (result.anyFetched() || result.metadataWritten)
                ++status_.succeeded;
            else if (result.failed > 0)
                ++status_.failed;
            else
                ++status_.skipped;

            // Rows read art through the metadata cache, so drop what this entry touched; the settings
            // row rebuilds the whole panel once the job ends rather than per entry.
            if (!entry.matchedSystemId.empty())
                AppState::instance().metaCache().invalidateSystem(entry.matchedSystemId);

            if (pending_.empty())
                status_.summary = buildSummary(status_, stopFlag_.load());
        }

        notifyLocked();
    }

    bool last = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_[index] = true;
        if (--activeWorkers_ == 0) {
            status_.running = false;
            status_.stopped = stopFlag_.load();
            status_.current.clear();
            status_.summary = buildSummary(status_, status_.stopped);
            savePendingLocked();
            last = true;
        }
    }
    if (last) {
        ScrapeLog::write("[RomMArt] " + status_.summary);
        SF_LOG_I("RomMArt", "%s", status_.summary.c_str());
        notifyLocked();
    }
}

void RomMArtQueue::savePendingLocked() {
    Json ids = Json::array();
    for (const auto& entry : pending_)
        ids.push_back(entry.rom.id);

    Json root = Json::object();
    root["pending"] = ids;

    const std::string path = queuePath();
    FileSystem::createDirectories(FileSystem::parentPath(path));
    if (!FileSystem::writeFile(path, root.dump(2)))
        SF_LOG_W("RomMArt", "Failed to save %s", path.c_str());
}

void RomMArtQueue::notifyLocked() {
    // Called with the mutex held: snapshot, then call outside it so a listener that re-enters the
    // queue (a row asking for status) cannot deadlock.
    ArtJobStatus snapshot = status_;
    std::vector<ArtStatusCb> cbs;
    cbs.reserve(listeners_.size());
    for (const auto& [id, cb] : listeners_)
        cbs.push_back(cb);
    if (cbs.empty())
        return;

    // Listeners are UI rows; callbacks are marshalled to the main thread. Copies of the functions stay
    // valid after a row is destroyed because rows deregister and check their own alive flag.
    brls::sync([cbs, snapshot]() {
        for (const auto& cb : cbs)
            cb(snapshot);
    });
}

} // namespace sf::romm
