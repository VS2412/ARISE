#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace arise {

// Captioner client over Ollama's /api/generate. Defaults to moondream pinned
// to CPU (num_gpu=0) so the primary mind on GPU isn't swap-thrashed when
// perception fires a caption mid-conversation.
//
// Inputs are accepted as either a file path or in-memory bytes. The image is
// base64-encoded into the request.images array; format must be one Ollama can
// decode (PNG/JPEG/WebP — not the raw P6 PPM the perception loop already
// writes for aHash, so the caller is expected to grab a fresh PNG when it
// decides a caption is worth the cost).
class VisionClient {
public:
    struct Config {
        std::string ollama_url          = "http://127.0.0.1:11434";
        std::string model               = "moondream";
        int         num_gpu             = 0;     // 0 = pin to CPU
        std::string keep_alive          = "30m"; // how long Ollama keeps weights resident
        std::string prompt              = "Describe what is on this screen in one short, plain sentence.";
        int         timeout_sec         = 60;    // CPU moondream takes a few seconds
        int         connect_timeout_sec = 2;
        int         max_caption_chars   = 240;   // hard truncate before returning
    };

    explicit VisionClient(Config cfg);
    ~VisionClient() = default;
    VisionClient(const VisionClient&)            = delete;
    VisionClient& operator=(const VisionClient&) = delete;

    // Caption a file on disk. Returns empty on any failure.
    std::string captionFile(const std::string& image_path);

    // Caption already-loaded image bytes (PNG/JPEG/WebP).
    std::string captionBytes(const std::vector<std::uint8_t>& bytes);

    // Cheap probe: succeeds once Ollama has answered any caption call.
    bool reachable() const { return reachable_.load(); }

    const Config& config() const { return cfg_; }

private:
    Config             cfg_;
    std::atomic<bool>  reachable_{false};

    std::string captionInternal_(const std::vector<std::uint8_t>& bytes);
};

// Helpers used by VisionClient and reused by tests.
namespace vision_util {

// Read entire file into bytes. Returns empty on error.
std::vector<std::uint8_t> readFileBytes(const std::string& path);

// Standard MIME-style base64 (no line wrapping, padded with '='). Output is
// intentionally a std::string for direct json embedding.
std::string base64Encode(const std::uint8_t* data, std::size_t n);
inline std::string base64Encode(const std::vector<std::uint8_t>& v) {
    return base64Encode(v.data(), v.size());
}

} // namespace vision_util

} // namespace arise
