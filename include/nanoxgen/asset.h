#pragma once

#include "nanoxgen/blob_storage.h"
#include "nanoxgen/device_contract.h"
#include "nanoxgen/generate.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace nanoxgen {

struct GuideInput {
    std::vector<Vec3> cvs;
    Vec3 root_normal{0.0f, 1.0f, 0.0f};
    Vec2 root_uv{};
    std::uint32_t triangle_index{kInvalidIndex};
    Vec2 barycentric{};
    float support_radius{0.0f}; // zero selects an automatic radius
};

struct AssetBuildInput {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> texcoords;
    std::vector<UInt3> triangles;
    std::vector<GuideInput> guides;
};

class Asset {
public:
    Asset() = default;
    explicit Asset(AlignedByteVector bytes) : _bytes(std::move(bytes)) {}
    explicit Asset(const std::vector<std::byte> &bytes)
        : _bytes(bytes.begin(), bytes.end()) {}

    [[nodiscard]] DeviceAssetView view() const noexcept { return DeviceAssetView{_bytes.data()}; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {_bytes.data(), _bytes.size()};
    }
    [[nodiscard]] bool empty() const noexcept { return _bytes.empty(); }

private:
    AlignedByteVector _bytes;
};

struct GeneratedCurves {
    std::uint32_t strand_count{};
    std::uint32_t cvs_per_strand{};
    std::vector<Vec3> points;
    std::vector<float> widths;
    std::vector<RootSample> roots;
};

// Direct renderer-facing output. Unlike GeneratedCurves followed by
// build_curve_batches, this path never materializes a separate Vec3/width
// intermediate: generation writes float4(position, radius) in one pass.
struct PackedGeneratedCurves {
    std::uint32_t strand_count{};
    std::uint32_t cvs_per_strand{};
    std::vector<std::uint32_t> point_counts;
    std::vector<PackedCurvePoint> points;
    std::vector<RootSample> roots;
    std::vector<Vec2> root_uvs;
};

// A compact compatibility input for XGen Interactive Grooming base splines.
// Default IGS hairs are linear before modifiers, so the public spline output
// can be represented losslessly at the algorithmic level by root/tip/UV/width
// rather than by retaining every generated CV.
struct LinearCurveSeed {
    Vec3 root{};
    Vec3 tip{};
    Vec2 root_uv{};
    float root_width{};
};

struct LinearModifierReferenceParams {
    std::uint32_t cvs_per_strand{8u};
    float length_scale{1.0f};
    float width_taper{0.0f};
    float width_taper_start{0.0f};
};

// Compatibility alias retained for the original v0.1 API. The reference path
// does not perform root sampling or guide interpolation.
using LinearGenerationParams = LinearModifierReferenceParams;

// CPU workers persist for the duration of one generation call and dynamically
// claim logical work blocks. The default tile contains as many strands as the
// unchanged CUDA kernel has threads per block, so one CPU worker serially
// executes the logical work corresponding to one CUDA block at a time.
struct CpuGenerationOptions {
    std::uint32_t worker_count{};             // zero selects hardware concurrency
    std::uint32_t strands_per_work_block{128u};
};

struct DeformedGeometryView {
    std::span<const Vec3> positions;
    std::span<const Vec3> normals;
    std::span<const Vec3> guide_cvs;
};

[[nodiscard]] Asset build_asset(const AssetBuildInput &input);
[[nodiscard]] std::string validate_asset(std::span<const std::byte> bytes);
void save_asset(const Asset &asset, const std::filesystem::path &path);
[[nodiscard]] Asset load_asset(const std::filesystem::path &path);
// Build the checked host descriptor after copying asset.bytes() to device_data.
// device_byte_capacity is the allocation size, not merely the copied size.
[[nodiscard]] DeviceAssetDescriptor make_device_asset_descriptor(
    const Asset &asset,
    const void *device_data,
    std::uint64_t device_byte_capacity);
[[nodiscard]] GeneratedCurves generate_cpu(const Asset &asset, const GenerationParams &params);
[[nodiscard]] GeneratedCurves generate_cpu(
    const Asset &asset, const GenerationParams &params, const CpuGenerationOptions &options);
[[nodiscard]] GeneratedCurves generate_deformed_cpu(
    const Asset &asset,
    const GenerationParams &params,
    const DeformedGeometryView &deformed,
    const CpuGenerationOptions &options = {});
[[nodiscard]] PackedGeneratedCurves generate_packed_cpu(
    const Asset &asset,
    const GenerationParams &params,
    float radius_scale = 1.0f,
    const CpuGenerationOptions &options = {});
[[nodiscard]] PackedGeneratedCurves generate_packed_deformed_cpu(
    const Asset &asset,
    const GenerationParams &params,
    const DeformedGeometryView &deformed,
    float radius_scale = 1.0f,
    const CpuGenerationOptions &options = {});
[[nodiscard]] GeneratedCurves generate_linear_modifier_reference_cpu(
    std::span<const LinearCurveSeed> seeds,
    const LinearModifierReferenceParams &params,
    const CpuGenerationOptions &options = {});
[[nodiscard]] GeneratedCurves generate_linear_cpu(
    std::span<const LinearCurveSeed> seeds,
    const LinearGenerationParams &params,
    const CpuGenerationOptions &options = {});
void write_curves_obj(const GeneratedCurves &curves, const std::filesystem::path &path);

} // namespace nanoxgen
