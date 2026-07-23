#include "nanoxgen/generate.h"
#include "nanoxgen/luisa/generate.h"
#include "nanoxgen/luisa/xgen_classic_collection.h"
#include "nanoxgen/luisa/xgen_classic_runtime.h"
#include "nanoxgen/luisa/xgen_expression.h"
#include "nanoxgen/xgen_classic_runtime.h"
#include "nanoxgen/xgen_expression.h"

#include <luisa/core/logging.h>
#include <luisa/core/stl/vector.h>
#include <luisa/dsl/syntax.h>
#include <luisa/runtime/buffer.h>
#include <luisa/runtime/context.h>
#include <luisa/runtime/device.h>
#include <luisa/runtime/stream.h>

#include <chrono>
#include <bit>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace luisa;
using namespace luisa::compute;

namespace {

float milliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<float, std::milli>(duration).count();
}

void test_external_device_collection_compile(
    Device &device, Stream &stream) {
    std::array<nanoxgen::ClassicFloatRuntimePlan, 2u> plans;
    for (std::size_t index = 0u; index < plans.size(); ++index) {
        nanoxgen::ClassicDescription description{};
        description.name = "collection_" + std::to_string(index);
        description.objects.push_back({"SplinePrimitive", {
            {"fxCVCount", "4", 1u},
            {"width", index == 0u ? "0.1" : "0.2", 2u}}, 1u});
        plans[index] =
            nanoxgen::compile_xgen_classic_float_runtime_plan(description);
    }
    std::array<nanoxgen::luisa_backend::ClassicCollectionCompileInput, 2u>
        inputs{{
            {&plans[0], 4u, {}, true},
            {&plans[1], 4u, {}, true},
    }};
    nanoxgen::NanoXGenContext context{3u};
    nanoxgen::luisa_backend::ClassicCollectionCompileOptions options{};
    options.context = &context;
    auto pipeline = nanoxgen::luisa_backend::compile_classic_collection(
        device, inputs, options);
    if (pipeline.description_count() != 2u ||
        pipeline.description_name(0u) != "collection_0" ||
        pipeline.description_name(1u) != "collection_1" ||
        pipeline.compile_stats().kernel_count != 6u ||
        pipeline.compile_stats().context_worker_count != 3u ||
        pipeline.compile_stats().worker_limit != 3u ||
        pipeline.output_is_points_a(0u)) {
        throw std::runtime_error(
            "external-device Classic collection pipeline is invalid");
    }

    const std::array<nanoxgen::RootSample, 1u> roots{{
        {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.25f, 0.75f},
         0u, {0.0f, 0.0f}, 0u},
    }};
    const std::array<std::uint32_t, 2u> offsets{{0u, 1u}};
    const std::array<nanoxgen::ClassicGuideInfluence, 1u> influences{{
        {0u, 1.0f},
    }};
    const std::array<luisa::float3, 4u> guides{{
        {0.0f, 0.0f, 0.0f}, {0.0f, 0.5f, 0.0f},
        {0.0f, 1.0f, 0.0f}, {0.0f, 1.5f, 0.0f},
    }};
    const std::array<std::uint32_t, 2u> root_runtime{{0u, 0u}};
    const std::array<float, 1u> runtime_inputs{{0.0f}};
    const std::array<luisa::float3, 1u> vector_inputs{{
        {1.0f, 0.0f, 0.0f},
    }};
    auto root_buffer = device.create_byte_buffer(sizeof(roots));
    auto offset_buffer =
        device.create_buffer<std::uint32_t>(offsets.size());
    auto influence_buffer = device.create_byte_buffer(sizeof(influences));
    auto guide_buffer = device.create_buffer<luisa::float3>(guides.size());
    auto root_runtime_buffer =
        device.create_buffer<std::uint32_t>(root_runtime.size());
    auto runtime_input_buffer =
        device.create_buffer<float>(runtime_inputs.size());
    auto tangent_buffer =
        device.create_buffer<luisa::float3>(vector_inputs.size());
    auto noise_domain_buffer =
        device.create_buffer<luisa::float3>(vector_inputs.size());
    auto points_a = device.create_buffer<luisa::float4>(4u);
    auto points_b = device.create_buffer<luisa::float4>(4u);
    auto states = device.create_buffer<luisa::float4>(1u);
    const nanoxgen::luisa_backend::ClassicCollectionDispatchResources
        resources{
            root_buffer.view(), offset_buffer.view(),
            influence_buffer.view(), guide_buffer.view(),
            root_runtime_buffer.view(), runtime_input_buffer.view(),
            tangent_buffer.view(), noise_domain_buffer.view(),
            points_a.view(), points_b.view(), states.view(),
            {}, {}, {}, {}};
    std::array<luisa::float4, 4u> output{};
    stream << root_buffer.copy_from(roots.data())
           << offset_buffer.copy_from(offsets.data())
           << influence_buffer.copy_from(influences.data())
           << guide_buffer.copy_from(guides.data())
           << root_runtime_buffer.copy_from(root_runtime.data())
           << runtime_input_buffer.copy_from(runtime_inputs.data())
           << tangent_buffer.copy_from(vector_inputs.data())
           << noise_domain_buffer.copy_from(vector_inputs.data());
    pipeline.encode(stream, 0u, resources, 1u);
    stream << points_b.copy_to(luisa::span{output}) << synchronize();
    for (std::size_t cv = 0u; cv < output.size(); ++cv) {
        if (output[cv].x != 0.0f ||
            output[cv].y != guides[cv].y ||
            output[cv].z != 0.0f ||
            output[cv].w != 0.05f) {
            throw std::runtime_error(
                "external-device Classic collection encode mismatch");
        }
    }
}

