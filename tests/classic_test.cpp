#include "nanoxgen/xgen_classic.h"
#include "nanoxgen/xgen_classic_runtime.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

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
    try {
        apply_xgen_classic_float_runtime_plan_cpu(curves, plan);
    } catch (const std::runtime_error &error) {
        require(std::string{error.what()}.find("negative") != std::string::npos,
                "negative width diagnostic mismatch");
        return;
    }
    throw std::runtime_error("negative Classic width was accepted");
}

} // namespace

int main() try {
    test_typed_parse();
    test_validation();
    test_limits();
    test_float_runtime_plan();
    test_float_runtime_fallbacks_and_validation();
    std::cout << "Classic XGen parser tests passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
}
