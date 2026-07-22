#pragma once

#include "nanoxgen/device_view.h"

#if !defined(__CUDA_ARCH__) && !defined(__HIP_DEVICE_COMPILE__)
#include <cmath>
#endif

namespace nanoxgen {

NXG_HOST_DEVICE inline float nxg_sqrt(float x) noexcept {
#if defined(__CUDA_ARCH__) || defined(__HIP_DEVICE_COMPILE__)
    return sqrtf(x);
#else
    return std::sqrt(x);
#endif
}

NXG_HOST_DEVICE inline float nxg_sin(float x) noexcept {
#if defined(__CUDA_ARCH__) || defined(__HIP_DEVICE_COMPILE__)
    return sinf(x);
#else
    return std::sin(x);
#endif
}

NXG_HOST_DEVICE inline float nxg_cos(float x) noexcept {
#if defined(__CUDA_ARCH__) || defined(__HIP_DEVICE_COMPILE__)
    return cosf(x);
#else
    return std::cos(x);
#endif
}

NXG_HOST_DEVICE inline float nxg_acos(float x) noexcept {
#if defined(__CUDA_ARCH__) || defined(__HIP_DEVICE_COMPILE__)
    return acosf(x);
#else
    return std::acos(x);
#endif
}

NXG_HOST_DEVICE inline float nxg_floor(float x) noexcept {
#if defined(__CUDA_ARCH__) || defined(__HIP_DEVICE_COMPILE__)
    return floorf(x);
#else
    return std::floor(x);
#endif
}

NXG_HOST_DEVICE inline float nxg_abs(float x) noexcept { return x < 0.0f ? -x : x; }

NXG_HOST_DEVICE inline float nxg_pow(float x, float y) noexcept {
#if defined(__CUDA_ARCH__) || defined(__HIP_DEVICE_COMPILE__)
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
    Vec3 surface_u{};
    Vec3 noise_domain{};
    float original_length{};
    float effective_noise_frequency{};
    float preserve_scale{1.0f};
};

NXG_HOST_DEVICE inline bool noise_is_enabled(const GenerationParams &params) noexcept {
    return params.noise_amplitude != 0.0f && params.noise_mask != 0.0f;
}

NXG_HOST_DEVICE inline float strand_parameter(
    const GenerationParams &params, std::uint32_t cv) noexcept {
    return static_cast<float>(cv) / static_cast<float>(params.cvs_per_strand - 1u);
}

NXG_HOST_DEVICE inline Vec3 evaluate_base_strand_cv(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    const StrandGenerationState &state,
    std::uint32_t cv) noexcept {
    const float t = strand_parameter(params, cv);
    return state.root.position +
        interpolate_offset(asset, deformed, state.root, state.guide_blend, t) *
            params.length_scale;
}

NXG_HOST_DEVICE inline Vec3 fallback_surface_u(Vec3 normal) noexcept {
    const Vec3 axis = normal.z > -0.999f && normal.z < 0.999f
        ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
    return normalize(cross(normal, axis));
}

NXG_HOST_DEVICE inline Vec3 root_surface_u(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const RootSample &root) noexcept {
    const UInt3 tri = asset.triangles()[root.triangle_index];
    const Vec3 *positions = frame_positions(asset, deformed);
    Vec3 tangent{};
    if ((asset.header().flags & HasTexcoords) != 0u) {
        const Vec2 *uvs = asset.texcoords();
        const Vec3 edge1 = positions[tri.y] - positions[tri.x];
        const Vec3 edge2 = positions[tri.z] - positions[tri.x];
        const Vec2 duv1{uvs[tri.y].x - uvs[tri.x].x,
                        uvs[tri.y].y - uvs[tri.x].y};
        const Vec2 duv2{uvs[tri.z].x - uvs[tri.x].x,
                        uvs[tri.z].y - uvs[tri.x].y};
        const float determinant = duv1.x * duv2.y - duv1.y * duv2.x;
        if (nxg_abs(determinant) > 1.0e-12f) {
            tangent = (edge1 * duv2.y - edge2 * duv1.y) / determinant;
            tangent = tangent - root.normal * dot(tangent, root.normal);
        }
    }
    return length_squared(tangent) > 1.0e-20f
        ? normalize(tangent) : fallback_surface_u(root.normal);
}

NXG_HOST_DEVICE inline Vec3 rest_root_position(
    DeviceAssetView asset, const RootSample &root) noexcept {
    const UInt3 tri = asset.triangles()[root.triangle_index];
    const float b1 = root.barycentric.x;
    const float b2 = root.barycentric.y;
    const float b0 = 1.0f - b1 - b2;
    return asset.positions()[tri.x] * b0 + asset.positions()[tri.y] * b1 +
           asset.positions()[tri.z] * b2;
}

NXG_HOST_DEVICE inline std::uint32_t noise_hash(
    std::int32_t x, std::int32_t y, std::int32_t z) noexcept {
    std::uint32_t seed = 0u;
    seed = seed * 1664525u + static_cast<std::uint32_t>(x) + 1013904223u;
    seed = seed * 1664525u + static_cast<std::uint32_t>(y) + 1013904223u;
    seed = seed * 1664525u + static_cast<std::uint32_t>(z) + 1013904223u;
    seed ^= seed >> 11u;
    seed ^= (seed << 7u) & 0x9d2c5680u;
    seed ^= (seed << 15u) & 0xefc60000u;
    seed ^= seed >> 18u;
    return (((seed & 0x00ff0000u) >> 4u) + (seed & 0xffu)) & 0xffu;
}

NXG_HOST_DEVICE inline float noise_s_curve(float t) noexcept {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

NXG_HOST_DEVICE inline float gradient_noise(
    DeviceAssetView asset, Vec3 sample) noexcept {
    const float fx = nxg_floor(sample.x);
    const float fy = nxg_floor(sample.y);
    const float fz = nxg_floor(sample.z);
    const std::int32_t ix = static_cast<std::int32_t>(fx);
    const std::int32_t iy = static_cast<std::int32_t>(fy);
    const std::int32_t iz = static_cast<std::int32_t>(fz);
    const Vec3 weights{sample.x - fx, sample.y - fy, sample.z - fz};
    float values[8]{};
    for (std::uint32_t corner = 0u; corner < 8u; ++corner) {
        const std::int32_t ox = static_cast<std::int32_t>(corner & 1u);
        const std::int32_t oy = static_cast<std::int32_t>((corner >> 1u) & 1u);
        const std::int32_t oz = static_cast<std::int32_t>((corner >> 2u) & 1u);
        const Vec3 gradient = asset.noise_gradients()[noise_hash(ix + ox, iy + oy, iz + oz)];
        values[corner] = gradient.x * (weights.x - static_cast<float>(ox)) +
                         gradient.y * (weights.y - static_cast<float>(oy)) +
                         gradient.z * (weights.z - static_cast<float>(oz));
    }
    const float alphas[3] = {
        noise_s_curve(weights.x), noise_s_curve(weights.y), noise_s_curve(weights.z)};
    for (std::int32_t dimension = 2; dimension >= 0; --dimension) {
        const std::int32_t count = 1 << dimension;
        for (std::int32_t value = 0; value < count; ++value) {
            const std::int32_t index = value * (1 << (3 - dimension));
            const std::int32_t axis = 3 - dimension - 1;
            const std::int32_t other = index + (1 << axis);
            values[index] = (1.0f - alphas[axis]) * values[index] +
                            alphas[axis] * values[other];
        }
    }
    return 0.5f * values[0] + 0.5f;
}

NXG_HOST_DEVICE inline Vec3 rotate_by(Vec3 value, Vec3 axis, float angle) noexcept {
    const float cosine = nxg_cos(angle);
    const float sine = nxg_sin(angle);
    const float one_minus_cosine = 1.0f - cosine;
    return {
        dot(value, {axis.x * axis.x * one_minus_cosine + cosine,
                    axis.x * axis.y * one_minus_cosine - axis.z * sine,
                    axis.x * axis.z * one_minus_cosine + axis.y * sine}),
        dot(value, {axis.x * axis.y * one_minus_cosine + axis.z * sine,
                    axis.y * axis.y * one_minus_cosine + cosine,
                    axis.y * axis.z * one_minus_cosine - axis.x * sine}),
        dot(value, {axis.x * axis.z * one_minus_cosine - axis.y * sine,
                    axis.y * axis.z * one_minus_cosine + axis.x * sine,
                    axis.z * axis.z * one_minus_cosine + cosine})};
}

NXG_HOST_DEVICE inline float base_strand_length(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    const StrandGenerationState &state) noexcept {
    Vec3 previous = evaluate_base_strand_cv(asset, deformed, params, state, 0u);
    float length = 0.0f;
    for (std::uint32_t cv = 1u; cv < params.cvs_per_strand; ++cv) {
        const Vec3 current = evaluate_base_strand_cv(asset, deformed, params, state, cv);
        length += nxg_sqrt(length_squared(current - previous));
        previous = current;
    }
    return length;
}

struct StrandNoiseCursor {
    std::uint32_t cv{};
    Vec3 previous_base{};
    Vec3 current_base{};
    Vec3 next_base{};
    Vec3 tangent{};
    Vec3 transported_normal{};
    float arc_length{};
};

NXG_HOST_DEVICE inline StrandNoiseCursor make_noise_cursor(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    const StrandGenerationState &state) noexcept {
    StrandNoiseCursor cursor{};
    cursor.current_base = evaluate_base_strand_cv(asset, deformed, params, state, 0u);
    cursor.previous_base = cursor.current_base;
    cursor.next_base = evaluate_base_strand_cv(asset, deformed, params, state, 1u);
    cursor.tangent = state.root.normal;
    cursor.transported_normal = state.surface_u;
    return cursor;
}

NXG_HOST_DEVICE inline Vec3 evaluate_and_advance_noise_cursor(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    const StrandGenerationState &state,
    StrandNoiseCursor &cursor) noexcept {
    if (cursor.cv > 0u) {
        cursor.arc_length += nxg_sqrt(length_squared(
            cursor.current_base - cursor.previous_base));
    }

    Vec3 next_tangent = cursor.tangent;
    if (cursor.cv + 1u < params.cvs_per_strand) {
        const Vec3 segment = cursor.next_base - cursor.current_base;
        if (length_squared(segment) > 0.0f) { next_tangent = normalize(segment); }
    }
    const Vec3 axis = cross(cursor.tangent, next_tangent);
    if (length_squared(axis) > 0.0f) {
        const float angle = nxg_acos(
            nxg_clamp(dot(cursor.tangent, next_tangent), -1.0f, 1.0f));
        cursor.transported_normal = normalize(
            rotate_by(cursor.transported_normal, axis, angle));
    }
    const Vec3 normal = cursor.transported_normal;
    const Vec3 binormal = cross(normal, next_tangent);
    const Vec3 tangent = cross(binormal, normal);

    Vec3 point = cursor.current_base;
    if (cursor.cv > 0u) {
        const float distance = cursor.arc_length * state.effective_noise_frequency;
        const float magnitude = params.noise_mask * params.noise_amplitude *
                                strand_parameter(params, cursor.cv);
        const Vec3 local{
            (gradient_noise(asset, {state.noise_domain.x + distance,
                                    state.noise_domain.y,
                                    state.noise_domain.z}) - 0.5f) * magnitude,
            (gradient_noise(asset, {state.noise_domain.x,
                                    state.noise_domain.y + distance,
                                    state.noise_domain.z}) - 0.5f) * magnitude,
            (gradient_noise(asset, {state.noise_domain.x,
                                    state.noise_domain.y,
                                    state.noise_domain.z + distance}) - 0.5f) * magnitude};
        point = point + normal * local.x + binormal * local.y + tangent * local.z;
    }

    cursor.tangent = next_tangent;
    cursor.previous_base = cursor.current_base;
    if (cursor.cv + 1u < params.cvs_per_strand) {
        cursor.current_base = cursor.next_base;
        if (cursor.cv + 2u < params.cvs_per_strand) {
            cursor.next_base = evaluate_base_strand_cv(
                asset, deformed, params, state, cursor.cv + 2u);
        }
    }
    ++cursor.cv;
    return point;
}

NXG_HOST_DEVICE inline float noisy_strand_length(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    const StrandGenerationState &state) noexcept {
    StrandNoiseCursor cursor = make_noise_cursor(asset, deformed, params, state);
    Vec3 previous = evaluate_and_advance_noise_cursor(asset, deformed, params, state, cursor);
    float length = 0.0f;
    for (std::uint32_t cv = 1u; cv < params.cvs_per_strand; ++cv) {
        const Vec3 current = evaluate_and_advance_noise_cursor(
            asset, deformed, params, state, cursor);
        length += nxg_sqrt(length_squared(current - previous));
        previous = current;
    }
    return length;
}

NXG_HOST_DEVICE inline StrandGenerationState make_strand_generation_state(
    DeviceAssetView asset,
    DeviceDeformedGeometryView deformed,
    const GenerationParams &params,
    std::uint32_t strand) noexcept {
    StrandGenerationState state{};
    state.root = sample_root(asset, deformed, params, strand);
    state.guide_blend = make_guide_blend_state(asset, deformed, params, state.root);
    if (noise_is_enabled(params)) {
        state.surface_u = root_surface_u(asset, deformed, state.root);
        state.original_length = base_strand_length(asset, deformed, params, state);
        state.effective_noise_frequency = state.original_length > 0.0f
            ? nxg_max(0.5f / state.original_length, params.noise_frequency)
            : params.noise_frequency;
        const float decorrelation = 1.0f - params.noise_correlation;
        const float domain_scale = 100.0f * decorrelation * decorrelation;
        state.noise_domain =
            (rest_root_position(asset, state.root) +
             Vec3{0.419276f, 0.184247f, 0.805721f}) * domain_scale;
        if (params.noise_preserve_length > 0.001f) {
            const float noisy_length = noisy_strand_length(asset, deformed, params, state);
            if (noisy_length > 0.0f) {
                const float target_length =
                    state.original_length * params.noise_preserve_length +
                    noisy_length * (1.0f - params.noise_preserve_length);
                if (nxg_abs(noisy_length - target_length) >= 0.0001f) {
                    state.preserve_scale = target_length / noisy_length;
                }
            }
        }
    }
    return state;
}

NXG_HOST_DEVICE inline float evaluate_strand_width(
    const GenerationParams &params, std::uint32_t cv) noexcept {
    const float t = strand_parameter(params, cv);
    return params.root_width * (1.0f - t) + params.tip_width * t;
}

NXG_HOST_DEVICE inline void generate_strand(
    DeviceAssetView asset,
    const GenerationParams &params,
    std::uint32_t strand,
    GeneratedOutputView output,
    DeviceDeformedGeometryView deformed = {}) noexcept {
    const StrandGenerationState state = make_strand_generation_state(
        asset, deformed, params, strand);
    const bool noise_enabled = noise_is_enabled(params);
    if (output.roots) { output.roots[strand] = state.root; }
    StrandNoiseCursor cursor{};
    if (noise_enabled) {
        cursor = make_noise_cursor(asset, deformed, params, state);
    }
    for (std::uint32_t cv = 0; cv < params.cvs_per_strand; ++cv) {
        Vec3 point = noise_enabled
            ? evaluate_and_advance_noise_cursor(asset, deformed, params, state, cursor)
            : evaluate_base_strand_cv(asset, deformed, params, state, cv);
        if (state.preserve_scale != 1.0f) {
            point = state.root.position +
                    (point - state.root.position) * state.preserve_scale;
        }
        const float width = evaluate_strand_width(params, cv);
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
    const bool noise_enabled = noise_is_enabled(params);
    if (output.roots) { output.roots[strand] = state.root; }
    if (output.root_uvs) { output.root_uvs[strand] = state.root.uv; }
    if (output.point_counts) { output.point_counts[strand] = params.cvs_per_strand; }
    StrandNoiseCursor cursor{};
    if (noise_enabled) {
        cursor = make_noise_cursor(asset, deformed, params, state);
    }
    for (std::uint32_t cv = 0u; cv < params.cvs_per_strand; ++cv) {
        Vec3 point = noise_enabled
            ? evaluate_and_advance_noise_cursor(asset, deformed, params, state, cursor)
            : evaluate_base_strand_cv(asset, deformed, params, state, cv);
        if (state.preserve_scale != 1.0f) {
            point = state.root.position +
                    (point - state.root.position) * state.preserve_scale;
        }
        const float width = evaluate_strand_width(params, cv);
        const std::uint64_t index = static_cast<std::uint64_t>(strand) *
                                    params.cvs_per_strand + cv;
        output.points[index] = {point.x, point.y, point.z,
                                0.5f * width * output.radius_scale};
    }
}

} // namespace nanoxgen
