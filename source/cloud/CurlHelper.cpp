#include "cloud/CurlHelper.hpp"
#include "app/Version.hpp"

#include <cstdio>
#include <fstream>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "util/FileSystem.hpp"
#include "util/Logger.hpp"

namespace sf::cloud {

namespace {

std::string userAgent()
{
    return std::string("NXStation/") + kAppVersion;
}

std::string trimHeaderLine(std::string line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
        line.pop_back();
    return line;
}

} // namespace

CurlHelper::CurlHelper()
{
    // curl_global_init() is done once in main(); a helper only owns its own handle.
    curl_ = curl_easy_init();
}

CurlHelper::~CurlHelper()
{
    if (curl_)
        curl_easy_cleanup(curl_);
    if (downloadCurl_)
        curl_easy_cleanup(downloadCurl_);
}

bool CurlHelper::ensureHandle()
{
    if (!curl_)
        curl_ = curl_easy_init();
    return curl_ != nullptr;
}

CURL* CurlHelper::ensureDownloadHandle()
{
    if (!downloadCurl_)
        downloadCurl_ = curl_easy_init();
    return downloadCurl_;
}

std::string CurlHelper::authHeaderCopy()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return authHeader_;
}

void CurlHelper::applyBaseOptions(CURL* handle, long transferTimeout)
{
    curl_easy_reset(handle);
    const std::string ua = userAgent();
    curl_easy_setopt(handle, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, connectTimeout_);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, transferTimeout);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    // Abort a stalled transfer instead of hanging until the full timeout.
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
#ifdef __SWITCH__
    curl_easy_setopt(handle, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
#endif
}

void CurlHelper::setTimeouts(long connectSeconds, long transferSeconds)
{
    connectTimeout_ = connectSeconds > 0 ? connectSeconds : 30;
    transferTimeout_ = transferSeconds > 0 ? transferSeconds : 120;
}

void CurlHelper::setDownloadTimeout(long seconds)
{
    downloadTimeout_ = seconds > 0 ? seconds : 900;
}

void CurlHelper::reset()
{
    if (!curl_)
        return;
    applyBaseOptions(curl_, transferTimeout_);
}

void CurlHelper::setBearerAuth(const std::string& token)
{
    std::lock_guard<std::mutex> lock(mutex_);
    authHeader_ = "Authorization: Bearer " + token;
}

void CurlHelper::clearAuth()
{
    std::lock_guard<std::mutex> lock(mutex_);
    authHeader_.clear();
}

size_t CurlHelper::writeBody(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t CurlHelper::writeHeader(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    std::string line(ptr, size * nmemb);
    line = trimHeaderLine(line);
    const auto colon = line.find(':');
    if (colon == std::string::npos || line.empty())
        return size * nmemb;
    std::string key = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ')
        value.erase(value.begin());
    (*headers)[key] = value;
    return size * nmemb;
}

size_t CurlHelper::readFile(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* in = static_cast<std::ifstream*>(userdata);
    in->read(ptr, static_cast<std::streamsize>(size * nmemb));
    return static_cast<size_t>(in->gcount());
}

CurlResponse CurlHelper::perform(const char* method, const std::string& url, const std::string* body,
                                 bool captureHeaders, bool uploadFile, const std::string* filePath)
{
    std::lock_guard<std::mutex> lock(mutex_);

    CurlResponse resp;
    if (!ensureHandle()) {
        resp.error = "curl not initialized";
        return resp;
    }

    std::ifstream uploadStream;
    if (uploadFile && filePath) {
        uploadStream.open(*filePath, std::ios::binary);
        if (!uploadStream) {
            resp.error = "Cannot open upload file";
            return resp;
        }
    }

    std::string responseBody;
    std::map<std::string, std::string> responseHeaders;

    const auto attempt = [&](void) -> CURLcode {
        responseBody.clear();
        responseHeaders.clear();

        reset();
        curl_slist* hdr = nullptr;
        auto appendHeader = [&](const std::string& line) {
            hdr = curl_slist_append(hdr, line.c_str());
        };

        if (!authHeader_.empty())
            appendHeader(authHeader_);

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &CurlHelper::writeBody);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &responseBody);
        if (captureHeaders) {
            curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, &CurlHelper::writeHeader);
            curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &responseHeaders);
        }

        if (method && method[0] != '\0')
            curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method);

        if (uploadFile && filePath) {
            uploadStream.clear();
            uploadStream.seekg(0, std::ios::beg);
            curl_easy_setopt(curl_, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(curl_, CURLOPT_READFUNCTION, &CurlHelper::readFile);
            curl_easy_setopt(curl_, CURLOPT_READDATA, &uploadStream);
            struct stat st {};
            if (stat(filePath->c_str(), &st) == 0)
                curl_easy_setopt(curl_, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(st.st_size));
        } else if (body) {
            appendHeader("Content-Type: application/x-www-form-urlencoded");
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body->c_str());
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
        }

        if (hdr)
            curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, hdr);

        const CURLcode code = curl_easy_perform(curl_);
        if (hdr)
            curl_slist_free_all(hdr);
        return code;
    };

    CURLcode code = attempt();

    // Handle-level init/connection failures on Switch are usually transient; one retry
    // with a fresh handle recovers without the caller noticing.
    const bool retryable = code == CURLE_FAILED_INIT || code == CURLE_COULDNT_CONNECT;
    if (retryable) {
        curl_easy_cleanup(curl_);
        curl_ = curl_easy_init();
        if (curl_)
            code = attempt();
    }

    resp.body = std::move(responseBody);
    resp.headers = std::move(responseHeaders);
    if (code != CURLE_OK) {
        resp.error = curl_easy_strerror(code);
        return resp;
    }

    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &resp.status);
    resp.ok = resp.status >= 200 && resp.status < 300;
    if (!resp.ok && resp.error.empty())
        resp.error = "HTTP " + std::to_string(resp.status);
    return resp;
}

