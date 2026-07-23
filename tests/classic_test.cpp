#include "nanoxgen/xgen_classic.h"
#include "nanoxgen/xgen_classic_runtime.h"
#include "nanoxgen/xpd.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nanoxgen;

namespace {

void require(bool condition, const char *message) {
    if (!condition) { throw std::runtime_error(message); }
}

ClassicCollection parse(const std::string &text,
                        const ClassicParseLimits &limits = {}) {
    std::istringstream input(text);
    return parse_xgen_classic_collection(input, limits);
}

void require_fails(const std::string &text, const char *needle,
                   const ClassicParseLimits &limits = {}) {
    try {
        (void)parse(text, limits);
    } catch (const std::exception &error) {
        if (std::string(error.what()).find(needle) == std::string::npos) {
            throw std::runtime_error(
                "expected diagnostic '" + std::string(needle) +
                "', got '" + error.what() + "'");
        }
        return;
    }
    throw std::runtime_error(
        "malformed Classic collection was accepted; expected '" +
        std::string(needle) + "'");
}

std::string fixture(std::string id = "42", std::string loc = ".25 .75 7",
                    std::string cv = "4 5 6") {
    return
        "# fixture\n"
        "FileVersion 18\n\n"
        "Palette\n"
        "\tname\tpalette\n"
        "\texpression\thash($id)\\nmap('${DESC}/density')\n"
        "\tendAttrs\n\n"
        "Description\n"
        "\tname\tfur\n"
        "\tdescriptionId\t2\n"
        "\tendAttrs\n"
        "SplinePrimitive\n"
        "\twidth\thash($id)*0.01+0.01\n"
        "\tendAttrs\n"
        "RandomGenerator\n"
        "\tdensity\t100\n"
        "\tendAttrs\n"
        "\tActive\tSplinePrimitive\n"
        "\tActive\tRandomGenerator\n"
        "Patches fur 1\n"
        "Patch Subd\n"
        "\tname\tpatchShape\n"
        "\tfaceIds\t1 7\n"
        "\tunknown\tpreserved\n"
        "Guides Spline 1\n"
        "\tid\t" + id + "\n"
        "\tloc\t" + loc + "\n"
        "\tblend\t.5\n"
        "\tinterp\t1:2:3\n"
        "\tCVs\t3\n"
        "\t1 2 3\n"
        "\t" + cv + "\n"
        "endObject\n"
        "endPatches\n";
}

void test_typed_parse() {
    const ClassicCollection collection = parse(fixture());
    require(collection.file_version == 18u, "FileVersion mismatch");
    require(collection.descriptions.size() == 1u, "description count mismatch");
    const ClassicAttribute *palette_name = find_classic_attribute(
        collection.palette_attributes, "name");
    require(palette_name && palette_name->value == "palette",
            "palette attribute mismatch");
    const ClassicDescription *description = find_classic_description(
        collection, "fur");
    require(description && description->objects.size() == 2u,
            "description objects mismatch");
    require(description->bindings.size() == 2u,
            "description bindings mismatch");
    require(description->patches.size() == 1u, "patch count mismatch");
    const ClassicPatch &patch = description->patches.front();
    require(patch.type == "Subd" && patch.name == "patchShape",
            "patch identity mismatch");
    require(patch.face_ids.size() == 1u && patch.face_ids[0] == 7u,
            "face IDs mismatch");
    require(find_classic_attribute(patch.attributes, "unknown") != nullptr,
            "unknown patch attribute was not preserved");
    require(patch.guides.size() == 1u, "guide count mismatch");
    const ClassicGuide &guide = patch.guides.front();
    require(guide.id == 42u && guide.face_id == 7u &&
                guide.patch_u == .25 && guide.patch_v == .75 && guide.blend == .5,
            "guide metadata mismatch");
    require(guide.interpolation_offset == 0u &&
                guide.interpolation_count == 3u &&
                patch.guide_interpolation == std::vector<double>({1.0, 2.0, 3.0}),
            "packed interpolation mismatch");
    require(guide.cv_offset == 0u && guide.cv_count == 3u &&
                patch.guide_cvs.size() == 3u,
            "packed guide CV range mismatch");
    require(patch.guide_cvs[0].x == 0.0 && patch.guide_cvs[0].y == 0.0 &&
                patch.guide_cvs[0].z == 0.0,
            "implicit guide root was not inserted");
    require(patch.guide_cvs[2].x == 4.0 && patch.guide_cvs[2].y == 5.0 &&
                patch.guide_cvs[2].z == 6.0,
            "guide CV payload mismatch");
}

void test_validation() {
    require_fails(fixture("42", ".25 .75 8"),
                  "guide face ID is not declared");
    require_fails(fixture("not-an-id"), "invalid guide ID");
    require_fails(fixture("42", ".25 nan 7"), "non-finite guide v");
    require_fails(fixture("42", ".25 .75 7", "4 inf 6"),
                  "non-finite guide CV");

    std::string duplicate = fixture();
    const std::string guide =
        "\tid\t42\n\tloc\t.25 .75 7\n\tblend\t.5\n"
        "\tinterp\t1:2:3\n\tCVs\t2\n\t1 2 3\n";
    const std::size_t patches = duplicate.find("Patches fur 1");
    duplicate.replace(patches, std::string("Patches fur 1").size(),
                      "Patches fur 2");
    const std::size_t guides = duplicate.find("Guides Spline 1");
    duplicate.replace(guides, std::string("Guides Spline 1").size(),
                      "Guides Spline 2");
    const std::size_t end = duplicate.find("endObject");
    duplicate.insert(end, guide);
    require_fails(duplicate, "duplicate guide ID");

    std::string count_mismatch = fixture();
    count_mismatch.replace(count_mismatch.find("Patches fur 1"),
                           std::string("Patches fur 1").size(),
                           "Patches fur 2");
    require_fails(count_mismatch, "Patches guide count does not match");
}

void test_limits() {
    ClassicParseLimits limits{};
    limits.max_line_bytes = 8u;
    require_fails(fixture(), "line byte limit exceeded", limits);
    limits = {};
    limits.max_guide_cvs = 2u;
    require_fails(fixture(), "guide CV limit exceeded", limits);
    limits = {};
    limits.max_source_bytes = 32u;
    require_fails(fixture(), "source byte limit exceeded", limits);
}

void test_float_runtime_plan() {
    const ClassicCollection collection = parse(
        "FileVersion 18\n"
        "Palette\n\tname\tpalette\n\tendAttrs\n"
        "Description\n\tname\tfloatRuntime\n\tdescriptionId\t7\n\tendAttrs\n"
        "SplinePrimitive\n"
        "\tlength\t2\n"
        "\twidth\t0.2\n"
        "\ttaper\t0.5\n"
        "\ttaperStart\t0.5\n"
        "\twidthRamp\trampUI(0,1,1:1,0.5,1)\n"
        "\tfxCVCount\t3\n"
        "\tendAttrs\n"
        "CutFXModule\n"
        "\tactive\ttrue\n"
        "\tname\tcut\n"
        "\tamount\t0.25*$cLength\n"
        "\trebuildType\t1\n"
        "\tendAttrs\n"
        "\tActive\tSplinePrimitive\n");
    const ClassicFloatRuntimePlan plan =
        compile_xgen_classic_float_runtime_plan(
            collection.descriptions.front());
    require(plan.lowering_complete(),
            "self-contained float runtime plan unexpectedly needs fallback");
    require(plan.fx_cv_count == 3u && plan.cuts.size() == 1u,
            "float runtime plan metadata mismatch");

    PackedGeneratedCurves curves{};
    curves.strand_count = 1u;
    curves.cvs_per_strand = 3u;
    curves.point_counts = {3u};
    curves.points = {{0.0f, 0.0f, 0.0f, 0.0f},
                     {1.0f, 0.0f, 0.0f, 0.0f},
                     {2.0f, 0.0f, 0.0f, 0.0f}};
    curves.roots.resize(1u);
    curves.root_uvs.resize(1u);
    apply_xgen_classic_float_runtime_plan_cpu(curves, plan);
    require(std::abs(curves.points[1].x - 1.5f) < 1.0e-6f &&
                std::abs(curves.points[2].x - 3.0f) < 1.0e-6f,
            "float runtime length/cut result mismatch");
    require(std::abs(curves.points[0].radius - 0.1f) < 1.0e-6f &&
                std::abs(curves.points[1].radius - 0.075f) < 1.0e-6f &&
                std::abs(curves.points[2].radius - 0.025f) < 1.0e-6f,
            "float runtime width/taper/ramp result mismatch");
    add_xgen_classic_renderer_endpoints(curves);
    require(curves.cvs_per_strand == 5u && curves.point_counts[0u] == 5u &&
                std::abs(curves.points[0u].x + 1.5f) < 1.0e-6f &&
                std::abs(curves.points[4u].x - 4.5f) < 1.0e-6f &&
                curves.points[0u].radius == curves.points[1u].radius &&
                curves.points[4u].radius == curves.points[3u].radius,
            "Classic renderer endpoint expansion mismatch");

    ClassicDescription id_description{};
    id_description.name = "id";
    id_description.objects.push_back({"SplinePrimitive", {
        {"width", "$id", 1u}}, 1u});
    const ClassicFloatRuntimePlan id_plan =
        compile_xgen_classic_float_runtime_plan(id_description);
    PackedGeneratedCurves id_curves{};
    id_curves.strand_count = 1u;
    id_curves.cvs_per_strand = 2u;
    id_curves.point_counts = {2u};
    id_curves.points.resize(2u);
    id_curves.points[1u].y = 1.0f;
    id_curves.roots.resize(1u);
    const std::uint32_t primitive_id = 7u;
    apply_xgen_classic_float_runtime_plan_cpu(
        id_curves, id_plan, 1.0f, {}, {}, {&primitive_id, 1u});
    require(id_curves.points[0u].radius == 3.5f &&
                id_curves.points[1u].radius == 3.5f,
            "Classic runtime ignored the per-face primitive ID");
}

void test_float_runtime_fallbacks_and_validation() {
    ClassicDescription unsupported{};
    unsupported.name = "unsupported";
    unsupported.objects.push_back({"SplinePrimitive", {
        {"width", "$faceid", 1u}}, 1u});
    const ClassicFloatRuntimePlan fallback =
        compile_xgen_classic_float_runtime_plan(unsupported);
    require(!fallback.lowering_complete() && !fallback.width &&
                !fallback.fallback_reasons.empty(),
            "unsupported runtime binding was not retained as fallback");

    ClassicDescription dynamic_vector{};
    dynamic_vector.name = "dynamicVector";
    dynamic_vector.objects.push_back({"SplinePrimitive", {
        {"length", "$freq=rand(); noise($Prefg*$freq)", 1u}}, 1u});
    const ClassicFloatRuntimePlan dynamic_vector_plan =
        compile_xgen_classic_float_runtime_plan(dynamic_vector);
    require(!dynamic_vector_plan.lowering_complete() &&
                dynamic_vector_plan.pref_noise_inputs.empty(),
            "dynamic $Prefg vector noise was silently accepted");

    ClassicDescription negative{};
    negative.name = "negative";
    negative.objects.push_back({"SplinePrimitive", {
        {"width", "-1", 1u}}, 1u});
    const ClassicFloatRuntimePlan plan =
        compile_xgen_classic_float_runtime_plan(negative);
    PackedGeneratedCurves curves{};
    curves.strand_count = 1u;
    curves.cvs_per_strand = 2u;
    curves.point_counts = {2u};
    curves.points = {{0.0f, 0.0f, 0.0f, 0.1f},
                     {0.0f, 1.0f, 0.0f, 0.1f}};
    curves.roots.resize(1u);
    apply_xgen_classic_float_runtime_plan_cpu(curves, plan);
    require(curves.strand_count == 0u && curves.points.empty(),
            "negative authored Classic width was not culled like XGen");

    ClassicDescription threshold{};
    threshold.name = "threshold";
    threshold.objects.push_back({"SplinePrimitive", {
        {"width", "$id == 1 ? 0.0001 : 0.000099", 1u}}, 1u});
    const ClassicFloatRuntimePlan threshold_plan =
        compile_xgen_classic_float_runtime_plan(threshold);
    PackedGeneratedCurves threshold_curves{};
    threshold_curves.strand_count = 2u;
    threshold_curves.cvs_per_strand = 2u;
    threshold_curves.point_counts = {2u, 2u};
    threshold_curves.points = {{0.0f, 0.0f, 0.0f, 0.1f},
                               {0.0f, 1.0f, 0.0f, 0.1f},
                               {1.0f, 0.0f, 0.0f, 0.1f},
                               {1.0f, 1.0f, 0.0f, 0.1f}};
    threshold_curves.roots.resize(2u);
    const std::array<std::uint32_t, 2u> ids{1u, 2u};
    apply_xgen_classic_float_runtime_plan_cpu(
        threshold_curves, threshold_plan, 1.0f, {}, {}, ids);
    require(threshold_curves.strand_count == 1u &&
                threshold_curves.points.size() == 2u &&
                threshold_curves.points[0u].radius == 0.00005f,
            "Classic width culling threshold does not match XGen");
}

void test_float_runtime_cut_culling() {
    ClassicDescription description{};
    description.name = "cutCulling";
    description.objects.push_back({"SplinePrimitive", {
        {"fxCVCount", "3", 1u}}, 1u});
    description.objects.push_back({"CutFXModule", {
        {"active", "true", 2u}, {"name", "cut", 3u},
        {"amount", "10", 4u}, {"rebuildType", "1", 5u}}, 2u});
    const ClassicFloatRuntimePlan plan =
        compile_xgen_classic_float_runtime_plan(description);
    PackedGeneratedCurves curves{};
    curves.strand_count = 1u;
    curves.cvs_per_strand = 3u;
    curves.point_counts = {3u};
    curves.points = {{0.0f, 0.0f, 0.0f, 0.1f},
                     {0.0f, 0.5f, 0.0f, 0.1f},
                     {0.0f, 1.0f, 0.0f, 0.1f}};
    curves.roots.resize(1u);
    curves.root_uvs.resize(1u);
    apply_xgen_classic_float_runtime_plan_cpu(curves, plan);
    require(curves.strand_count == 0u && curves.points.empty() &&
                curves.point_counts.empty() && curves.roots.empty() &&
                curves.root_uvs.empty(),
            "fully cut Classic strand was not culled");
}

void test_float_runtime_spline_length_binding() {
    ClassicDescription description{};
    description.name = "splineLength";
    description.objects.push_back({"SplinePrimitive", {
        {"fxCVCount", "4", 1u}, {"width", "$cLength", 2u}}, 1u});
    const ClassicFloatRuntimePlan plan =
        compile_xgen_classic_float_runtime_plan(description);
    require(plan.lowering_complete(),
            "spline-length binding fixture unexpectedly needs fallback");
    PackedGeneratedCurves curves{};
    curves.strand_count = 1u;
    curves.cvs_per_strand = 4u;
    curves.point_counts = {4u};
    curves.points = {{0.0f, 0.0f, 0.0f, 0.0f},
                     {1.0f, 1.0f, 0.0f, 0.0f},
                     {2.0f, -1.0f, 0.0f, 0.0f},
                     {3.0f, 0.0f, 0.0f, 0.0f}};
    curves.roots.resize(1u);
    apply_xgen_classic_float_runtime_plan_cpu(curves, plan);
    // Autodesk SgCurve::length uses exactly 2*N+4 fixed intervals and raw
    // endpoint CVs. This oracle is 0.5 * 3.796864797465667; the control
    // polygon would incorrectly produce a radius near 2.53225.
    for (const PackedCurvePoint point : curves.points) {
        require(std::abs(point.radius - 1.8984324f) < 2.0e-6f,
                "Classic $cLength did not use SgCurve fixed sampling");
    }
}

void test_float_runtime_clump() {
    ClassicDescription description{};
    description.name = "clump";
    description.objects.push_back({"SplinePrimitive", {
        {"fxCVCount", "3", 1u}}, 1u});
    description.objects.push_back({"ClumpingFXModule", {
        {"active", "true", 2u}, {"name", "Clumping1", 3u},
        {"mask", "1", 4u}, {"clump", "1", 5u},
        {"clumpScale", "rampUI(0,0,1:1,0,1)", 6u},
        {"clumpVariance", "0", 7u}, {"cut", "0", 8u},
        {"copy", "0", 9u}, {"copyVariance", "0", 10u},
        {"curl", "0", 11u}, {"offset", "0", 12u},
        {"flatness", "0", 13u}, {"frame", "0", 14u},
        {"noise", "0", 15u}, {"useControlMaps", "0", 16u},
        {"clumpVolumize", "false", 17u},
        {"noiseScale", "rampUI(0,0,1)", 18u},
        {"noiseFrequency", "1", 19u},
        {"noiseCorrelation", "0", 20u}}, 2u});
    const ClassicFloatRuntimePlan plan =
        compile_xgen_classic_float_runtime_plan(description);
    require(plan.lowering_complete() && plan.clumps.size() == 1u &&
                plan.effects.size() == 1u &&
                plan.effects[0].type == ClassicFloatEffectType::Clump,
            "basic ClumpingFX module did not lower");
    PackedGeneratedCurves curves{};
    curves.strand_count = 1u;
    curves.cvs_per_strand = 3u;
    curves.point_counts = {3u};
    curves.roots.resize(1u);
    curves.points = {{0.0f, 0.0f, 0.0f, 0.1f},
                     {0.0f, 1.0f, 0.0f, 0.1f},
                     {0.0f, 2.0f, 0.0f, 0.1f}};
    ClassicClumpRuntimeData data{};
    data.module_name = "Clumping1";
    data.cvs_per_guide = 3u;
    data.guide_axes = {{1.0f, 0.0f, 0.0f},
                       {1.0f, 1.0f, 0.0f},
                       {1.0f, 2.0f, 0.0f}};
    data.guide_normals = {{0.0f, 1.0f, 0.0f}};
    data.guide_tangents = {{1.0f, 0.0f, 0.0f}};
    data.guide_uvs = {{0.0f, 0.0f}};
    data.guide_face_ids = {0u};
    data.guide_random_prefixes = {0u};
    data.strand_guide_indices = {0u};
    const std::array bindings{std::move(data)};
    apply_xgen_classic_float_runtime_plan_cpu(
        curves, plan, 1.0f, {}, {}, {}, bindings);
    require(curves.points[0].x == 0.0f &&
                std::abs(curves.points[1].x - 1.0f) < 1.0e-6f &&
                std::abs(curves.points[2].x - 1.0f) < 1.0e-6f,
            "basic ClumpingFX geometry mismatch");

    for (ClassicAttribute &attribute : description.objects[1].attributes) {
        if (attribute.name == "clump") { attribute.value = "0"; }
    }
    const ClassicFloatRuntimePlan rebuild_plan =
        compile_xgen_classic_float_runtime_plan(description);
    PackedGeneratedCurves curved{};
    curved.strand_count = 1u;
    curved.cvs_per_strand = 4u;
    curved.point_counts = {4u};
    curved.roots.resize(1u);
    curved.points = {{0.0f, 0.0f, 0.0f, 0.1f},
                     {0.5f, 0.7f, 0.0f, 0.1f},
                     {-0.2f, 1.4f, 0.0f, 0.1f},
                     {0.0f, 2.0f, 0.0f, 0.1f}};
    ClassicClumpRuntimeData long_guide{};
    long_guide.module_name = "Clumping1";
    long_guide.cvs_per_guide = 4u;
    long_guide.guide_axes = {{0.0f, 0.0f, 0.0f},
                             {0.0f, 2.0f, 0.0f},
                             {0.0f, 4.0f, 0.0f},
                             {0.0f, 6.0f, 0.0f}};
    long_guide.guide_normals = {{0.0f, 1.0f, 0.0f}};
    long_guide.guide_tangents = {{1.0f, 0.0f, 0.0f}};
    long_guide.guide_uvs = {{0.0f, 0.0f}};
    long_guide.guide_face_ids = {0u};
    long_guide.guide_random_prefixes = {0u};
    long_guide.strand_guide_indices = {0u};
    const std::array rebuild_binding{long_guide};
    const std::vector<PackedCurvePoint> original = curved.points;
    apply_xgen_classic_float_runtime_plan_cpu(
        curved, rebuild_plan, 1.0f, {}, {}, {}, rebuild_binding);
    require(curved.points.front().x == original.front().x &&
                curved.points.back().x == original.back().x &&
                std::abs(curved.points[1].x - original[1].x) > 1.0e-3f,
            "ClumpingFX did not rebuild a full-length affected curve");
    const std::vector<PackedCurvePoint> rebuilt = curved.points;

    ClassicDescription noisy_description = description;
    for (ClassicAttribute &attribute : noisy_description.objects[1].attributes) {
        if (attribute.name == "noise") { attribute.value = "1"; }
        if (attribute.name == "noiseScale") {
            attribute.value = "rampUI(0,1,1:1,1,1)";
        }
        if (attribute.name == "useControlMaps") { attribute.value = "1"; }
    }
    const ClassicFloatRuntimePlan noisy_plan =
        compile_xgen_classic_float_runtime_plan(noisy_description);
    require(noisy_plan.lowering_complete() &&
                noisy_plan.clumps[0].use_control_maps,
            "ClumpingFX noise/control-map metadata did not lower");
    curved.points = original;
    apply_xgen_classic_float_runtime_plan_cpu(
        curved, noisy_plan, 1.0f, {}, {}, {}, rebuild_binding);
    bool noise_changed_curve = false;
    for (std::size_t point = 0u; point < curved.points.size(); ++point) {
        const PackedCurvePoint value = curved.points[point];
        require(std::isfinite(value.x) && std::isfinite(value.y) &&
                    std::isfinite(value.z),
                "ClumpingFX noise produced a non-finite point");
        noise_changed_curve |=
            std::memcmp(&value, &rebuilt[point], sizeof(value)) != 0;
    }
    require(noise_changed_curve, "ClumpingFX noise did not move the guide");

    for (ClassicAttribute &attribute : description.objects[1].attributes) {
        if (attribute.name == "mask") { attribute.value = "0"; }
    }
    const ClassicFloatRuntimePlan masked_plan =
        compile_xgen_classic_float_runtime_plan(description);
    curved.points = original;
    apply_xgen_classic_float_runtime_plan_cpu(
        curved, masked_plan, 1.0f, {}, {}, {}, rebuild_binding);
    require(std::memcmp(curved.points.data(), original.data(),
                        original.size() * sizeof(PackedCurvePoint)) == 0,
            "zero ClumpingFX mask modified curve CVs");
}

void test_xpd3_reader() {
    std::vector<std::byte> bytes;
    const auto u8 = [&](std::uint8_t value) {
        bytes.push_back(static_cast<std::byte>(value));
    };
    const auto u32 = [&](std::uint32_t value) {
        for (unsigned int shift = 0u; shift < 32u; shift += 8u) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    };
    const auto u64 = [&](std::uint64_t value) {
        u32(static_cast<std::uint32_t>(value));
        u32(static_cast<std::uint32_t>(value >> 32u));
    };
    const auto f32 = [&](float value) { u32(std::bit_cast<std::uint32_t>(value)); };
    for (const char value : std::string{"XPD3"}) { u8(value); }
    u8(0u);                   // file version
    u32(0u);                  // Point
    u8(1u);                   // primitive version
    f32(0.0f);                // time
    u32(1u);                  // CVs
    u32(0u);                  // World
    u32(1u);                  // blocks
    u32(9u);                  // block-string bytes
    for (const char value : std::string{"Location"}) { u8(value); }
    u8(0u);
    u32(6u);                  // floats per point
    u32(0u);                  // keys
    u32(0u);                  // key-string bytes
    u32(2u);                  // faces
    u32(7u);                  // populated face ID
    u32(9u);                  // empty face ID
    u32(1u);                  // populated primitive count
    u32(0u);                  // empty primitive count
    constexpr std::uint64_t header_size = 87u;
    u64(header_size);
    u64(std::numeric_limits<std::uint64_t>::max());
    for (const float value : {1.0f, 0.25f, 0.75f, 4.0f, 5.0f, 6.0f}) {
        f32(value);
    }
    require(bytes.size() == header_size + 24u, "XPD fixture size mismatch");
    const XpdDocument document = parse_xpd_document(bytes);
    require(document.blocks.size() == 1u &&
                document.blocks[0].name == "Location" &&
                document.blocks[0].floats_per_primitive == 6u,
            "XPD block metadata mismatch");
    require(document.faces.size() == 2u &&
                document.faces[0].face_id == 7 &&
                document.faces[0].primitive_count == 1u &&
                document.faces[1].face_id == 9 &&
                document.faces[1].primitive_count == 0u,
            "XPD face metadata mismatch");
    std::vector<float> record(6u);
    copy_xpd_primitive(document, 0u, 0u, 0u, record);
    require(record == std::vector<float>({1.0f, 0.25f, 0.75f,
                                          4.0f, 5.0f, 6.0f}),
            "XPD primitive payload mismatch");
    bytes.pop_back();
    try {
        (void)parse_xpd_document(bytes);
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("truncated XPD payload was accepted");
}

} // namespace

int main() try {
    test_typed_parse();
    test_validation();
    test_limits();
    test_float_runtime_plan();
    test_float_runtime_fallbacks_and_validation();
    test_float_runtime_cut_culling();
    test_float_runtime_spline_length_binding();
    test_float_runtime_clump();
    test_xpd3_reader();
    std::cout << "Classic XGen parser tests passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
}
