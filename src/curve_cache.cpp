#include "nanoxgen/curve_cache.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace nanoxgen {
namespace {

constexpr std::size_t kSectionAlignment = 16u;
constexpr std::uint32_t kKnownFlags = CurveCacheHasTexcoords |
    CurveCacheHasPatchUVs | CurveCacheHasFaceUVs | CurveCacheHasFaceIds |
    CurveCacheHasMotion;

std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

template<typename T>
std::uint64_t append_array(std::vector<std::byte> &blob, std::span<const T> values) {
    if (values.empty()) { return 0u; }
    const std::size_t offset = align_up(blob.size(), std::max(kSectionAlignment, alignof(T)));
    if (values.size_bytes() > std::numeric_limits<std::size_t>::max() - offset) {
        throw std::invalid_argument("curve cache size overflows size_t");
    }
    blob.resize(offset + values.size_bytes());
    std::memcpy(blob.data() + offset, values.data(), values.size_bytes());
    return offset;
}

// Non-cryptographic integrity checksum. Processing the renderer-host byte
// sequence per 64-bit word avoids the serial dependency chain of
// byte-at-a-time FNV when validating multi-million-CV caches.
std::uint64_t content_hash(std::span<const std::byte> bytes) noexcept {
    constexpr std::uint64_t prime = 0x9e3779b185ebca87ull;
    std::uint64_t hash = 0x243f6a8885a308d3ull ^ static_cast<std::uint64_t>(bytes.size());
    std::size_t offset = 0u;
    for (; offset + sizeof(std::uint64_t) <= bytes.size(); offset += sizeof(std::uint64_t)) {
        std::uint64_t word{};
        std::memcpy(&word, bytes.data() + offset, sizeof(word));
        hash ^= word;
        hash *= prime;
        hash ^= hash >> 29u;
    }
    std::uint64_t tail{};
    const std::size_t remaining = bytes.size() - offset;
    if (remaining != 0u) { std::memcpy(&tail, bytes.data() + offset, remaining); }
    hash ^= tail + prime;
    hash *= 0xc2b2ae3d27d4eb4full;
    hash ^= hash >> 32u;
    return hash;
}

template<typename T>
bool section_fits(const CurveCacheHeader &header, std::uint64_t offset, std::uint64_t count) {
    if (count == 0u) { return offset == 0u; }
    if (offset < sizeof(CurveCacheHeader) || offset > header.byte_size) { return false; }
    if (count > std::numeric_limits<std::uint64_t>::max() / sizeof(T)) { return false; }
    const std::uint64_t size = count * sizeof(T);
    return size <= header.byte_size - offset && offset % alignof(T) == 0u;
}

bool finite(Vec2 value) noexcept { return std::isfinite(value.x) && std::isfinite(value.y); }

} // namespace

const CurveCacheHeader &CurveCacheView::header() const noexcept {
    return *reinterpret_cast<const CurveCacheHeader *>(_data);
}

const std::uint32_t *CurveCacheView::point_counts() const noexcept {
    return at<std::uint32_t>(header().point_counts_offset);
}
const PackedCurvePoint *CurveCacheView::points() const noexcept {
    return at<PackedCurvePoint>(header().points_offset);
}
const float *CurveCacheView::motion_times() const noexcept {
    return at<float>(header().motion_times_offset);
}
const Vec3 *CurveCacheView::motion_points() const noexcept {
    return at<Vec3>(header().motion_points_offset);
}
const Vec2 *CurveCacheView::texcoords() const noexcept { return at<Vec2>(header().texcoords_offset); }
const Vec2 *CurveCacheView::patch_uvs() const noexcept { return at<Vec2>(header().patch_uvs_offset); }
const Vec2 *CurveCacheView::face_uvs() const noexcept { return at<Vec2>(header().face_uvs_offset); }
const std::uint32_t *CurveCacheView::face_ids() const noexcept {
    return at<std::uint32_t>(header().face_ids_offset);
}

