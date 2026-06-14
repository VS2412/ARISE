#include "util/vision_client.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace arise {

namespace vision_util {

std::vector<std::uint8_t> readFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg();
    if (sz <= 0) return {};
    std::vector<std::uint8_t> out(static_cast<std::size_t>(sz));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), sz);
    if (!f) out.clear();
    return out;
}

std::string base64Encode(const std::uint8_t* data, std::size_t n) {
    static constexpr char kAlpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((n + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= n) {
        std::uint32_t v = (std::uint32_t(data[i]) << 16)
                        | (std::uint32_t(data[i+1]) << 8)
                        |  std::uint32_t(data[i+2]);
        out.push_back(kAlpha[(v >> 18) & 0x3F]);
        out.push_back(kAlpha[(v >> 12) & 0x3F]);
        out.push_back(kAlpha[(v >>  6) & 0x3F]);
        out.push_back(kAlpha[ v        & 0x3F]);
        i += 3;
    }
    if (i < n) {
        std::uint32_t v = std::uint32_t(data[i]) << 16;
        if (i + 1 < n) v |= std::uint32_t(data[i+1]) << 8;
        out.push_back(kAlpha[(v >> 18) & 0x3F]);
        out.push_back(kAlpha[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < n ? kAlpha[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

} // namespace vision_util

namespace {

std::size_t writeCb(void* ptr, std::size_t sz, std::size_t nm, std::string* out) {
    out->append(static_cast<char*>(ptr), sz * nm);
    return sz * nm;
}

std::string trimWs(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

} // namespace

VisionClient::VisionClient(Config cfg) : cfg_(std::move(cfg)) {}

std::string VisionClient::captionFile(const std::string& image_path) {
    auto bytes = vision_util::readFileBytes(image_path);
    if (bytes.empty()) {
        log::debug("VisionClient: empty/missing file " + image_path);
        return {};
    }
    return captionInternal_(bytes);
}

std::string VisionClient::captionBytes(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return {};
    return captionInternal_(bytes);
}

std::string VisionClient::captionInternal_(const std::vector<std::uint8_t>& bytes) {
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    const std::string url = cfg_.ollama_url + "/api/generate";

    json body = {
        {"model",      cfg_.model},
        {"prompt",     cfg_.prompt},
        {"stream",     false},
        {"keep_alive", cfg_.keep_alive},
        {"images",     json::array({ vision_util::base64Encode(bytes) })},
        {"options",    {
            {"num_gpu",     cfg_.num_gpu},
            {"temperature", 0.2},
        }},
    };
    std::string payload  = body.dump();
    std::string response;

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  long(payload.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        long(cfg_.timeout_sec));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, long(cfg_.connect_timeout_sec));

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        log::debug(std::string("VisionClient: curl ") + curl_easy_strerror(rc));
        return {};
    }

    try {
        auto j = json::parse(response);
        if (j.contains("error")) {
            log::warn("VisionClient: ollama error: " +
                      j["error"].get<std::string>());
            return {};
        }
        if (!j.contains("response")) {
            log::warn("VisionClient: response missing 'response' field");
            return {};
        }
        std::string caption = trimWs(j["response"].get<std::string>());
        if (cfg_.max_caption_chars > 0 &&
            caption.size() > std::size_t(cfg_.max_caption_chars)) {
            caption.resize(cfg_.max_caption_chars);
            caption.append("…");
        }
        if (!caption.empty()) reachable_.store(true, std::memory_order_relaxed);
        return caption;
    } catch (const std::exception& e) {
        log::warn(std::string("VisionClient: parse: ") + e.what());
        return {};
    }
}

} // namespace arise
