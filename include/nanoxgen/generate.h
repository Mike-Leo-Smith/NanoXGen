#pragma once

#include "nanoxgen/device_view.h"

#if !defined(__CUDA_ARCH__) && !defined(__HIP_DEVICE_COMPILE__)
#include <cmath>
#endif

namespace nanoxgen {

NXG_HOST_DEVICE inline float nxg_sqrt(float x) noexcept {
#if defined(__CUDA_ARCH__)
    return sqrtf(x);
#else
    return std::sqrt(x);
#endif
}

NXG_HOST_DEVICE inline float nxg_sin(float x) noexcept {
#if defined(__CUDA_ARCH__)
    return sinf(x);
#else
    return std::sin(x);
#endif
}

NXG_HOST_DEVICE inline float nxg_pow(float x, float y) noexcept {
#if defined(__CUDA_ARCH__)
    return powf(x, y);
#else
    return std::pow(x, y);
#endif
}

NXG_HOST_DEVICE inline float nxg_max(float a, float b) noexcept { return a > b ? a : b; }
NXG_HOST_DEVICE inline float nxg_min(float a, float b) noexcept { return a < b ? a : b; }
NXG_HOST_DEVICE inline float nxg_clamp(float x, float lo, float hi) noexcept {
    return nxg_min(nxg_max(x, lo), hi);
}

NXG_HOST_DEVICE inline Vec3 normalize(Vec3 v) noexcept {
    const float l2 = length_squared(v);
    return l2 > 1.0e-20f ? v / nxg_sqrt(l2) : Vec3{0.0f, 1.0f, 0.0f};
}

NXG_HOST_DEVICE inline std::uint32_t hash32(std::uint32_t x) noexcept {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

NXG_HOST_DEVICE inline float random01(std::uint32_t seed, std::uint32_t strand, std::uint32_t dimension) noexcept {
    const std::uint32_t h = hash32(seed ^ hash32(strand + 0x9e3779b9u * (dimension + 1u)));
    return static_cast<float>(h >> 8u) * (1.0f / 16777216.0f);
}

NXG_HOST_DEVICE inline const Vec3 *frame_positions(
    DeviceAssetView asset, DeviceDeformedGeometryView deformed) noexcept {
    return deformed.positions ? deformed.positions : asset.positions();
}

NXG_HOST_DEVICE inline const Vec3 *frame_normals(
    DeviceAssetView asset, DeviceDeformedGeometryView deformed) noexcept {
    return deformed.normals ? deformed.normals : asset.normals();
}

NXG_HOST_DEVICE inline const Vec3 *frame_guide_cvs(
    DeviceAssetView asset, DeviceDeformedGeometryView deformed) noexcept {
    return deformed.guide_cvs ? deformed.guide_cvs : asset.guide_cvs();
}

NXG_HOST_DEVICE inline RootSample sample_root(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    std::uint32_t strand) noexcept {
    const auto &h = asset.header();
    const float choose = random01(params.seed, strand, 0u) * static_cast<float>(h.triangle_count);
    std::uint32_t bucket = static_cast<std::uint32_t>(choose);
    if (bucket >= h.triangle_count) { bucket = h.triangle_count - 1u; }
    const auto entry = asset.alias_table()[bucket];
    const std::uint32_t triangle_index =
        random01(params.seed, strand, 1u) < entry.probability ? bucket : entry.alias;

    const float r1 = random01(params.seed, strand, 2u);
    const float r2 = random01(params.seed, strand, 3u);
    const float s = nxg_sqrt(r1);
    const float b0 = 1.0f - s;
    const float b1 = s * (1.0f - r2);
    const float b2 = s * r2;

    const UInt3 tri = asset.triangles()[triangle_index];
    const Vec3 *positions = frame_positions(asset, deformed);
    const Vec3 p = positions[tri.x] * b0 + positions[tri.y] * b1 + positions[tri.z] * b2;
    Vec3 n = normalize(cross(positions[tri.y] - positions[tri.x],
                             positions[tri.z] - positions[tri.x]));
    if ((h.flags & HasNormals) != 0u) {
        const Vec3 *normals = frame_normals(asset, deformed);
        n = normalize(normals[tri.x] * b0 + normals[tri.y] * b1 + normals[tri.z] * b2);
    }
    Vec2 uv{};
    if ((h.flags & HasTexcoords) != 0u) {
        uv = asset.texcoords()[tri.x] * b0 + asset.texcoords()[tri.y] * b1 + asset.texcoords()[tri.z] * b2;
    }
    return {p, n, uv, triangle_index, {b1, b2}};
}

NXG_HOST_DEVICE inline Vec3 guide_root(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GuideRecord &guide) noexcept {
    if (deformed.guide_cvs) { return deformed.guide_cvs[guide.first_cv]; }
    if (deformed.positions && guide.triangle_index < asset.header().triangle_count) {
        const UInt3 tri = asset.triangles()[guide.triangle_index];
        const float b1 = guide.barycentric.x;
        const float b2 = guide.barycentric.y;
        const float b0 = 1.0f - b1 - b2;
        return deformed.positions[tri.x] * b0 + deformed.positions[tri.y] * b1 +
               deformed.positions[tri.z] * b2;
    }
    return guide.root_position;
}

NXG_HOST_DEVICE inline Vec3 guide_normal(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GuideRecord &guide) noexcept {
    if (guide.triangle_index >= asset.header().triangle_count ||
        (!deformed.positions && !deformed.normals)) {
        return guide.root_normal;
    }
    const UInt3 tri = asset.triangles()[guide.triangle_index];
    const float b1 = guide.barycentric.x;
    const float b2 = guide.barycentric.y;
    const float b0 = 1.0f - b1 - b2;
    if (deformed.normals) {
        return normalize(deformed.normals[tri.x] * b0 + deformed.normals[tri.y] * b1 +
                         deformed.normals[tri.z] * b2);
    }
    const Vec3 *positions = frame_positions(asset, deformed);
    return normalize(cross(positions[tri.y] - positions[tri.x],
                           positions[tri.z] - positions[tri.x]));
}

NXG_HOST_DEVICE inline Vec3 follow_deformed_guide_root(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GuideRecord &guide,
    Vec3 point) noexcept {
    // When only the patch is deformed, translate the immutable rest guide with
    // its bound root. Otherwise subtracting a deformed root from a rest-space
    // CV would incorrectly cancel rigid patch motion.
    if (!deformed.guide_cvs && deformed.positions &&
        guide.triangle_index < asset.header().triangle_count) {
        point = point + guide_root(asset, deformed, guide) - guide.root_position;
    }
    return point;
}

NXG_HOST_DEVICE inline Vec3 sample_guide(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GuideRecord &guide,
    float t) noexcept {
    const Vec3 *cvs = frame_guide_cvs(asset, deformed);
    if (guide.cv_count <= 1u) {
        return follow_deformed_guide_root(asset, deformed, guide, cvs[guide.first_cv]);
    }
    const float x = nxg_clamp(t, 0.0f, 1.0f) * static_cast<float>(guide.cv_count - 1u);
    std::uint32_t i = static_cast<std::uint32_t>(x);
    if (i >= guide.cv_count - 1u) { i = guide.cv_count - 2u; }
    const float f = x - static_cast<float>(i);
    const Vec3 a = cvs[guide.first_cv + i];
    const Vec3 b = cvs[guide.first_cv + i + 1u];
    return follow_deformed_guide_root(asset, deformed, guide, a * (1.0f - f) + b * f);
}

struct GuideBlendState {
    std::uint32_t indices[kGuideStencilSize]{};
    float weights[kGuideStencilSize]{};
    float weight_sum{};
    std::uint32_t nearest{kInvalidIndex};
};

NXG_HOST_DEVICE inline GuideBlendState make_guide_blend_state(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    const RootSample &root) noexcept {
    GuideBlendState blend{};
    for (std::uint32_t j = 0u; j < kGuideStencilSize; ++j) {
        blend.indices[j] = kInvalidIndex;
    }
    const std::uint32_t *stencil = asset.triangle_guides() +
        root.triangle_index * asset.header().guide_stencil_size;
    float nearest_d2 = 3.402823466e+38f;

    for (std::uint32_t j = 0; j < asset.header().guide_stencil_size; ++j) {
        const std::uint32_t guide_index = stencil[j];
        if (guide_index == kInvalidIndex) { continue; }
        const GuideRecord &guide = asset.guides()[guide_index];
        const Vec3 current_root = guide_root(asset, deformed, guide);
        const float d2 = length_squared(root.position - current_root);
        if (dot(root.normal, guide_normal(asset, deformed, guide)) < params.normal_rejection_cos) { continue; }
        if (d2 < nearest_d2) { nearest_d2 = d2; blend.nearest = guide_index; }
        const float radius = guide.support_radius * params.guide_support_scale;
        if (radius <= 0.0f || d2 >= radius * radius) { continue; }
        const float w = nxg_pow(1.0f - nxg_sqrt(d2) / radius,
                                nxg_max(params.guide_weight_power, 0.0f));
        blend.indices[j] = guide_index;
        blend.weights[j] = w;
        blend.weight_sum += w;
    }
    return blend;
}

NXG_HOST_DEVICE inline Vec3 interpolate_offset(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const RootSample &root,
    const GuideBlendState &blend,
    float t) noexcept {
    Vec3 offset{};
    for (std::uint32_t j = 0u; j < kGuideStencilSize; ++j) {
        const std::uint32_t guide_index = blend.indices[j];
        if (guide_index == kInvalidIndex) { continue; }
        const GuideRecord &guide = asset.guides()[guide_index];
        offset = offset + (sample_guide(asset, deformed, guide, t) -
                           guide_root(asset, deformed, guide)) * blend.weights[j];
    }

    if (blend.weight_sum > 1.0e-12f) { return offset / blend.weight_sum; }
    if (blend.nearest != kInvalidIndex) {
        const GuideRecord &guide = asset.guides()[blend.nearest];
        return sample_guide(asset, deformed, guide, t) - guide_root(asset, deformed, guide);
    }
    return root.normal * t;
}

// Convenience overload for point queries outside the generator. Generation
// itself precomputes GuideBlendState once per strand.
NXG_HOST_DEVICE inline Vec3 interpolate_offset(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    const RootSample &root,
    float t) noexcept {
    const GuideBlendState blend = make_guide_blend_state(asset, deformed, params, root);
    return interpolate_offset(asset, deformed, root, blend, t);
}

struct StrandGenerationState {
    RootSample root{};
    GuideBlendState guide_blend{};
    Vec3 noise_direction{};
    float noise_phase{};
};

NXG_HOST_DEVICE inline StrandGenerationState make_strand_generation_state(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    std::uint32_t strand) noexcept {
    constexpr float two_pi = 6.2831853071795864769f;
    StrandGenerationState state{};
    state.root = sample_root(asset, deformed, params, strand);
    state.guide_blend = make_guide_blend_state(asset, deformed, params, state.root);
    if (params.noise_amplitude != 0.0f) {
        const Vec3 tangent_axis = state.root.normal.z > -0.999f && state.root.normal.z < 0.999f
            ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
        const Vec3 tangent = normalize(cross(state.root.normal, tangent_axis));
        const Vec3 bitangent = normalize(cross(state.root.normal, tangent));
        state.noise_phase = two_pi * random01(params.seed, strand, 4u);
        const float angle = two_pi * random01(params.seed, strand, 5u);
        state.noise_direction = tangent * nxg_sin(angle) +
                                bitangent * nxg_sin(angle + 1.57079632679f);
    }
    return state;
}

NXG_HOST_DEVICE inline void evaluate_strand_cv(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    const StrandGenerationState &state,
    std::uint32_t cv,
    Vec3 &point,
    float &width) noexcept {
    constexpr float two_pi = 6.2831853071795864769f;
    const float t = params.cvs_per_strand > 1u
        ? static_cast<float>(cv) / static_cast<float>(params.cvs_per_strand - 1u) : 0.0f;
    point = state.root.position +
            interpolate_offset(asset, deformed, state.root, state.guide_blend, t) * params.length_scale;
    if (params.noise_amplitude != 0.0f) {
        const float envelope = t * t;
        point = point + state.noise_direction * (params.noise_amplitude * envelope *
            nxg_sin(two_pi * params.noise_frequency * t + state.noise_phase));
    }
    width = params.root_width * (1.0f - t) + params.tip_width * t;
}

NXG_HOST_DEVICE inline void generate_strand(
    DeviceAssetView asset,
    const GenerationParams &params,
    std::uint32_t strand,
    GeneratedOutputView output,
    DeviceDeformedGeometryView deformed = {}) noexcept {
    const StrandGenerationState state = make_strand_generation_state(
        asset, deformed, params, strand);
    if (output.roots) { output.roots[strand] = state.root; }

    for (std::uint32_t cv = 0; cv < params.cvs_per_strand; ++cv) {
        Vec3 point{};
        float width = 0.0f;
        evaluate_strand_cv(asset, deformed, params, state, cv, point, width);
        const std::uint64_t index = static_cast<std::uint64_t>(strand) * params.cvs_per_strand + cv;
        if (output.points) { output.points[index] = point; }
        if (output.widths) { output.widths[index] = width; }
    }
}

NXG_HOST_DEVICE inline void generate_packed_strand(
    DeviceAssetView asset,
    const GenerationParams &params,
    std::uint32_t strand,
    DevicePackedCurveOutputView output,
    DeviceDeformedGeometryView deformed = {}) noexcept {
    const StrandGenerationState state = make_strand_generation_state(
        asset, deformed, params, strand);
    if (output.roots) { output.roots[strand] = state.root; }
    if (output.root_uvs) { output.root_uvs[strand] = state.root.uv; }
    for (std::uint32_t cv = 0u; cv < params.cvs_per_strand; ++cv) {
        Vec3 point{};
        float width = 0.0f;
        evaluate_strand_cv(asset, deformed, params, state, cv, point, width);
        const std::uint64_t index = static_cast<std::uint64_t>(strand) *
                                    params.cvs_per_strand + cv;
        output.points[index] = {point.x, point.y, point.z,
                                0.5f * width * output.radius_scale};
    }
}

} // namespace nanoxgen