nanoxgen::Asset make_luisa_generation_asset() {
    nanoxgen::AssetBuildInput input{};
    input.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                       {0.0f, 0.0f, 1.0f}, {1.0f, 0.1f, 1.0f}};
    input.normals.assign(input.positions.size(), {0.0f, 1.0f, 0.0f});
    input.texcoords = {{0.0f, 0.0f}, {1.0f, 0.0f},
                       {0.0f, 1.0f}, {1.0f, 1.0f}};
    input.triangles = {{0u, 1u, 2u}, {1u, 3u, 2u}};
    nanoxgen::GuideInput a{};
    a.triangle_index = 0u;
    a.barycentric = {0.2f, 0.3f};
    a.root_normal = {0.0f, 1.0f, 0.0f};
    a.support_radius = 10.0f;
    a.cvs = {{0.2f, 0.0f, 0.3f}, {0.18f, 0.3f, 0.33f},
             {0.25f, 0.7f, 0.38f}, {0.35f, 1.1f, 0.45f}};
    nanoxgen::GuideInput b{};
    b.triangle_index = 1u;
    b.barycentric = {0.35f, 0.25f};
    b.root_normal = {0.0f, 1.0f, 0.0f};
    b.support_radius = 10.0f;
    b.cvs = {{0.75f, 0.05f, 0.6f}, {0.82f, 0.4f, 0.57f},
             {0.78f, 0.78f, 0.68f}, {0.68f, 1.2f, 0.76f}};
    input.guides = {std::move(a), std::move(b)};
    return nanoxgen::build_asset(input);
}

float test_luisa_generation(Device &device, Stream &stream) {
    constexpr std::uint32_t strand_count = 1024u;
    constexpr std::uint32_t cvs_per_strand = 8u;
    const nanoxgen::Asset asset = make_luisa_generation_asset();
    nanoxgen::GenerationParams params{};
    params.strand_count = strand_count;
    params.cvs_per_strand = cvs_per_strand;
    params.seed = 0x1234abcdu;
    params.root_width = 0.03f;
    params.tip_width = 0.002f;
    const nanoxgen::PackedGeneratedCurves reference =
        nanoxgen::generate_packed_cpu(asset, params, 1.0f, {1u, 128u});
    auto asset_buffer = device.create_byte_buffer(asset.bytes().size());
    auto root_buffer = device.create_byte_buffer(
        reference.roots.size() * sizeof(nanoxgen::RootSample));
    auto point_buffer =
        device.create_buffer<luisa::float4>(reference.points.size());
    auto shader = device.compile(
        nanoxgen::luisa_backend::make_packed_generate_from_roots_kernel(
            asset, params));
    std::vector<luisa::float4> actual(reference.points.size());
    stream << asset_buffer.copy_from(asset.bytes().data())
           << root_buffer.copy_from(reference.roots.data())
           << shader(asset_buffer, root_buffer, point_buffer)
                  .dispatch(strand_count)
           << point_buffer.copy_to(luisa::span{actual})
           << synchronize();
    float max_error = 0.0f;
    for (std::size_t index = 0u; index < actual.size(); ++index) {
        max_error = std::max({
            max_error,
            std::abs(actual[index].x - reference.points[index].x),
            std::abs(actual[index].y - reference.points[index].y),
            std::abs(actual[index].z - reference.points[index].z),
            std::abs(actual[index].w - reference.points[index].radius)});
    }
    if (max_error > 2.0e-5f) {
        throw std::runtime_error(
            "Luisa backend-neutral generation mismatch (max=" +
            std::to_string(max_error) + ")");
    }
    return max_error;
}

