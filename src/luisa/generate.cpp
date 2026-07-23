#include "nanoxgen/luisa/generate.h"

#include "packed_io.h"

#include <luisa/core/stl/vector.h>
#include <luisa/dsl/sugar.h>

#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace nanoxgen::luisa_backend {

using namespace luisa;
using namespace luisa::compute;
using ::operator+;
using ::operator-;
using ::operator*;
using ::operator/;
using ::operator!=;

namespace {

static_assert(sizeof(Vec3) == 12u);
static_assert(sizeof(UInt3) == 12u);
static_assert(sizeof(GuideRecord) == 56u);
static_assert(offsetof(GuideRecord, first_cv) == 0u);
static_assert(offsetof(GuideRecord, triangle_index) == 8u);
static_assert(offsetof(GuideRecord, support_radius) == 12u);
static_assert(offsetof(GuideRecord, root_position) == 32u);
static_assert(offsetof(GuideRecord, root_normal) == 44u);
static_assert(sizeof(RootSample) == 48u);
static_assert(offsetof(RootSample, position) == 0u);
static_assert(offsetof(RootSample, normal) == 12u);
static_assert(offsetof(RootSample, triangle_index) == 32u);

} // namespace

PackedGenerateFromRootsKernel make_packed_generate_from_roots_kernel(
    const Asset &asset, const GenerationParams &params, float radius_scale) {
    const std::string validation = validate_asset(asset.bytes());
    if (!validation.empty()) {
        throw std::invalid_argument("Classic Luisa asset is invalid: " + validation);
    }
    const AssetHeader header = asset.view().header();
    if (header.guide_count == 0u ||
        header.guide_stencil_size != kGuideStencilSize ||
        params.strand_count == 0u || params.cvs_per_strand < 2u ||
        !std::isfinite(params.guide_support_scale) ||
        params.guide_support_scale < 0.0f ||
        !std::isfinite(params.guide_weight_power) ||
        !std::isfinite(params.normal_rejection_cos) ||
        !std::isfinite(params.length_scale) || params.length_scale < 0.0f ||
        !std::isfinite(params.root_width) || params.root_width < 0.0f ||
        !std::isfinite(params.tip_width) || params.tip_width < 0.0f ||
        !std::isfinite(radius_scale) || radius_scale < 0.0f) {
        throw std::invalid_argument("Classic Luisa generation arguments are invalid");
    }
    if (params.noise_amplitude != 0.0f && params.noise_mask != 0.0f) {
        throw std::invalid_argument(
            "Classic Luisa base generation does not accept generic noise; "
            "lower the authored NoiseFX plan instead");
    }

    return Kernel1D{[=](ByteBufferVar asset_bytes, ByteBufferVar roots,
                        BufferFloat4 points) noexcept {
        set_block_size(128u, 1u, 1u);
        const UInt strand = dispatch_id().x;
        const UInt root_offset = strand * static_cast<uint>(sizeof(RootSample));
        const Float3 root_position = packed_io::read_packed_float3(
            roots,
            root_offset + static_cast<uint>(offsetof(RootSample, position)));
        const Float3 root_normal = packed_io::read_packed_float3(
            roots,
            root_offset + static_cast<uint>(offsetof(RootSample, normal)));
        const UInt triangle = roots.read<uint>(
            root_offset +
            static_cast<uint>(offsetof(RootSample, triangle_index)));

        vector<Expr<uint>> guide_indices;
        vector<Expr<float>> guide_weights;
        guide_indices.reserve(kGuideStencilSize);
        guide_weights.reserve(kGuideStencilSize);
        Float weight_sum{0.0f};
        Float nearest_distance{3.402823466e+38f};
        UInt nearest{kInvalidIndex};
        for (std::uint32_t slot = 0u; slot < kGuideStencilSize; ++slot) {
            const UInt stencil_offset =
                static_cast<uint>(header.triangle_guides_offset) +
                (triangle * kGuideStencilSize + slot) *
                    static_cast<uint>(sizeof(std::uint32_t));
            const UInt candidate = asset_bytes.read<uint>(stencil_offset);
            const Bool valid = candidate != kInvalidIndex;
            const UInt safe = ite(valid, candidate, 0u);
            const UInt guide_offset =
                static_cast<uint>(header.guides_offset) +
                safe * static_cast<uint>(sizeof(GuideRecord));
            const Float3 guide_root = packed_io::read_packed_float3(
                asset_bytes,
                guide_offset +
                static_cast<uint>(offsetof(GuideRecord, root_position)));
            const Float3 guide_normal = packed_io::read_packed_float3(
                asset_bytes,
                guide_offset +
                static_cast<uint>(offsetof(GuideRecord, root_normal)));
            const Float3 delta = root_position - guide_root;
            const Float distance_squared = dot(delta, delta);
            const Bool normal_ok = dot(root_normal, guide_normal) >=
                                   params.normal_rejection_cos;
            const Bool usable = valid & normal_ok;
            const Bool nearer = usable & (distance_squared < nearest_distance);
            nearest = ite(nearer, safe, nearest);
            nearest_distance = ite(
                nearer, distance_squared, nearest_distance);
            const Float radius = asset_bytes.read<float>(
                guide_offset +
                static_cast<uint>(offsetof(GuideRecord, support_radius))) *
                params.guide_support_scale;
            const Bool inside = usable & (radius > 0.0f) &
                                (distance_squared < radius * radius);
            const Float weight = ite(
                inside,
                pow(1.0f - sqrt(distance_squared) / max(radius, 1.0e-20f),
                    max(params.guide_weight_power, 0.0f)),
                0.0f);
            guide_indices.emplace_back(safe);
            guide_weights.emplace_back(weight);
            weight_sum += weight;
        }

        const auto sample_guide = [&](Expr<uint> guide_index,
                                      Expr<float> t) noexcept {
            const UInt guide_offset =
                static_cast<uint>(header.guides_offset) +
                guide_index * static_cast<uint>(sizeof(GuideRecord));
            const UInt first_cv = asset_bytes.read<uint>(
                guide_offset +
                static_cast<uint>(offsetof(GuideRecord, first_cv)));
            const UInt count_and_flags = asset_bytes.read<uint>(
                guide_offset + 4u);
            const UInt cv_count = count_and_flags & 0xffffu;
            const Float x = clamp(t, 0.0f, 1.0f) *
                            cast<float>(cv_count - 1u);
            const UInt lower = min(cast<uint>(x), cv_count - 2u);
            const Float fraction = x - cast<float>(lower);
            const UInt cv_offset =
                static_cast<uint>(header.guide_cvs_offset) +
                (first_cv + lower) * static_cast<uint>(sizeof(Vec3));
            const Float3 a =
                packed_io::read_packed_float3(asset_bytes, cv_offset);
            const Float3 b = packed_io::read_packed_float3(
                asset_bytes,
                cv_offset + static_cast<uint>(sizeof(Vec3)));
            return lerp(a, b, fraction);
        };

        const UInt safe_nearest = ite(nearest != kInvalidIndex, nearest, 0u);
        const UInt nearest_offset =
            static_cast<uint>(header.guides_offset) +
            safe_nearest * static_cast<uint>(sizeof(GuideRecord));
        const Float3 nearest_root = packed_io::read_packed_float3(
            asset_bytes,
            nearest_offset +
            static_cast<uint>(offsetof(GuideRecord, root_position)));
        for (std::uint32_t cv = 0u; cv < params.cvs_per_strand; ++cv) {
            const float t_constant = static_cast<float>(cv) /
                                     static_cast<float>(params.cvs_per_strand - 1u);
            const Float t = t_constant;
            Float3 offset = make_float3(0.0f);
            for (std::uint32_t slot = 0u; slot < kGuideStencilSize; ++slot) {
                const UInt guide = guide_indices[slot];
                const UInt guide_offset =
                    static_cast<uint>(header.guides_offset) +
                    guide * static_cast<uint>(sizeof(GuideRecord));
                const Float3 guide_root = packed_io::read_packed_float3(
                    asset_bytes, guide_offset + static_cast<uint>(
                        offsetof(GuideRecord, root_position)));
                offset += (sample_guide(guide, t) - guide_root) *
                          guide_weights[slot];
            }
            const Float3 nearest_value =
                sample_guide(safe_nearest, t) - nearest_root;
            offset = ite(
                weight_sum > 1.0e-12f, offset / weight_sum,
                ite(nearest != kInvalidIndex, nearest_value, root_normal * t));
            const Float3 position =
                root_position + offset * params.length_scale;
            const float width = params.root_width * (1.0f - t_constant) +
                                params.tip_width * t_constant;
            points.write(
                strand * params.cvs_per_strand + cv,
                make_float4(position, 0.5f * width * radius_scale));
        }
    }};
}