CurveCache build_curve_cache(const CurveCacheBuildInput &input) {
    if (input.point_counts.empty() || input.points.empty()) {
        throw std::invalid_argument("curve cache needs curves and points");
    }
    if (input.point_counts.size() > std::numeric_limits<std::uint32_t>::max() ||
        input.points.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("curve cache v1 count exceeds uint32");
    }
    std::uint64_t point_sum = 0u;
    for (const std::uint32_t count : input.point_counts) {
        if (count < 2u) { throw std::invalid_argument("curve cache curve has fewer than two CVs"); }
        point_sum += count;
    }
    if (point_sum != input.points.size()) {
        throw std::invalid_argument("curve cache point counts do not match point array");
    }
    const auto require_optional = [&](std::size_t size, std::size_t expected, const char *name) {
        if (size != 0u && size != expected) {
            throw std::invalid_argument(std::string{"curve cache "} + name + " size mismatch");
        }
    };
    require_optional(input.texcoords.size(), input.points.size(), "texcoord");
    require_optional(input.patch_uvs.size(), input.point_counts.size(), "patch-UV");
    require_optional(input.face_uvs.size(), input.point_counts.size(), "face-UV");
    require_optional(input.face_ids.size(), input.point_counts.size(), "face-ID");
    if (input.motion_times.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("curve cache has too many motion samples");
    }
    if (input.motion_times.empty() != input.motion_points.empty()) {
        throw std::invalid_argument("curve cache motion times and positions must both be present");
    }
    if (!input.motion_times.empty() &&
        (input.points.size() > std::numeric_limits<std::size_t>::max() /
                               input.motion_times.size() ||
         input.motion_points.size() != input.points.size() * input.motion_times.size())) {
        throw std::invalid_argument("curve cache motion-position size mismatch");
    }
    float previous_time = -std::numeric_limits<float>::infinity();
    for (const float time : input.motion_times) {
        if (!std::isfinite(time) || time <= previous_time) {
            throw std::invalid_argument("curve cache motion times must be finite and increasing");
        }
        previous_time = time;
    }

    std::vector<std::byte> bytes(sizeof(CurveCacheHeader));
    CurveCacheHeader header{};
    header.strand_count = static_cast<std::uint32_t>(input.point_counts.size());
    header.point_count = static_cast<std::uint32_t>(input.points.size());
    header.motion_sample_count = static_cast<std::uint32_t>(input.motion_times.size());
    if (!input.texcoords.empty()) { header.flags |= CurveCacheHasTexcoords; }
    if (!input.patch_uvs.empty()) { header.flags |= CurveCacheHasPatchUVs; }
    if (!input.face_uvs.empty()) { header.flags |= CurveCacheHasFaceUVs; }
    if (!input.face_ids.empty()) { header.flags |= CurveCacheHasFaceIds; }
    if (!input.motion_times.empty()) { header.flags |= CurveCacheHasMotion; }
    header.point_counts_offset = append_array(bytes, input.point_counts);
    header.points_offset = append_array(bytes, input.points);
    header.motion_times_offset = append_array(bytes, input.motion_times);
    header.motion_points_offset = append_array(bytes, input.motion_points);
    header.texcoords_offset = append_array(bytes, input.texcoords);
    header.patch_uvs_offset = append_array(bytes, input.patch_uvs);
    header.face_uvs_offset = append_array(bytes, input.face_uvs);
    header.face_ids_offset = append_array(bytes, input.face_ids);
    header.byte_size = bytes.size();
    std::memcpy(bytes.data(), &header, sizeof(header));
    header.content_hash = content_hash(std::span{bytes}.subspan(sizeof(CurveCacheHeader)));
    std::memcpy(bytes.data(), &header, sizeof(header));
    CurveCache result{std::move(bytes)};
    const std::string error = validate_curve_cache(result.bytes());
    if (!error.empty()) { throw std::runtime_error("built an invalid curve cache: " + error); }
    return result;
}

