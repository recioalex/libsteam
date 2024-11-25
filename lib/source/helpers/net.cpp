#include <steam.hpp>

#include <steam/helpers/net.hpp>

#include <steam/helpers/utils.hpp>
#include <steam/helpers/file.hpp>

#include <filesystem>
#include <cstdio>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <fmt/format.h>

namespace steam {

    void Net::init() {
        curl_global_init(CURL_GLOBAL_ALL);
    }

    void Net::exit() {
        curl_global_cleanup();
    }

    Net::Net() {
        this->m_ctx = curl_easy_init();
    }

    Net::~Net() {
        curl_easy_cleanup(this->m_ctx);
    }

    static size_t writeToString(void *contents, size_t size, size_t nmemb, void *userdata) {
        static_cast<std::string *>(userdata)->append((char *)contents, size * nmemb);
        return size * nmemb;
    }

    static size_t readFromFile(void *contents, size_t size, size_t nmemb, void *userdata) {
        FILE *file = static_cast<FILE *>(userdata);

        return fread(contents, size, nmemb, file);
    }

    static size_t writeToFile(void *contents, size_t size, size_t nmemb, void *userdata) {
        FILE *file = static_cast<FILE *>(userdata);

        return fwrite(contents, size, nmemb, file);
    }

    int progressCallback(void *contents, curl_off_t dlTotal, curl_off_t dlNow, curl_off_t ulTotal, curl_off_t ulNow) {
        auto &net = *static_cast<Net *>(contents);

        if (dlTotal > 0)
            net.m_progress = float(dlNow) / dlTotal;
        else if (ulTotal > 0)
            net.m_progress = float(ulNow) / ulTotal;
        else
            net.m_progress = 0.0F;

        return net.m_shouldCancel ? CURLE_ABORTED_BY_CALLBACK : CURLE_OK;
    }