ClassicBaseGenerateKernel make_classic_base_generate_kernel(
    std::uint32_t cvs_per_strand, float diameter, float radius_scale,
    bool root_relative) {
    if (cvs_per_strand < 3u || !std::isfinite(diameter) || diameter < 0.0f ||
        !std::isfinite(radius_scale) || radius_scale < 0.0f) {
        throw std::invalid_argument(
            "Classic Luisa base generation arguments are invalid");
    }
    static_assert(sizeof(ClassicGuideInfluence) == 8u);
    static_assert(offsetof(ClassicGuideInfluence, guide_index) == 0u);
    static_assert(offsetof(ClassicGuideInfluence, weight) == 4u);
    return Kernel1D{[=](ByteBufferVar roots, BufferUInt influence_offsets,
                        ByteBufferVar influences,
                        BufferFloat3 rebuilt_guides,
                        BufferFloat4 points) noexcept {
        set_block_size(128u, 1u, 1u);
        const UInt strand = dispatch_id().x;
        const UInt root_offset = strand * static_cast<uint>(sizeof(RootSample));
        const Float3 root_position = packed_io::read_packed_float3(
            roots,
            root_offset + static_cast<uint>(offsetof(RootSample, position)));
        const UInt begin = influence_offsets.read(strand);
        const UInt end = influence_offsets.read(strand + 1u);
        Float weight_sum{0.0f};
        UInt influence = begin;
        $while (influence < end) {
            const UInt byte_offset = influence *
                static_cast<uint>(sizeof(ClassicGuideInfluence));
            weight_sum += influences.read<float>(byte_offset + 4u);
            influence += 1u;
        };
        const float radius = 0.5f * diameter * radius_scale;
        // Keep the CV loop in device control flow. Host-side unrolling makes
        // first-use compilation scale with the authored CV count, while the
        // extra loop branch is negligible next to guide interpolation.
        $for (cv, 0u, cvs_per_strand) {
            Float3 offset = make_float3(0.0f);
            influence = begin;
            $while (influence < end) {
                const UInt byte_offset = influence *
                    static_cast<uint>(sizeof(ClassicGuideInfluence));
                const UInt guide = influences.read<uint>(byte_offset);
                const Float weight = influences.read<float>(byte_offset + 4u);
                const UInt first = guide * cvs_per_strand;
                offset += (rebuilt_guides.read(first + cv) -
                           rebuilt_guides.read(first)) * weight;
                influence += 1u;
            };
            const Float3 local = offset / max(weight_sum, 1.0e-20f);
            const Float3 position =
                root_relative ? local : root_position + local;
            points.write(strand * cvs_per_strand + cv,
                         make_float4(position, radius));
        };
    }};
}

} // namespace nanoxgen::luisa_backend
