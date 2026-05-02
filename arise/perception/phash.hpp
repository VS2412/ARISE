#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace arise::vision {

// 64-bit average-hash (aHash) of an image: downscale to 8x8 grayscale,
// compare each cell to the mean → 1 bit per cell. Cheap, decent recall for
// "did the screen change". Reads grim's P6 PPM output directly so we don't
// need an image library.
std::optional<uint64_t> aHashFromPpm(const std::string& path);

// 64-bit Hamming distance. 0 = identical, 64 = totally different.
int hammingDistance(std::uint64_t a, std::uint64_t b);

// Test helper: synthesise a P6 PPM by calling fill(x,y) for every pixel.
// Returns true on success. Used by unit tests so we can hash deterministic
// frames without screenshotting anything.
using PixelFn = std::function<std::array<unsigned char, 3>(int x, int y)>;
bool writePpm(const std::string& path, int w, int h, const PixelFn& fill);

} // namespace arise::vision
