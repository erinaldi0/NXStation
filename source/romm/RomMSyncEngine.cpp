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
#include <borealis.hpp>

namespace sf::romm {

namespace {

/** "save (slot)" / "state (slot)" for the progress line. */
std::string describeOperation(const RomMSave& op) {
    const std::string kind = RomMSaveMapper::isStateFile(op.fileName) ? "state" : "save";
    return kind + " (" + (op.slot.empty() ? "no slot" : op.slot) + ")";
}

} // namespace

RomMSyncEngine::RomMSyncEngine(RomMClient& client, RomMSaveMapper& mapper, RomMStateStore& store,
                               const RomMConfig& config)
    : client_(client), mapper_(mapper), store_(store), config_(config) {}

bool RomMSyncEngine::syncForRom(const std::string& romPath, const std::string& systemId,
                                const std::string& coreId, SyncDetailedCallback progress,
                                std::atomic<bool>* abort) {
    if (syncInProgress_.exchange(true)) {
        lastResult_ = "Sync already in progress";
        return false;
    }

    SyncContext ctx;
    ctx.romPath = romPath;
    ctx.systemId = systemId;
    ctx.coreId = coreId;
    ctx.progress = std::move(progress);
    ctx.abort = abort;

    bool result = runSyncForRom(ctx);
    syncInProgress_.store(false);
    return result;
}

bool RomMSyncEngine::syncAll(SyncDetailedCallback progress, std::atomic<bool>* abort) {
    if (syncInProgress_.exchange(true)) {
        lastResult_ = "Sync already in progress";
        return false;
    }

    // Sync every mapped ROM
    const auto mappings = mapper_.allMappings();
    if (config_.deviceId.empty()) {
        syncInProgress_.store(false);
        lastResult_ = "RomM is not paired (no device id)";
        return false;
    }
    if (mappings.empty()) {
        syncInProgress_.store(false);
        lastResult_ = "Nothing to sync yet — download a ROM from the RomM library first";
        return true;
    }

    size_t synced = 0;
    size_t failed = 0;
    const size_t total = mappings.size();
    size_t completed = 0;

    for (const auto& mapping : mappings) {
        if (abort && abort->load()) break;

        SyncContext ctx;
        ctx.romPath = mapping.romPath;
        ctx.systemId = mapping.systemId;
        ctx.coreId = mapping.coreId;
        ctx.progress = [&, completed](const SyncDetailedProgress& p) {
            if (progress) {
                SyncDetailedProgress adapted = p;
                adapted.currentFile = completed;
                adapted.totalFiles = total;
                progress(adapted);
            }
        };
        ctx.abort = abort;

        if (runSyncForRom(ctx))
            ++synced;
        else
            ++failed;
        ++completed;
    }

    syncInProgress_.store(false);

    if (abort && abort->load())
        lastResult_ = "Sync cancelled (" + std::to_string(synced) + " of " + std::to_string(total) + ")";
    else {
        lastResult_ = "Synced " + std::to_string(synced) + " of " + std::to_string(total) + " ROMs";
        if (failed > 0)
            lastResult_ += " (" + std::to_string(failed) + " failed)";
    }

    store_.setLastSyncResult(lastResult_);
    store_.save();

    return failed == 0;
}

bool RomMSyncEngine::runSyncForRom(SyncContext& ctx) {
    lastResult_.clear();

    emitProgress(ctx, SyncDetailedProgress::Stage::Connecting, "Connecting to RomM…");

    // Find mapping
    auto mappingOpt = mapper_.findMapping(ctx.romPath, ctx.systemId);
    if (!mappingOpt) {
        lastResult_ = "No RomM mapping for this ROM";
        emitProgress(ctx, SyncDetailedProgress::Stage::Error, lastResult_);
        return false;
    }
    ctx.rommRomId = mappingOpt->rommRomId;

    // RomM identifies the caller by device_id — negotiate takes it at the top level of the payload
    // and the save endpoints as a query parameter. Without it the server answers 400 for every ROM,
    // so refuse before sending anything rather than failing once per game.
    if (config_.deviceId.empty()) {
        lastResult_ = "RomM is not paired (no device id)";
        emitProgress(ctx, SyncDetailedProgress::Stage::Error, lastResult_);
        return false;
    }
    client_.setDeviceId(config_.deviceId);

    // Collect local files
    emitProgress(ctx, SyncDetailedProgress::Stage::Negotiating, "Scanning local saves…");
    if (!collectLocalFiles(ctx)) return false;

    // Negotiate with server
    emitProgress(ctx, SyncDetailedProgress::Stage::Negotiating, "Negotiating with server…");
    if (!negotiate(ctx)) return false;

    size_t completed = 0;
    size_t failed = 0;
    size_t processed = 0;
    const size_t total = ctx.negotiateResp.operations.size();

    // One pass over the operations the server planned. The session is closed with counts rather than
    // with the ids of what was done, so the engine counts as it goes.
    for (const auto& op : ctx.negotiateResp.operations) {
        if (ctx.abort && ctx.abort->load()) break;

        if (op.action == "no_op") {
            ++processed;
            continue;
        }

        bool ok = false;
        if (op.action == "upload") {
            emitProgress(ctx, SyncDetailedProgress::Stage::Uploading, "Uploading " + describeOperation(op),
                         op.fileName, -1, processed, total);
            ok = performUpload(ctx, op);
        } else if (op.action == "download") {
            emitProgress(ctx, SyncDetailedProgress::Stage::Downloading, "Downloading " + describeOperation(op),
                         op.fileName, -1, processed, total);
            ok = performDownload(ctx, op);
        } else if (op.action == "conflict") {
            // Both sides changed since the last sync, so the server leaves the choice to the policy.
            emitProgress(ctx, SyncDetailedProgress::Stage::ResolvingConflicts, op.reason,
                         op.fileName, -1, processed, total);
            if (config_.conflictPolicy == ConflictPolicy::KeepLocal)
                ok = performUpload(ctx, op);
            else if (config_.conflictPolicy == ConflictPolicy::Ask)
                ok = true;  // Acknowledged only; nothing on either side is changed.
            else
                ok = performDownload(ctx, op);  // KeepRemote / KeepBoth (archived inside)
        } else if (op.action == "delete") {
            emitProgress(ctx, SyncDetailedProgress::Stage::ResolvingConflicts, "Processing deletion",
                         op.fileName, -1, processed, total);
            ok = performDelete(ctx, op);
        } else {
            SF_LOG_W("RomM", "Unknown sync action: %s", op.action.c_str());
            ++processed;
            continue;
        }

        if (ok)
            ++completed;
        else
            ++failed;
        ++processed;
    }

    // Complete session
    emitProgress(ctx, SyncDetailedProgress::Stage::Completing, "Finalizing sync…");
    // Not fatal: a session the server already closed, or one its cleanup expired, answers 400, and
    // the counts are still worth reporting.
    if (!client_.completeSyncSession(ctx.negotiateResp.sessionId, completed, failed))
        SF_LOG_W("RomM", "Could not complete sync session: %s", client_.lastError().c_str());

    store_.setLastSessionId(ctx.negotiateResp.sessionId);
    store_.clearPendingOperations();

    // Update state store
    store_.setLastSuccessfulSync(std::time(nullptr));

    lastResult_ = "Synced " + std::to_string(completed) + " of " + std::to_string(total) + " operation(s)";
    if (failed > 0)
        lastResult_ += " (" + std::to_string(failed) + " failed)";

    store_.setLastSyncResult(lastResult_);
    store_.save();

    emitProgress(ctx, SyncDetailedProgress::Stage::Done, "Sync complete", lastResult_, 100);
    return failed == 0;
}

bool RomMSyncEngine::collectLocalFiles(SyncContext& ctx) {
    if (ctx.abort && ctx.abort->load()) return false;

    auto localFiles = mapper_.discoverSaves(ctx.romPath, ctx.systemId, ctx.coreId);

    // Convert LocalSaveFile to RomMSave for negotiation
    ctx.localSaves.clear();
    ctx.localSaves.reserve(localFiles.size());
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
        ctx.localSaves.push_back(std::move(save));
    }