void test_luisa_classic_primitive_culling(Device &device, Stream &stream) {
    constexpr std::uint32_t strand_count = 2u;
    constexpr std::uint32_t cvs = 2u;
    nanoxgen::ClassicDescription description{};
    description.name = "luisaPrimitiveCulling";
    description.objects.push_back({"SplinePrimitive", {
        {"fxCVCount", "2", 1u},
        {"width", "$id == 1 ? 0.0001 : 0.000099", 2u}}, 1u});
    const nanoxgen::ClassicFloatRuntimePlan plan =
        nanoxgen::compile_xgen_classic_float_runtime_plan(description);
    if (!plan.lowering_complete()) {
        throw std::runtime_error(
            "Luisa Classic primitive-culling fixture did not lower");
    }

    const std::array<nanoxgen::RootSample, strand_count> root_samples{};
    const std::array<std::uint32_t, strand_count * 2u> root_runtime{
        1u, 0u, 2u, 0u};
    const std::array<float, 1u> ptex_values{};
    const std::array<luisa::float4, strand_count * cvs> source{
        luisa::make_float4(0.0f, 0.0f, 0.0f, 0.1f),
        luisa::make_float4(0.0f, 1.0f, 0.0f, 0.1f),
        luisa::make_float4(1.0f, 0.0f, 0.0f, 0.1f),
        luisa::make_float4(1.0f, 1.0f, 0.0f, 0.1f)};
    std::array<luisa::float4, strand_count> states{};
    auto roots = device.create_byte_buffer(
        root_samples.size() * sizeof(nanoxgen::RootSample));
    auto runtime = device.create_buffer<std::uint32_t>(root_runtime.size());
    auto ptex = device.create_buffer<float>(ptex_values.size());
    auto a = device.create_buffer<luisa::float4>(source.size());
    auto b = device.create_buffer<luisa::float4>(source.size());
    auto state_buffer = device.create_buffer<luisa::float4>(states.size());
    auto primitive = device.compile(
        nanoxgen::luisa_backend::make_classic_runtime_primitive_kernel(
            plan, cvs));
    stream << roots.copy_from(root_samples.data())
           << runtime.copy_from(root_runtime.data())
           << ptex.copy_from(ptex_values.data())
           << a.copy_from(source.data())
           << primitive(a, b, roots, runtime, ptex, state_buffer)
                  .dispatch(strand_count)
           << state_buffer.copy_to(luisa::span{states})
           << synchronize();
    if (states[0u].x < 0.0f || states[1u].x >= 0.0f ||
        states[0u].y != 0.0001f || states[1u].y != 0.000099f) {
        throw std::runtime_error(
            "Luisa Classic primitive width culling does not match XGen");
    }
}

