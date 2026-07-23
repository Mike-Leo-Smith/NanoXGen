#include "nanoxgen/asset.h"
#include "nanoxgen/xgen_classic_roots.h"
#include "nanoxgen/xgen_classic_ptex.h"
#include "nanoxgen/xgen_samples.h"

#include <Ptexture.h>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message) {
    if (!condition) { throw std::runtime_error(message); }
}

struct TemporaryDescription {
    std::filesystem::path path;

    explicit TemporaryDescription(float value) {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path = std::filesystem::temp_directory_path() /
               ("nanoxgen-classic-roots-" + std::to_string(stamp));
        const std::filesystem::path map_directory =
            path / "paintmaps" / "density";
        std::filesystem::create_directories(map_directory);
        const std::filesystem::path map_path =
            map_directory / "testPatch.ptx";
        Ptex::String error;
        Ptex::PtexPtr<Ptex::PtexWriter> writer{Ptex::PtexWriter::open(
            map_path.c_str(), Ptex::mt_quad, Ptex::dt_float, 1, -1, 1,
            error, false)};
        if (!writer) { throw std::runtime_error(error.c_str()); }
        require(writer->writeConstantFace(
                    0, Ptex::FaceInfo{Ptex::Res{2, 2}}, &value),
                "cannot write root PTEX fixture");
        require(writer->close(error), "cannot close root PTEX fixture");
    }