std::string validate_curve_cache(std::span<const std::byte> bytes) {
    if (bytes.size() < sizeof(CurveCacheHeader)) { return "blob is smaller than CurveCacheHeader"; }
    CurveCacheHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kCurveCacheMagic) { return "bad curve-cache magic"; }
    if (header.version_major != kCurveCacheVersionMajor) { return "unsupported curve-cache major version"; }
    if ((header.flags & ~kKnownFlags) != 0u) { return "curve cache has unknown flags"; }
    if (header.byte_size != bytes.size()) { return "curve-cache byte_size mismatch"; }
    if (header.strand_count == 0u || header.point_count == 0u) { return "curve cache is empty"; }
    if (((header.flags & CurveCacheHasMotion) != 0u) !=
        (header.motion_sample_count != 0u)) {
        return "curve-cache motion flag/count mismatch";
    }
    const std::uint64_t motion_point_count =
        static_cast<std::uint64_t>(header.motion_sample_count) * header.point_count;
    const auto optional_count = [&](std::uint32_t flag, std::uint64_t count) {
        return (header.flags & flag) != 0u ? count : 0u;
    };
    if (!section_fits<std::uint32_t>(header, header.point_counts_offset, header.strand_count) ||
        !section_fits<PackedCurvePoint>(header, header.points_offset, header.point_count) ||
        !section_fits<float>(header, header.motion_times_offset, header.motion_sample_count) ||
        !section_fits<Vec3>(header, header.motion_points_offset, motion_point_count) ||
        !section_fits<Vec2>(header, header.texcoords_offset,
                           optional_count(CurveCacheHasTexcoords, header.point_count)) ||
        !section_fits<Vec2>(header, header.patch_uvs_offset,
                           optional_count(CurveCacheHasPatchUVs, header.strand_count)) ||
        !section_fits<Vec2>(header, header.face_uvs_offset,
                           optional_count(CurveCacheHasFaceUVs, header.strand_count)) ||
        !section_fits<std::uint32_t>(header, header.face_ids_offset,
                                    optional_count(CurveCacheHasFaceIds, header.strand_count))) {
        return "one or more curve-cache sections are out of bounds";
    }
    if (content_hash(bytes.subspan(sizeof(CurveCacheHeader))) != header.content_hash) {
        return "curve-cache content hash mismatch";
    }
    const CurveCacheView view{bytes.data()};
    std::uint64_t point_sum = 0u;
    for (std::uint32_t strand = 0u; strand < header.strand_count; ++strand) {
        const std::uint32_t count = view.point_counts()[strand];
        if (count < 2u) { return "curve cache contains a curve with fewer than two CVs"; }
        point_sum += count;
    }
    if (point_sum != header.point_count) { return "curve-cache point counts do not sum to point_count"; }
    for (std::uint32_t point = 0u; point < header.point_count; ++point) {
        const PackedCurvePoint value = view.points()[point];
        if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
            !std::isfinite(value.z) || !std::isfinite(value.radius) || value.radius < 0.0f) {
            return "curve cache contains an invalid point or radius";
        }
        if (view.texcoords() && !finite(view.texcoords()[point])) {
            return "curve cache contains an invalid texcoord";
        }
    }
    float previous_time = -std::numeric_limits<float>::infinity();
    for (std::uint32_t sample = 0u; sample < header.motion_sample_count; ++sample) {
        const float time = view.motion_times()[sample];
        if (!std::isfinite(time) || time <= previous_time) {
            return "curve cache contains invalid motion times";
        }
        previous_time = time;
    }
    for (std::uint64_t point = 0u; point < motion_point_count; ++point) {
        const Vec3 value = view.motion_points()[point];
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) {
            return "curve cache contains an invalid motion position";
        }
    }
    for (std::uint32_t strand = 0u; strand < header.strand_count; ++strand) {
        if (view.patch_uvs() && !finite(view.patch_uvs()[strand])) {
            return "curve cache contains an invalid patch UV";
        }
        if (view.face_uvs() && !finite(view.face_uvs()[strand])) {
            return "curve cache contains an invalid face UV";
        }
    }
    return {};
}

void save_curve_cache(const CurveCache &cache, const std::filesystem::path &path) {
    const std::string error = validate_curve_cache(cache.bytes());
    if (!error.empty()) { throw std::runtime_error("refusing to save invalid curve cache: " + error); }
    std::ofstream output(path, std::ios::binary);
    if (!output) { throw std::runtime_error("failed to open curve cache: " + path.string()); }
    output.write(reinterpret_cast<const char *>(cache.bytes().data()),
                 static_cast<std::streamsize>(cache.bytes().size()));
    if (!output) { throw std::runtime_error("failed while writing curve cache: " + path.string()); }
}

CurveCache load_curve_cache(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) { throw std::runtime_error("failed to open curve cache: " + path.string()); }
    const std::streampos end = input.tellg();
    if (end < 0) { throw std::runtime_error("failed to query curve-cache size"); }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(bytes.data()), end);
    if (!input) { throw std::runtime_error("failed to read curve cache: " + path.string()); }
    const std::string error = validate_curve_cache(bytes);
    if (!error.empty()) { throw std::runtime_error("invalid curve cache: " + error); }
    return CurveCache{std::move(bytes)};
}

} // namespace nanoxgen