float test_luisa_classic_effects(Device &device, Stream &stream) {
    constexpr std::uint32_t strand_count = 64u;
    constexpr std::uint32_t cvs = 7u;
    nanoxgen::ClassicDescription description{};
    description.name = "luisaEffects";
    description.attributes.push_back({"descriptionId", "9", 1u});
    description.objects.push_back({"SplinePrimitive", {
        {"fxCVCount", "7", 2u},
        {"length", "0.9+0.2*map('fixture')", 3u},
        {"width", "0.02+hash($id)*0.01", 4u},
        {"taper", "0.25", 5u}, {"taperStart", "0.35", 6u},
        {"widthRamp", "rampUI(0,1,1:1,0.8,1)", 7u}}, 2u});
    description.objects.push_back({"ClumpingFXModule", {
        {"active", "true", 8u}, {"name", "clump", 9u},
        {"mask", "0.75", 10u}, {"clump", "0.6", 11u},
        {"clumpScale", "rampUI(0,0.5,1:1,0,1)", 12u},
        {"clumpVariance", "0", 13u}, {"cut", "0", 14u},
        {"copy", "0", 15u}, {"copyVariance", "0", 16u},
        {"curl", "0", 17u}, {"offset", "0", 18u},
        {"flatness", "0", 19u}, {"frame", "0", 20u},
        {"noise", "0.02", 21u},
        {"noiseScale", "rampUI(0,0,1:1,1,1)", 22u},
        {"noiseFrequency", "1.7", 23u},
        {"noiseCorrelation", "20", 24u},
        {"useControlMaps", "0", 25u},
        {"clumpVolumize", "false", 26u}}, 8u});
    description.objects.push_back({"NoiseFXModule", {
        {"active", "true", 27u}, {"name", "noise", 28u},
        {"mask", "0.8", 29u}, {"magnitude", "0.035", 30u},
        {"magnitudeScale", "rampUI(0,0,1:1,1,1)", 31u},
        {"frequency", "2.25", 32u}, {"correlation", "rand(0,100)", 33u},
        {"preserveLength", "35", 34u}, {"mode", "0", 35u}}, 27u});
    description.objects.push_back({"CutFXModule", {
        {"active", "true", 36u}, {"name", "cut", 37u},
        {"amount", "0.05*$cLength", 38u},
        {"rebuildType", "1", 39u}}, 36u});
    const nanoxgen::ClassicFloatRuntimePlan plan =
        nanoxgen::compile_xgen_classic_float_runtime_plan(description);
    if (!plan.lowering_complete() || plan.clumps.size() != 1u ||
        plan.noises.size() != 1u || plan.cuts.size() != 1u ||
        plan.effects.size() != 3u) {
        throw std::runtime_error("Luisa Classic effects fixture did not lower");
    }

    nanoxgen::PackedGeneratedCurves reference{};
    reference.strand_count = strand_count;
    reference.cvs_per_strand = cvs;
    reference.point_counts.assign(strand_count, cvs);
    reference.points.resize(static_cast<std::size_t>(strand_count) * cvs);
    reference.roots.resize(strand_count);
    reference.root_uvs.resize(strand_count);
    std::vector<luisa::float4> source(reference.points.size());
    std::vector<luisa::float3> tangents(strand_count);
    std::vector<luisa::float3> noise_domains(strand_count);
    std::vector<nanoxgen::Vec3> cpu_tangents(strand_count);
    std::vector<nanoxgen::Vec3> cpu_noise_domains(strand_count);
    std::vector<std::uint32_t> primitive_ids(strand_count);
    std::vector<std::uint32_t> prefixes(strand_count);
    std::vector<std::uint32_t> root_runtime(strand_count * 2u);
    std::vector<float> ptex_values(strand_count);
    for (std::uint32_t strand = 0u; strand < strand_count; ++strand) {
        const float root_x = static_cast<float>(strand % 8u) * 0.07f;
        const float root_z = static_cast<float>(strand / 8u) * 0.06f;
        auto &root = reference.roots[strand];
        root.position = {root_x, 0.0f, root_z};
        root.normal = {0.0f, 1.0f, 0.0f};
        root.uv = {static_cast<float>(strand % 11u) / 11.0f,
                   static_cast<float>(strand % 13u) / 13.0f};
        root.surface_face_id = strand % 5u;
        reference.root_uvs[strand] = root.uv;
        cpu_tangents[strand] = {1.0f, 0.0f, 0.0f};
        tangents[strand] = luisa::make_float3(1.0f, 0.0f, 0.0f);
        cpu_noise_domains[strand] = root.position;
        noise_domains[strand] =
            luisa::make_float3(root.position.x, root.position.y,
                               root.position.z);
        primitive_ids[strand] = strand + 1u;
        const std::array<double, 3u> prefix_input{
            static_cast<double>(root.uv.x), static_cast<double>(root.uv.y),
            static_cast<double>(nanoxgen::xgen_runtime_face_seed(
                plan.description_id, plan.description_name,
                root.surface_face_id))};
        prefixes[strand] = nanoxgen::xgen_seexpr_hash_prefix(prefix_input);
        root_runtime[strand * 2u] = primitive_ids[strand];
        root_runtime[strand * 2u + 1u] = prefixes[strand];
        ptex_values[strand] = static_cast<float>(strand % 7u) / 6.0f;
        for (std::uint32_t cv = 0u; cv < cvs; ++cv) {
            const float t = static_cast<float>(cv) /
                            static_cast<float>(cvs - 1u);
            const nanoxgen::PackedCurvePoint point{
                root_x + 0.03f * t * t,
                0.6f * t,
                root_z + 0.02f * std::sin(t * 2.3f),
                0.0f};
            const std::size_t index =
                static_cast<std::size_t>(strand) * cvs + cv;
            reference.points[index] = point;
            source[index] = luisa::make_float4(
                point.x, point.y, point.z, point.radius);
        }
    }
    nanoxgen::ClassicClumpRuntimeData clump_data{};
    clump_data.module_name = "clump";
    clump_data.cvs_per_guide = cvs;
    for (std::uint32_t cv = 0u; cv < cvs; ++cv) {
        const float t = static_cast<float>(cv) /
                        static_cast<float>(cvs - 1u);
        clump_data.guide_axes.push_back({
            0.12f + 0.025f * t * t,
            0.72f * t,
            0.08f + 0.018f * std::sin(t * 1.9f)});
    }
    clump_data.guide_normals = {{0.0f, 1.0f, 0.0f}};
    clump_data.guide_tangents = {{1.0f, 0.0f, 0.0f}};
    clump_data.guide_reference_positions = {clump_data.guide_axes.front()};
    clump_data.guide_uvs = {{0.25f, 0.75f}};
    clump_data.guide_face_ids = {2u};
    const std::array<double, 3u> guide_prefix_input{
        0.25, 0.75,
        static_cast<double>(nanoxgen::xgen_runtime_face_seed(
            plan.description_id, plan.description_name, 2u))};
    clump_data.guide_random_prefixes = {
        nanoxgen::xgen_seexpr_hash_prefix(guide_prefix_input)};
    clump_data.strand_guide_indices.assign(strand_count, 0u);
    clump_data.strand_guide_indices.back() = nanoxgen::kInvalidIndex;
    nanoxgen::prepare_xgen_classic_clump_runtime_data(clump_data);
    const std::array clump_bindings{clump_data};
    nanoxgen::apply_xgen_classic_float_runtime_plan_cpu(
        reference, plan, 1.0f, cpu_tangents, prefixes, primitive_ids,
        clump_bindings, ptex_values, cpu_noise_domains);
    if (reference.strand_count != strand_count) {
        throw std::runtime_error("Luisa Classic effects fixture unexpectedly culled");
    }

    auto roots = device.create_byte_buffer(
        reference.roots.size() * sizeof(nanoxgen::RootSample));
    auto runtime = device.create_buffer<std::uint32_t>(root_runtime.size());
    auto ptex = device.create_buffer<float>(ptex_values.size());
    auto tangent_buffer = device.create_buffer<luisa::float3>(tangents.size());
    auto noise_domain_buffer =
        device.create_buffer<luisa::float3>(noise_domains.size());
    std::vector<luisa::float4> clump_axes;
    float clump_distance = 0.0f;
    for (std::size_t cv = 0u;
         cv < clump_data.guide_render_axes.size(); ++cv) {
        if (cv != 0u) {
            const nanoxgen::Vec3 delta =
                clump_data.guide_axes[cv] -
                clump_data.guide_axes[cv - 1u];
            clump_distance += std::sqrt(
                delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        }
        const nanoxgen::Vec3 value = clump_data.guide_render_axes[cv];
        clump_axes.emplace_back(
            value.x, value.y, value.z, clump_distance);
    }
    const nanoxgen::Vec3 clump_normal = clump_data.guide_normals.front();
    const nanoxgen::Vec3 clump_tangent = clump_data.guide_tangents.front();
    const nanoxgen::Vec2 clump_uv = clump_data.guide_uvs.front();
    const std::array clump_frames{
        luisa::make_float4(
            clump_normal.x, clump_normal.y, clump_normal.z, clump_uv.x),
        luisa::make_float4(
            clump_tangent.x, clump_tangent.y, clump_tangent.z, clump_uv.y),
        luisa::make_float4(
            clump_data.guide_reference_positions.front().x,
            clump_data.guide_reference_positions.front().y,
            clump_data.guide_reference_positions.front().z,
            clump_data.guide_spline_lengths.front())};
    const std::array<std::uint32_t, 2u> clump_runtime{
        clump_data.guide_face_ids.front(),
        clump_data.guide_random_prefixes.front()};
    auto clump_axes_buffer =
        device.create_buffer<luisa::float4>(clump_axes.size());
    auto clump_frames_buffer =
        device.create_buffer<luisa::float4>(clump_frames.size());
    auto clump_runtime_buffer =
        device.create_buffer<std::uint32_t>(clump_runtime.size());
    auto clump_guides_buffer = device.create_buffer<std::uint32_t>(
        clump_data.strand_guide_indices.size());
    auto a = device.create_buffer<luisa::float4>(source.size());
    auto b = device.create_buffer<luisa::float4>(source.size());
    auto states = device.create_buffer<luisa::float4>(strand_count);
    auto primitive = device.compile(
        nanoxgen::luisa_backend::make_classic_runtime_primitive_kernel(
            plan, cvs));
    auto clump = device.compile(
        nanoxgen::luisa_backend::make_classic_runtime_clump_kernel(
            plan, plan.clumps.front(), cvs, 1u));
    auto noise = device.compile(
        nanoxgen::luisa_backend::make_classic_runtime_noise_kernel(
            plan, plan.noises.front(), cvs));
    auto cut = device.compile(
        nanoxgen::luisa_backend::make_classic_runtime_cut_kernel(
            plan, plan.cuts.front(), cvs));
    auto width = device.compile(
        nanoxgen::luisa_backend::make_classic_runtime_width_kernel(plan, cvs));
    std::vector<luisa::float4> actual(source.size());
    stream << roots.copy_from(reference.roots.data())
           << runtime.copy_from(root_runtime.data())
           << ptex.copy_from(ptex_values.data())
           << tangent_buffer.copy_from(tangents.data())
           << noise_domain_buffer.copy_from(noise_domains.data())
           << clump_axes_buffer.copy_from(clump_axes.data())
           << clump_frames_buffer.copy_from(clump_frames.data())
           << clump_runtime_buffer.copy_from(clump_runtime.data())
           << clump_guides_buffer.copy_from(
                  clump_data.strand_guide_indices.data())
           << a.copy_from(source.data())
           << primitive(a, b, roots, runtime, ptex, states)
                  .dispatch(strand_count)
           << clump(b, a, roots, runtime, ptex, states,
                    clump_axes_buffer, clump_frames_buffer,
                    clump_runtime_buffer, clump_guides_buffer)
                  .dispatch(strand_count)
           << noise(a, b, roots, runtime, ptex, tangent_buffer,
                    noise_domain_buffer, states)
                  .dispatch(strand_count)
           << cut(b, a, roots, runtime, ptex, states).dispatch(strand_count)
           << width(a, roots, runtime, ptex, states).dispatch(strand_count)
           << a.copy_to(luisa::span{actual})
           << synchronize();
    float max_error = 0.0f;
    for (std::size_t index = 0u; index < actual.size(); ++index) {
        const auto &expected = reference.points[index];
        max_error = std::max({max_error,
            std::abs(actual[index].x - expected.x),
            std::abs(actual[index].y - expected.y),
            std::abs(actual[index].z - expected.z),
            std::abs(actual[index].w - expected.radius)});
    }
    if (max_error > 1.0e-4f) {
        throw std::runtime_error(
            "Luisa Classic Clump/Noise/Cut/width mismatch (max=" +
            std::to_string(max_error) + ")");
    }
    return max_error;
}

} // namespace

