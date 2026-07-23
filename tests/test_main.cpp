#include "nanoxgen/asset.h"
#include "nanoxgen/context.h"
#include "nanoxgen/curve_cache.h"
#include "nanoxgen/curve_payload.h"
#include "nanoxgen/xgen.h"
#include "../tools/xgen_classic_typed_validation.h"
#include "../tools/maya_xgen_cache_validation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nanoxgen;

namespace {

void require(bool condition, const char *message) {
    if (!condition) { throw std::runtime_error(message); }
}

float strand_arc_length(const GeneratedCurves &curves, std::uint32_t strand) {
    const std::size_t first = static_cast<std::size_t>(strand) * curves.cvs_per_strand;
    float length = 0.0f;
    for (std::uint32_t cv = 1u; cv < curves.cvs_per_strand; ++cv) {
        length += std::sqrt(length_squared(
            curves.points[first + cv] - curves.points[first + cv - 1u]));
    }
    return length;
}

AssetBuildInput fixture() {
    AssetBuildInput input{};
    input.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    input.normals.assign(3u, {0.0f, 1.0f, 0.0f});
    input.texcoords = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    input.triangles = {{0u, 1u, 2u}};
    GuideInput guide{};
    guide.root_normal = {0.0f, 1.0f, 0.0f};
    guide.triangle_index = 0u;
    guide.barycentric = {0.25f, 0.25f};
    guide.support_radius = 10.0f;
    guide.cvs = {{0.25f, 0.0f, 0.25f}, {0.25f, 0.5f, 0.25f}, {0.50f, 1.0f, 0.25f}};
    input.guides.push_back(guide);
    return input;
}

void test_blob_round_trip() {
    const Asset asset = build_asset(fixture());
    require(validate_asset(asset.bytes()).empty(), "fresh asset must validate");
    require(asset.view().header().triangle_count == 1u, "triangle count");
    require(asset.view().header().guide_count == 1u, "guide count");

    AlignedByteVector corrupt(asset.bytes().begin(), asset.bytes().end());
    corrupt.back() ^= std::byte{1};
    require(validate_asset(corrupt) == "content hash mismatch", "corruption must be detected");
}

void test_determinism_and_roots() {
    const Asset asset = build_asset(fixture());
    GenerationParams params{};
    params.strand_count = 128u;
    params.cvs_per_strand = 7u;
    params.seed = 42u;
    params.noise_amplitude = 0.0f;
    const GeneratedCurves a = generate_cpu(asset, params);
    const GeneratedCurves b = generate_cpu(asset, params);
    require(a.points.size() == b.points.size(), "point count");
    require(std::memcmp(a.points.data(), b.points.data(), a.points.size() * sizeof(Vec3)) == 0,
            "generation must be bitwise deterministic");
    for (std::uint32_t strand = 0; strand < params.strand_count; ++strand) {
        const RootSample root = a.roots[strand];
        require(root.position.x >= 0.0f && root.position.z >= 0.0f && root.position.x + root.position.z <= 1.00001f,
                "root must lie inside the triangle");
        const Vec3 first = a.points[static_cast<std::size_t>(strand) * params.cvs_per_strand];
        require(length_squared(first - root.position) < 1.0e-10f, "first CV must equal the root");
    }
}

void test_guide_interpolation() {
    const Asset asset = build_asset(fixture());
    GenerationParams params{};
    params.strand_count = 1u;
    params.cvs_per_strand = 3u;
    params.seed = 7u;
    const GeneratedCurves curves = generate_cpu(asset, params);
    const Vec3 root = curves.roots.front().position;
    const Vec3 middle = curves.points[1];
    const Vec3 tip = curves.points[2];
    require(std::abs(middle.y - 0.5f) < 1.0e-6f, "guide midpoint height");
    require(std::abs(tip.y - 1.0f) < 1.0e-6f, "guide tip height");
    require(std::abs((tip.x - root.x) - 0.25f) < 1.0e-6f, "root-relative guide bend");
}

void test_deformed_geometry_generation() {
    const Asset asset = build_asset(fixture());
    GenerationParams params{};
    params.strand_count = 32u;
    params.cvs_per_strand = 7u;
    params.seed = 81u;
    params.noise_amplitude = 0.0f;
    const GeneratedCurves base = generate_cpu(asset, params, {3u, 5u});

    const DeviceAssetView view = asset.view();
    const Vec3 translation{2.0f, 3.0f, -4.0f};
    std::vector<Vec3> positions(view.positions(),
                                view.positions() + view.header().vertex_count);
    std::vector<Vec3> normals(view.normals(),
                              view.normals() + view.header().vertex_count);
    std::vector<Vec3> guide_cvs(view.guide_cvs(),
                                view.guide_cvs() + view.header().guide_cv_count);
    for (Vec3 &point : positions) { point = point + translation; }
    for (Vec3 &point : guide_cvs) { point = point + translation; }

    const GeneratedCurves moved = generate_deformed_cpu(
        asset, params, DeformedGeometryView{positions, normals, guide_cvs}, {4u, 5u});
    require(moved.strand_count == base.strand_count &&
            moved.cvs_per_strand == base.cvs_per_strand,
            "deformation must preserve curve topology");
    for (std::size_t strand = 0u; strand < base.roots.size(); ++strand) {
        require(base.roots[strand].triangle_index == moved.roots[strand].triangle_index &&
                std::memcmp(&base.roots[strand].barycentric,
                            &moved.roots[strand].barycentric, sizeof(Vec2)) == 0,
                "deformation must preserve root identity");
        const Vec3 root_delta = moved.roots[strand].position - base.roots[strand].position;
        require(length_squared(root_delta - translation) < 1.0e-11f,
                "deformed root must follow the patch");
    }
    for (std::size_t point = 0u; point < base.points.size(); ++point) {
        const Vec3 delta = moved.points[point] - base.points[point];
        require(length_squared(delta - translation) < 1.0e-10f,
                "deformed spline must follow translated mesh and guides");
    }

    const GeneratedCurves patch_only = generate_deformed_cpu(
        asset, params, DeformedGeometryView{positions, normals, {}}, {4u, 5u});
    for (std::size_t point = 0u; point < base.points.size(); ++point) {
        const Vec3 delta = patch_only.points[point] - base.points[point];
        require(length_squared(delta - translation) < 1.0e-10f,
                "rest guides must follow their deformed bound roots");
    }
}

void test_cpu_persistent_work_queue() {
    const Asset asset = build_asset(fixture());
    GenerationParams params{};
    params.strand_count = 257u;
    params.cvs_per_strand = 11u;
    params.seed = 91u;
    params.noise_amplitude = 0.03f;

    const GeneratedCurves serial = generate_cpu(asset, params, CpuGenerationOptions{1u, 17u});
    const GeneratedCurves parallel = generate_cpu(asset, params, CpuGenerationOptions{4u, 17u});
    require(std::memcmp(serial.roots.data(), parallel.roots.data(),
                        serial.roots.size() * sizeof(RootSample)) == 0,
            "persistent CPU scheduling must cover every root exactly once");
    require(std::memcmp(serial.points.data(), parallel.points.data(),
                        serial.points.size() * sizeof(Vec3)) == 0,
            "persistent CPU scheduling must preserve deterministic points");
    require(std::memcmp(serial.widths.data(), parallel.widths.data(),
                        serial.widths.size() * sizeof(float)) == 0,
            "persistent CPU scheduling must preserve deterministic widths");
}

void test_context_thread_pool() {
    require(available_worker_count() >= 1u,
            "available worker count must never be zero");

    ThreadPool pool{4u};
    NanoXGenContext borrowed{pool};
    require(borrowed.worker_count() == 4u && !borrowed.owns_executor(),
            "context must expose a borrowed renderer executor");
    std::atomic_size_t visits{};
    std::atomic_size_t sum{};
    pool.parallel_for(257u, [&](std::size_t index) {
        visits.fetch_add(1u, std::memory_order_relaxed);
        sum.fetch_add(index, std::memory_order_relaxed);
    });
    require(visits.load() == 257u &&
                sum.load() == (256u * 257u) / 2u,
            "thread pool must execute every task exactly once");

    std::atomic_size_t nested_visits{};
    pool.parallel_for(7u, [&](std::size_t) {
        pool.parallel_for(13u, [&](std::size_t) {
            nested_visits.fetch_add(1u, std::memory_order_relaxed);
        });
    });
    require(nested_visits.load() == 91u,
            "thread pool must support recursive parallel work");

    ThreadPool one_worker{1u};
    std::atomic_size_t one_worker_visits{};
    one_worker.parallel_for(2u, [&](std::size_t) {
        one_worker.parallel_for(3u, [&](std::size_t) {
            one_worker_visits.fetch_add(1u, std::memory_order_relaxed);
        });
    });
    require(one_worker_visits.load() == 6u,
            "single-worker nested scheduling must not deadlock");

    bool propagated = false;
    try {
        pool.parallel_for(8u, [](std::size_t index) {
            if (index == 3u) {
                throw std::runtime_error("expected task failure");
            }
        });
    } catch (const std::runtime_error &) {
        propagated = true;
    }
    require(propagated,
            "thread pool must propagate task exceptions to the caller");

    NanoXGenContext owned{2u};
    require(owned.worker_count() == 2u && owned.owns_executor(),
            "context must own an explicitly sized fallback pool");
}

void test_xgen_noise_math_and_length_preservation() {
    const Asset asset = build_asset(fixture());
    const DeviceAssetView view = asset.view();
    const std::array<std::pair<Vec3, float>, 6u> samples = {{
        {{0.0f, 0.0f, 0.0f}, 0.5f},
        {{0.0457031233125f, 0.0f, 0.0f}, 0.513789508165f},
        {{0.0f, 0.0457031233125f, 0.0f}, 0.518000278518f},
        {{0.0f, 0.0f, 0.0457031233125f}, 0.496315375404f},
        {{1.234f, -2.5f, 3.75f}, 0.672824373032f},
        {{-3.2f, 4.1f, -0.7f}, 0.515891027958f},
    }};
    for (const auto &[point, expected] : samples) {
        require(std::abs(gradient_noise(view, point) - expected) < 2.0e-6f,
                "XGen-compatible gradient noise reference sample");
    }

    GenerationParams params{};
    params.strand_count = 32u;
    params.cvs_per_strand = 33u;
    params.seed = 173u;
    params.noise_amplitude = 0.24f;
    params.noise_frequency = 0.0f;
    params.noise_correlation = 0.75f;
    const StrandGenerationState state = make_strand_generation_state(
        view, {}, params, 0u);
    const Vec3 expected_domain =
        (rest_root_position(view, state.root) +
         Vec3{0.419276f, 0.184247f, 0.805721f}) * 6.25f;
    require(length_squared(state.noise_domain - expected_domain) < 1.0e-11f,
            "XGen correlation percentage-to-domain mapping");
    require(std::abs(state.effective_noise_frequency - 0.5f / state.original_length) <
                1.0e-6f,
            "XGen noise frequency floor");

    GenerationParams base_params = params;
    base_params.noise_amplitude = 0.0f;
    const GeneratedCurves base = generate_cpu(asset, base_params, {1u, 16u});
    params.noise_frequency = 2.25f;
    params.noise_preserve_length = 0.0f;
    const GeneratedCurves noisy = generate_cpu(asset, params, {1u, 16u});
    params.noise_preserve_length = 1.0f;
    const GeneratedCurves preserved = generate_cpu(asset, params, {1u, 16u});
    params.noise_preserve_length = 0.4f;
    const GeneratedCurves partially_preserved = generate_cpu(asset, params, {1u, 16u});

    std::uint32_t exercised = 0u;
    for (std::uint32_t strand = 0u; strand < params.strand_count; ++strand) {
        const std::size_t root_index = static_cast<std::size_t>(strand) * params.cvs_per_strand;
        require(length_squared(noisy.points[root_index] - base.points[root_index]) < 1.0e-12f &&
                length_squared(preserved.points[root_index] - base.points[root_index]) < 1.0e-12f,
                "XGen noise must keep the root fixed");
        const float base_length = strand_arc_length(base, strand);
        const float noisy_length = strand_arc_length(noisy, strand);
        if (std::abs(noisy_length - base_length) <= 2.0e-4f) { continue; }
        ++exercised;
        require(std::abs(strand_arc_length(preserved, strand) - base_length) < 2.0e-5f,
                "preserveLength=1 must restore base control-polygon length");
        const float target = 0.4f * base_length + 0.6f * noisy_length;
        require(std::abs(strand_arc_length(partially_preserved, strand) - target) < 2.0e-5f,
                "partial preserveLength must blend base and noisy length");
    }
    require(exercised > 0u, "noise preserve-length test must exercise scaling");

    const Vec3 translation{2.0f, 3.0f, -4.0f};
    std::vector<Vec3> positions(view.positions(), view.positions() + view.header().vertex_count);
    std::vector<Vec3> normals(view.normals(), view.normals() + view.header().vertex_count);
    std::vector<Vec3> guide_cvs(view.guide_cvs(), view.guide_cvs() + view.header().guide_cv_count);
    for (Vec3 &point : positions) { point = point + translation; }
    for (Vec3 &point : guide_cvs) { point = point + translation; }
    params.noise_preserve_length = 0.4f;
    const GeneratedCurves moved = generate_deformed_cpu(
        asset, params, DeformedGeometryView{positions, normals, guide_cvs}, {1u, 16u});
    for (std::size_t point = 0u; point < moved.points.size(); ++point) {
        require(length_squared((moved.points[point] - partially_preserved.points[point]) -
                               translation) < 2.0e-11f,
                "reference-root noise domain must not swim under rigid motion");
    }
}

void test_area_weighted_sampling() {
    AssetBuildInput input{};
    input.positions = {
        {0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
        {10.0f, 0.0f, 0.0f}, {11.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 1.0f}};
    input.normals.assign(input.positions.size(), {0.0f, 1.0f, 0.0f});
    input.triangles = {{0u, 1u, 2u}, {3u, 4u, 5u}}; // areas 1.0 and 0.5
    GuideInput guide{};
    guide.root_normal = {0.0f, 1.0f, 0.0f};
    guide.support_radius = 100.0f;
    guide.cvs = {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    input.guides.push_back(guide);
    const Asset asset = build_asset(input);

    GenerationParams params{};
    params.strand_count = 30000u;
    params.cvs_per_strand = 2u;
    params.seed = 123u;
    const GeneratedCurves curves = generate_cpu(asset, params);
    std::uint32_t first_triangle = 0u;
    for (const RootSample root : curves.roots) { first_triangle += root.triangle_index == 0u ? 1u : 0u; }
    const float frequency = static_cast<float>(first_triangle) / static_cast<float>(params.strand_count);
    require(frequency > 0.64f && frequency < 0.69f, "root sampling must track rest triangle area");
}

void test_linear_compatibility_generation() {
    const std::vector<LinearCurveSeed> seeds = {
        {{1.0f, 2.0f, 3.0f}, {2.0f, 4.0f, 2.0f}, {0.25f, 0.75f}, 0.02f},
        {{-1.0f, 0.0f, 1.0f}, {-1.0f, 2.0f, 1.0f}, {0.5f, 0.5f}, 0.04f},
    };
    LinearModifierReferenceParams params{};
    params.cvs_per_strand = 5u;
    params.length_scale = 0.75f;
    params.width_taper = 0.8f;
    params.width_taper_start = 0.25f;
    const GeneratedCurves curves = generate_linear_modifier_reference_cpu(seeds, params, {2u, 1u});
    require(curves.strand_count == 2u && curves.cvs_per_strand == 5u,
            "linear compatibility topology");
    require(length_squared(curves.points[0] - seeds[0].root) == 0.0f,
            "linear compatibility root");
    const Vec3 expected_tip = seeds[0].root + (seeds[0].tip - seeds[0].root) * 0.75f;
    require(length_squared(curves.points[4] - expected_tip) == 0.0f,
            "linear compatibility cut length");
    require(curves.widths[0] == 0.02f &&
            std::abs(curves.widths[4] - 0.004f) < 1.0e-8f,
            "linear compatibility width taper");
}

void test_renderer_curve_payload() {
    const std::vector<std::uint32_t> counts = {2u, 3u};
    const std::vector<Vec3> points = {
        {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {1.0f, 2.0f, 0.0f},
    };
    const std::vector<float> widths = {0.02f, 0.04f, 0.06f, 0.04f, 0.02f};
    const std::vector<Vec2> root_uvs = {{0.25f, 0.5f}, {0.75f, 0.125f}};
    const CurveSetView base{2u, 0u, counts, points, widths, {}, root_uvs, 0.0f};

    const std::vector<std::uint32_t> motion_counts = {3u, 2u};
    const std::vector<Vec3> motion_points = {
        {0.0f, 0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.5f, 0.0f},
        {1.0f, 0.5f, 0.0f}, {1.0f, 2.5f, 0.0f},
    };
    const CurveSetView motion{2u, 0u, motion_counts, motion_points, {}, {}, {}, 0.02f};
    const std::array<MotionSampleView, 1u> samples = {{{0.5f, motion}}};
    const std::vector<float> ids = {11.0f, 12.0f};
    const std::vector<Vec3> colors = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    const std::array<UniformFloatPrimvarView, 1u> float_primvars = {{{"id", ids}}};
    const std::array<UniformColorPrimvarView, 1u> color_primvars = {{{"color", colors}}};

    CurvePayloadOptions options{};
    options.max_strands_per_batch = 1u;
    options.resample_cv_count = 4u;
    options.radius_scale = 2.0f;
    options.transform.values[3] = 10.0f;
    const std::vector<CurveBatch> batches = build_curve_batches(
        base, samples, float_primvars, color_primvars, options);
    require(batches.size() == 2u, "renderer payload batch count");
    require(batches[0].first_strand == 0u && batches[1].first_strand == 1u,
            "renderer payload batch offsets");
    require(batches[0].point_counts == std::vector<std::uint32_t>{4u} &&
            batches[1].point_counts == std::vector<std::uint32_t>{4u},
            "renderer payload resampled topology");
    require(batches[0].points.size() == 4u && batches[0].points[0].x == 10.0f &&
            batches[0].points[3].y == 1.0f,
            "renderer payload transform and endpoint resampling");
    require(batches[0].points[0].radius == 0.02f &&
            batches[0].points[3].radius == 0.04f,
            "renderer payload width-to-radius conversion");
    require(batches[0].root_uvs[0].x == 0.25f && batches[1].root_uvs[0].x == 0.75f,
            "renderer payload root UV slicing");
    require(batches[0].motion_times == std::vector<float>{0.5f} &&
            batches[0].motion_points.size() == 4u &&
            batches[0].motion_points.front().x == 10.0f &&
            batches[0].motion_points.front().y == 0.5f &&
            batches[0].motion_points.back().y == 1.5f,
            "renderer payload motion resampling");
    require(batches[0].uniform_floats[0].name == "id" &&
            batches[0].uniform_floats[0].values == std::vector<float>{11.0f} &&
            batches[1].uniform_floats[0].values == std::vector<float>{12.0f},
            "renderer payload float primvar slicing");
    require(batches[0].uniform_colors[0].name == "color" &&
            batches[1].uniform_colors[0].values[0].y == 1.0f,
            "renderer payload color primvar slicing");
}

void test_direct_packed_generation_math() {
    const Asset asset = build_asset(fixture());
    GenerationParams params{};
    params.strand_count = 19u;
    params.cvs_per_strand = 11u;
    params.seed = 117u;
    params.root_width = 0.03f;
    params.tip_width = 0.002f;
    params.noise_amplitude = 0.04f;
    params.noise_frequency = 3.25f;
    const GeneratedCurves soa = generate_cpu(asset, params, {3u, 4u});

    std::vector<PackedCurvePoint> packed(soa.points.size());
    std::vector<RootSample> roots(soa.strand_count);
    std::vector<Vec2> root_uvs(soa.strand_count);
    const DevicePackedCurveOutputView output{
        packed.data(), roots.data(), root_uvs.data(), 1.5f};
    for (std::uint32_t strand = 0u; strand < soa.strand_count; ++strand) {
        generate_packed_strand(asset.view(), params, strand, output);
    }
    require(std::memcmp(roots.data(), soa.roots.data(),
                        roots.size() * sizeof(RootSample)) == 0,
            "packed and SoA generation must share root math");
    for (std::size_t strand = 0u; strand < roots.size(); ++strand) {
        require(std::memcmp(&root_uvs[strand], &roots[strand].uv, sizeof(Vec2)) == 0,
                "packed generation root UV");
    }
    for (std::size_t point = 0u; point < packed.size(); ++point) {
        require(std::memcmp(&packed[point], &soa.points[point], sizeof(Vec3)) == 0,
                "packed and SoA positions must be bitwise equal");
        require(std::abs(packed[point].radius -
                         0.5f * soa.widths[point] * 1.5f) <= 5.0e-9f,
                "packed generation radius conversion within float rounding");
    }

    const PackedGeneratedCurves public_output =
        generate_packed_cpu(asset, params, 1.5f, {1u, 128u});
    require(public_output.points.size() == packed.size(),
            "public packed generation point count");
    require(public_output.point_counts.size() == params.strand_count &&
            std::all_of(public_output.point_counts.begin(), public_output.point_counts.end(),
                        [&](std::uint32_t count) { return count == params.cvs_per_strand; }),
            "public packed generation must materialize renderer point counts");
    require(public_output.root_uvs.size() == params.strand_count,
            "public packed generation UV count");
    require(std::memcmp(public_output.points.data(), packed.data(),
                        packed.size() * sizeof(PackedCurvePoint)) == 0,
            "public packed generation must use shared device math");

    const PackedGeneratedCurves explicit_output = generate_packed_roots_cpu(
        asset, params, public_output.roots, 1.5f, {2u, 4u});
    require(std::memcmp(explicit_output.points.data(), public_output.points.data(),
                        public_output.points.size() * sizeof(PackedCurvePoint)) == 0 &&
                std::memcmp(explicit_output.roots.data(), public_output.roots.data(),
                            public_output.roots.size() * sizeof(RootSample)) == 0,
            "explicit roots must reproduce packed generation bitwise");
    std::vector<RootSample> invalid_roots = public_output.roots;
    invalid_roots.front().triangle_index = asset.view().header().triangle_count;
    bool rejected_root = false;
    try {
        (void)generate_packed_roots_cpu(
            asset, params, invalid_roots, 1.0f, {1u, 128u});
    } catch (const std::invalid_argument &) {
        rejected_root = true;
    }
    require(rejected_root, "explicit root validation");
}

void test_checked_device_generation_contract() {
    const Asset asset = build_asset(fixture());
    const DeviceAssetDescriptor device_asset = make_device_asset_descriptor(
        asset, asset.bytes().data(), asset.bytes().size());
    GenerationParams params{};
    params.strand_count = 7u;
    params.cvs_per_strand = 5u;
    const std::uint64_t point_count =
        static_cast<std::uint64_t>(params.strand_count) * params.cvs_per_strand;
    std::vector<PackedCurvePoint> points(point_count);
    std::vector<RootSample> roots(params.strand_count);
    std::vector<Vec2> root_uvs(params.strand_count);
    std::vector<std::uint32_t> point_counts(params.strand_count);
    DevicePackedCurveOutputDescriptor output{
        {points.data(), roots.data(), root_uvs.data(), 1.0f, point_counts.data()},
        points.size(), roots.size(), root_uvs.size(), point_counts.size()};

    require(validate_device_packed_generation_request(
                device_asset, {}, params, output) == DeviceGenerationError::None,
            "valid checked device request");

    const std::array<DeviceMotionSampleDescriptor, 2u> motion_samples = {{
        {{}, 0.25f},
        {{}, 0.75f},
    }};
    std::vector<Vec3> motion_points(point_count * motion_samples.size());
    DeviceMotionOutputDescriptor motion_output{motion_points.data(), motion_points.size()};
    require(validate_device_motion_generation_request(
                device_asset, motion_samples.data(), motion_samples.size(), params,
                motion_output) == DeviceGenerationError::None,
            "valid checked device motion request");

    auto unordered_motion = motion_samples;
    unordered_motion[1].time = unordered_motion[0].time;
    require(validate_device_motion_generation_request(
                device_asset, unordered_motion.data(), unordered_motion.size(), params,
                motion_output) == DeviceGenerationError::InvalidMotionTimes,
            "checked device motion times");

    motion_output.point_capacity -= 1u;
    require(validate_device_motion_generation_request(
                device_asset, motion_samples.data(), motion_samples.size(), params,
                motion_output) == DeviceGenerationError::MotionPointCapacityTooSmall,
            "checked device motion capacity");
    motion_output.point_capacity += 1u;

    output.point_capacity = point_count - 1u;
    require(validate_device_packed_generation_request(
                device_asset, {}, params, output) ==
                DeviceGenerationError::PointCapacityTooSmall,
            "checked device point capacity");
    output.point_capacity = point_count;

    DeviceDeformedGeometryDescriptor bad_deformation{};
    bad_deformation.geometry.positions = asset.view().positions();
    bad_deformation.position_count = asset.view().header().vertex_count - 1u;
    require(validate_device_packed_generation_request(
                device_asset, bad_deformation, params, output) ==
                DeviceGenerationError::DeformedPositionCountMismatch,
            "checked device deformation size");

    DeviceLaunchConfig bad_config{};
    bad_config.block_size = 0u;
    require(validate_device_packed_generation_request(
                device_asset, {}, params, output, bad_config) ==
                DeviceGenerationError::InvalidBlockSize,
            "checked device launch geometry");

    output.output.radius_scale = std::numeric_limits<float>::quiet_NaN();
    require(validate_device_packed_generation_request(
                device_asset, {}, params, output) ==
                DeviceGenerationError::InvalidRadiusScale,
            "checked device radius scale");

    const DeviceAssetDescriptor short_asset{
        device_asset.asset, device_asset.header, device_asset.header.byte_size - 1u};
    output.output.radius_scale = 1.0f;
    require(validate_device_packed_generation_request(
                short_asset, {}, params, output) ==
                DeviceGenerationError::AssetCapacityTooSmall,
            "checked device asset capacity");

    const DeviceAssetDescriptor misaligned_asset{
        DeviceAssetView{asset.bytes().data() + 1u}, device_asset.header,
        device_asset.byte_capacity - 1u};
    require(validate_device_packed_generation_request(
                misaligned_asset, {}, params, output) ==
                DeviceGenerationError::MisalignedAsset,
            "checked device asset alignment");

    DeviceAssetDescriptor bad_noise_table = device_asset;
    bad_noise_table.header.noise_gradients_offset =
        bad_noise_table.header.byte_size - sizeof(float);
    require(validate_device_packed_generation_request(
                bad_noise_table, {}, params, output) ==
                DeviceGenerationError::InvalidAssetMetadata,
            "checked device noise-table bounds");

    GenerationParams overflowing_params = params;
    overflowing_params.strand_count = std::numeric_limits<std::uint32_t>::max();
    overflowing_params.cvs_per_strand = std::numeric_limits<std::uint32_t>::max();
    require(validate_device_motion_generation_request(
                device_asset, motion_samples.data(), motion_samples.size(), overflowing_params,
                motion_output) == DeviceGenerationError::OutputSizeOverflow,
            "checked device motion output overflow");
}

void test_renderer_curve_payload_64k_boundary() {
    constexpr std::uint32_t strand_count = 64u * 1024u + 1u;
    std::vector<Vec3> points(static_cast<std::size_t>(strand_count) * 2u);
    std::vector<Vec2> root_uvs(strand_count);
    std::vector<float> ids(strand_count);
    for (std::uint32_t strand = 0u; strand < strand_count; ++strand) {
        points[static_cast<std::size_t>(strand) * 2u] = {static_cast<float>(strand), 0.0f, 0.0f};
        points[static_cast<std::size_t>(strand) * 2u + 1u] = {static_cast<float>(strand), 1.0f, 0.0f};
        root_uvs[strand] = {static_cast<float>(strand), 0.0f};
        ids[strand] = static_cast<float>(strand);
    }
    const CurveSetView source{
        strand_count, 2u, {}, points, {}, {}, root_uvs, 0.02f};
    const std::array<UniformFloatPrimvarView, 1u> primvars = {{{"id", ids}}};
    const std::vector<CurveBatch> batches = build_curve_batches(source, {}, primvars);
    require(batches.size() == 2u, "64K boundary must create two batches");
    require(batches[0].point_counts.size() == 64u * 1024u &&
            batches[0].points.size() == 128u * 1024u,
            "first 64K batch size");
    require(batches[1].first_strand == 64u * 1024u &&
            batches[1].point_counts.size() == 1u && batches[1].points.size() == 2u,
            "64K tail batch size");
    require(batches[1].root_uvs[0].x == static_cast<float>(64u * 1024u) &&
            batches[1].uniform_floats[0].values[0] == static_cast<float>(64u * 1024u),
            "64K tail attributes remain aligned");
}

void test_exact_curve_cache() {
    const std::vector<std::uint32_t> counts = {2u, 3u};
    const std::vector<PackedCurvePoint> points = {
        {0.0f, 0.0f, 0.0f, 0.01f}, {0.0f, 1.0f, 0.0f, 0.005f},
        {1.0f, 0.0f, 0.0f, 0.02f}, {1.0f, 1.0f, 0.0f, 0.01f},
        {1.0f, 2.0f, 0.0f, 0.002f},
    };
    const std::vector<Vec2> texcoords = {
        {0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 0.5f}, {1.0f, 1.0f}};
    const std::vector<Vec2> patch_uvs = {{0.2f, 0.3f}, {0.7f, 0.8f}};
    const std::vector<Vec2> face_uvs = {{0.1f, 0.2f}, {0.6f, 0.7f}};
    const std::vector<std::uint32_t> face_ids = {3u, 9u};
    const std::vector<float> motion_times = {0.5f};
    std::vector<Vec3> motion_points;
    motion_points.reserve(points.size());
    for (const PackedCurvePoint point : points) {
        motion_points.push_back({point.x + 0.25f, point.y, point.z});
    }
    const CurveCache cache = build_curve_cache(
        {counts, points, texcoords, patch_uvs, face_uvs, face_ids,
         motion_times, motion_points});
    require(validate_curve_cache(cache.bytes()).empty(), "fresh curve cache must validate");
    const CurveCacheView view = cache.view();
    require(view.header().strand_count == 2u && view.header().point_count == 5u,
            "curve-cache topology");
    require(std::memcmp(view.points(), points.data(), points.size() * sizeof(PackedCurvePoint)) == 0,
            "curve cache must preserve official packed floats bitwise");
    require(std::memcmp(view.texcoords(), texcoords.data(), texcoords.size() * sizeof(Vec2)) == 0 &&
            std::memcmp(view.patch_uvs(), patch_uvs.data(), patch_uvs.size() * sizeof(Vec2)) == 0 &&
            std::memcmp(view.face_uvs(), face_uvs.data(), face_uvs.size() * sizeof(Vec2)) == 0 &&
            std::memcmp(view.face_ids(), face_ids.data(), face_ids.size() * sizeof(std::uint32_t)) == 0,
            "curve cache must preserve public XGen attributes bitwise");
    require(view.header().motion_sample_count == 1u && view.motion_times()[0] == 0.5f &&
            std::memcmp(view.motion_points(), motion_points.data(),
                        motion_points.size() * sizeof(Vec3)) == 0,
            "curve cache must preserve aligned motion samples bitwise");

    AlignedByteVector corrupt(cache.bytes().begin(), cache.bytes().end());
    corrupt.back() ^= std::byte{1};
    require(validate_curve_cache(corrupt) == "curve-cache content hash mismatch",
            "curve-cache corruption must be detected");
}

template<typename T>
std::vector<std::byte> xgen_array_bytes(std::span<const T> values) {
    const std::span<const std::byte> bytes = std::as_bytes(values);
    return {bytes.begin(), bytes.end()};
}

template<typename T>
XGenArray xgen_array(std::uint64_t tag, std::initializer_list<T> values) {
    const std::span<const T> span{values.begin(), values.size()};
    return {tag, xgen_array_bytes(span)};
}

void test_self_contained_xgen_round_trip() {
    XGenDocument empty_group{};
    empty_group.metadata_json = R"json({"Header":{"Version":1,"Type":"XgSplineData","GroupVersion":1,"GroupCount":1,"GroupBase64":false,"GroupDeflate":true,"GroupDeflateLevel":9},"Items":[],"RefMeshArray":[],"CustomData":{}})json";
    empty_group.version = 1u;
    empty_group.group_version = 1u;
    empty_group.group_deflate = true;
    empty_group.group_deflate_level = 9u;
    empty_group.groups = {{0u, 0u, {}}};
    require(parse_xgen_document(serialize_xgen_document(empty_group)).groups[0].arrays.empty(),
            "self-contained XGen parser must support an empty generic group");

    XGenDocument source{};
    source.metadata_json = R"json({
  "Header":{"Version":1,"Type":"XgSplineData","GroupVersion":1,"GroupCount":2,"GroupBase64":false,"GroupDeflate":true,"GroupDeflateLevel":9},
  "Items":[{"Name":"groom","Id":"test","Mode":"Density","PrimitiveInfos":0,"Positions":1,"PatchUVs":2,"FaceUV":4294967296,"FaceId":4294967297,"WIDTH_CV":4294967298}],
  "RefMeshArray":[],"CustomData":{}
})json";
    source.version = 1u;
    source.group_version = 1u;
    source.group_deflate = true;
    source.group_deflate_level = 9u;
    source.groups = {
        {0u, 0u,
         {xgen_array<std::uint32_t>(kXGenUInt32ArrayTag,
                                    {0u, 2u, 0u, 2u, 3u, 0u}),
          xgen_array<Vec3>(kXGenVec3ArrayTag,
                           {{9.0f, 0.0f, 0.0f}, {9.0f, 1.0f, 0.0f},
                            {3.0f, 0.0f, 0.0f}, {3.0f, 0.5f, 0.0f},
                            {3.0f, 1.0f, 0.0f}}),
          xgen_array<Vec2>(kXGenVec2ArrayTag,
                           {{0.9f, 0.8f}, {0.9f, 0.8f}, {0.3f, 0.2f},
                            {0.3f, 0.2f}, {0.3f, 0.2f}})}},
        {1u, 17u,
         {xgen_array<Vec2>(kXGenVec2ArrayTag, {{0.7f, 0.6f}, {0.1f, 0.2f}}),
          xgen_array<std::uint32_t>(kXGenUInt32ArrayTag, {9u, 3u}),
          xgen_array<float>(kXGenFloatArrayTag,
                            {0.09f, 0.04f, 0.03f, 0.02f, 0.01f}),
          xgen_array<std::uint32_t>(0xdeadbeefull, {11u, 22u})}}
    };

    const std::vector<std::byte> encoded = serialize_xgen_document(source);
    const XGenDocument parsed = parse_xgen_document(encoded);
    require(parsed.groups.size() == 2u && parsed.groups[1].flags == 17u,
            "self-contained XGen group framing");
    require(serialize_xgen_document(parsed) == encoded,
            "self-contained XGen serialization must round-trip exactly");

    const XGenEvaluatedCurves curves = materialize_xgen_curves(parsed);
    require(curves.point_counts == std::vector<std::uint32_t>({3u, 2u}) &&
            curves.face_ids == std::vector<std::uint32_t>({3u, 9u}),
            "XGen curves must canonicalize by face and UV identity");
    require(curves.positions.size() == 5u && curves.positions.front().x == 3.0f &&
            curves.positions.back().x == 9.0f,
            "XGen canonicalization must keep varying arrays aligned");
    require(curves.texcoords[0].x == 0.0f && curves.texcoords[0].y == 0.0f &&
            curves.texcoords[1].y == 0.5f && curves.texcoords[2].y == 1.0f,
            "XGen renderer texcoords must match XgFnSpline (0,t)");

    const XGenEvaluatedCurves source_curves = materialize_xgen_curves(
        parsed, XGenCurveOrder::Source);
    require(source_curves.face_ids == std::vector<std::uint32_t>({9u, 3u}),
            "source-order materialization must preserve XGen Item order");
    const XGenPackedCurves packed = materialize_xgen_packed_curves(
        parsed, XGenCurveOrder::Source);
    require(packed.point_counts == source_curves.point_counts &&
                packed.points.size() == source_curves.positions.size() &&
                packed.points.front().radius == 0.5f * source_curves.widths.front(),
            "minimal packed XGen path must preserve topology and convert width to radius");
    const auto require_packed_equal = [&](const XGenPackedCurves &actual,
                                          const XGenPackedCurves &expected,
                                          const char *message) {
        require(actual.point_counts == expected.point_counts &&
                    actual.points.size() == expected.points.size() &&
                    std::memcmp(actual.points.data(), expected.points.data(),
                                expected.points.size() * sizeof(PackedCurvePoint)) == 0,
                message);
    };
    XGenPackedCurves full_source{};
    full_source.point_counts = source_curves.point_counts;
    for (std::size_t point = 0u; point < source_curves.positions.size(); ++point) {
        full_source.points.push_back({
            source_curves.positions[point].x, source_curves.positions[point].y,
            source_curves.positions[point].z, 0.5f * source_curves.widths[point]});
    }
    XGenPackedCurves full_canonical{};
    full_canonical.point_counts = curves.point_counts;
    for (std::size_t point = 0u; point < curves.positions.size(); ++point) {
        full_canonical.points.push_back({
            curves.positions[point].x, curves.positions[point].y,
            curves.positions[point].z, 0.5f * curves.widths[point]});
    }
    require_packed_equal(
        packed, full_source,
        "document packed source path must be bit-exact with full materialization");
    require_packed_equal(
        materialize_xgen_packed_curves(parsed, XGenCurveOrder::Canonical),
        full_canonical,
        "document packed canonical path must be bit-exact with full materialization");
    require_packed_equal(
        parse_xgen_packed_curves(encoded, XGenCurveOrder::Source), full_source,
        "resident fused source path must be bit-exact with full materialization");
    require_packed_equal(
        parse_xgen_packed_curves(encoded, XGenCurveOrder::Canonical), full_canonical,
        "resident fused canonical path must be bit-exact with full materialization");

    XGenDocument invalid_channels = parsed;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(invalid_channels.groups[0].arrays[2].bytes.data(), &nan, sizeof(nan));
    const std::vector<std::byte> invalid_patch_uv =
        serialize_xgen_document(invalid_channels);
    require_packed_equal(
        parse_xgen_packed_curves(invalid_patch_uv, XGenCurveOrder::Source),
        full_source,
        "fused source path must not read a non-renderer PatchUV channel");
    bool rejected_invalid_channel = false;
    try {
        (void)parse_xgen_packed_curves(
            invalid_patch_uv, XGenCurveOrder::Canonical);
    } catch (const std::runtime_error &) {
        rejected_invalid_channel = true;
    }
    require(rejected_invalid_channel,
            "fused canonical path must validate identity UVs");

    invalid_channels = parsed;
    const float negative_width = -0.01f;
    std::memcpy(
        invalid_channels.groups[1].arrays[2].bytes.data(),
        &negative_width, sizeof(negative_width));
    rejected_invalid_channel = false;
    try {
        (void)parse_xgen_packed_curves(
            serialize_xgen_document(invalid_channels), XGenCurveOrder::Source);
    } catch (const std::runtime_error &) {
        rejected_invalid_channel = true;
    }
    require(rejected_invalid_channel,
            "fused source path must reject negative renderer widths");

    invalid_channels = parsed;
    const float infinity = std::numeric_limits<float>::infinity();
    std::memcpy(
        invalid_channels.groups[0].arrays[1].bytes.data(), &infinity,
        sizeof(infinity));
    rejected_invalid_channel = false;
    try {
        (void)parse_xgen_packed_curves(
            serialize_xgen_document(invalid_channels), XGenCurveOrder::Source);
    } catch (const std::runtime_error &) {
        rejected_invalid_channel = true;
    }
    require(rejected_invalid_channel,
            "fused source path must reject non-finite renderer positions");

    XGenDocument identity_document = parsed;
    process_xgen_document(identity_document, {});
    require(serialize_xgen_document(identity_document) == encoded,
            "identity XGen document processing must be byte-exact");

    XGenEvaluatedCurves processed = curves;
    process_xgen_curves(processed, {{1.0f, 2.0f, 3.0f}, 0.5f, 2.0f});
    require(processed.positions[0].x == 4.0f && processed.positions[0].y == 2.0f &&
            processed.positions[1].y == 2.25f && processed.widths[0] == 0.06f,
            "self-contained XGen processing must preserve roots and transform shapes");

    XGenDocument processed_document = parsed;
    const XGenArray unknown_before = processed_document.groups[1].arrays[3];
    process_xgen_document(processed_document,
                          {{1.0f, 2.0f, 3.0f}, 0.5f, 2.0f});
    const XGenEvaluatedCurves document_curves = materialize_xgen_curves(
        parse_xgen_document(serialize_xgen_document(processed_document)));
    require(document_curves.positions[0].x == 4.0f &&
            processed_document.metadata_json == parsed.metadata_json &&
            processed_document.groups[1].flags == 17u &&
            processed_document.groups[1].arrays[3].type_tag == unknown_before.type_tag &&
            processed_document.groups[1].arrays[3].bytes == unknown_before.bytes,
            "document processing must preserve metadata, groups, and unknown arrays");

    const XGenDocument generated = build_xgen_document(
        curves, {"generated", 128u, 9u});
    const XGenEvaluatedCurves regenerated = materialize_xgen_curves(
        parse_xgen_document(serialize_xgen_document(generated)));
    require(regenerated.point_counts == curves.point_counts &&
            regenerated.face_ids == curves.face_ids &&
            std::memcmp(regenerated.positions.data(), curves.positions.data(),
                        curves.positions.size() * sizeof(Vec3)) == 0 &&
            std::memcmp(regenerated.widths.data(), curves.widths.data(),
                        curves.widths.size() * sizeof(float)) == 0,
            "self-contained XGen generator must preserve renderer geometry bitwise");

    GenerationParams generation{};
    generation.strand_count = 4u;
    generation.cvs_per_strand = 3u;
    const GeneratedCurves native = generate_cpu(build_asset(fixture()), generation);
    const XGenEvaluatedCurves native_xgen = make_xgen_curves(native);
    require(native_xgen.point_counts == std::vector<std::uint32_t>(4u, 3u) &&
            std::memcmp(native_xgen.positions.data(), native.points.data(),
                        native.points.size() * sizeof(Vec3)) == 0 &&
            native_xgen.widths == native.widths,
            "native NanoXGen curves must adapt directly to the XGen writer");

    std::vector<std::byte> corrupt = encoded;
    corrupt.front() ^= std::byte{1};
    bool rejected = false;
    try {
        (void)parse_xgen_document(corrupt);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, "XGen parser must reject bad file magic");

    for (std::size_t length = 0u; length < encoded.size(); ++length) {
        rejected = false;
        try {
            (void)parse_xgen_document(std::span<const std::byte>{encoded}.first(length));
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        require(rejected, "XGen parser must reject every truncated prefix");
        rejected = false;
        try {
            (void)parse_xgen_packed_curves(
                std::span<const std::byte>{encoded}.first(length));
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        require(rejected, "fused XGen parser must reject every truncated prefix");
    }

    corrupt = encoded;
    corrupt.back() ^= std::byte{0x80};
    rejected = false;
    try {
        (void)parse_xgen_document(corrupt);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, "XGen parser must reject a corrupt compressed group");
    rejected = false;
    try {
        (void)parse_xgen_packed_curves(corrupt);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, "fused XGen parser must reject a corrupt compressed group");

    XGenEvaluatedCurves duplicate_curves = source_curves;
    duplicate_curves.face_ids[1] = duplicate_curves.face_ids[0];
    duplicate_curves.face_uvs[1] = duplicate_curves.face_uvs[0];
    duplicate_curves.patch_uvs[1] = duplicate_curves.patch_uvs[0];
    const XGenDocument duplicate_document = build_xgen_document(duplicate_curves);
    const std::vector<std::byte> duplicate_encoded =
        serialize_xgen_document(duplicate_document);
    require(parse_xgen_packed_curves(
                duplicate_encoded, XGenCurveOrder::Source).point_counts.size() == 2u,
            "source-order fused parsing must not require canonical identities");
    rejected = false;
    try {
        (void)parse_xgen_packed_curves(
            duplicate_encoded, XGenCurveOrder::Canonical);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, "fused canonical parsing must reject duplicate identities");
    rejected = false;
    try {
        (void)materialize_xgen_curves(
            duplicate_document, XGenCurveOrder::Canonical);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, "full canonical materialization must reject duplicate identities");
}

void test_classic_typed_batch_validation() {
    struct Position {
        float x;
        float y;
        float z;
    };
    const std::vector<int> counts{2, 3};
    std::vector<Position> positions{
        {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {1.0f, 2.0f, 0.0f}};
    std::vector<float> widths{0.2f, 0.1f, 0.4f, 0.2f, 0.0f};
    std::vector<float> u{0.25f, 0.75f};
    std::vector<float> v{0.5f, 0.125f};
    std::vector<int> face_ids{3, 9};
    classic_typed::Curves curves;
    classic_typed::append_batch<Position>(
        counts, positions, widths, std::nullopt, u, v, face_ids, curves);
    require(curves.point_counts == std::vector<std::uint32_t>({2u, 3u}) &&
                curves.points.size() == positions.size() &&
                curves.points[0].radius == 0.1f &&
                curves.points[2].radius == 0.2f &&
                curves.face_uvs[1].x == 0.75f && curves.face_ids[1] == 9u,
            "Classic typed conversion must directly pack topology, radius, U/V, and FaceID");

    classic_typed::Curves constant_curves;
    classic_typed::append_batch<Position>(
        counts, positions, {}, 0.3f, u, v, face_ids, constant_curves);
    require(constant_curves.points.front().radius == 0.15f &&
                constant_curves.points.back().radius == 0.15f,
            "Classic typed conversion must support a validated constant width");
    classic_typed::Curves varying_curves;
    classic_typed::append_batch<Position>(
        std::vector<int>{4},
        std::vector<Position>{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
                              {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        std::vector<float>{0.2f, 0.4f}, std::nullopt,
        std::vector<float>{0.5f}, std::vector<float>{0.25f},
        std::vector<int>{7}, varying_curves);
    require(varying_curves.points[0].radius == 0.1f &&
                varying_curves.points[1].radius == 0.1f &&
                varying_curves.points[2].radius == 0.2f &&
                varying_curves.points[3].radius == 0.2f,
            "Classic B-spline varying widths must expand duplicated endpoints");

    const auto rejects = [&](const std::vector<int> &test_counts,
                             const std::vector<Position> &test_positions,
                             const std::vector<float> &test_widths,
                             std::optional<float> constant_width,
                             const std::vector<float> &test_u,
                             const std::vector<float> &test_v,
                             const std::vector<int> &test_face_ids) {
        classic_typed::Curves rejected_output;
        try {
            classic_typed::append_batch<Position>(
                test_counts, test_positions, test_widths, constant_width,
                test_u, test_v, test_face_ids, rejected_output);
        } catch (const std::runtime_error &) {
            return rejected_output.point_counts.empty() && rejected_output.points.empty();
        }
        return false;
    };

    std::vector<Position> bad_positions = positions;
    bad_positions[1].x = std::numeric_limits<float>::quiet_NaN();
    require(rejects(counts, bad_positions, widths, std::nullopt, u, v, face_ids),
            "Classic typed conversion must reject NaN positions transactionally");
    bad_positions = positions;
    bad_positions[1].z = std::numeric_limits<float>::infinity();
    require(rejects(counts, bad_positions, widths, std::nullopt, u, v, face_ids),
            "Classic typed conversion must reject infinite positions");
    std::vector<float> bad_widths = widths;
    bad_widths[2] = -0.1f;
    require(rejects(counts, positions, bad_widths, std::nullopt, u, v, face_ids),
            "Classic typed conversion must reject negative widths");
    bad_widths = widths;
    bad_widths[2] = std::numeric_limits<float>::infinity();
    require(rejects(counts, positions, bad_widths, std::nullopt, u, v, face_ids),
            "Classic typed conversion must reject infinite widths");
    std::vector<float> bad_u = u;
    bad_u[0] = std::numeric_limits<float>::quiet_NaN();
    require(rejects(counts, positions, widths, std::nullopt, bad_u, v, face_ids),
            "Classic typed conversion must reject non-finite U/V");
    require(rejects(counts, positions, {0.2f, 0.1f}, std::nullopt, u, v, face_ids),
            "Classic typed conversion must reject inconsistent width cardinality");
    require(rejects({2, 4}, positions, widths, std::nullopt, u, v, face_ids),
            "Classic typed conversion must reject inconsistent point topology");
    require(rejects({1, 4}, positions, widths, std::nullopt, u, v, face_ids),
            "Classic typed conversion must reject one-vertex curves");
    require(rejects(counts, positions, widths, std::nullopt, {0.25f}, v, face_ids),
            "Classic typed conversion must reject inconsistent U cardinality");
    require(rejects(counts, positions, {}, -0.1f, u, v, face_ids),
            "Classic typed conversion must reject a negative constant width");

    curves.motion_positions.resize(2u);
    std::vector<Position> motion_positions = positions;
    for (Position &position : motion_positions) { position.x += 2.0f; }
    classic_typed::append_motion_batch<Position>(
        counts, motion_positions, 0u, 0u, 1u, curves);
    for (Position &position : motion_positions) { position.y += 3.0f; }
    classic_typed::append_motion_batch<Position>(
        counts, motion_positions, 0u, 0u, 2u, curves);
    require(
        curves.motion_positions[0u].size() == positions.size() &&
            curves.motion_positions[0u][1u].x == 2.0f &&
            curves.motion_positions[1u][1u].y == 4.0f,
        "Classic typed conversion lost sample-major motion positions");
    bool rejected_motion = false;
    try {
        classic_typed::append_motion_batch<Position>(
            std::vector<int>{2, 2}, motion_positions,
            0u, 0u, 1u, curves);
    } catch (const std::runtime_error &) {
        rejected_motion = true;
    }
    require(
        rejected_motion,
        "Classic typed conversion accepted changing motion topology");
}

void test_maya_xgen_cache_identity_validation() {
    XGenEvaluatedCurves curves{};
    curves.point_counts = {2u, 2u};
    curves.face_ids = {7u, 7u};
    curves.face_uvs = {{0.25f, 0.5f}, {0.25f, 0.5f}};
    curves.patch_uvs = {{0.125f, 0.75f}, {0.125f, 0.75f}};
    bool rejected_duplicate = false;
    try {
        maya_xgen_cache::validate_unique_canonical_identities(curves);
    } catch (const std::runtime_error &) {
        rejected_duplicate = true;
    }
    require(rejected_duplicate,
            "Interactive canonical caching must reject duplicate exact identities");

    curves.patch_uvs[1].x = std::nextafter(0.125f, 1.0f);
    maya_xgen_cache::validate_unique_canonical_identities(curves);
    curves.face_ids = {8u, 7u};
    bool rejected_order = false;
    try {
        maya_xgen_cache::validate_unique_canonical_identities(curves);
    } catch (const std::runtime_error &) {
        rejected_order = true;
    }
    require(rejected_order,
            "Interactive canonical caching must require exact identity ordering");
}

} // namespace

int main() try {
    test_blob_round_trip();
    test_determinism_and_roots();
    test_guide_interpolation();
    test_deformed_geometry_generation();
    test_cpu_persistent_work_queue();
    test_context_thread_pool();
    test_xgen_noise_math_and_length_preservation();
    test_area_weighted_sampling();
    test_linear_compatibility_generation();
    test_renderer_curve_payload();
    test_direct_packed_generation_math();
    test_checked_device_generation_contract();
    test_renderer_curve_payload_64k_boundary();
    test_exact_curve_cache();
    test_self_contained_xgen_round_trip();
    test_classic_typed_batch_validation();
    test_maya_xgen_cache_identity_validation();
    std::cout << "all NanoXGen tests passed\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "test failure: " << e.what() << '\n';
    return 1;
}
