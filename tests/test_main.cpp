#include "nanoxgen/asset.h"
#include "nanoxgen/curve_cache.h"
#include "nanoxgen/curve_payload.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace nanoxgen;

namespace {

void require(bool condition, const char *message) {
    if (!condition) { throw std::runtime_error(message); }
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

    std::vector<std::byte> corrupt(asset.bytes().begin(), asset.bytes().end());
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
        require(packed[point].radius == 0.5f * soa.widths[point] * 1.5f,
                "packed generation radius conversion");
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

    std::vector<std::byte> corrupt(cache.bytes().begin(), cache.bytes().end());
    corrupt.back() ^= std::byte{1};
    require(validate_curve_cache(corrupt) == "curve-cache content hash mismatch",
            "curve-cache corruption must be detected");
}

} // namespace

int main() try {
    test_blob_round_trip();
    test_determinism_and_roots();
    test_guide_interpolation();
    test_deformed_geometry_generation();
    test_cpu_persistent_work_queue();
    test_area_weighted_sampling();
    test_linear_compatibility_generation();
    test_renderer_curve_payload();
    test_direct_packed_generation_math();
    test_checked_device_generation_contract();
    test_renderer_curve_payload_64k_boundary();
    test_exact_curve_cache();
    std::cout << "all NanoXGen tests passed\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "test failure: " << e.what() << '\n';
    return 1;
}