CurlResponse CurlHelper::get(const std::string& url)
{
    return perform(nullptr, url, nullptr, false, false, nullptr);
}

CurlResponse CurlHelper::postForm(const std::string& url, const std::string& formBody)
{
    return perform("POST", url, &formBody, false, false, nullptr);
}

CurlResponse CurlHelper::postJson(const std::string& url, const std::string& jsonBody)
{
    CurlResponse resp;
    if (!curl_) {
        resp.error = "curl not initialized";
        return resp;
    }

    reset();
    curl_slist* hdr = nullptr;
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    if (!authHeader_.empty())
        hdr = curl_slist_append(hdr, authHeader_.c_str());

    std::string responseBody;
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, jsonBody.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &CurlHelper::writeBody);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &responseBody);

    const CURLcode code = curl_easy_perform(curl_);
    curl_slist_free_all(hdr);

    resp.body = std::move(responseBody);
    if (code != CURLE_OK) {
        resp.error = curl_easy_strerror(code);
        return resp;
    }
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &resp.status);
    resp.ok = resp.status >= 200 && resp.status < 300;
    if (!resp.ok && resp.error.empty())
        resp.error = "HTTP " + std::to_string(resp.status);
    return resp;
}

CurlResponse CurlHelper::postJsonCaptureHeaders(const std::string& url, const std::string& jsonBody)
{
    CurlResponse resp;
    if (!curl_) {
        resp.error = "curl not initialized";
        return resp;
    }

    reset();
    curl_slist* hdr = nullptr;
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    if (!authHeader_.empty())
        hdr = curl_slist_append(hdr, authHeader_.c_str());

    std::string responseBody;
    std::map<std::string, std::string> responseHeaders;
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, jsonBody.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &CurlHelper::writeBody);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, &CurlHelper::writeHeader);
    curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &responseHeaders);

    const CURLcode code = curl_easy_perform(curl_);
    curl_slist_free_all(hdr);

    resp.body = std::move(responseBody);
    resp.headers = std::move(responseHeaders);
    if (code != CURLE_OK) {
        resp.error = curl_easy_strerror(code);
        return resp;
    }
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &resp.status);
    resp.ok = resp.status >= 200 && resp.status < 300;
    if (!resp.ok && resp.error.empty())
        resp.error = "HTTP " + std::to_string(resp.status);
    return resp;
}

CurlResponse CurlHelper::patchCaptureHeaders(const std::string& url)
{
    return perform("PATCH", url, nullptr, true, false, nullptr);
}

CurlResponse CurlHelper::uploadFromFile(const std::string& url, const std::string& filePath)
{
    return perform(nullptr, url, nullptr, false, true, &filePath);
}

