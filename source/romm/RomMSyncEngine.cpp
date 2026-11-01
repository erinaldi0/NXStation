#include "romm/RomMSyncEngine.hpp"
#include "romm/RomMClient.hpp"
#include "romm/RomMSaveMapper.hpp"
#include "romm/RomMStateStore.hpp"
#include "romm/RomMConfig.hpp"
#include "util/FileSystem.hpp"
#include "util/Logger.hpp"
#include "util/Hash.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <borealis.hpp>

namespace sf::romm {

namespace {

/** "save (slot)" / "state (slot)" for the progress line. */
std::string describeOperation(const SyncCandidate& c) {
    return c.type + " (" + (c.slot.empty() ? "no slot" : c.slot) + ")";
}

const char* actionWord(const SyncCandidate& c) {
    switch (c.action) {
        case SyncCandidate::Action::Upload: return "Uploading";
        case SyncCandidate::Action::Download: return "Downloading";
        case SyncCandidate::Action::Conflict: return "Conflict";
        case SyncCandidate::Action::Delete: return "Processing deletion";
    }
    return "Operation";
}

SyncDetailedProgress::Stage actionStage(const SyncCandidate& c) {
    switch (c.action) {
        case SyncCandidate::Action::Upload: return SyncDetailedProgress::Stage::Uploading;
        case SyncCandidate::Action::Download: return SyncDetailedProgress::Stage::Downloading;
        case SyncCandidate::Action::Conflict:
        case SyncCandidate::Action::Delete: return SyncDetailedProgress::Stage::ResolvingConflicts;
    }
    return SyncDetailedProgress::Stage::Idle;
}

} // namespace

RomMSyncEngine::RomMSyncEngine(RomMClient& client, RomMSaveMapper& mapper, RomMStateStore& store,
                               const RomMConfig& config)
    : client_(client), mapper_(mapper), store_(store), config_(config) {}

SyncCandidate::Resolution RomMSyncEngine::seedResolution() const {
    switch (config_.conflictPolicy) {
        case ConflictPolicy::KeepLocal: return SyncCandidate::Resolution::KeepLocal;
        case ConflictPolicy::KeepRemote: return SyncCandidate::Resolution::KeepRemote;
        // The picker is the Ask implementation: a conflict row starts uncommitted, and the
        // picker's Apply honors whatever the user sets per row. Without a selection UI (the
        // auto wrappers), Acknowledge is what Ask means — acknowledge, change neither side.
        case ConflictPolicy::Ask: return SyncCandidate::Resolution::Acknowledge;
        case ConflictPolicy::KeepBoth: break;
    }
    return SyncCandidate::Resolution::KeepBoth;
}

void RomMSyncEngine::cacheDeviceNames() {
    // Best effort. A plain API token may not see the device list; the persisted cache from the
    // first successful fetch is what the picker shows offline.
    std::vector<Device> devices;
    if (!client_.fetchDevices(devices))
        return;
    bool changed = false;
    for (const auto& d : devices) {
        if (d.name.empty())
            continue;
        if (store_.getDeviceName(d.id) != d.name) {
            store_.setDeviceName(d.id, d.name);
            changed = true;
        }
    }
    if (changed)
        store_.save();
}

std::string RomMSyncEngine::deviceNameFor(const std::string& deviceId) const {
    if (deviceId.empty())
        return "unknown";
    if (deviceId == config_.deviceId)
        return config_.deviceName.empty() ? "This device" : config_.deviceName;
    const auto cached = store_.getDeviceName(deviceId);
    if (cached && !cached->empty())
        return *cached;
    return deviceId;
}

std::vector<RomMSave> RomMSyncEngine::collectLocalFiles(const RomMapping& mapping,
                                                         std::atomic<bool>* abort) {
    if (abort && abort->load())
        return {};

    auto localFiles = mapper_.discoverSaves(mapping.romPath, mapping.systemId, mapping.coreId);

    // Convert LocalSaveFile to RomMSave for negotiation
    std::vector<RomMSave> localEntries;
    localEntries.reserve(localFiles.size());
    for (const auto& local : localFiles) {
        RomMSave save;
        save.romId = local.rommRomId;
        save.slot = local.slot;
        save.deviceId = config_.deviceId;
        save.type = local.type;
        save.hash = local.hash;
        save.size = local.size;
        save.coreId = local.coreId;
        save.localPath = local.localPath;
        // The two fields negotiate requires besides the hash: the name as it sits on disk, and its
        // mtime (RomM pairs a slot by which side is newer, so a zero timestamp makes every
        // comparison the server's win).
        save.fileName = FileSystem::filenameOf(local.localPath);
        save.updatedAt = local.modifiedTime;
        localEntries.push_back(std::move(save));
    }

    // Filter by config
    if (!config_.syncSaves) {
        localEntries.erase(
            std::remove_if(localEntries.begin(), localEntries.end(),
                           [](const RomMSave& s) { return s.type == "save"; }),
            localEntries.end());
    }
    if (!config_.syncStates) {
        localEntries.erase(
            std::remove_if(localEntries.begin(), localEntries.end(),
                           [](const RomMSave& s) { return s.type == "state"; }),
            localEntries.end());
    }

    SF_LOG_I("RomM", "Found %zu local save file(s) for %s", localEntries.size(), mapping.romPath.c_str());
    return localEntries;
}

bool RomMSyncEngine::negotiateFor(const std::vector<PreparedRom>& roms, NegotiateResponse& out,
                                  std::atomic<bool>* abort) {
    if (abort && abort->load())
        return false;

    std::vector<RomMSave> sends;
    std::vector<std::string> romIds;
    for (const auto& rom : roms) {
        for (const auto& local : rom.localEntries)
            sends.push_back(local);
        romIds.push_back(rom.mapping.rommRomId);
    }

    // The scope is the prepared ROMs. Without it RomM answers with the device's whole save
    // library, and a save for a game that is not on this device has no local folder to be
    // written to. RomM caps the scope at 500 ids per request.
    std::vector<std::string> scope;
    scope.reserve(std::min(romIds.size(), size_t(500)));
    for (size_t i = 0; i < romIds.size() && i < 500; ++i)
        scope.push_back(romIds[i]);

    if (!client_.negotiate(sends, out, scope)) {
        lastResult_ = "Negotiation failed: " + client_.lastError();
        return false;
    }

    SF_LOG_I("RomM", "Negotiation: %zu upload(s), %zu download(s), %zu conflict(s), %zu delete(s), %zu no-op",
             out.totalUpload, out.totalDownload, out.totalConflict, out.totalDelete, out.totalNoOp);
    return true;
}

std::vector<SyncCandidate> RomMSyncEngine::prepareCandidates(const std::string& romPath,
                                                              const std::string& systemId,
                                                              const std::string& coreId,
                                                              std::string& errorOut,
                                                              std::atomic<bool>* abort) {
    errorOut.clear();

    auto mappingOpt = mapper_.findMapping(romPath, systemId);
    if (!mappingOpt) {
        errorOut = "No RomM mapping for this ROM";
        return {};
    }
    if (!FileSystem::exists(mappingOpt->romPath)) {
        errorOut = "ROM is not downloaded locally: " + FileSystem::filenameOf(mappingOpt->romPath);
        return {};
    }
    if (config_.deviceId.empty()) {
        errorOut = "RomM is not paired (no device id)";
        return {};
    }
    client_.setDeviceId(config_.deviceId);

    PreparedRom rom;
    rom.mapping = *mappingOpt;
    rom.localEntries = collectLocalFiles(rom.mapping, abort);
    if (abort && abort->load())
        return {};

    cacheDeviceNames();

    NegotiateResponse negotiation;
    if (!negotiateFor({rom}, negotiation, abort)) {
        errorOut = lastResult_;
        return {};
    }

    return buildCandidates(
        rom.mapping, rom.localEntries, negotiation, seedResolution(),
        [this](const RomMSave& op) {
            if (auto existing = mapper_.resolveLocalPath(op))
                return existing;
            return mapper_.downloadTargetPath(op);  // a download has no file yet: build the destination
        },
        [this](const std::string& deviceId) { return deviceNameFor(deviceId); },
        [](const std::string& fileName) { return RomMSaveMapper::isStateFile(fileName); });
}

std::vector<SyncCandidate> RomMSyncEngine::prepareCandidatesAll(std::string& errorOut,
                                                                 std::atomic<bool>* abort) {
    errorOut.clear();

    if (config_.deviceId.empty()) {
        errorOut = "RomM is not paired (no device id)";
        return {};
    }
    client_.setDeviceId(config_.deviceId);

    // Only the ROMs that are on this device. A mapping for a game that was never downloaded has no
    // local folder, so a save for it has nowhere to go — that was the "Cannot resolve local path"
    // warning repeating once per ROM.
    std::vector<PreparedRom> roms;
    for (const auto& mapping : mapper_.allMappings()) {
        if (abort && abort->load())
            break;
        if (!FileSystem::exists(mapping.romPath))
            continue;
        PreparedRom rom;
        rom.mapping = mapping;
        rom.localEntries = collectLocalFiles(mapping, abort);
        roms.push_back(std::move(rom));
    }

    if (roms.empty()) {
        errorOut = "Nothing to sync yet — download a ROM from the RomM library first";
        return {};
    }

    cacheDeviceNames();

    NegotiateResponse negotiation;
    if (!negotiateFor(roms, negotiation, abort)) {
        errorOut = lastResult_;
        return {};
    }

    std::vector<SyncCandidate> out;
    auto pathResolver = [this](const RomMSave& op) -> std::optional<std::string> {
        if (auto existing = mapper_.resolveLocalPath(op))
            return existing;
        return mapper_.downloadTargetPath(op);
    };
    for (const auto& rom : roms) {
        auto candidates = buildCandidates(
            rom.mapping, rom.localEntries, negotiation, seedResolution(),
            pathResolver,
            [this](const std::string& deviceId) { return deviceNameFor(deviceId); },
            [](const std::string& fileName) { return RomMSaveMapper::isStateFile(fileName); });
        out.insert(out.end(), std::make_move_iterator(candidates.begin()),
                   std::make_move_iterator(candidates.end()));
    }
    return out;
}

bool RomMSyncEngine::applyCandidates(std::vector<SyncCandidate>& candidates,
                                     SyncDetailedCallback progress, std::atomic<bool>* abort) {
    if (syncInProgress_.exchange(true)) {
        lastResult_ = "Sync already in progress";
        return false;
    }
    bool result = applyCandidatesLocked(candidates, progress, abort);
    syncInProgress_.store(false);
    return result;
}

bool RomMSyncEngine::applyCandidatesLocked(std::vector<SyncCandidate>& candidates,
                                           const SyncDetailedCallback& progress,
                                           std::atomic<bool>* abort) {
    lastResult_.clear();

    const size_t total = static_cast<size_t>(std::count_if(candidates.begin(), candidates.end(),
                                                           [](const SyncCandidate& c) { return c.selected; }));
    size_t processed = 0;
    size_t completed = 0;
    size_t failed = 0;

    // Session counts per negotiate session (one per prepare), in encounter order. The session is
    // closed with counts, not with the ids of what was done.
    std::vector<std::pair<std::string, std::pair<size_t, size_t>>> sessions;
    auto countsFor = [&](const std::string& sessionId) -> std::pair<size_t, size_t>& {
        for (auto& entry : sessions) {
            if (entry.first == sessionId)
                return entry.second;
        }
        sessions.emplace_back(sessionId, std::make_pair(size_t(0), size_t(0)));
        return sessions.back().second;
    };

    for (auto& c : candidates) {
        if (abort && abort->load())
            break;
        if (!c.selected)
            continue;

        emitProgress(progress, actionStage(c), std::string(actionWord(c)) + " " + describeOperation(c),
                     c.gameName, -1, processed, total);

        std::string result;
        bool ok = applyCandidate(c, result);
        c.success = ok;
        c.result = result;

        auto& counts = countsFor(c.sessionId);
        if (ok)
            ++completed;
        else
            ++failed;
        ++counts.second;
        ++processed;
    }

    // Complete each session that saw work. Not fatal: a session the server already closed, or one
    // its cleanup expired, answers 400, and the counts are still worth reporting.
    for (const auto& [sessionId, counts] : sessions) {
        if (sessionId.empty() || sessionId == "0")
            continue;
        if (!client_.completeSyncSession(sessionId, counts.first, counts.second))
            SF_LOG_W("RomM", "Could not complete sync session: %s", client_.lastError().c_str());
    }

    if (!sessions.empty()) {
        store_.setLastSessionId(sessions.front().first);
        store_.clearPendingOperations();
    }

    store_.setLastSuccessfulSync(std::time(nullptr));

    if (abort && abort->load())
        lastResult_ = "Sync cancelled (" + std::to_string(completed) + " of " + std::to_string(total) + ")";
    else {
        lastResult_ = "Synced " + std::to_string(completed) + " of " + std::to_string(total) + " operation(s)";
        if (failed > 0)
            lastResult_ += " (" + std::to_string(failed) + " failed)";
    }

    store_.setLastSyncResult(lastResult_);
    store_.save();

    if (total > 0)
        emitProgress(progress, SyncDetailedProgress::Stage::Done, "Sync complete", lastResult_, 100);
    return failed == 0;
}

bool RomMSyncEngine::applyCandidate(const SyncCandidate& c, std::string& result) {
    switch (c.action) {
        case SyncCandidate::Action::Upload:
            return performUpload(c, result);
        case SyncCandidate::Action::Download:
            return performDownload(c, result);
        case SyncCandidate::Action::Conflict:
            switch (c.resolution) {
                case SyncCandidate::Resolution::KeepLocal:
                    // Local wins: send it, which is what closes the server's conflict.
                    return performUpload(c, result);
                case SyncCandidate::Resolution::Acknowledge:
                    // Ask with no picker side: acknowledge only, change neither side.
                    result = "acknowledged";
                    return true;
                default:
                    // KeepRemote / KeepBoth take the server copy (KeepBoth archives local first).
                    return performDownload(c, result);
            }
        case SyncCandidate::Action::Delete:
            if (c.resolution == SyncCandidate::Resolution::KeepLocal) {
                result = "kept local (not deleted)";
                return true;
            }
            return performDelete(c, result);
    }
    return false;
}

bool RomMSyncEngine::performDownload(const SyncCandidate& c, std::string& result) {
    // A download has no local file yet, so the destination was built at prepare time. If it could
    // not be resolved (or the file vanished between selection and apply), there is nothing to do.
    if (c.localPath.empty()) {
        SF_LOG_W("RomM", "Cannot resolve local path for %s", c.fileName.c_str());
        result = "no local path";
        return false;
    }

    const std::string localPath = c.localPath;
    const std::string tempPath = localPath + ".romm_download";

    // Download to a temp file, then replace atomically.
    if (!client_.downloadSave(c.remoteSaveId, tempPath, c.sessionId)) {
        SF_LOG_E("RomM", "Failed to download %s: %s", c.fileName.c_str(), client_.lastError().c_str());
        result = client_.lastError();
        return false;
    }

    const std::string downloadedHash = computeFileHash(tempPath);
    // RomM hashes an archive over its entries (name + each entry's hash), not over the raw file, so
    // a zipped save's hash cannot be compared with this app's file hash. Plain saves can.
    const bool verifiable = !c.remoteHash.empty() && !looksLikeZipArchive(tempPath);
    if (verifiable && downloadedHash != c.remoteHash) {
        SF_LOG_E("RomM", "Hash mismatch for %s: expected %s, got %s",
                 c.fileName.c_str(), c.remoteHash.c_str(), downloadedHash.c_str());
        FileSystem::removeFile(tempPath);
        result = "hash mismatch";
        return false;
    }

    // A local file that also changed is handled by the row's resolution (seeded from the policy).
    if (FileSystem::exists(localPath)) {
        const std::string localHash = computeFileHash(localPath);
        if (localHash != downloadedHash) {
            if (c.resolution == SyncCandidate::Resolution::KeepBoth) {
                const std::string backupPath = localPath + ".conflict_" + std::to_string(std::time(nullptr));
                if (FileSystem::renameFile(localPath, backupPath)) {
                    store_.recordConflictBackup(localPath, backupPath);
                    SF_LOG_I("RomM", "Archived conflicting local save to %s", backupPath.c_str());
                }
            } else if (c.resolution == SyncCandidate::Resolution::KeepLocal) {
                SF_LOG_I("RomM", "Keeping local save (row resolution: keep local)");
                FileSystem::removeFile(tempPath);
                result = "kept local";
                return false;  // counted as failed: the planned operation was not performed
            }
            // KeepRemote: overwrite
        }
    }

    if (!writeFileAtomic(localPath, tempPath)) {
        SF_LOG_E("RomM", "Failed to write save file %s", localPath.c_str());
        result = "write failed";
        return false;
    }

    // Update fingerprint
    FileFingerprint fp;
    fp.hash = downloadedHash;
    fp.size = c.remoteSize;
    fp.modifiedTime = std::time(nullptr);
    fp.rommSaveId = c.remoteSaveId;
    store_.setFingerprint(localPath, fp);

    // No confirm call: the content request carries device_id, which is what records the device's
    // row for the save on the server.
    result = "downloaded";
    return true;
}

bool RomMSyncEngine::performUpload(const SyncCandidate& c, std::string& result) {
    const std::string localPath = c.localPath;
    if (localPath.empty() || !FileSystem::exists(localPath)) {
        SF_LOG_W("RomM", "Local file missing for upload %s", localPath.empty() ? c.fileName.c_str() : localPath.c_str());
        result = "local file missing";
        return false;
    }

    // Re-hash: the file may have changed between the scan and this upload, so send what is on disk
    // now rather than the bytes the scan hashed. An upload is always attributed to this device,
    // even when the row was a conflict owned by another one.
    RomMSave meta;
    meta.romId = c.rommRomId;
    meta.slot = c.slot;
    meta.type = c.type;
    meta.coreId = c.coreId;
    meta.fileName = FileSystem::filenameOf(localPath);
    meta.hash = computeFileHash(localPath);
    meta.size = static_cast<int64_t>(FileSystem::fileSize(localPath));
    meta.sessionId = c.sessionId;
    meta.deviceId = config_.deviceId;

    UploadResponse uploadResp;
    if (!client_.uploadSave(localPath, meta, uploadResp)) {
        SF_LOG_E("RomM", "Failed to upload %s: %s", c.fileName.c_str(), client_.lastError().c_str());
        result = client_.lastError();
        return false;
    }

    // Update fingerprint with new RomM save ID
    FileFingerprint fp;
    fp.hash = meta.hash;
    fp.size = meta.size;
    fp.modifiedTime = std::time(nullptr);
    fp.rommSaveId = uploadResp.saveId;
    store_.setFingerprint(localPath, fp);

    result = "uploaded";
    return true;
}

bool RomMSyncEngine::performDelete(const SyncCandidate& c, std::string& result) {
    // Server says this slot was emptied here, so the local copy goes too
    if (c.localPath.empty())
        return false;

    if (FileSystem::exists(c.localPath)) {
        if (c.resolution == SyncCandidate::Resolution::KeepBoth) {
            const std::string backupPath = c.localPath + ".deleted_" + std::to_string(std::time(nullptr));
            FileSystem::renameFile(c.localPath, backupPath);
            store_.recordConflictBackup(c.localPath, backupPath);
            result = "archived";
        } else {
            FileSystem::removeFile(c.localPath);
            result = "deleted";
        }
        store_.removeFingerprint(c.localPath);
    } else {
        result = "nothing on disk";
    }
    return true;
}

bool RomMSyncEngine::syncForRom(const std::string& romPath, const std::string& systemId,
                                const std::string& coreId, SyncDetailedCallback progress,
                                std::atomic<bool>* abort) {
    if (syncInProgress_.exchange(true)) {
        lastResult_ = "Sync already in progress";
        return false;
    }

    emitProgress(progress, SyncDetailedProgress::Stage::Connecting, "Connecting to RomM…");

    std::string error;
    auto candidates = prepareCandidates(romPath, systemId, coreId, error, abort);
    if (candidates.empty()) {
        lastResult_ = error.empty() ? "Nothing to sync" : error;
        // A prepare that ran and produced no work is not a failure; the auto flow treated
        // "no operations" as success, so keep that when the error is empty.
        bool result = error.empty();
        if (!error.empty())
            emitProgress(progress, SyncDetailedProgress::Stage::Error, lastResult_);
        syncInProgress_.store(false);
        return result;
    }

    emitProgress(progress, SyncDetailedProgress::Stage::Negotiating, "Negotiated; applying operations…");
    bool result = applyCandidatesLocked(candidates, progress, abort);
    syncInProgress_.store(false);
    return result;
}

bool RomMSyncEngine::syncAll(SyncDetailedCallback progress, std::atomic<bool>* abort) {
    if (syncInProgress_.exchange(true)) {
        lastResult_ = "Sync already in progress";
        return false;
    }

    emitProgress(progress, SyncDetailedProgress::Stage::Connecting, "Connecting to RomM…");

    std::string error;
    auto candidates = prepareCandidatesAll(error, abort);
    if (candidates.empty()) {
        lastResult_ = error.empty() ? lastResult_ : error;
        bool result = error.empty();
        if (!error.empty())
            emitProgress(progress, SyncDetailedProgress::Stage::Error, lastResult_);
        syncInProgress_.store(false);
        return result;
    }

    emitProgress(progress, SyncDetailedProgress::Stage::Negotiating, "Negotiated; applying operations…");
    bool result = applyCandidatesLocked(candidates, progress, abort);
    syncInProgress_.store(false);
    return result;
}

bool RomMSyncEngine::looksLikeZipArchive(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char magic[4] = {};
    if (!in.read(magic, 4)) return false;
    return magic[0] == 'P' && magic[1] == 'K';
}

void RomMSyncEngine::emitProgress(const SyncDetailedCallback& progress, SyncDetailedProgress::Stage stage,
                                  const std::string& message, const std::string& detail,
                                  int percent, size_t current, size_t total) {
    if (!progress) return;

    SyncDetailedProgress p;
    p.stage = stage;
    p.message = message;
    p.detail = detail;
    p.percent = percent >= 0 ? percent : (total > 0 ? static_cast<int>((current * 100) / total) : 0);
    p.currentFile = current;
    p.totalFiles = total;

    // Ensure callback runs on main thread via brls::sync
    brls::sync([cb = progress, p]() { cb(p); });
}

std::string RomMSyncEngine::computeFileHash(const std::string& path) {
    return sf::Hash::md5File(path);
}

bool RomMSyncEngine::writeFileAtomic(const std::string& path, const std::string& tempPath) {
    FileSystem::createDirectories(FileSystem::parentPath(path));
    // Overwrite: a save/state file may already exist from a previous sync.
    return FileSystem::renameFileOverwrite(tempPath, path);
}

} // namespace sf::romm
