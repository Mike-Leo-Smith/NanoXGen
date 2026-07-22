#pragma once

#include "nanoxgen/blob_storage.h"
#include "nanoxgen/types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace nanoxgen {

inline constexpr std::uint64_t kCurveCacheMagic = 0x454843414347584eull; // "NXGCACHE"
inline constexpr std::uint16_t kCurveCacheVersionMajor = 1u;

enum CurveCacheFlags : std::uint32_t {
    CurveCacheHasTexcoords = 1u << 0u,
    CurveCacheHasPatchUVs = 1u << 1u,
    CurveCacheHasFaceUVs = 1u << 2u,
    CurveCacheHasFaceIds = 1u << 3u,
    CurveCacheHasMotion = 1u << 4u,
};

// Pointer-free and directly uploadable. All offsets are from the beginning of
// this header. The current format stores one fully evaluated shutter sample.
struct alignas(64) CurveCacheHeader {
    std::uint64_t magic{kCurveCacheMagic};
    std::uint16_t version_major{kCurveCacheVersionMajor};
    std::uint16_t version_minor{};
    std::uint32_t flags{};
    std::uint64_t byte_size{};
    std::uint64_t content_hash{};
    std::uint32_t strand_count{};
    std::uint32_t point_count{};
    std::uint32_t motion_sample_count{};
    std::uint32_t reserved{};
    std::uint64_t point_counts_offset{};
    std::uint64_t points_offset{};
    std::uint64_t motion_times_offset{};
    std::uint64_t motion_points_offset{};
    std::uint64_t texcoords_offset{};
    std::uint64_t patch_uvs_offset{};
    std::uint64_t face_uvs_offset{};
    std::uint64_t face_ids_offset{};
};

static_assert(alignof(CurveCacheHeader) == 64u);
static_assert(std::is_trivially_copyable_v<CurveCacheHeader>);

struct CurveCacheBuildInput {
    std::span<const std::uint32_t> point_counts;
    std::span<const PackedCurvePoint> points;
    std::span<const Vec2> texcoords;
    std::span<const Vec2> patch_uvs;
    std::span<const Vec2> face_uvs;
    std::span<const std::uint32_t> face_ids;
    // Sample-major absolute positions. Each sample contains points.size()
    // entries in exactly the same curve/CV order as the base sample.
    std::span<const float> motion_times;
    std::span<const Vec3> motion_points;
};

class CurveCacheView {
public:
    explicit CurveCacheView(const std::byte *data = nullptr) : _data(data) {}

    [[nodiscard]] const CurveCacheHeader &header() const noexcept;
    [[nodiscard]] const std::uint32_t *point_counts() const noexcept;
    [[nodiscard]] const PackedCurvePoint *points() const noexcept;
    [[nodiscard]] const float *motion_times() const noexcept;
    [[nodiscard]] const Vec3 *motion_points() const noexcept;
    [[nodiscard]] const Vec2 *texcoords() const noexcept;
    [[nodiscard]] const Vec2 *patch_uvs() const noexcept;
    [[nodiscard]] const Vec2 *face_uvs() const noexcept;
    [[nodiscard]] const std::uint32_t *face_ids() const noexcept;

private:
    template<typename T>
    [[nodiscard]] const T *at(std::uint64_t offset) const noexcept {
        return offset == 0u ? nullptr :
            reinterpret_cast<const T *>(_data + static_cast<std::size_t>(offset));
    }
    const std::byte *_data{};
};

class CurveCache {
public:
    CurveCache() = default;
    explicit CurveCache(AlignedByteVector bytes) : _bytes(std::move(bytes)) {}
    explicit CurveCache(const std::vector<std::byte> &bytes)
        : _bytes(bytes.begin(), bytes.end()) {}

    [[nodiscard]] CurveCacheView view() const noexcept { return CurveCacheView{_bytes.data()}; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {_bytes.data(), _bytes.size()};
    }
    [[nodiscard]] bool empty() const noexcept { return _bytes.empty(); }

private:
    AlignedByteVector _bytes;
};

[[nodiscard]] CurveCache build_curve_cache(const CurveCacheBuildInput &input);
[[nodiscard]] std::string validate_curve_cache(std::span<const std::byte> bytes);
void save_curve_cache(const CurveCache &cache, const std::filesystem::path &path);
[[nodiscard]] CurveCache load_curve_cache(const std::filesystem::path &path);

} // namespace nanoxgen
