#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#if defined(__CUDACC__) || defined(__HIPCC__)
#define NXG_HOST_DEVICE __host__ __device__
#define NXG_DEVICE __device__
#else
#define NXG_HOST_DEVICE
#define NXG_DEVICE
#endif

namespace nanoxgen {

inline constexpr std::uint64_t kMagic = 0x4e4547584f4e414eull; // "NANOXGEN", little endian
inline constexpr std::uint16_t kVersionMajor = 0;
inline constexpr std::uint16_t kVersionMinor = 2;
inline constexpr std::uint32_t kGuideStencilSize = 8;
inline constexpr std::uint32_t kNoiseGradientCount = 256;
inline constexpr std::uint32_t kInvalidIndex = 0xffffffffu;

struct Vec2 {
    float x{};
    float y{};
};

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct UInt3 {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};
};

NXG_HOST_DEVICE inline Vec2 operator+(Vec2 a, Vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
NXG_HOST_DEVICE inline Vec2 operator*(Vec2 a, float s) noexcept { return {a.x * s, a.y * s}; }
NXG_HOST_DEVICE inline Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
NXG_HOST_DEVICE inline Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
NXG_HOST_DEVICE inline Vec3 operator*(Vec3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
NXG_HOST_DEVICE inline Vec3 operator/(Vec3 a, float s) noexcept { return {a.x / s, a.y / s, a.z / s}; }
NXG_HOST_DEVICE inline float dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
NXG_HOST_DEVICE inline Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
NXG_HOST_DEVICE inline float length_squared(Vec3 v) noexcept { return dot(v, v); }

struct AliasEntry {
    float probability{};
    std::uint32_t alias{};
};

struct GuideRecord {
    std::uint32_t first_cv{};
    std::uint16_t cv_count{};
    std::uint16_t flags{};
    std::uint32_t triangle_index{kInvalidIndex};
    float support_radius{};
    Vec2 barycentric{}; // weights for triangle vertices y and z; x = 1-y-z
    Vec2 root_uv{};
    Vec3 root_position{};
    Vec3 root_normal{};
};

enum AssetFlags : std::uint32_t {
    HasNormals = 1u << 0u,
    HasTexcoords = 1u << 1u,
};

// A pointer-free, relocatable header. All offsets are byte offsets from the
// first byte of this header, so the same blob is directly readable on a GPU.
struct alignas(64) AssetHeader {
    std::uint64_t magic{kMagic};
    std::uint16_t version_major{kVersionMajor};
    std::uint16_t version_minor{kVersionMinor};
    std::uint32_t flags{};
    std::uint64_t byte_size{};
    std::uint64_t content_hash{};

    std::uint32_t vertex_count{};
    std::uint32_t triangle_count{};
    std::uint32_t guide_count{};
    std::uint32_t guide_cv_count{};
    std::uint32_t guide_stencil_size{kGuideStencilSize};
    std::uint32_t noise_gradient_count{kNoiseGradientCount};

    std::uint64_t positions_offset{};
    std::uint64_t normals_offset{};
    std::uint64_t texcoords_offset{};
    std::uint64_t triangles_offset{};
    std::uint64_t alias_table_offset{};
    std::uint64_t guides_offset{};
    std::uint64_t guide_cvs_offset{};
    std::uint64_t triangle_guides_offset{};
    std::uint64_t noise_gradients_offset{};
};

static_assert(std::is_trivially_copyable_v<Vec2>);
static_assert(std::is_trivially_copyable_v<Vec3>);
static_assert(std::is_trivially_copyable_v<AssetHeader>);
static_assert(alignof(AssetHeader) == 64);

struct GenerationParams {
    std::uint32_t strand_count{1024};
    std::uint32_t cvs_per_strand{8};
    std::uint32_t seed{1};
    std::uint32_t reserved{};
    float guide_support_scale{1.0f};
    float guide_weight_power{2.0f};
    float normal_rejection_cos{-0.25f};
    float length_scale{1.0f};
    float root_width{0.01f};
    float tip_width{0.001f};
    float noise_amplitude{0.0f};
    float noise_frequency{2.0f};        // cycles per scene-unit of curve length
    float noise_mask{1.0f};
    float noise_correlation{0.0f};      // normalized XGen UI value in [0, 1]
    float noise_preserve_length{0.0f};  // normalized XGen UI value in [0, 1]
};

struct RootSample {
    Vec3 position{};
    Vec3 normal{};
    Vec2 uv{};
    std::uint32_t triangle_index{};
    Vec2 barycentric{};
};

// Renderer-facing point layout. NanoXGen and XGen widths are diameters, while
// ray tracing curve APIs commonly consume a radius in the fourth component.
struct alignas(16) PackedCurvePoint {
    float x{};
    float y{};
    float z{};
    float radius{};
};

static_assert(sizeof(PackedCurvePoint) == 16u);
static_assert(alignof(PackedCurvePoint) == 16u);

struct GeneratedOutputView {
    Vec3 *points{};        // strand-major, fixed cvs_per_strand
    float *widths{};       // strand-major, fixed cvs_per_strand
    RootSample *roots{};   // one record per strand
};

// Optional frame-local geometry. Null pointers select the immutable arrays in
// the asset. Root identities remain tied to the rest-pose alias table and the
// deterministic triangle/barycentric samples.
struct DeviceDeformedGeometryView {
    const Vec3 *positions{};
    const Vec3 *normals{};
    const Vec3 *guide_cvs{};
};

struct DevicePackedCurveOutputView {
    PackedCurvePoint *points{};
    RootSample *roots{};
    Vec2 *root_uvs{};
    float radius_scale{1.0f};
    std::uint32_t *point_counts{};
};

} // namespace nanoxgen
