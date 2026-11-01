#include "romm/RomMSyncCandidate.hpp"

#include <algorithm>

namespace sf::romm {

namespace {

SyncCandidate::Action actionFromString(const std::string& value) {
    if (value == "upload") return SyncCandidate::Action::Upload;
    if (value == "download") return SyncCandidate::Action::Download;
    if (value == "conflict") return SyncCandidate::Action::Conflict;
    if (value == "delete") return SyncCandidate::Action::Delete;
    return SyncCandidate::Action::Upload;  // never used: unknown actions are skipped
}

/** The local scan entry an operation refers to, matched the way RomM pairs them. */
const RomMSave* localEntryFor(const std::vector<RomMSave>& localEntries, const RomMSave& op) {
    for (const auto& local : localEntries) {
        if (local.romId != op.romId) continue;
        // Slot is what RomM pairs on; the name is the server's, which for a slotted save carries its
        // version tag and will not equal the client's.
        if (local.slot == op.slot) return &local;
        if (!op.fileName.empty() && local.fileName == op.fileName) return &local;
    }
    return nullptr;
}

} // namespace

std::vector<SyncCandidate> buildCandidates(
    const RomMapping& mapping,
    const std::vector<RomMSave>& localEntries,
    const NegotiateResponse& negotiation,
    SyncCandidate::Resolution seedResolution,
    const std::function<std::optional<std::string>(const RomMSave& op)>& pathResolver,
    const std::function<std::string(const std::string& deviceId)>& deviceNameFor,
    const std::function<bool(const std::string& fileName)>& isStateFile)
{
    std::vector<SyncCandidate> out;
    out.reserve(negotiation.operations.size());

    for (const auto& op : negotiation.operations) {
        const SyncCandidate::Action action = actionFromString(op.action);
        if (op.action == "no_op" || op.action.empty())
            continue;

        SyncCandidate c;
        c.action = action;
        c.resolution = seedResolution;
        c.sessionId = negotiation.sessionId;
        c.rommRomId = op.romId;
        c.gameName = mapping.displayName;
        c.systemId = mapping.systemId;
        c.slot = op.slot;
        c.reason = op.reason;

        const RomMSave* local = localEntryFor(localEntries, op);
        if (local) {
            c.localExists = true;
            c.localHash = local->hash;
            c.localSize = local->size;
            c.localModifiedTime = local->updatedAt;
            c.localPath = local->localPath;
            c.type = local->type;
            c.coreId = local->coreId;
        }

        // The upload and delete rows have no server save row yet, so their device is this one.
        // Download and conflict rows belong to whichever device owns the server row.
        c.deviceId = !op.deviceId.empty() ? op.deviceId : (local ? local->deviceId : std::string());
        c.deviceName = deviceNameFor(c.deviceId);

        c.fileName = !op.fileName.empty() ? op.fileName
                                          : (local && !local->fileName.empty() ? local->fileName : std::string());
        if (c.type.empty())
            c.type = isStateFile(c.fileName) ? "state" : "save";

        c.remoteSaveId = op.id;
        c.remoteHash = op.hash;
        c.remoteSize = op.size;
        c.remoteUpdatedAt = op.updatedAt;

        // A download has no local file at scan time, so its destination comes from the resolver;
        // unresolvable rows stay empty and are the ones apply skips with a warning.
        if (c.localPath.empty()) {
            const auto resolved = pathResolver(op);
            if (resolved)
                c.localPath = *resolved;
        }

        out.push_back(std::move(c));
    }

    // Stable display order: game header order is the build order, and within a game rows group
    // by type then slot so the picker's grouping stays contiguous.
    std::stable_sort(out.begin(), out.end(), [](const SyncCandidate& a, const SyncCandidate& b) {
        if (a.type != b.type) return a.type < b.type;
        if (a.slot != b.slot) return a.slot < b.slot;
        return a.fileName < b.fileName;
    });

    return out;
}

} // namespace sf::romm