    // Filter by config
    if (!config_.syncSaves) {
        ctx.localSaves.erase(
            std::remove_if(ctx.localSaves.begin(), ctx.localSaves.end(),
                           [](const RomMSave& s) { return s.type == "save"; }),
            ctx.localSaves.end());
    }
    if (!config_.syncStates) {
        ctx.localSaves.erase(
            std::remove_if(ctx.localSaves.begin(), ctx.localSaves.end(),
                           [](const RomMSave& s) { return s.type == "state"; }),
            ctx.localSaves.end());
    }

    SF_LOG_I("RomM", "Found %zu local save files for %s", ctx.localSaves.size(), ctx.romPath.c_str());
    return true;
}

bool RomMSyncEngine::negotiate(SyncContext& ctx) {
    if (ctx.abort && ctx.abort->load()) return false;

    if (!client_.negotiate(ctx.localSaves, ctx.negotiateResp)) {
        lastResult_ = "Negotiation failed: " + client_.lastError();
        emitProgress(ctx, SyncDetailedProgress::Stage::Error, lastResult_);
        return false;
    }

    SF_LOG_I("RomM", "Negotiation: %zu upload(s), %zu download(s), %zu conflict(s), %zu delete(s), %zu no-op",
             ctx.negotiateResp.totalUpload, ctx.negotiateResp.totalDownload,
             ctx.negotiateResp.totalConflict, ctx.negotiateResp.totalDelete,
             ctx.negotiateResp.totalNoOp);
    return true;
}