    void Net::setCommonSettings(std::string &response, const std::string &url, u32 timeout, const std::map<std::string, std::string> &extraHeaders, const std::string &body) {
        this->m_headers = curl_slist_append(this->m_headers, "Cache-Control: no-cache");

        if (!extraHeaders.empty())
            for (const auto &[key, value] : extraHeaders) {
                std::string entry = key;
                entry += ": ";
                entry += value;

                this->m_headers = curl_slist_append(this->m_headers, entry.c_str());
            }

        if (!body.empty())
            curl_easy_setopt(this->m_ctx, CURLOPT_POSTFIELDS, body.c_str());

        curl_easy_setopt(this->m_ctx, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
        curl_easy_setopt(this->m_ctx, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
        curl_easy_setopt(this->m_ctx, CURLOPT_URL, url.c_str());
        curl_easy_setopt(this->m_ctx, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(this->m_ctx, CURLOPT_HTTPHEADER, this->m_headers);
        curl_easy_setopt(this->m_ctx, CURLOPT_USERAGENT, "libsteam/1.0");
        curl_easy_setopt(this->m_ctx, CURLOPT_DEFAULT_PROTOCOL, "https");
        curl_easy_setopt(this->m_ctx, CURLOPT_WRITEFUNCTION, writeToString);
        curl_easy_setopt(this->m_ctx, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(this->m_ctx, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(this->m_ctx, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(this->m_ctx, CURLOPT_TIMEOUT_MS, 0L);
        curl_easy_setopt(this->m_ctx, CURLOPT_CONNECTTIMEOUT_MS, timeout);
        curl_easy_setopt(this->m_ctx, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(this->m_ctx, CURLOPT_XFERINFOFUNCTION, progressCallback);
        curl_easy_setopt(this->m_ctx, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(this->m_ctx, CURLOPT_NOPROGRESS, 0L);
    }

    std::optional<i32> Net::execute() {
        CURLcode result = curl_easy_perform(this->m_ctx);

        i32 responseCode = 0;
        curl_easy_getinfo(this->m_ctx, CURLINFO_RESPONSE_CODE, &responseCode);

        curl_slist_free_all(this->m_headers);
        this->m_headers      = nullptr;
        this->m_progress     = 0.0F;
        this->m_shouldCancel = false;

        if (result != CURLE_OK)
            return std::nullopt;
        else
            return responseCode;
    }

    std::future<Response<std::string>> Net::getString(const std::string &url, u32 timeout, const std::map<std::string, std::string> &extraHeaders, const std::string &body) {
        this->m_transmissionActive.lock();

        return std::async(std::launch::async, [=, this] {
            std::string response;

            curl_easy_setopt(this->m_ctx, CURLOPT_CUSTOMREQUEST, "GET");
            setCommonSettings(response, url, timeout, extraHeaders, body);

            auto responseCode = execute();

            this->m_transmissionActive.unlock();

            return Response<std::string> { responseCode.value_or(0), response };
        });
    }

    std::future<Response<nlohmann::json>> Net::getJson(const std::string &url, u32 timeout, const std::map<std::string, std::string> &extraHeaders, const std::string &body) {
        this->m_transmissionActive.lock();

        return std::async(std::launch::async, [=, this] {
            std::string response;

            curl_easy_setopt(this->m_ctx, CURLOPT_CUSTOMREQUEST, "GET");
            setCommonSettings(response, url, timeout, extraHeaders, body);

            auto responseCode = execute();

            this->m_transmissionActive.unlock();

            return Response<nlohmann::json> { responseCode.value_or(0), nlohmann::json::parse(response, nullptr, false) };
        });
    }

    std::future<Response<std::string>> Net::uploadFile(const std::string &url, const std::fs::path &filePath, u32 timeout, const std::map<std::string, std::string> &extraHeaders, const std::string &body) {
        this->m_transmissionActive.lock();

        return std::async(std::launch::async, [=, this] {
            std::string response;

            fs::File file(filePath.string(), fs::File::Mode::Read);
            if (!file.isValid())
                return Response<std::string> { 400, {} };

            curl_mime *mime     = curl_mime_init(this->m_ctx);
            curl_mimepart *part = curl_mime_addpart(mime);

            auto fileName = filePath.filename().string();
            curl_mime_data_cb(
                    part, file.getSize(), [](char *buffer, size_t size, size_t nitems, void *arg) -> size_t {
                        auto file = static_cast<FILE*>(arg);
                        return fread(buffer, size, nitems, file); }, [](void *arg, curl_off_t offset, int origin) -> int {
                        auto file = static_cast<FILE*>(arg);
                        fseek(file, offset, origin);
                        return CURL_SEEKFUNC_OK; }, [](void *arg) {
                        auto file = static_cast<FILE*>(arg);
                        fclose(file); }, file.getHandle());
            curl_mime_filename(part, fileName.c_str());
            curl_mime_name(part, "file");

            setCommonSettings(response, url, timeout, extraHeaders, body);
            curl_easy_setopt(this->m_ctx, CURLOPT_MIMEPOST, mime);
            curl_easy_setopt(this->m_ctx, CURLOPT_CUSTOMREQUEST, "POST");

            auto responseCode = execute();

            this->m_transmissionActive.unlock();

            return Response<std::string> { responseCode.value_or(0), response };
        });
    }

    std::future<Response<void>> Net::downloadFile(const std::string &url, const std::fs::path &filePath, u32 timeout, const std::map<std::string, std::string> &extraHeaders, const std::string &body) {
        this->m_transmissionActive.lock();

        return std::async(std::launch::async, [=, this] {
            std::string response;

            fs::File file(filePath.string(), fs::File::Mode::Create);
            if (!file.isValid())
                return Response<void> { 400 };

            setCommonSettings(response, url, timeout, extraHeaders, body);
            curl_easy_setopt(this->m_ctx, CURLOPT_CUSTOMREQUEST, "GET");
            curl_easy_setopt(this->m_ctx, CURLOPT_WRITEFUNCTION, writeToFile);
            curl_easy_setopt(this->m_ctx, CURLOPT_WRITEDATA, file.getHandle());
            auto responseCode = execute();

            this->m_transmissionActive.unlock();

            return Response<void> { responseCode.value_or(0) };
        });
    }

    std::string Net::encode(const std::string &input) {
        auto escapedString = curl_easy_escape(this->m_ctx, input.c_str(), std::strlen(input.c_str()));

        if (escapedString != nullptr) {
            std::string output = escapedString;
            curl_free(escapedString);

            return output;
        }

        return {};
    }

}
