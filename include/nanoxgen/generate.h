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

NXG_HOST_DEVICE inline RootSample sample_root(
    DeviceAssetView asset, const GenerationParams &params, std::uint32_t strand) noexcept {
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
    const Vec3 p = asset.positions()[tri.x] * b0 + asset.positions()[tri.y] * b1 + asset.positions()[tri.z] * b2;
    Vec3 n = normalize(cross(asset.positions()[tri.y] - asset.positions()[tri.x],
                             asset.positions()[tri.z] - asset.positions()[tri.x]));
    if ((h.flags & HasNormals) != 0u) {
        n = normalize(asset.normals()[tri.x] * b0 + asset.normals()[tri.y] * b1 + asset.normals()[tri.z] * b2);
    }
    Vec2 uv{};
    if ((h.flags & HasTexcoords) != 0u) {
        uv = asset.texcoords()[tri.x] * b0 + asset.texcoords()[tri.y] * b1 + asset.texcoords()[tri.z] * b2;
    }
    return {p, n, uv, triangle_index, {b1, b2}};
}

NXG_HOST_DEVICE inline Vec3 sample_guide(DeviceAssetView asset, const GuideRecord &guide, float t) noexcept {
    if (guide.cv_count <= 1u) { return asset.guide_cvs()[guide.first_cv]; }
    const float x = nxg_clamp(t, 0.0f, 1.0f) * static_cast<float>(guide.cv_count - 1u);
    std::uint32_t i = static_cast<std::uint32_t>(x);
    if (i >= guide.cv_count - 1u) { i = guide.cv_count - 2u; }
    const float f = x - static_cast<float>(i);
    const Vec3 a = asset.guide_cvs()[guide.first_cv + i];
    const Vec3 b = asset.guide_cvs()[guide.first_cv + i + 1u];
    return a * (1.0f - f) + b * f;
}

NXG_HOST_DEVICE inline Vec3 interpolate_offset(
    DeviceAssetView asset, const GenerationParams &params, const RootSample &root, float t) noexcept {
    const std::uint32_t *stencil = asset.triangle_guides() +
        root.triangle_index * asset.header().guide_stencil_size;
    float weight_sum = 0.0f;
    Vec3 offset{};
    std::uint32_t nearest = kInvalidIndex;
    float nearest_d2 = 3.402823466e+38f;

    for (std::uint32_t j = 0; j < asset.header().guide_stencil_size; ++j) {
        const std::uint32_t guide_index = stencil[j];
        if (guide_index == kInvalidIndex) { continue; }
        const GuideRecord &guide = asset.guides()[guide_index];
        const float d2 = length_squared(root.position - guide.root_position);
        if (dot(root.normal, guide.root_normal) < params.normal_rejection_cos) { continue; }
        if (d2 < nearest_d2) { nearest_d2 = d2; nearest = guide_index; }
        const float radius = guide.support_radius * params.guide_support_scale;
        if (radius <= 0.0f || d2 >= radius * radius) { continue; }
        const float w = nxg_pow(1.0f - nxg_sqrt(d2) / radius,
                                nxg_max(params.guide_weight_power, 0.0f));
        offset = offset + (sample_guide(asset, guide, t) - guide.root_position) * w;
        weight_sum += w;
    }

    if (weight_sum > 1.0e-12f) { return offset / weight_sum; }
    if (nearest != kInvalidIndex) {
        const GuideRecord &guide = asset.guides()[nearest];
        return sample_guide(asset, guide, t) - guide.root_position;
    }
    return root.normal * t;
}

NXG_HOST_DEVICE inline void generate_strand(
    DeviceAssetView asset,
    const GenerationParams &params,
    std::uint32_t strand,
    GeneratedOutputView output) noexcept {
    constexpr float two_pi = 6.2831853071795864769f;
    const RootSample root = sample_root(asset, params, strand);
    output.roots[strand] = root;
    const Vec3 tangent_axis = root.normal.z > -0.999f && root.normal.z < 0.999f
        ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 tangent = normalize(cross(root.normal, tangent_axis));
    const Vec3 bitangent = normalize(cross(root.normal, tangent));
    const float phase = two_pi * random01(params.seed, strand, 4u);
    const float angle = two_pi * random01(params.seed, strand, 5u);
    const Vec3 noise_dir = tangent * nxg_sin(angle) + bitangent * nxg_sin(angle + 1.57079632679f);

    for (std::uint32_t cv = 0; cv < params.cvs_per_strand; ++cv) {
        const float t = params.cvs_per_strand > 1u
            ? static_cast<float>(cv) / static_cast<float>(params.cvs_per_strand - 1u) : 0.0f;
        Vec3 p = root.position + interpolate_offset(asset, params, root, t) * params.length_scale;
        const float envelope = t * t;
        p = p + noise_dir * (params.noise_amplitude * envelope *
            nxg_sin(two_pi * params.noise_frequency * t + phase));
        const std::uint64_t index = static_cast<std::uint64_t>(strand) * params.cvs_per_strand + cv;
        output.points[index] = p;
        output.widths[index] = params.root_width * (1.0f - t) + params.tip_width * t;
    }
}

} // namespace nanoxgen
