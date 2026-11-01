#include "romm/RomMSyncCandidate.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace sf::romm;

namespace {

int gFailures = 0;

void expect(bool cond, const char* msg)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++gFailures;
    }
}

using Resolver = std::function<std::optional<std::string>(const RomMSave&)>;
using NameFor = std::function<std::string(const std::string&)>;
using StateCheck = std::function<bool(const std::string&)>;

const RomMapping kMapping = {
    "42",           // rommRomId
    "psx",          // systemId
    "/roms/psx/game.scb",
    "Final Fantasy Tactics",
    "pcsx_rearmed",
    "",
};

// Lambdas shared by the tests: everything buildCandidates needs is passed in, so the test
// needs no I/O and no borealis.
const Resolver kResolver = [](const RomMSave& op) -> std::optional<std::string> {
    if (op.action == "download")
        return std::string("/saves/psx/game/" + op.fileName);
    return std::nullopt;  // matches the mapper: nothing to resolve without a local entry
};
const NameFor kNames = [](const std::string& deviceId) {
    return deviceId == "dev-1" ? "This device" : deviceId == "dev-2" ? "Steam Deck" : deviceId;
};
const StateCheck kIsState = [](const std::string& fileName) {
    return fileName.size() >= 6 && fileName.compare(fileName.size() - 6, 6, ".state") == 0;
};

void testUploadClassification()
{
    std::vector<RomMSave> local;
    RomMSave entry;
    entry.romId = "42";
    entry.slot = "0";
    entry.deviceId = "dev-1";
    entry.type = "save";
    entry.hash = "abc";
    entry.size = 1024;
    entry.fileName = "game.srm";
    entry.localPath = "/saves/psx/game/game.srm";
    entry.updatedAt = 1700000000;
    local.push_back(entry);

    NegotiateResponse neg;
    neg.sessionId = "sess-1";
    RomMSave op;
    op.action = "upload";
    op.romId = "42";
    op.slot = "0";
    op.fileName = "game.srm";
    neg.operations.push_back(op);

    const auto candidates = buildCandidates(kMapping, local, neg,
                                            SyncCandidate::Resolution::KeepBoth,
                                            kResolver, kNames, kIsState);
    expect(candidates.size() == 1, "upload produces one candidate");
    const auto& c = candidates[0];
    expect(c.action == SyncCandidate::Action::Upload, "upload action");
    expect(c.selected, "candidates default to selected");
    expect(c.sessionId == "sess-1", "candidate carries the negotiate session");
    expect(c.rommRomId == "42" && c.gameName == "Final Fantasy Tactics" && c.systemId == "psx",
           "identity fields");
    expect(c.slot == "0", "slot identity");
    expect(c.deviceId == "dev-1", "upload rows attribute to the local entry's device");
    expect(c.deviceName == "This device", "device name resolved");
    expect(c.type == "save", "type from the local entry");
    expect(c.localPath == "/saves/psx/game/game.srm", "upload keeps the existing local path");
    expect(c.localHash == "abc" && c.localSize == 1024 && c.localExists, "local side populated");
    expect(c.resolution == SyncCandidate::Resolution::KeepBoth, "resolution seeded from the policy");
}

void testDownloadClassificationAndDeviceOwnership()
{
    NegotiateResponse neg;
    neg.sessionId = "sess-2";
    RomMSave op;
    op.action = "download";
    op.romId = "42";
    op.slot = "auto";
    op.fileName = "game.auto.state";
    op.deviceId = "dev-2";  // the server row's owner, not this device
    op.hash = "def";
    op.size = 2048;
    op.updatedAt = 1700000500;
    neg.operations.push_back(op);

    const auto candidates = buildCandidates(kMapping, {}, neg,
                                            SyncCandidate::Resolution::KeepRemote,
                                            kResolver, kNames, kIsState);
    expect(candidates.size() == 1, "download produces one candidate");
    const auto& c = candidates[0];
    expect(c.action == SyncCandidate::Action::Download, "download action");
    expect(c.deviceId == "dev-2", "download rows keep the server row's device");
    expect(c.deviceName == "Steam Deck", "device name resolved through the lookup");
    expect(c.type == "state", "type resolved from the file name when no local entry");
    expect(c.localPath == "/saves/psx/game/game.auto.state", "download destination from the resolver");
    expect(!c.localExists, "download has no local file at scan time");
    expect(c.remoteHash == "def" && c.remoteSize == 2048 && c.remoteUpdatedAt == 1700000500,
           "remote side populated");
    expect(c.resolution == SyncCandidate::Resolution::KeepRemote, "resolution seeded from the policy");
}