    ~TemporaryDescription() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

nanoxgen::ClassicDescription description() {
    nanoxgen::ClassicDescription result{};
    result.name = "test";
    result.attributes.push_back({"descriptionId", "7", 1u});
    nanoxgen::ClassicObject generator{};
    generator.type = "RandomGenerator";
    generator.attributes.push_back({"density", "64", 2u});
    generator.attributes.push_back(
        {"mask", "map('${DESC}/paintmaps/density')", 3u});
    result.objects.emplace_back(std::move(generator));
    nanoxgen::ClassicPatch patch{};
    patch.type = "Subd";
    patch.name = "testPatch";
    patch.face_ids = {0u};
    result.patches.emplace_back(std::move(patch));
    return result;
}

nanoxgen::ClassicAlembicAssetInput surface() {
    nanoxgen::ClassicAlembicAssetInput result{};
    result.asset.positions = {{0.0f, 0.0f, 0.0f},
                              {1.0f, 0.0f, 0.0f},
                              {0.0f, 0.0f, 1.0f},
                              {1.0f, 0.0f, 1.0f}};
    result.asset.triangles = {{0u, 1u, 3u}, {0u, 3u, 2u}};
    result.asset.reference_positions = result.asset.positions;
    result.asset.normals.assign(
        result.asset.positions.size(), {0.0f, 1.0f, 0.0f});
    result.asset.reference_normals = result.asset.normals;
    nanoxgen::GuideInput guide{};
    guide.cvs = {{0.5f, 0.0f, 0.5f}, {0.5f, 1.0f, 0.5f}};
    guide.root_uv = {0.5f, 0.5f};
    guide.triangle_index = 0u;
    guide.barycentric = {0.0f, 0.5f};
    guide.reference_root_position = guide.cvs.front();
    guide.reference_root_normal = {0.0f, 1.0f, 0.0f};
    guide.reference_root_tangent = {1.0f, 0.0f, 0.0f};
    guide.reference_root_binormal = {0.0f, 0.0f, 1.0f};
    guide.support_radii = {2.0f};
    result.asset.guides.push_back(std::move(guide));
    result.surface_faces.push_back(
        {"testPatch", 0u, 0u, 2u, 1u, 1.0f,
         1.0f, 1.0f, {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 1.0f}});
    return result;
}

void test_full_mask_and_generation() {
    const TemporaryDescription fixture{1.0f};
    const nanoxgen::ClassicAlembicAssetInput input = surface();
    const nanoxgen::ClassicRootPlan first =
        nanoxgen::build_xgen_classic_random_root_plan(
            description(), input, fixture.path);
    const nanoxgen::ClassicRootPlan second =
        nanoxgen::build_xgen_classic_random_root_plan(
            description(), input, fixture.path);
    require(first.candidate_count == 64u && first.roots.size() == 64u &&
                first.mask_rejected_count == 0u,
            "full-mask root count mismatch");
    require(first.ptex_maps.size() == 1u,
            "root plan did not retain its PTEX dependency");
    require(first.primitive_ids.size() == first.roots.size() &&
                first.primitive_ids.front() == 1u &&
                first.primitive_ids.back() == 64u,
            "per-face primitive IDs do not match RandomGenerator candidates");
    require(first.reference_positions.size() == first.roots.size(),
            "root plan did not retain $Prefg positions");
    require(first.roots.size() == second.roots.size() &&
                std::memcmp(first.roots.data(), second.roots.data(),
                            first.roots.size() * sizeof(nanoxgen::RootSample)) == 0,
            "root plan is not deterministic");
    for (const nanoxgen::RootSample &root : first.roots) {
        require(root.surface_face_id == 0u && root.triangle_index < 2u &&
                    root.uv.x >= 0.0f && root.uv.x < 1.0f &&
                    root.uv.y >= 0.0f && root.uv.y < 1.0f,
                "root identity or coordinates are invalid");
    }

    nanoxgen::ClassicDescription runtime_description = description();
    runtime_description.objects.push_back({"SplinePrimitive", {
        {"fxCVCount", "4", 4u},
        {"length",
         "$freq=2; 0.5+map('${DESC}/paintmaps/density')+long()+"
         "noise($Prefg*$freq)",
         5u}}, 4u});
    const std::array<nanoxgen::ClassicAttribute, 1u> palette{{
        {"custom_float_long", "hash($id)<.5?1:0", 6u}}};
    const nanoxgen::ClassicFloatRuntimePlan runtime =
        nanoxgen::compile_xgen_classic_float_runtime_plan(
            runtime_description, palette);
    require(runtime.lowering_complete() && runtime.ptex_paths.size() == 1u &&
                runtime.custom_inputs.size() == 1u &&
                runtime.pref_noise_inputs.size() == 1u &&
                runtime.pref_noise_inputs.front().frequency == 2.0f,
            "runtime map expression did not lower");
    const nanoxgen::ClassicRuntimeInputData runtime_data =
        nanoxgen::build_xgen_classic_runtime_input_data(
            runtime, fixture.path, "testPatch", first);
    require(runtime_data.strand_count == first.roots.size() &&
                runtime_data.values_per_strand == 3u &&
                runtime_data.values.size() == first.roots.size() * 3u,
            "runtime PTEX table dimensions mismatch");
    for (std::size_t strand = 0u; strand < first.roots.size(); ++strand) {
        require(runtime_data.values[strand * 3u] == 1.0f,
                "runtime PTEX sample mismatch");
        const float custom = runtime_data.values[strand * 3u + 1u];
        require(custom == 0.0f || custom == 1.0f,
                "runtime custom sample mismatch");
        require(runtime_data.values[strand * 3u + 2u] ==
                    nanoxgen::xgen_classic_noise_float(
                        first.reference_positions[strand] * 2.0f),
                "runtime $Prefg noise sample mismatch");
    }

    const nanoxgen::Asset asset = nanoxgen::build_asset(input.asset);
    nanoxgen::GenerationParams params{};
    params.strand_count = static_cast<std::uint32_t>(first.roots.size());
    params.cvs_per_strand = 4u;
    const nanoxgen::PackedGeneratedCurves curves =
        nanoxgen::generate_packed_roots_cpu(asset, params, first.roots);
    require(curves.strand_count == first.roots.size() &&
                curves.points.size() == first.roots.size() * 4u &&
                std::memcmp(curves.roots.data(), first.roots.data(),
                            first.roots.size() * sizeof(nanoxgen::RootSample)) == 0,
            "explicit roots did not survive packed generation");
    for (const nanoxgen::PackedCurvePoint &point : curves.points) {
        require(std::isfinite(point.x) && std::isfinite(point.y) &&
                    std::isfinite(point.z) && std::isfinite(point.radius),
                "explicit-root generation produced a non-finite point");
    }
}

void test_partial_mask_and_limit() {
    const TemporaryDescription fixture{0.5f};
    const nanoxgen::ClassicAlembicAssetInput input = surface();
    const nanoxgen::ClassicRootPlan roots =
        nanoxgen::build_xgen_classic_random_root_plan(
            description(), input, fixture.path);
    require(roots.candidate_count == 64u && roots.roots.size() == 32u &&
                roots.roots.size() + roots.mask_rejected_count == 64u,
            "partial PTEX mask was not applied");
    nanoxgen::ClassicRootGenerationLimits limits{};
    limits.max_candidates = 63u;
    try {
        (void)nanoxgen::build_xgen_classic_random_root_plan(
            description(), input, fixture.path, limits);
    } catch (const std::runtime_error &) { return; }
    throw std::runtime_error("root candidate limit was ignored");
}

void test_patch_authored_primitive_cull() {
    const TemporaryDescription fixture{1.0f};
    const nanoxgen::ClassicAlembicAssetInput input = surface();
    nanoxgen::ClassicDescription culled = description();
    culled.patches.front().culled_primitives.push_back(
        {0u, {1u, 29u, 64u}});
    const nanoxgen::ClassicRootPlan roots =
        nanoxgen::build_xgen_classic_random_root_plan(
            culled, input, fixture.path);
    require(roots.candidate_count == 64u && roots.roots.size() == 61u &&
                roots.patch_culled_count == 3u &&
                roots.mask_rejected_count == 0u &&
                roots.guide_rejected_count == 0u,
            "patch-authored primitive cull count mismatch");
    require(roots.primitive_ids.front() == 2u &&
                roots.primitive_ids[27u] == 30u &&
                roots.primitive_ids.back() == 63u,
            "patch-authored primitive cull did not retain ID gaps");
}

void test_face_umbrella_guide_prefilter() {
    const TemporaryDescription fixture{1.0f};
    nanoxgen::ClassicAlembicAssetInput input = surface();
    // The root/guide distance remains inside the guide radius, but XGen first
    // discards guides outside the active reference face's expanded umbrella
    // AABB. This matters on strongly curved subdivision patches.
    input.surface_faces.front().reference_bounds_min = {-3.0f, -3.0f, -3.0f};
    input.surface_faces.front().reference_bounds_max = {-2.0f, -2.0f, -2.0f};
    const nanoxgen::ClassicRootPlan roots =
        nanoxgen::build_xgen_classic_random_root_plan(
            description(), input, fixture.path);
    require(roots.roots.empty() && roots.guide_rejected_count == 64u,
            "face umbrella guide prefilter was not applied");
}

void test_maya_2027_sample_pattern() {
    const nanoxgen::Vec2 base = nanoxgen::xgen_random_sample(0u, 0u, 0u);
    require(std::bit_cast<std::uint32_t>(base.x) == 0x3f07d46du &&
                std::bit_cast<std::uint32_t>(base.y) == 0x3f248854u,
            "embedded Maya sample pattern changed");
    const nanoxgen::Vec2 rotated =
        nanoxgen::xgen_random_sample(0u, 1u, 0u);
    require(rotated.x == 1.0f - base.y && rotated.y == base.x,
            "Maya sample symmetry mismatch");
    const nanoxgen::Vec2 extended = nanoxgen::xgen_random_sample(
        nanoxgen::kXgenSamplesPerGroup, 0u, 0u);
    require(extended.x == base.x * 0.5f && extended.y == base.y * 0.5f,
            "Maya hierarchical sample extension mismatch");
}

void test_directional_guide_weight() {
    nanoxgen::GuideInput guide{};
    guide.reference_root_position = {0.0f, 0.0f, 0.0f};
    guide.reference_root_normal = {0.0f, 0.0f, 1.0f};
    guide.reference_root_tangent = {1.0f, 0.0f, 0.0f};
    guide.reference_root_binormal = {0.0f, 1.0f, 0.0f};
    guide.support_radii = {2.0f, 0.5f, 2.0f};
    guide.support_angles = {1.0f, 3.0f};
    const float wrapped = nanoxgen::evaluate_xgen_classic_guide_weight(
        guide, {0.625f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    require(std::abs(wrapped - 0.5f) < 1.0e-6f,
            "directional support wrap interpolation mismatch");
    const float positive_y = nanoxgen::evaluate_xgen_classic_guide_weight(
        guide, {0.0f, 0.625f, 0.0f}, {0.0f, 0.0f, 1.0f});
    require(std::abs(positive_y - 0.6875f) < 1.0e-6f,
            "XGen positive orientation mapping mismatch");
    const float negative_y = nanoxgen::evaluate_xgen_classic_guide_weight(
        guide, {0.0f, -0.25f, 0.0f}, {0.0f, 0.0f, 1.0f});
    require(std::abs(negative_y - 0.5f) < 1.0e-6f,
            "XGen negative orientation mapping mismatch");
    require(nanoxgen::evaluate_xgen_classic_guide_weight(
                guide, {0.1f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}) == 0.0f,
            "opposite-facing guide was not rejected");
}

} // namespace

int main() try {
    test_full_mask_and_generation();
    test_partial_mask_and_limit();
    test_patch_authored_primitive_cull();
    test_face_umbrella_guide_prefilter();
    test_maya_2027_sample_pattern();
    test_directional_guide_weight();
    std::cout << "Classic root generation tests passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
}