bool RomMSyncEngine::performDownload(SyncContext& ctx, const RomMSave& op) {
    auto localPathOpt = mapper_.resolveLocalPath(op);
    if (!localPathOpt) {
        SF_LOG_W("RomM", "Cannot resolve local path for %s", op.fileName.c_str());
        return false;
    }

    const std::string localPath = *localPathOpt;
    const std::string tempPath = localPath + ".romm_download";

    // Download to a temp file, then replace atomically.
    if (!client_.downloadSave(op.id, tempPath, ctx.negotiateResp.sessionId)) {
        SF_LOG_E("RomM", "Failed to download %s: %s", op.fileName.c_str(), client_.lastError().c_str());
        return false;
    }

    const std::string downloadedHash = computeFileHash(tempPath);
    // RomM hashes an archive over its entries (name + each entry's hash), not over the raw file, so
    // a zipped save's hash cannot be compared with this app's file hash. Plain saves can.
    const bool verifiable = !op.hash.empty() && !looksLikeZipArchive(tempPath);
    if (verifiable && downloadedHash != op.hash) {
        SF_LOG_E("RomM", "Hash mismatch for %s: expected %s, got %s",
                 op.fileName.c_str(), op.hash.c_str(), downloadedHash.c_str());
        FileSystem::removeFile(tempPath);
        return false;
    }

    // A local file that also changed is handled by the conflict policy.
    if (FileSystem::exists(localPath)) {
        const std::string localHash = computeFileHash(localPath);
        if (localHash != downloadedHash) {
            if (config_.conflictPolicy == ConflictPolicy::KeepBoth) {
                const std::string backupPath = localPath + ".conflict_" + std::to_string(std::time(nullptr));
                if (FileSystem::renameFile(localPath, backupPath)) {
                    store_.recordConflictBackup(localPath, backupPath);
                    SF_LOG_I("RomM", "Archived conflicting local save to %s", backupPath.c_str());
                }
            } else if (config_.conflictPolicy == ConflictPolicy::KeepLocal) {
                SF_LOG_I("RomM", "Keeping local save (conflict policy: keep_local)");
                FileSystem::removeFile(tempPath);
                return false;
            }
            // KeepRemote: overwrite
        }
    }

    if (!writeFileAtomic(localPath, tempPath)) {
        SF_LOG_E("RomM", "Failed to write save file %s", localPath.c_str());
        return false;
    }

    // Update fingerprint
    FileFingerprint fp;
    fp.hash = downloadedHash;
    fp.size = op.size;
    fp.modifiedTime = std::time(nullptr);
    fp.rommSaveId = op.id;
    store_.setFingerprint(localPath, fp);

    // No confirm call: the content request carries device_id, which is what records the device's
    // row for the save on the server.
    return true;
}

