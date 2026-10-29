#pragma once
// Experimental bounded decoder. See ../THIRD_PARTY_NOTICES.txt for research attribution.
// Unknown layouts fail closed; this is not an official ZBrush file specification.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace zthumb {
constexpr std::size_t kMaxPrefix = 1024 * 1024;
constexpr std::uint32_t kMaxDimension = 512;
constexpr std::uint32_t kStart = 0x18003;
constexpr std::uint32_t kEnd = 0x8803;

struct Image {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgb; // top-down packed RGB; fourth plane is ignored
};
struct Chunk {
    std::uint16_t kind = 0;
    std::size_t body = 0, bodyEnd = 0, end = 0;
};
inline std::uint16_t U16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (static_cast<unsigned>(p[1]) << 8));
}
inline std::uint32_t U32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
         | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline float F32(const std::uint8_t* p) noexcept {
    static_assert(sizeof(float) == 4, "32-bit float required");
    const auto bits = U32(p);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
inline bool ReadChunk(const std::vector<std::uint8_t>& data, std::size_t offset,
                      std::size_t limit, Chunk& out) noexcept {
    limit = (std::min)(limit, data.size());
    if (offset > limit || limit - offset < 14) return false;
    const auto* p = data.data() + offset;
    const std::uint32_t length = U32(p + 6);
    if (U32(p) != kStart || length < 14 || length > kMaxPrefix || length > limit - offset)
        return false;
    const auto end = offset + length;
    if (U32(data.data() + end - 4) != kEnd) return false;
    out = {U16(p + 4), offset + 10, end - 4, end};
    return true;
}

// Called with the fixed 176-byte initial read before allocating a variable read.
inline bool RequiredPrefix(const std::vector<std::uint8_t>& data,
                           std::size_t& required) noexcept {
    required = 0;
    if (data.size() < 176 || data.size() > kMaxPrefix ||
        std::memcmp(data.data(), "ZBrush File.", 12) != 0) return false;
    if (std::memcmp(data.data() + 76, "LTZ\0", 4) != 0 &&
        std::memcmp(data.data() + 76, "RPZ\0", 4) != 0) return false;
    Chunk name, dimensions;
    static constexpr std::uint8_t imageName[8] = {0, 0, 'I', 'm', 'a', 'g', 'e', 0};
    if (!ReadChunk(data, 92, data.size(), name) || name.kind != 12 || name.end != 114 ||
        name.bodyEnd - name.body != 8 ||
        std::memcmp(data.data() + name.body, imageName, 8) != 0) return false;
    if (!ReadChunk(data, 114, data.size(), dimensions) || dimensions.kind != 1 ||
        dimensions.end != 162 || dimensions.bodyEnd - dimensions.body != 34) return false;
    float rect[8];
    for (std::size_t i = 0; i < 8; ++i) {
        rect[i] = F32(data.data() + dimensions.body + i * 4);
        if (!std::isfinite(rect[i])) return false;
    }
    if (rect[2] < 1 || rect[2] > kMaxDimension || rect[3] < 1 || rect[3] > kMaxDimension ||
        std::floor(rect[2]) != rect[2] || std::floor(rect[3]) != rect[3] ||
        rect[0] != 0 || rect[1] != 0 || rect[4] != 0 || rect[5] != 0 ||
        rect[6] != rect[2] || rect[7] != rect[3]) return false;
    const auto* p = data.data() + 162;
    const auto layout = U16(p + 4);
    const auto length = U32(p + 6);
    if (U32(p) != kStart || (layout != 8 && layout != 10) ||
        length < 14 || length > kMaxPrefix - 162) return false;
    required = 162 + length;
    return true;
}

inline bool Decode(const std::vector<std::uint8_t>& data, Image& result) {
    std::size_t required;
    if (!RequiredPrefix(data, required) || required > data.size()) return false;
    Chunk dimensions, outer, inner;
    if (!ReadChunk(data, 114, data.size(), dimensions)) return false;
    float rect[8];
    for (std::size_t i = 0; i < 8; ++i) {
        rect[i] = F32(data.data() + dimensions.body + i * 4);
        if (!std::isfinite(rect[i])) return false;
    }
    // Range checks precede float-to-integer conversions to avoid undefined behavior.
    if (rect[2] < 1 || rect[2] > kMaxDimension || rect[3] < 1 || rect[3] > kMaxDimension ||
        std::floor(rect[2]) != rect[2] || std::floor(rect[3]) != rect[3] ||
        rect[0] != 0 || rect[1] != 0 || rect[4] != 0 || rect[5] != 0 ||
        rect[6] != rect[2] || rect[7] != rect[3]) return false;
    const auto width = static_cast<std::uint32_t>(rect[2]);
    const auto height = static_cast<std::uint32_t>(rect[3]);
    if (!ReadChunk(data, 162, data.size(), outer)) return false;
    const std::size_t extra = outer.kind == 8 ? 34 : 38;
    if (!ReadChunk(data, outer.body + extra, outer.bodyEnd, inner) || inner.kind != 6 ||
        inner.bodyEnd - inner.body < 36 ||
        outer.bodyEnd - inner.end != (outer.kind == 10 ? 1u : 0u)) return false;
    const auto* p = data.data() + inner.body;
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    const std::size_t expected = pixels * 4;
    const std::uint32_t compressed = U32(p + 20);
    if (U32(p) != expected || compressed < 8) return false;
    const std::size_t encodedStart = inner.body + 32;
    // Difference-based checks avoid overflow even for adversarial compressed sizes.
    if (compressed - 8 != inner.bodyEnd - encodedStart - 4) return false;
    const std::size_t encodedEnd = inner.bodyEnd - 4;
    std::vector<std::uint8_t> planes;
    planes.reserve(expected);
    std::size_t pos = encodedStart;
    while (pos < encodedEnd) {
        const auto code = data[pos++];
        const std::size_t count = code >= 128 ? 256u - code : code;
        if (count == 0 || count > expected - planes.size()) return false;
        if (code >= 128) {
            if (count > encodedEnd - pos) return false;
            planes.insert(planes.end(), data.data() + pos, data.data() + pos + count);
            pos += count;
        } else {
            if (pos >= encodedEnd) return false;
            planes.insert(planes.end(), count, data[pos++]);
        }
    }
    if (planes.size() != expected) return false;
    Image image;
    image.width = width;
    image.height = height;
    image.rgb.resize(pixels * 3);
    for (std::size_t i = 0; i < pixels; ++i) {
        image.rgb[i * 3] = planes[pixels * 2 + i];
        image.rgb[i * 3 + 1] = planes[i];
        image.rgb[i * 3 + 2] = planes[pixels + i];
    }
    result = std::move(image);
    return true;
}
} // namespace zthumb
