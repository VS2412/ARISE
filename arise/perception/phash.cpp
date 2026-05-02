#include "perception/phash.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <vector>

namespace arise::vision {

namespace {

// Skip ASCII whitespace and `# ...\n` comments inside a PPM header.
void skipWsAndComments(std::ifstream& f) {
    while (true) {
        int c = f.peek();
        if (c == EOF) return;
        if (c == '#') { std::string s; std::getline(f, s); continue; }
        if (std::isspace(static_cast<unsigned char>(c))) { f.get(); continue; }
        break;
    }
}

bool readUint(std::ifstream& f, int& out) {
    skipWsAndComments(f);
    if (!(f >> out)) return false;
    return true;
}

} // namespace

int hammingDistance(std::uint64_t a, std::uint64_t b) {
    return __builtin_popcountll(a ^ b);
}

std::optional<std::uint64_t> aHashFromPpm(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;

    std::string magic;
    if (!(f >> magic)) return std::nullopt;
    if (magic != "P6") return std::nullopt;

    int w = 0, h = 0, maxv = 0;
    if (!readUint(f, w) || !readUint(f, h) || !readUint(f, maxv)) return std::nullopt;
    if (w < 8 || h < 8 || maxv != 255) return std::nullopt;

    // Header is terminated by exactly one whitespace before the binary body.
    f.get();

    const std::size_t total = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3;
    std::vector<unsigned char> rgb(total);
    f.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(total));
    if (static_cast<std::size_t>(f.gcount()) != total) return std::nullopt;

    // 8x8 cell sums (Rec.601 grayscale).
    std::array<std::uint64_t, 64> sum{};
    std::array<std::uint64_t, 64> count{};
    for (int y = 0; y < h; ++y) {
        int by = std::min(7, (y * 8) / h);
        for (int x = 0; x < w; ++x) {
            int bx = std::min(7, (x * 8) / w);
            std::size_t i = (static_cast<std::size_t>(y) * w + x) * 3;
            unsigned R = rgb[i], G = rgb[i + 1], B = rgb[i + 2];
            unsigned gray = (R * 299u + G * 587u + B * 114u) / 1000u;
            sum  [by * 8 + bx] += gray;
            count[by * 8 + bx] += 1;
        }
    }

    std::array<unsigned, 64> avg{};
    std::uint64_t total_avg = 0;
    for (int i = 0; i < 64; ++i) {
        avg[i] = count[i] ? static_cast<unsigned>(sum[i] / count[i]) : 0;
        total_avg += avg[i];
    }
    unsigned mean = static_cast<unsigned>(total_avg / 64);

    std::uint64_t hash = 0;
    for (int i = 0; i < 64; ++i)
        if (avg[i] > mean) hash |= (std::uint64_t{1} << i);
    return hash;
}

bool writePpm(const std::string& path, int w, int h, const PixelFn& fill) {
    if (w <= 0 || h <= 0) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << "P6\n" << w << " " << h << "\n255\n";
    std::vector<unsigned char> row(static_cast<std::size_t>(w) * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            auto rgb = fill(x, y);
            row[3 * x + 0] = rgb[0];
            row[3 * x + 1] = rgb[1];
            row[3 * x + 2] = rgb[2];
        }
        f.write(reinterpret_cast<const char*>(row.data()),
                static_cast<std::streamsize>(row.size()));
    }
    return f.good();
}

} // namespace arise::vision