int main(int argc, char **argv) try {
    if (argc != 3) {
        std::cerr << "usage: nanoxgen_luisa_tests <runtime-dir> <backend>\n";
        return 2;
    }
    const std::string runtime_dir = argv[1];
    const std::string backend = argv[2];
    Context context{runtime_dir.c_str()};
    Device device = context.create_device(backend.c_str());
    if (device.backend_name() != backend) {
        throw std::runtime_error("LuisaCompute loaded an unexpected backend");
    }
    Stream stream = device.create_stream();
    test_external_device_collection_compile(device, stream);

    constexpr std::uint32_t count = 65536u;
    std::vector<std::uint32_t> input(count);
    std::vector<std::uint32_t> hashes(count);
    std::vector<float> widths(count);
    for (std::uint32_t index = 0u; index < count; ++index) {
        input[index] = index * 17u + 11u;
    }

    Buffer<std::uint32_t> input_buffer =
        device.create_buffer<std::uint32_t>(count);
    Buffer<std::uint32_t> hash_buffer =
        device.create_buffer<std::uint32_t>(count);
    Buffer<float> width_buffer = device.create_buffer<float>(count);
    Kernel1D evaluate = [](BufferUInt input_values, BufferUInt output_hashes,
                           BufferFloat output_widths) noexcept {
        set_block_size(128u, 1u, 1u);
        UInt index = dispatch_id().x;
        UInt value = input_values.read(index);
        value = value ^ (value >> 16u);
        value = value * 0x7feb352du;
        value = value ^ (value >> 15u);
        value = value * 0x846ca68bu;
        value = value ^ (value >> 16u);
        output_hashes.write(index, value);
        Float unit = cast<float>(value >> 8u) * (1.0f / 16777216.0f);
        output_widths.write(index, unit * 0.01f + 0.01f);
    };

    const auto compile_start = std::chrono::steady_clock::now();
    auto shader = device.compile(evaluate);
    const auto compile_end = std::chrono::steady_clock::now();
    const auto dispatch_start = std::chrono::steady_clock::now();
    stream << input_buffer.copy_from(luisa::span{input})
           << shader(input_buffer, hash_buffer, width_buffer).dispatch(count)
           << hash_buffer.copy_to(luisa::span{hashes})
           << width_buffer.copy_to(luisa::span{widths})
           << synchronize();
    const auto dispatch_end = std::chrono::steady_clock::now();

    nanoxgen::XgenExpressionCompileOptions expression_options{};
    expression_options.expression_name = "amount";
    expression_options.object_type = "CutFXModule";
    const nanoxgen::XgenFloatExpressionProgram expression_program =
        nanoxgen::make_xgen_float_expression_program(
            nanoxgen::compile_xgen_scalar_expression(
                "$a=hash($id+355) <= .3? rand(0.1,0.7):rand(0.2,0);"
                "$a*$cLength*rampUI(0,0,3:0.5,1,3:1,0,3)",
                expression_options));
    const nanoxgen::ClassicFloatRuntimeExpression classic_expression{
        "CutFXModule", "Cut1", "amount", expression_program};
    constexpr std::uint32_t expression_count = 4096u;
    std::vector<float> expression_inputs(
        expression_count * expression_program.inputs.size());
    std::vector<float> expression_contexts(expression_count * 4u);
    std::vector<std::uint32_t> expression_prefixes(expression_count);
    std::vector<float> expression_results(expression_count);
    std::vector<float> expression_reference(expression_count);
    for (std::uint32_t i = 0u; i < expression_count; ++i) {
        for (std::size_t input_index = 0u;
             input_index < expression_program.inputs.size(); ++input_index) {
            expression_inputs[i + input_index * expression_count] =
                expression_program.inputs[input_index] == "id"
                    ? static_cast<float>(i)
                    : 0.5f + static_cast<float>(i % 17u) * 0.03125f;
        }
        expression_contexts[i] = static_cast<float>(i % 101u) / 101.0f;
        expression_contexts[i + expression_count] =
            static_cast<float>((i * 7u) % 103u) / 103.0f;
        expression_contexts[i + expression_count * 2u] =
            nanoxgen::xgen_runtime_face_seed(
                2u, "rabbitShape", i % 152u);
        expression_contexts[i + expression_count * 3u] =
            static_cast<float>(i % 257u) / 256.0f;
        const std::array<double, 3u> prefix_arguments{
            static_cast<double>(expression_contexts[i]),
            static_cast<double>(expression_contexts[i + expression_count]),
            static_cast<double>(
                expression_contexts[i + expression_count * 2u])};
        expression_prefixes[i] =
            nanoxgen::xgen_seexpr_hash_prefix(prefix_arguments);
        nanoxgen::ClassicFloatRuntimeContext runtime_context{};
        runtime_context.id = i;
        runtime_context.u = expression_contexts[i];
        runtime_context.v = expression_contexts[i + expression_count];
        runtime_context.face_seed =
            expression_contexts[i + expression_count * 2u];
        runtime_context.t = expression_contexts[i + expression_count * 3u];
        runtime_context.random_prefix = expression_prefixes[i];
        runtime_context.has_random_prefix = true;
        for (std::size_t input_index = 0u;
             input_index < expression_program.inputs.size(); ++input_index) {
            if (expression_program.inputs[input_index] == "cLength") {
                runtime_context.c_length =
                    expression_inputs[i + input_index * expression_count];
            }
        }
        expression_reference[i] =
            nanoxgen::evaluate_xgen_classic_float_runtime_expression(
                classic_expression, runtime_context);
    }
    Buffer<float> expression_input_buffer =
        device.create_buffer<float>(expression_inputs.size());
    Buffer<float> expression_context_buffer =
        device.create_buffer<float>(expression_contexts.size());
    Buffer<float> expression_output_buffer =
        device.create_buffer<float>(expression_results.size());
    Buffer<std::uint32_t> expression_prefix_buffer =
        device.create_buffer<std::uint32_t>(expression_prefixes.size());
    std::size_t c_length_input = 0u;
    for (; c_length_input < expression_program.inputs.size(); ++c_length_input) {
        if (expression_program.inputs[c_length_input] == "cLength") { break; }
    }
    if (c_length_input == expression_program.inputs.size()) {
        throw std::runtime_error("Classic JIT fixture has no cLength input");
    }
    Kernel1D expression_kernel =
        [&](BufferFloat expression_input, BufferFloat expression_context,
            BufferUInt random_prefix, BufferFloat output) noexcept {
            set_block_size(128u, 1u, 1u);
            UInt index = dispatch_id().x;
            output.write(
                index,
                nanoxgen::luisa_backend::lower_classic_runtime_expression(
                    classic_expression,
                    {index,
                     expression_context.read(index),
                     expression_context.read(index + expression_count),
                     expression_context.read(index + expression_count * 2u),
                     expression_input.read(
                         index + expression_count *
                                     static_cast<std::uint32_t>(c_length_input)),
                     0.0f,
                     expression_context.read(index + expression_count * 3u),
                     random_prefix.read(index),
                     true}));
        };
    const auto expression_compile_start = std::chrono::steady_clock::now();
    auto expression_shader = device.compile(expression_kernel);
    const auto expression_compile_end = std::chrono::steady_clock::now();
    const auto expression_dispatch_start = std::chrono::steady_clock::now();
    stream << expression_input_buffer.copy_from(luisa::span{expression_inputs})
           << expression_context_buffer.copy_from(luisa::span{expression_contexts})
           << expression_prefix_buffer.copy_from(luisa::span{expression_prefixes})
           << expression_shader(expression_input_buffer, expression_context_buffer,
                                expression_prefix_buffer,
                                expression_output_buffer)
                  .dispatch(expression_count)
           << expression_output_buffer.copy_to(luisa::span{expression_results})
           << synchronize();
    const auto expression_dispatch_end = std::chrono::steady_clock::now();

    const float generation_max_absolute_error =
        test_luisa_generation(device, stream);
    test_luisa_classic_primitive_culling(device, stream);
    const float classic_effects_max_absolute_error =
        test_luisa_classic_effects(device, stream);

    std::uint64_t checksum = 1469598103934665603ull;
    for (std::uint32_t index = 0u; index < count; ++index) {
        const std::uint32_t expected_hash = nanoxgen::hash32(input[index]);
        const float expected_width =
            static_cast<float>(expected_hash >> 8u) *
                (1.0f / 16777216.0f) * 0.01f +
            0.01f;
        if (hashes[index] != expected_hash ||
            std::abs(widths[index] - expected_width) > 1.0e-7f) {
            throw std::runtime_error(
                "LuisaCompute CPU/GPU deterministic expression mismatch");
        }
        checksum ^= hashes[index];
        checksum *= 1099511628211ull;
    }
    std::size_t expression_bit_mismatches = 0u;
    std::uint32_t expression_max_ulp = 0u;
    float expression_max_absolute_error = 0.0f;
    for (std::uint32_t index = 0u; index < expression_count; ++index) {
        const std::uint32_t actual_bits =
            std::bit_cast<std::uint32_t>(expression_results[index]);
        const std::uint32_t expected_bits =
            std::bit_cast<std::uint32_t>(expression_reference[index]);
        if (actual_bits != expected_bits) {
            ++expression_bit_mismatches;
            const std::uint32_t ulp = actual_bits > expected_bits
                                          ? actual_bits - expected_bits
                                          : expected_bits - actual_bits;
            expression_max_ulp = std::max(expression_max_ulp, ulp);
            expression_max_absolute_error = std::max(
                expression_max_absolute_error,
                std::abs(expression_results[index] - expression_reference[index]));
        }
    }
    if (expression_max_absolute_error > 1.2e-7f) {
        std::uint32_t worst_index = 0u;
        float worst_error = 0.0f;
        for (std::uint32_t index = 0u; index < expression_count; ++index) {
            const float error = std::abs(
                expression_results[index] - expression_reference[index]);
            if (error > worst_error) {
                worst_error = error;
                worst_index = index;
            }
        }
        throw std::runtime_error(
            "LuisaCompute float expression IR exceeded the absolute error bound (" +
            std::to_string(expression_max_absolute_error) + ", index=" +
            std::to_string(worst_index) + ", expected=" +
            std::to_string(expression_reference[worst_index]) + ", actual=" +
            std::to_string(expression_results[worst_index]) + ")");
    }
    std::cout << "{\"backend\":\"" << backend
              << "\",\"element_count\":" << count
              << ",\"compile_ms\":"
              << milliseconds(compile_end - compile_start)
              << ",\"upload_dispatch_download_ms\":"
              << milliseconds(dispatch_end - dispatch_start)
              << ",\"xgen_expression_count\":" << expression_count
              << ",\"xgen_expression_semantics\":\"nanoxgen-fast-float\""
              << ",\"xgen_expression_compile_ms\":"
              << milliseconds(expression_compile_end - expression_compile_start)
              << ",\"xgen_expression_dispatch_ms\":"
              << milliseconds(expression_dispatch_end - expression_dispatch_start)
              << ",\"xgen_expression_bit_mismatches\":"
              << expression_bit_mismatches
              << ",\"xgen_expression_max_ulp\":" << expression_max_ulp
              << ",\"xgen_expression_max_absolute_error\":"
              << expression_max_absolute_error
              << ",\"luisa_generation_points\":" << (1024u * 8u)
              << ",\"luisa_generation_max_absolute_error\":"
              << generation_max_absolute_error
              << ",\"luisa_classic_effects_max_absolute_error\":"
              << classic_effects_max_absolute_error
              << ",\"checksum\":\"0x" << std::hex << checksum
              << "\"}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