CurlResponse CurlHelper::downloadToFile(const std::string& url, const std::string& filePath,
                                        const ProgressFn& progress)
{
    // Dedicated handle + mutex: a multi-minute ROM transfer must not wait behind (or block)
    // the short API calls that share the API handle across pool workers.
    std::lock_guard<std::mutex> lock(downloadMutex_);

    CURL* handle = ensureDownloadHandle();
    CurlResponse resp;
    if (!handle) {
        resp.error = "curl not initialized";
        return resp;
    }

    const std::string auth = authHeaderCopy();
    progressFn_ = progress;

    // RomM serves 202 + Retry-After while a stored file is converted to a requested format, so
    // a non-2xx first response can be a wait-and-retry instead of a failure.
    constexpr int kMaxAttempts = 3;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        resp = CurlResponse{};

        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        if (!out) {
            resp.error = "Cannot open output file";
            SF_LOG_E("Cloud", "Cannot open output file: %s", filePath.c_str());
            progressFn_ = nullptr;
            return resp;
        }

        applyBaseOptions(handle, downloadTimeout_);

        curl_slist* hdr = nullptr;
        if (!auth.empty())
            hdr = curl_slist_append(hdr, auth.c_str());

        std::map<std::string, std::string> headers;
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &CurlHelper::writeFile);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, &CurlHelper::writeHeader);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, &headers);
        if (hdr)
            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, hdr);

        if (progressFn_) {
            // Report bytes as they arrive; a true return from the callback aborts the transfer.
            curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, &CurlHelper::xferInfo);
            curl_easy_setopt(handle, CURLOPT_XFERINFODATA, this);
            progressFn_(0, 0);
        }

        const CURLcode code = curl_easy_perform(handle);
        if (hdr)
            curl_slist_free_all(hdr);
        out.close();

        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &resp.status);
        resp.headers = std::move(headers);

        if (code == CURLE_ABORTED_BY_CALLBACK) {
            resp.error = "Transfer aborted";
            FileSystem::removeFile(filePath);
            progressFn_ = nullptr;
            return resp;
        }

        if (code != CURLE_OK) {
            resp.error = curl_easy_strerror(code);
            SF_LOG_E("Cloud", "Download attempt %d failed (%s): %s", attempt + 1, url.c_str(),
                     resp.error.c_str());
            const bool retryable = code == CURLE_FAILED_INIT || code == CURLE_COULDNT_CONNECT;
            FileSystem::removeFile(filePath);
            if (!retryable || attempt + 1 >= kMaxAttempts) {
                progressFn_ = nullptr;
                return resp;
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        resp.ok = resp.status >= 200 && resp.status < 300;
        if (resp.ok) {
            SF_LOG_I("Cloud", "Downloaded %s -> %s (HTTP %ld)", url.c_str(), filePath.c_str(),
                     resp.status);
            progressFn_ = nullptr;
            return resp;
        }

        resp.error = "HTTP " + std::to_string(resp.status);
        SF_LOG_E("Cloud", "Download HTTP %ld for %s", resp.status, url.c_str());
        FileSystem::removeFile(filePath);

        // 4xx will not improve on retry; only conversion (202) and server errors are worth waiting for.
        if (resp.status != 202 || resp.status < 500) {
            progressFn_ = nullptr;
            return resp;
        }

        long retryAfter = 5;
        const auto it = headers.find("Retry-After");
        if (it != headers.end()) {
            try {
                retryAfter = std::max(1L, std::min(60L, std::stol(it->second)));
            } catch (const std::exception&) {}
        }
        std::this_thread::sleep_for(std::chrono::seconds(retryAfter));
    }

    if (resp.error.empty())
        resp.error = "Download failed";
    progressFn_ = nullptr;
    return resp;
}

size_t CurlHelper::writeFile(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* out = static_cast<std::ofstream*>(userdata);
    out->write(ptr, static_cast<std::streamsize>(size * nmemb));
    return size * nmemb;
}

int CurlHelper::xferInfo(void* userdata, curl_off_t toDownload, curl_off_t downloaded,
                          curl_off_t, curl_off_t)
{
    auto* self = static_cast<CurlHelper*>(userdata);
    if (!self->progressFn_)
        return 0;
    return self->progressFn_(static_cast<int64_t>(downloaded), static_cast<int64_t>(toDownload)) ? 1 : 0;
}

CurlResponse CurlHelper::deleteUrl(const std::string& url)
{
    return perform("DELETE", url, nullptr, false, false, nullptr);
}

} // namespace sf::cloud
