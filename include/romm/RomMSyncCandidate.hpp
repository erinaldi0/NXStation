#pragma once

#include "romm/RomMClient.hpp"
#include "romm/RomMSaveMapper.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sf::romm {

/**
 * One save/state file the negotiation planned an operation for. The picker lists these
 * grouped game -> saves/states -> slot -> device, so a candidate carries all four plus
 * both sides (server row and local file), which is everything apply needs to run from a
 * selection alone.
 */
struct SyncCandidate {
    enum class Action {
        Upload,    // local newer: send the file
        Download,  // remote newer: fetch the file
        Conflict,  // both sides changed since the last sync
        Delete,    // the server says this slot was emptied on this device
    };

    /**
     * Per-file override of the global conflict policy. Seeded from the policy at build
     * time; the picker cycles it. Acknowledge leaves both sides untouched (what the
     * Ask policy meant when there was no picker).
     */
    enum class Resolution {
        KeepBoth,
        KeepLocal,
        KeepRemote,
        Acknowledge,
    };

    Action action = Action::Upload;
    Resolution resolution = Resolution::KeepBoth;
    bool selected = true;

    std::string sessionId;
    std::string rommRomId;
    std::string gameName;
    std::string systemId;

    // Identity for the grouping: slot, device (id + name), type, file name.
    std::string slot;      // empty means no slot — RomM treats those as archives
    std::string deviceId;  // owner of this row: this device, or the server row's device
    std::string deviceName;
    std::string fileName;
    std::string type;      // "save" or "state"
    std::string coreId;    // for states
    std::string reason;    // the server's reason for the operation, when it gave one

    std::string localPath;  // existing file, or the resolved destination for a download
    std::string remoteSaveId;
    std::string remoteHash;
    int64_t remoteSize = 0;
    time_t remoteUpdatedAt = 0;

    std::string localHash;  // empty when the file was not on disk at scan time
    int64_t localSize = 0;
    time_t localModifiedTime = 0;
    bool localExists = false;

    // Filled by apply, so the picker can color the rows by outcome.
    bool success = false;
    std::string result;

    bool canResolve() const { return action == Action::Conflict || action == Action::Delete; }
};

/**
 * Map one negotiation onto candidates. Pure: no I/O, no borealis — the scan entries, the
 * operations, the path resolver, the device-name lookup and the policy seed are all passed
 * in, which is what the host test exercises.
 *
 * `pathResolver` resolves a local path for an operation (the existing file for conflict and
 * delete rows, the destination for a download). Rows it cannot resolve get an empty
 * localPath and are the ones apply skips with a warning.
 */
std::vector<SyncCandidate> buildCandidates(
    const RomMapping& mapping,
    const std::vector<RomMSave>& localEntries,
    const NegotiateResponse& negotiation,
    SyncCandidate::Resolution seedResolution,
    const std::function<std::optional<std::string>(const RomMSave& op)>& pathResolver,
    const std::function<std::string(const std::string& deviceId)>& deviceNameFor,
    const std::function<bool(const std::string& fileName)>& isStateFile);

} // namespace sf::romm