bool RomMSyncEngine::performUpload(SyncContext& ctx, const RomMSave& op) {
    // The operation carries the server's side; the local file is known from the entry that was sent
    // at scan time, matched on (rom_id, slot).
    const RomMSave* local = localEntryFor(ctx, op);
    std::string localPath;
    if (local) {
        localPath = local->localPath;
    } else {
        auto localPathOpt = mapper_.resolveLocalPath(op);
        if (!localPathOpt) {
            SF_LOG_W("RomM", "Local file not found for upload %s", op.fileName.c_str());
            return false;
        }
        localPath = *localPathOpt;
    }

    if (!FileSystem::exists(localPath)) {
        SF_LOG_W("RomM", "Local file missing for upload %s", localPath.c_str());
        return false;
    }

    // Re-hash: the file may have changed between the scan and this upload, so send what is on disk
    // now rather than the bytes the scan hashed.
    RomMSave meta = local ? *local : op;
    meta.hash = computeFileHash(localPath);
    meta.size = static_cast<int64_t>(FileSystem::fileSize(localPath));
    meta.fileName = FileSystem::filenameOf(localPath);
    meta.sessionId = ctx.negotiateResp.sessionId;
    meta.deviceId = config_.deviceId;

    UploadResponse uploadResp;
    if (!client_.uploadSave(localPath, meta, uploadResp)) {
        SF_LOG_E("RomM", "Failed to upload %s: %s", op.fileName.c_str(), client_.lastError().c_str());
        return false;
    }

    // Update fingerprint with new RomM save ID
    FileFingerprint fp;
    fp.hash = meta.hash;
    fp.size = meta.size;
    fp.modifiedTime = std::time(nullptr);
    fp.rommSaveId = uploadResp.saveId;
    store_.setFingerprint(localPath, fp);

    return true;
}

bool RomMSyncEngine::performDelete(SyncContext& ctx, const RomMSave& op) {
    // Server says this slot was emptied here, so the local copy goes too
    auto localPathOpt = mapper_.resolveLocalPath(op);
    if (!localPathOpt) return false;

    if (FileSystem::exists(*localPathOpt)) {
        if (config_.conflictPolicy == ConflictPolicy::KeepBoth) {
            const std::string backupPath = *localPathOpt + ".deleted_" + std::to_string(std::time(nullptr));
            FileSystem::renameFile(*localPathOpt, backupPath);
            store_.recordConflictBackup(*localPathOpt, backupPath);
        } else {
            FileSystem::removeFile(*localPathOpt);
        }
        store_.removeFingerprint(*localPathOpt);
    }
    return true;
}

const RomMSave* RomMSyncEngine::localEntryFor(SyncContext& ctx, const RomMSave& op) const {
    for (const auto& local : ctx.localSaves) {
        if (local.romId != op.romId) continue;
        // Slot is what RomM pairs on; the name is the server's, which for a slotted save carries its
        // version tag and will not equal the client's.
        if (local.slot == op.slot) return &local;
        if (!op.fileName.empty() && local.fileName == op.fileName) return &local;
    }
    return nullptr;
}

bool RomMSyncEngine::looksLikeZipArchive(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char magic[4] = {};
    if (!in.read(magic, 4)) return false;
    return magic[0] == 'P' && magic[1] == 'K';
}

void RomMSyncEngine::emitProgress(SyncContext& ctx, SyncDetailedProgress::Stage stage,
                                  const std::string& message, const std::string& detail,
                                  int percent, size_t current, size_t total) {
    if (!ctx.progress) return;

    SyncDetailedProgress p;
    p.stage = stage;
    p.message = message;
    p.detail = detail;
    p.percent = percent >= 0 ? percent : (total > 0 ? static_cast<int>((current * 100) / total) : 0);
    p.currentFile = current;
    p.totalFiles = total;

    // Ensure callback runs on main thread via brls::sync
    brls::sync([cb = ctx.progress, p]() { cb(p); });
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
