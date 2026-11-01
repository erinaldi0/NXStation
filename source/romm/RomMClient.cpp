#include "romm/RomMClient.hpp"
#include "romm/RomMConfig.hpp"
#include "util/Json.hpp"
#include "util/Logger.hpp"
#include "util/FileSystem.hpp"
#include "util/Url.hpp"

#include <sstream>
#include <cstdio>
#include <ctime>
#include <iomanip>

namespace sf::romm {

namespace {

std::string jsonStringField(const Json& obj, const std::string& key) {
    if (!obj.contains(key)) return {};
    const Json& v = obj[key];
    if (v.is_string()) return v.get<std::string>();
    return {};
}

int64_t jsonNumberField(const Json& obj, const std::string& key, int64_t fallback = 0) {
    if (!obj.contains(key) || !obj[key].is_number()) return fallback;
    return static_cast<int64_t>(obj[key].get<double>());
}

bool jsonBoolField(const Json& obj, const std::string& key, bool fallback = false) {
    if (!obj.contains(key)) return fallback;
    if (obj[key].is_boolean()) return obj[key].get<bool>();
    if (obj[key].is_number()) return obj[key].get<double>() != 0.0;
    return fallback;
}

/** Joins a JSON string array (regions / genres / developers / publishers) into one label. */
std::string joinedStringField(const Json& obj, const std::string& key) {
    if (!obj.contains(key) || !obj[key].is_array()) return {};
    std::string out;
    for (const auto& v : obj[key]) {
        if (!v.is_string()) continue;
        const std::string s = v.get<std::string>();
        if (s.empty()) continue;
        if (!out.empty()) out += ", ";
        out += s;
    }
    return out;
}

/** Collect a JSON array of strings, e.g. `merged_screenshots`. */
std::vector<std::string> stringArrayField(const Json& obj, const std::string& key) {
    std::vector<std::string> out;
    if (!obj.contains(key) || !obj[key].is_array())
        return out;
    for (const auto& v : obj[key]) {
        if (!v.is_string())
            continue;
        const std::string s = v.get<std::string>();
        if (!s.empty())
            out.push_back(s);
    }
    return out;
}

/**
 * Release dates arrive as epoch seconds (ScreenScraper) or epoch milliseconds (the merged
 * `metadatum` row), and some builds send them as a plain string. `jsonStringField` returns
 * nothing for a number, so date fields must be read with this instead.
 */
std::string releaseDateField(const Json& obj, const std::string& key) {
    if (!obj.contains(key))
        return {};
    const Json& v = obj[key];
    if (v.is_string())
        return v.get<std::string>();
    if (!v.is_number())
        return {};

    double value = v.get<double>();
    if (value <= 0)
        return {};
    if (value > 1e12)  // epoch milliseconds
        value /= 1000.0;

    const std::time_t seconds = static_cast<std::time_t>(value);
    std::tm tm = {};
#if defined(_WIN32)
    if (gmtime_s(&tm, &seconds) != 0)
        return {};
#else
    if (!gmtime_r(&seconds, &tm))
        return {};
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

time_t parseIso8601(const std::string& str) {
    std::tm tm = {};
    std::istringstream ss(str);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return 0;
    return std::mktime(&tm);
}

/** RomM ids are ints on the wire; the app keeps them as strings for its mapping files. */
long long idFromString(const std::string& value) {
    try {
        return std::stoll(value);
    } catch (const std::exception&) {
        return 0;
    }
}

/** Format an epoch time the way negotiate's `updated_at` wants it (UTC, no offset). */
std::string iso8601Utc(time_t seconds) {
    std::tm tm = {};
#if defined(_WIN32)
    if (gmtime_s(&tm, &seconds) != 0)
        return {};
#else
    if (!gmtime_r(&seconds, &tm))
        return {};
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

const Json* libraryPageItems(const Json& j) {
    if (j.contains("data") && j["data"].is_array()) return &j["data"];
    if (j.is_array()) return &j;
    if (j.contains("items") && j["items"].is_array()) return &j["items"];
    if (j.contains("roms") && j["roms"].is_array()) return &j["roms"];
    return nullptr;
}

RomMLibraryEntry parseLibraryEntry(const Json& item) {
    RomMLibraryEntry entry;

    // id is integer per SimpleRomSchema
    double id = jsonNumberField(item, "id", 0);
    if (id == 0) id = jsonNumberField(item, "rom_id", 0);
    if (id == 0) id = jsonNumberField(item, "romId", 0);
    entry.id = std::to_string(static_cast<long long>(id));

    entry.name = jsonStringField(item, "name");
    if (entry.name.empty()) entry.name = jsonStringField(item, "fs_name");

    // Official SimpleRomSchema: platform_fs_slug is the stable key, platform_display_name the label.
    entry.platform = jsonStringField(item, "platform_fs_slug");
    if (entry.platform.empty()) entry.platform = jsonStringField(item, "platform_slug");
    if (entry.platform.empty()) entry.platform = jsonStringField(item, "platform");
    entry.platformDisplay = jsonStringField(item, "platform_display_name");
    if (entry.platformDisplay.empty()) entry.platformDisplay = jsonStringField(item, "platform_custom_name");

    // regions is a string array in the official schema; older builds used a single "region".
    entry.region = joinedStringField(item, "regions");
    if (entry.region.empty()) entry.region = jsonStringField(item, "region");
    entry.version = jsonStringField(item, "revision");
    if (entry.version.empty()) entry.version = jsonStringField(item, "version");

    entry.totalSize = jsonNumberField(item, "fs_size_bytes", 0);
    if (entry.totalSize == 0) entry.totalSize = jsonNumberField(item, "total_size", 0);

    entry.folderName = jsonStringField(item, "fs_path");
    entry.fsName = jsonStringField(item, "fs_name");

    // RomFileSchema entries carry id / file_name / file_size_bytes / is_top_level.
    if (item.contains("files") && item["files"].is_array()) {
        for (const auto& f : item["files"]) {
            RomFileEntry file;
            if (f.is_string()) {
                file.fileName = f.get<std::string>();
            } else if (f.is_object()) {
                file.id = static_cast<long long>(jsonNumberField(f, "id", 0));
                file.fileName = jsonStringField(f, "file_name");
                file.sizeBytes = jsonNumberField(f, "file_size_bytes", 0);
                file.topLevel = jsonBoolField(f, "is_top_level", true);
            }
            if (!file.fileName.empty()) entry.files.push_back(std::move(file));
        }
    }

    if (entry.files.empty()) {
        // Some RomM builds omit `files` in the paginated response; fs_name is the stored file
        // name for single-file ROMs, which is what the content endpoint path needs.
        std::string single = entry.fsName;
        if (single.empty()) single = jsonStringField(item, "file_name");
        if (single.empty()) single = jsonStringField(item, "file");
        if (!single.empty()) {
            RomFileEntry file;
            file.fileName = single;
            file.sizeBytes = entry.totalSize;
            entry.files.push_back(std::move(file));
        }
    }

    entry.multiFile = jsonBoolField(item, "has_multiple_files", false) || entry.files.size() > 1;

    // Metadata RomM has already resolved (ScreenScraper/IGDB merge) — used for the focus panel
    // when there is no local scrape entry for a not-yet-downloaded ROM.
    entry.summary = jsonStringField(item, "summary");
    if (item.contains("metadatum") && item["metadatum"].is_object()) {
        const Json& meta = item["metadatum"];
        entry.genres = joinedStringField(meta, "genres");
        entry.developer = joinedStringField(meta, "developers");
        entry.publisher = joinedStringField(meta, "publishers");
        entry.releaseDate = releaseDateField(meta, "first_release_date");
    }
    entry.coverUrl = jsonStringField(item, "url_cover");

    // Cached provider images. Present only when the server has media on disk (scan with
    // media processing on); the fetcher falls back to `url_cover` when these are empty.
    entry.coverSmallPath = jsonStringField(item, "path_cover_small");
    entry.coverLargePath = jsonStringField(item, "path_cover_large");
    entry.screenshotPaths = stringArrayField(item, "merged_screenshots");
    entry.videoPath = jsonStringField(item, "path_video");
    entry.manualPath = jsonStringField(item, "path_manual");

    if (item.contains("sibling_roms") && item["sibling_roms"].is_array()) {
        for (const auto& s : item["sibling_roms"]) {
            if (s.is_object()) {
                const std::string sibling = jsonStringField(s, "name");
                if (!sibling.empty()) entry.siblingNames.push_back(sibling);
            }
        }
    }

    return entry;
}

} // namespace

RomMClient::RomMClient() = default;

RomMClient::~RomMClient() = default;

void RomMClient::configure(const std::string& serverUrl, const std::string& accessToken,
                           const std::string& clientToken) {
    baseUrl_ = serverUrl;
    if (!baseUrl_.empty() && baseUrl_.back() == '/') {
        baseUrl_.pop_back();
    }
    accessToken_ = accessToken;
    clientToken_ = clientToken;
    updateAuthHeaders();
    ready_ = !baseUrl_.empty() && !accessToken_.empty();
    lastError_.clear();
}

std::string RomMClient::buildUrl(const std::string& path) const {
    return baseUrl_ + "/api" + path;
}

std::unique_ptr<RomMClient> RomMClient::fork() const {
    // RomMClient is not copyable (CurlHelper holds a mutex), so a fork is a new client that is
    // configured from this one's server + tokens rather than a copy.
    auto copy = std::make_unique<RomMClient>();
    copy->configure(baseUrl_, accessToken_, clientToken_);
    copy->deviceId_ = deviceId_;
    return copy;
}

std::string RomMClient::assetUrl(const std::string& path) const {
    if (path.empty())
        return {};

    // Split the `?ts=...` cache-buster off the resource path; it is a query, not part of it.
    std::string assetPath = path;
    std::string query;
    if (const auto q = assetPath.find('?'); q != std::string::npos) {
        query = assetPath.substr(q);
        assetPath.resize(q);
    }

    // Some fields already carry a full URL (url_cover points at the provider), and some
    // already include the mount. Anything else is relative to /assets/romm/resources/.
    if (assetPath.rfind("http://", 0) == 0 || assetPath.rfind("https://", 0) == 0)
        return path;
    if (assetPath.front() != '/')
        assetPath = "/assets/romm/resources/" + assetPath;

    // baseUrl_ may include a reverse-proxy subpath, so build from it rather than the host root. The
    // query is encoded too: `?ts=` carries a space, which makes the request line invalid and the server
    // answers 400 for every cached asset.
    return baseUrl_ + url::encodePathSegment(assetPath) + url::encodeQuery(query);
}

void RomMClient::updateAuthHeaders() {
    if (!accessToken_.empty()) {
        curl_.setBearerAuth(accessToken_);
    } else if (!clientToken_.empty()) {
        curl_.setBearerAuth(clientToken_);
    } else {
        curl_.clearAuth();
    }
}

std::string RomMClient::errorDetail(const std::string& body) {
    if (body.empty())
        return {};
    try {
        const auto j = Json::parse(body);
        if (j.contains("detail") && j["detail"].is_string())
            return j["detail"].get<std::string>();
    } catch (const std::exception&) {
        // Not every error body is JSON (a proxy can answer with HTML); the status is all we have.
    }
    return {};
}

bool RomMClient::checkResponse(const sf::cloud::CurlResponse& resp, const char* operation) {
    if (!resp.ok) {
        lastError_ = resp.error.empty() ? std::string(operation) + " failed: HTTP " + std::to_string(resp.status)
                                        : std::string(operation) + " failed: " + resp.error;
        // FastAPI answers a 4xx with {"detail": "..."}, and negotiate can 400 for two different
        // reasons that look identical without it (no device_id, or the device's sync toggle off).
        const std::string detail = errorDetail(resp.body);
        if (!detail.empty())
            lastError_ += ": " + detail;
        if (resp.status == 401)
            lastError_ += " (token invalid or expired — re-pair in RomM settings)";
        SF_LOG_E("RomM", "%s", lastError_.c_str());
        return false;
    }
    return true;
}

bool RomMClient::exchangePairingCode(const std::string& code, PairingResponse& out) {
    // Allow pairing with just base URL configured (no auth tokens needed for initial pairing)
    if (baseUrl_.empty()) {
        lastError_ = "Client not configured: server URL not set";
        return false;
    }

    Json body = Json::object();
    body["code"] = code;

    const sf::cloud::CurlResponse resp = curl_.postJson(buildUrl("/client-tokens/exchange"), body.dump());
    if (!checkResponse(resp, "Pairing code exchange")) return false;

    try {
        const auto j = Json::parse(resp.body);
        
        // Debug: log the response
        SF_LOG_I("RomM", "Pairing response: %s", resp.body.c_str());
        
        // According to user, the token field is called raw_token
        out.accessToken = jsonStringField(j, "raw_token");
        if (out.accessToken.empty()) out.accessToken = jsonStringField(j, "rawToken");
        if (out.accessToken.empty()) out.accessToken = jsonStringField(j, "access_token");
        if (out.accessToken.empty()) out.accessToken = jsonStringField(j, "accessToken");
        if (out.accessToken.empty()) out.accessToken = jsonStringField(j, "token");
        
        // The pairing response doesn't have a separate client_token - we use the access_token
        out.clientToken = out.accessToken;
        
        double expiresIn = jsonNumberField(j, "expires", 0);
        if (expiresIn == 0) expiresIn = jsonNumberField(j, "expires_in", 0);
        if (expiresIn == 0) expiresIn = jsonNumberField(j, "expiresIn", 0);
        if (expiresIn == 0) expiresIn = 3600;
        out.expiresAt = std::time(nullptr) + static_cast<time_t>(expiresIn);

        if (out.accessToken.empty()) {
            lastError_ = "Missing raw_token in pairing response: " + resp.body;
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        lastError_ = ex.what();
        return false;
    }
}

bool RomMClient::registerDevice(const std::string& deviceName, Device& out) {
    if (!ready_) {
        lastError_ = "Client not authenticated";
        return false;
    }

    Json body = Json::object();
    body["name"] = deviceName;
    body["platform"] = "switch";

    const sf::cloud::CurlResponse resp = curl_.postJson(buildUrl("/devices"), body.dump());
    if (!checkResponse(resp, "Device registration")) return false;

    try {
        const auto j = Json::parse(resp.body);
        SF_LOG_I("RomM", "Device registration response: %s", resp.body.c_str());
        
        auto getId = [&](const Json& obj, const std::initializer_list<const char*>& keys) -> std::string {
            for (const char* key : keys) {
                std::string val = jsonStringField(obj, key);
                if (!val.empty()) return val;
            }
            return "";
        };
        
        out.id = getId(j, {"id", "device_id", "deviceId", "uuid"});
        out.name = jsonStringField(j, "name");
        out.platform = jsonStringField(j, "platform");
        out.lastSeen = jsonStringField(j, "last_seen");
        if (out.lastSeen.empty()) out.lastSeen = jsonStringField(j, "lastSeen");

        if (out.id.empty()) {
            lastError_ = "Missing device ID in registration response: " + resp.body;
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        lastError_ = ex.what();
        return false;
    }
}

bool RomMClient::updateDevice(const std::string& deviceId, const std::string& name) {
    if (!ready_) {
        lastError_ = "Client not authenticated";
        return false;
    }

    Json body = Json::object();
    body["name"] = name;

    const sf::cloud::CurlResponse resp = curl_.patchCaptureHeaders(buildUrl("/devices/" + deviceId));
    if (!resp.ok) {
        // Try with JSON body
        const sf::cloud::CurlResponse resp2 = curl_.postJson(buildUrl("/devices/" + deviceId), body.dump());
        if (!checkResponse(resp2, "Device update")) return false;
    }
    return true;
}

bool RomMClient::fetchDevices(std::vector<Device>& out) {
    if (!ready_) {
        lastError_ = "Client not authenticated";
        return false;
    }

    const sf::cloud::CurlResponse resp = curl_.get(buildUrl("/devices"));
    if (!checkResponse(resp, "Fetch devices")) return false;

    try {
        const auto j = Json::parse(resp.body);
        const Json* items = libraryPageItems(j);
        if (!items) {
            lastError_ = "Invalid devices response format";
            return false;
        }

        for (const auto& item : *items) {
            if (!item.is_object())
                continue;
            Device device;
            // Device ids are UUIDs on current builds, but some builds answer with an integer,
            // so accept both shapes like the registration parser does.
            device.id = jsonStringField(item, "id");
            if (device.id.empty()) {
                const long long id = jsonNumberField(item, "id", 0);
                if (id) device.id = std::to_string(id);
            }
            if (device.id.empty()) continue;
            device.name = jsonStringField(item, "name");
            device.platform = jsonStringField(item, "platform");
            device.lastSeen = jsonStringField(item, "last_seen");
            if (device.lastSeen.empty()) device.lastSeen = jsonStringField(item, "lastSeen");
            out.push_back(std::move(device));
        }
        return true;
    } catch (const std::exception& ex) {
        lastError_ = ex.what();
        return false;
    }
}

bool RomMClient::negotiate(const std::vector<RomMSave>& localSaves, NegotiateResponse& out,
                           const std::vector<std::string>& romIds) {
    if (!ready_) {
        lastError_ = "Client not authenticated";
        return false;
    }

    out = NegotiateResponse{};

    Json body = Json::object();
    // device_id belongs at the top level. RomM infers it from the token only when that token is
    // device-bound; the app used to put it inside each save entry, which is not the payload schema
    // and is what made every startup negotiate answer 400.
    body["device_id"] = deviceId_;

    Json savesArray = Json::array();
    for (const auto& save : localSaves) {
        Json s = Json::object();
        s["rom_id"] = idFromString(save.romId);
        // ClientSaveState requires file_name, updated_at and file_size_bytes; they were missing from
        // the earlier payload. A slot is optional, and an omitted slot is meaningful: RomM treats a
        // null slot as an archival upload that is never paired with a slotted server save.
        s["file_name"] = save.fileName;
        s["updated_at"] = iso8601Utc(save.updatedAt);
        s["file_size_bytes"] = save.size;
        if (!save.slot.empty()) s["slot"] = save.slot;
        if (!save.coreId.empty()) s["emulator"] = save.coreId;
        if (!save.hash.empty()) s["content_hash"] = save.hash;
        savesArray.push_back(std::move(s));
    }
    body["saves"] = std::move(savesArray);

    // The read-only ROM scope. Without it RomM offers downloads for every save in the user's
    // library, so a negotiation for one game comes back with saves belonging to games this device
    // does not have, and the engine has no local folder to put them in.
    if (!romIds.empty()) {
        Json ids = Json::array();
        for (const auto& id : romIds) {
            const long long value = idFromString(id);
            if (value > 0) ids.push_back(value);
        }
        if (ids.size() > 0) body["rom_ids"] = std::move(ids);
    }

    const sf::cloud::CurlResponse resp = curl_.postJson(buildUrl("/sync/negotiate"), body.dump());
    if (!checkResponse(resp, "Sync negotiate")) return false;

    try {
        const auto j = Json::parse(resp.body);
        const long long sessionId = jsonNumberField(j, "session_id", 0);
        // Kept as a string because the app stores it and builds the complete URL from it.
        out.sessionId = sessionId ? std::to_string(sessionId) : jsonStringField(j, "session_id");

        // The current schema answers with one flat operations array (action per entry) rather than
        // the older uploads/downloads/conflicts/deletes arrays.
        if (j.contains("operations") && j["operations"].is_array()) {
            for (const auto& op : j["operations"]) {
                RomMSave save;
                save.action = jsonStringField(op, "action");
                save.romId = std::to_string(jsonNumberField(op, "rom_id", 0));
                const long long saveId = jsonNumberField(op, "save_id", 0);
                // Null for uploads and deletes: the server has no row for them yet.
                if (saveId) save.id = std::to_string(saveId);
                save.slot = jsonStringField(op, "slot");
                save.fileName = jsonStringField(op, "file_name");
                save.coreId = jsonStringField(op, "emulator");
                save.reason = jsonStringField(op, "reason");
                save.hash = jsonStringField(op, "server_content_hash");
                save.updatedAt = parseIso8601(jsonStringField(op, "server_updated_at"));
                // Download and conflict rows belong to whichever device owns the server save; the
                // picker groups by device, so the owner must survive parsing. Upload and delete
                // rows have no server row, so they stay attributed to this device.
                save.deviceId = jsonStringField(op, "device_id");
                if (save.deviceId.empty()) save.deviceId = deviceId_;
                save.sessionId = out.sessionId;
                out.operations.push_back(std::move(save));
            }
        }

        out.totalUpload = static_cast<size_t>(jsonNumberField(j, "total_upload", 0));
        out.totalDownload = static_cast<size_t>(jsonNumberField(j, "total_download", 0));
        out.totalConflict = static_cast<size_t>(jsonNumberField(j, "total_conflict", 0));
        out.totalDelete = static_cast<size_t>(jsonNumberField(j, "total_delete", 0));
        out.totalNoOp = static_cast<size_t>(jsonNumberField(j, "total_no_op", 0));

        return true;
    } catch (const std::exception& ex) {
        lastError_ = ex.what();
        return false;
    }
}

bool RomMClient::uploadSave(const std::string& localPath, const RomMSave& meta, UploadResponse& out) {
    if (!ready_) {
        lastError_ = "Client not authenticated";
        return false;
    }

    // One request now: RomM takes the save as a multipart `saveFile` part with the metadata as query
    // parameters. The earlier two-step create + PUT /content is not part of the current API.
    std::string url = buildUrl("/saves") +
                      "?rom_id=" + meta.romId +
                      "&device_id=" + sf::url::encodeQueryValue(deviceId_) +
                      "&session_id=" + meta.sessionId;
    if (!meta.slot.empty()) url += "&slot=" + sf::url::encodeQueryValue(meta.slot);
    if (!meta.coreId.empty()) url += "&emulator=" + sf::url::encodeQueryValue(meta.coreId);
    if (!meta.hash.empty()) url += "&content_hash=" + sf::url::encodeQueryValue(meta.hash);

    const sf::cloud::CurlResponse resp = curl_.postMultipart(url, {}, "saveFile", localPath);
    if (!checkResponse(resp, "Save upload")) return false;

    try {
        const auto j = Json::parse(resp.body);
        const long long saveId = jsonNumberField(j, "id", 0);
        if (!saveId) {
            lastError_ = "Missing save ID in upload response";
            return false;
        }

        out.saveId = std::to_string(saveId);
        out.sessionId = meta.sessionId;
        return true;
    } catch (const std::exception& ex) {
        lastError_ = ex.what();
        return false;
    }
}

bool RomMClient::downloadSave(const std::string& saveId, const std::string& localPath,
                              const std::string& sessionId) {
    if (!ready_) {
        lastError_ = "Client not authenticated";
        return false;
    }

    // device_id is what makes the download count as a sync: RomM records the device's row for the
    // save only when it can identify the device, and session_id increments the session counter.
    std::string url = buildUrl("/saves/" + saveId + "/content") +
                      "?device_id=" + sf::url::encodeQueryValue(deviceId_);
    if (!sessionId.empty()) url += "&session_id=" + sessionId;

    const sf::cloud::CurlResponse resp = curl_.downloadToFile(url, localPath);
    return checkResponse(resp, "Save download");
}

bool RomMClient::completeSyncSession(const std::string& sessionId,
                                     size_t operationsCompleted, size_t operationsFailed) {
    if (!ready_) {
        lastError_ = "Client not authenticated";
        return false;
    }
    if (sessionId.empty() || sessionId == "0") {
        lastError_ = "No sync session to complete";
        return false;
    }

    // The session closes with counts, not with the list of operation ids it used to take. A session
    // already completed answers 400, so the engine keeps this step non-fatal.
    Json body = Json::object();
    body["operations_completed"] = static_cast<long long>(operationsCompleted);
    body["operations_failed"] = static_cast<long long>(operationsFailed);

    const sf::cloud::CurlResponse resp = curl_.postJson(buildUrl("/sync/sessions/" + sessionId + "/complete"), body.dump());
    return checkResponse(resp, "Complete sync session");
}

bool RomMClient::fetchLibrary(std::vector<RomMLibraryEntry>& out,
                              const std::function<void(int fetched, int total)>& progress,
                              std::atomic<bool>* abort) {
    if (!ready_) {
        lastError_ = "Client not authenticated";
        return false;
    }

    // RomM paginates with limit/offset (the default page is 50 rows), so walk every page.
    constexpr int kPageLimit = 100;
    constexpr int kMaxPages = 100;

    int offset = 0;
    int total = -1;
    bool parsed = false;

    for (int page = 0; page < kMaxPages; ++page) {
        // Checked between requests: the progress page's B button aborts the whole refresh, and the
        // library fetch and the art batch share the same flag so one press stops both stages.
        if (abort && abort->load()) {
            lastError_ = "Fetch library aborted";
            return false;
        }

        const std::string url = buildUrl("/roms?limit=" + std::to_string(kPageLimit) +
                                         "&offset=" + std::to_string(offset));
        const sf::cloud::CurlResponse resp = curl_.get(url);
        if (!checkResponse(resp, "Fetch library")) return false;

        Json j;
        try {
            j = Json::parse(resp.body);
        } catch (const std::exception& ex) {
            lastError_ = ex.what();
            return false;
        }

        const Json* items = libraryPageItems(j);
        if (!items) {
            lastError_ = "Invalid library response format";
            return false;
        }
        parsed = true;

        const int pageCount = static_cast<int>(items->size());
        if (total < 0) {
            if (j.contains("meta") && j["meta"].is_object())
                total = static_cast<int>(jsonNumberField(j["meta"], "total", 0));
            if (total <= 0)
                total = static_cast<int>(jsonNumberField(j, "total", 0));
        }

        for (const auto& item : *items) {
            RomMLibraryEntry entry = parseLibraryEntry(item);
            if (!entry.id.empty())
                out.push_back(std::move(entry));
        }

        SF_LOG_I("RomM", "Library page %d: %d entries (server total %d)", page + 1, pageCount, total);
        if (progress)
            progress(static_cast<int>(out.size()), total > 0 ? total : static_cast<int>(out.size()));

        if (pageCount == 0) break;
        offset += pageCount;
        if (total > 0 && static_cast<int>(out.size()) >= total) break;
        // A short page only means "last page" when the server did not report a total
        // (some builds cap the page size below the requested limit).
        if (total <= 0 && pageCount < kPageLimit) break;
    }

    return parsed;
}

bool RomMClient::fetchSimpleRom(const std::string& romId, RomMLibraryEntry& out) {
    if (!ready_) {
        lastError_ = "Client not authenticated";
        return false;
    }

    // SimpleRomSchema is the documented shape that carries `files`; DetailedRomSchema does too,
    // so try the plain rom endpoint as a fallback for builds without /simple.
    const std::vector<std::string> paths = {"/roms/" + romId + "/simple", "/roms/" + romId};
    bool parsedAny = false;

    for (const std::string& path : paths) {
        const sf::cloud::CurlResponse resp = curl_.get(buildUrl(path));
        if (!checkResponse(resp, "Fetch rom")) continue;

        try {
            const Json j = Json::parse(resp.body);
            RomMLibraryEntry entry = parseLibraryEntry(j);
            if (entry.id.empty()) continue;
            parsedAny = true;
            if (!entry.files.empty()) {
                out = std::move(entry);
                return true;
            }
            if (out.id.empty()) out = std::move(entry);
        } catch (const std::exception& ex) {
            lastError_ = ex.what();
        }
    }

    return parsedAny;
}

bool RomMClient::downloadRom(const std::string& romId, const std::string& fileName,
                             const std::string& localPath, const sf::cloud::ProgressFn& progress,
                             bool allowResume) {
    if (!ready_) {
        lastError_ = "Client not authenticated";
        return false;
    }

    std::string url = buildUrl("/roms/" + romId + "/content/" + fileName);
    const sf::cloud::CurlResponse resp = curl_.downloadToFile(url, localPath, progress, allowResume);
    return checkResponse(resp, "ROM download");
}

bool RomMClient::downloadToUrl(const std::string& url, const std::string& localPath,
                               const sf::cloud::ProgressFn& progress, bool allowResume) {
    if (!ready_) {
        lastError_ = "Client not authenticated";
        return false;
    }

    const sf::cloud::CurlResponse resp = curl_.downloadToFile(url, localPath, progress, allowResume);
    return checkResponse(resp, "URL download");
}

} // namespace sf::romm