void testConflictAndAskSeeding()
{
    NegotiateResponse neg;
    neg.sessionId = "sess-3";
    RomMSave op;
    op.action = "conflict";
    op.romId = "42";
    op.slot = "1";
    op.fileName = "game.srm";
    op.deviceId = "dev-2";
    op.reason = "both sides changed";
    neg.operations.push_back(op);

    const auto ask = buildCandidates(kMapping, {}, neg, SyncCandidate::Resolution::Acknowledge,
                                     kResolver, kNames, kIsState);
    expect(ask.size() == 1, "conflict produces one candidate");
    // Ask seeds Acknowledge: the picker is the Ask implementation, and the auto wrappers
    // acknowledge without touching either side.
    expect(ask[0].action == SyncCandidate::Action::Conflict, "conflict action");
    expect(ask[0].resolution == SyncCandidate::Resolution::Acknowledge, "Ask seeds Acknowledge");
    expect(ask[0].canResolve(), "conflict rows can be resolved per file");
    expect(ask[0].reason == "both sides changed", "server reason kept for the row");

    // A conflict whose local entry matches by file name when the slot differs (the server's
    // file_name carries a version tag the client's does not).
    RomMSave localEntry;
    localEntry.romId = "42";
    localEntry.slot = "1";
    localEntry.deviceId = "dev-1";
    localEntry.fileName = "game.srm";
    localEntry.localPath = "/saves/psx/game/game.srm";
    NegotiateResponse otherSlot;
    otherSlot.operations.push_back(op);
    const auto matched = buildCandidates(kMapping, {localEntry}, otherSlot,
                                         SyncCandidate::Resolution::KeepBoth,
                                         kResolver, kNames, kIsState);
    expect(matched.size() == 1 && matched[0].localPath == "/saves/psx/game/game.srm",
           "conflict keeps the local file path for the resolution to act on");
    expect(matched.size() == 1 && matched[0].deviceId == "dev-2",
           "conflict rows keep the server row's device, not the local entry's");
}

void testDeleteAndUnresolvableRows()
{
    NegotiateResponse neg;
    neg.sessionId = "sess-4";
    RomMSave op;
    op.action = "delete";
    op.romId = "42";
    op.slot = "3";
    op.fileName = "game.srm";
    neg.operations.push_back(op);

    const auto candidates = buildCandidates(kMapping, {}, neg, SyncCandidate::Resolution::KeepBoth,
                                            kResolver, kNames, kIsState);
    expect(candidates.size() == 1, "delete produces one candidate");
    const auto& c = candidates[0];
    expect(c.action == SyncCandidate::Action::Delete, "delete action");
    expect(c.canResolve(), "delete rows support the archive choice");
    // Delete rows have no server save row, so they belong to this device.
    expect(c.deviceId.empty(), "delete rows have no server device until matched to a local entry");
    // Nothing on disk and nothing resolvable: the row apply skips with a warning.
    expect(c.localPath.empty(), "unresolvable rows keep an empty local path");
}

void testNoOpSkippedAndOrdering()
{
    NegotiateResponse neg;
    neg.sessionId = "sess-5";

    RomMSave noOp;
    noOp.action = "no_op";
    noOp.romId = "42";
    noOp.slot = "0";
    neg.operations.push_back(noOp);

    RomMSave stateOp;
    stateOp.action = "download";
    stateOp.romId = "42";
    stateOp.slot = "1";
    stateOp.fileName = "slot1.state";
    neg.operations.push_back(stateOp);

    RomMSave saveOp;
    saveOp.action = "download";
    saveOp.romId = "42";
    stateOp.romId = "42";
    saveOp.slot = "0";
    saveOp.fileName = "slot0.srm";
    neg.operations.push_back(saveOp);

    const auto candidates = buildCandidates(kMapping, {}, neg, SyncCandidate::Resolution::KeepBoth,
                                            kResolver, kNames, kIsState);
    expect(candidates.size() == 2, "no_op rows are skipped");
    // Grouping stays contiguous: saves before states, slots in order.
    expect(!candidates.empty() && candidates[0].type == "save", "rows group by type");
    expect(candidates.size() == 2 && candidates[1].type == "state", "states after saves");
    expect(candidates.size() == 2 && candidates[0].slot == "0", "slot order within a type");
}

void testLocalEntryMatching()
{
    // A download matched to a local entry by slot keeps the entry's device when the server
    // row does not report one.
    RomMSave localEntry;
    localEntry.romId = "42";
    localEntry.slot = "0";
    localEntry.deviceId = "dev-1";
    localEntry.type = "save";
    localEntry.fileName = "game.srm";
    localEntry.hash = "abc";

    NegotiateResponse neg;
    neg.sessionId = "sess-6";
    RomMSave op;
    op.action = "download";
    op.romId = "42";
    op.slot = "0";
    op.fileName = "game.srm";
    neg.operations.push_back(op);

    const auto candidates = buildCandidates(kMapping, {localEntry}, neg,
                                            SyncCandidate::Resolution::KeepRemote,
                                            kResolver, kNames, kIsState);
    expect(candidates.size() == 1 && candidates[0].deviceId == "dev-1",
           "device falls back to the local entry when the server row reports none");
    expect(candidates.size() == 1 && candidates[0].localPath == "/saves/psx/game/game.srm",
           "the download destination wins over the entry's empty path only when the entry has none");
}

} // namespace

int main()
{
    testUploadClassification();
    testDownloadClassificationAndDeviceOwnership();
    testConflictAndAskSeeding();
    testDeleteAndUnresolvableRows();
    testNoOpSkippedAndOrdering();
    testLocalEntryMatching();

    if (gFailures == 0)
        std::printf("romm_sync_candidates_test: all checks passed\n");
    return gFailures == 0 ? 0 : 1;
}
