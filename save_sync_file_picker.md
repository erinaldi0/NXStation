# RomM Save Sync — Per-File Picker

Goal: choose which save/state files to upload or download **per file**, with rows
grouped by game → saves/states → slot → device. Selection is per session; nothing
deselected is sent to the server, so it is simply re-proposed by the next negotiation.

Decisions agreed:
- Backend: RomM save sync only.
- Conflicts: per-file choice in the picker (`Keep both` / `Keep local` / `Keep remote`),
  which implements `ConflictPolicy::Ask` as the picker itself.
- Auto sync (before launch / after return) stays fully automatic; the picker is an
  explicit manual action.

Branch: `feature/save-sync-file-picker` (from `feature/romm-art-fetcher`).

## Steps

### 1. Candidate model — `include/romm/RomMSyncCandidate.hpp` + `source/romm/RomMSyncCandidate.cpp`

- `SyncCandidate`: one sync operation on one file. Carries `Action`
  (Upload / Download / Conflict / Delete), per-file `Resolution` (KeepBoth / KeepLocal /
  KeepRemote / Acknowledge), `selected`, identity (game, system, slot, device id + name,
  type save/state, fileName), server + local sides (id, hash, size, mtime), resolved
  `localPath`, `sessionId`, and result fields filled by apply.
- `buildCandidates(...)`: pure builder (no I/O, no borealis) mapping negotiate operations
  to candidates. Takes the local scan entries, the negotiation, a download-path resolver,
  a device-name lookup, and the policy seed for default resolutions. This is what the
  host test exercises.

### 2. Device names — `RomMClient::fetchDevices()` + `RomMStateStore` cache

- `GET /api/devices` on the client (client-token scope returns the user's devices).
- Negotiate entries now carry `device_id` when the server reports it; the parser keeps
  the server's owner device for download/conflict rows instead of assuming this device.
- State store persists a `device_names` cache (`deviceId -> display name`) so the picker
  shows names offline after the first fetch; fallback is the short id, or the config
  device name for rows owned by this device.

### 3. Engine split — prepare → apply (`RomMSyncEngine`)

- `prepareCandidates(romPath, systemId, coreId, out)` — mapping check, local scan,
  negotiate, `buildCandidates`. No side effects.
- `prepareAllCandidates(out)` — same over the local mappings (the same "not downloaded →
  skip" filter `syncAll` already applies).
- `applyCandidates(candidates, progress, abort)` — processes **selected** candidates only
  (self-contained), dispatching upload / download / conflict-by-resolution / delete,
  completing each negotiated session with the counts, updating fingerprints only for
  applied files.
- `syncForRom` / `syncAll` become wrappers: prepare → select-all → apply. Auto behavior
  is unchanged (`Ask` still acknowledges conflicts without touching either side).
- Per-file candidates whose local file vanished between prepare and apply are skipped with
  a warning (the picker flow can span minutes between selection and apply).

### 4. Service layer — `RomMService`

- `prepareCandidatesAsync(...)` and `prepareAllCandidatesAsync(...)`: run prepare on the
  pool, return candidates on the main thread.
- `applyCandidatesAsync(candidates, progress, done)`: runs apply on the pool with the
  shared abort flag; `done` receives the applied candidates back so the UI can color rows
  by result.
- `deviceName(id)` accessor for the picker.

### 5. Picker UI — `include/ui/SaveSyncPickView.hpp` + `source/ui/SaveSyncPickView.cpp`

- Pushed activity styled after `CloudRestorePickView`.
- Layout: title + status + scroller; list grouped **Game → Saves/States → Slot → device
  row**. Each row: selection mark, direction glyph (`↑` upload, `↓` download, `⚠` conflict,
  `✕` delete), file name, device name, size, freshness/reason, conflict resolution.
- **A/X toggles selection** on a row; **X cycles** the resolution on conflict rows
  (and the archive choice on delete rows). Header/footer actions: Select all, Select none,
  Apply sync (N selected).
- Apply runs `applyCandidatesAsync`; per-file results color the rows
  (`success` / `error` theme colors), summary line at the end. B returns to the caller.
- Empty selection does nothing (no server session churn).

### 6. Entry points

- Settings → RomM Sync → **Choose Files to Sync** (picker over all mapped, locally
  downloaded ROMs). Existing "Sync Now" stays as-is (auto, select-all).
- Game list **Y menu** → **Sync Save Files…** — picker scoped to that game
  (`GameOptionsAction::SyncSaveFiles`, only shown for RomM games that are downloaded).

### 7. Tests

- `tests/romm_sync_candidates_test.cpp` (host build, mirrors `playtime_analytics_test`):
  action classification (upload/download/conflict/delete/no-op), slot + device identity,
  default selection, resolution seeding from the policy, local-vs-remote sides populated,
  rows whose game path is unresolvable. Registered as a CMake test target.

### 8. Docs

- `context.md`: picker note under the Cloud/RomM area + key files table.
- `changelog.md`: feature entry.

## Semantics summary

- Deselected rows are skipped this session; next negotiation proposes them again.
- Conflict rows: `Keep local` = keep local, don't upload; `Keep remote` = download over
  local; `Keep both` = archive local first, then write remote (current KeepBoth behavior).
- Delete rows: `Keep both` = archive local copy + fingerprint removed; `Keep local` = don't
  touch the file.
- Selection is not persisted. Fingerprints/conflict backups recorded only for applied files.
